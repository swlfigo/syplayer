#include "media/demuxer.h"
#include "media/video_geometry.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace syp::media {

Demuxer::~Demuxer() {
    if (fmt_ == nullptr) return;
    // open_avio 路径不接管调用方的 AVIOContext：不把 fmt_->pb 置空就直接
    // avformat_close_input——这一处照抄
    // tools/syp_probe/packet_digest.cpp 的 demux_avio 已经验证过的处理：
    // avformat_close_input 自己会用 AVFMT_FLAG_CUSTOM_IO 判断，命中时只清
    // 它*局部*的 pb 变量，既不 close 也不 free 调用方持有的 AVIOContext；
    // 而 read_close(s) 在这之前执行，某些 demuxer 的 read_close 依赖 s->pb
    // 仍然有效，提前置空反而会在这些 demuxer 上出问题。
    avformat_close_input(&fmt_);
}

void Demuxer::build_tracks() {
    tracks_.clear();
    tracks_.reserve(fmt_->nb_streams);
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        const AVStream*          st = fmt_->streams[i];
        const AVCodecParameters* p  = st->codecpar;

        TrackInfo t;
        t.index     = static_cast<int32_t>(i);
        t.codec_id  = static_cast<int32_t>(p->codec_id);
        t.is_video  = (p->codec_type == AVMEDIA_TYPE_VIDEO);
        t.time_base = st->time_base;
        // 封面图（专辑封面）在 mp4 里是一条 is_video 为真、只有一个采样的
        // 轨。见 TrackInfo::attached_pic 的注释。
        t.attached_pic = (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        // HLS 选轨（after_open 回调）把未选中 variant 的流设成了
        // AVDISCARD_ALL。这些流仍然在 streams[] 里，必须原样报给调用方，
        // 否则 TrackPlayer 会把一条永远不会有帧的流绑成主视频轨。
        t.discard = (st->discard == AVDISCARD_ALL);
        if (t.is_video) {
            t.width  = p->width;
            t.height = p->height;
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            t.sample_rate = p->sample_rate;
            t.channels    = p->ch_layout.nb_channels;
        }

        // SAR：与 ffplay 同一取法（fftools/ffplay.c:3040），用
        // av_guess_sample_aspect_ratio()（libavformat/avformat.c:664）——
        // **优先取流上的** st->sample_aspect_ratio，无效（num/den <= 0）才退回
        // codecpar 的。原来的写法是"读 codecpar->sample_
        // aspect_ratio"，那会丢掉只写在容器层的 SAR：MP4 的 pasp、tkhd 宽高
        // ≠ 编码宽高、tkhd 矩阵缩放都写进流上（libavformat/mov.c:5284/5289/
        // 5667），MKV 的 DisplayWidth/Height 也写在流上（matroskadec.c:2996），
        // 这些都**不进** codecpar。两者都无效时返回 {0,1}，按 TrackInfo 契约
        // 表示未知、按 1:1 处理。结果已经 av_reduce 约分过。
        // 钉住的用例：tests/test_demuxer.cpp
        // track_info_reports_container_level_sample_aspect_ratio（素材只有
        // 容器层 3:4，码流层 1:1）。
        const AVRational sar = av_guess_sample_aspect_ratio(fmt_, fmt_->streams[i], nullptr);
        t.sar_num = sar.num;
        t.sar_den = sar.den;

        // 旋转：从 codecpar 的 coded_side_data 取 displaymatrix。
        //
        // 【符号依据：FFmpeg 8.1.2 源码三处一致】
        // - doc/ffmpeg.texi:1546,1550：`-display_rotation R` 是"视频应
        //   **逆时针**旋转 R 度后显示"。
        // - fftools/cmdutils.c:1557：FFmpeg 自己的播放器取
        //   `theta = -round(av_display_rotation_get(matrix))` 作为需要
        //   施加的旋转角。
        // - fftools/ffplay.c:2023-2033：`theta==90` 插入 `transpose=clock`
        //   （顺时针 90°），`theta==270` 插入 `transpose=cclock`——theta
        //   本身就是"顺时针施加的角度"，跟本文件 rotation_deg 的契约
        //   （TrackInfo::rotation_deg 注释）同一语义。
        // ⇒ rotation_deg = normalize_rotation(-av_display_rotation_get())，
        // 即取负。这个符号由 test_demuxer 的
        // track_info_reports_display_rotation_ccw90_needs_cw270 /
        // track_info_reports_display_rotation_ccw270_needs_cw90 两条用例
        // （90/270 双向 + 模 360 关系断言）钉住。
        t.rotation_deg = 0;
        if (const AVPacketSideData* sd = av_packet_side_data_get(
                p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
            const double raw = av_display_rotation_get(
                reinterpret_cast<const int32_t*>(sd->data));
            t.rotation_deg = normalize_rotation(-raw);
        }
        // av_rescale_q 传入 AV_NOPTS_VALUE（INT64_MIN）是未定义行为——这一步
        // 判空是为了不把它喂给 av_rescale_q，跟下面 normalize_duration 收口
        // 最终微秒值是两件事：这里挡的是「乘法溢出」，normalize_duration
        // 挡的是「把 NOPTS/负值透传给调用方」。
        int64_t track_us = 0;
        if (st->duration != AV_NOPTS_VALUE) {
            track_us = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000000});
        }
        t.duration_us = normalize_duration(track_us);
        tracks_.push_back(t);
    }
    // AVFormatContext::duration 单位是 AV_TIME_BASE（微秒），直接就是
    // normalize_duration 期望的量纲，不需要 rescale、也不用像上面那样单独
    // 挡 av_rescale_q 的未定义行为——直接走同一个归一化函数收口。
    duration_us_ = normalize_duration(fmt_->duration);
}

std::unique_ptr<Demuxer> Demuxer::open_file(const std::string& path, syp_status* err) {
    syp_status local = SYP_OK;
    if (err == nullptr) err = &local;
    *err = SYP_OK;

    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        *err = SYP_ERR_OOM;
        return nullptr;
    }
    fmt->probesize            = kDefaultProbeSize;
    fmt->max_analyze_duration = kDefaultMaxAnalyzeDurationUs;

    const int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        // 失败时 avformat_open_input 已经把 fmt 释放并置空，不能再 free。
        *err = SYP_ERR_IO;
        return nullptr;
    }

    const int find_rc = avformat_find_stream_info(fmt, nullptr);
    if (find_rc < 0) {
        avformat_close_input(&fmt);
        *err = SYP_ERR_IO;
        return nullptr;
    }

    auto d  = std::unique_ptr<Demuxer>(new Demuxer());
    d->fmt_ = fmt;
    d->build_tracks();
    return d;
}

std::unique_ptr<Demuxer> Demuxer::open_avio(AVIOContext* ctx, syp_status* err) {
    syp_status local = SYP_OK;
    if (err == nullptr) err = &local;
    *err = SYP_OK;

    if (ctx == nullptr) {
        *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        *err = SYP_ERR_OOM;
        return nullptr;
    }
    // fmt->pb 在 avformat_open_input 之前设置，这样 open 失败路径也用的
    // 是这个调用方提供的 AVIOContext，不会误打开别的输入。AVFMT_FLAG_
    // CUSTOM_IO 这一行是显式冗余，不是必须——插探针实测过（FFmpeg
    // 8.1.2）：avformat_open_input 内部发现 fmt->pb 已经非空时，自己就会
    // 补上这个 flag（open_avio 路径命中 2 次，跟场景 B + H 的调用数吻合；
    // open_file 路径不设 pb，flag 不会被补，命中 0 次）。这里仍然显式设置，
    // 是不想让这份代码的正确性依赖 FFmpeg 这条"自动补齐"的内部行为——
    // 那条行为没有写进公开 API 契约，换个版本完全可能不补；显式设置的
    // 代价是一行赋值，换来的是不用去核对每个支持版本是否都会自动补。
    fmt->pb = ctx;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt->probesize            = kDefaultProbeSize;
    fmt->max_analyze_duration = kDefaultMaxAnalyzeDurationUs;

    const int rc = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (rc < 0) {
        // 同 open_file：失败时 fmt 已被释放并置空，不能再 free。
        *err = SYP_ERR_IO;
        return nullptr;
    }

    const int find_rc = avformat_find_stream_info(fmt, nullptr);
    if (find_rc < 0) {
        avformat_close_input(&fmt);
        *err = SYP_ERR_IO;
        return nullptr;
    }

    auto d  = std::unique_ptr<Demuxer>(new Demuxer());
    d->fmt_ = fmt;
    d->build_tracks();
    return d;
}

std::unique_ptr<Demuxer> Demuxer::open_prepared(
    AVFormatContext* prepared,
    const std::string& url,
    AVDictionary** opts,
    const std::function<void(AVFormatContext*)>& after_open,
    syp_status* err) {
    syp_status local = SYP_OK;
    if (err == nullptr) err = &local;
    *err = SYP_OK;

    if (prepared == nullptr || url.empty()) {
        *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    AVFormatContext* fmt      = prepared;
    fmt->probesize            = kDefaultProbeSize;
    fmt->max_analyze_duration = kDefaultMaxAnalyzeDurationUs;

    // opts 直接透传，不在这里复制一份：avformat_open_input 会把**未被
    // 消费**的条目写回 *opts，调用方靠这个判断"我给的选项有没有被认出来"。
    // 中途换成本地副本会把这个信号吞掉。
    const int rc = avformat_open_input(&fmt, url.c_str(), nullptr, opts);
    if (rc < 0) {
        // 与 open_file/open_avio 一致：失败时 fmt 已被释放并置空。
        // 顶层 AVIOContext（如果 io_open 已经建过）也已经由
        // avformat_open_input 的失败路径经 ff_format_io_close → 调用方的
        // io_close2 关掉了（libavformat/demux.c，FFmpeg 8.1.2），这里不需要
        // 也不能再碰它。
        *err = SYP_ERR_IO;
        return nullptr;
    }
    const int find_rc = avformat_find_stream_info(fmt, nullptr);
    if (find_rc < 0) {
        avformat_close_input(&fmt);
        *err = SYP_ERR_IO;
        return nullptr;
    }

    // find_stream_info 之后、build_tracks() 之前——见 demuxer.h 里
    // after_open 上方的注释，这个位置是契约的一部分，不是实现细节。
    if (after_open) after_open(fmt);

    auto d  = std::unique_ptr<Demuxer>(new Demuxer());
    d->fmt_ = fmt;
    d->build_tracks();
    return d;
}

Demuxer::ReadResult Demuxer::read(AVPacket** out, int32_t* track_index) {
    if (out == nullptr || track_index == nullptr) return ReadResult::Error;
    *out         = nullptr;
    *track_index = -1;

    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) return ReadResult::Error;

    const int rc = av_read_frame(fmt_, pkt);
    if (rc < 0) {
        av_packet_free(&pkt);
        return (rc == AVERROR_EOF) ? ReadResult::Eof : ReadResult::Error;
    }

    *track_index = pkt->stream_index;
    *out         = pkt;
    return ReadResult::Packet;
}

syp_status Demuxer::seek(int64_t ts_us) {
    // stream_index = -1 时 ts 的单位是 AV_TIME_BASE（微秒）。
    const int rc = av_seek_frame(fmt_, -1, ts_us, AVSEEK_FLAG_BACKWARD);
    return (rc >= 0) ? SYP_OK : SYP_ERR_IO;
}

}  // namespace syp::media
