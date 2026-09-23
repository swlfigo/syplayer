#include "frame_digest.h"

#include "frame_digest_internal.h"
#include "packet_digest_internal.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

using syp::media::Frame;
using syp::media::Pipeline;
using syp::media::StepOutcome;

namespace syp::probe {

namespace {

using detail::hex32;
using detail::sha256_of;

std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

// 视频哈希：把 AVFrame 的各平面拷进一块「没有行尾 padding」的连续 scratch
// buffer，再对这块 buffer 一次性求哈希。
//
// 为什么不手写「每平面按 pix_fmt 算高度、逐行按 stride 剥」的循环：那正是
// av_image_copy() 内部在做的事——它靠 libavutil 自己的 pix_fmt 描述符
// （av_pix_fmt_desc_get 背后那套表）决定平面数、色度平面的宽高降采样比例、
// 每个分量的位深，这些数据对 yuv420p 之类的常见格式是常识，但对
// 半平面（nv12）、高位深（yuv420p10le）、带 alpha（yuva420p）这类格式，
// 手写平面几何很容易漏一种。复用 libavutil 已经维护、已经被整个 FFmpeg
// 生态验证过的实现，比重新推导一遍这套表更不容易错——效果跟手写版完全
// 一样：目标 linesize 传 1（tight，不对齐）时，av_image_copy 逐行拷贝的
// 字节数就是「有效宽度 × 该分量字节数」，行与行之间不留任何 padding，
// 这正是 stride-aware 剥行要达到的效果。
//
// 实测（FFmpeg 8.1.2，align=1）：yuv420p/nv12/yuv420p10le/yuva420p/
// rgb24/yuv422p/yuv444p/gray 这 8 种格式，`linesize[i]` 都恰好等于「有效
// 宽度 × 分量字节数」、平面间连续无洞（奇数宽 17 也验过，chroma 宽高按
// `(w+1)>>1` 算），跟手写循环逐字节等价。但不是所有格式都能在 align=1
// 下分配成功——PAL8（要求最小对齐 4）与硬解专用格式（如 VideoToolbox）
// 会返回失败，见下面失败分支的处理：报错而不是静默退化。
//
// 返回 false 表示这个 pix_fmt 无法在当前策略下求出有意义的摘要——调用方
// 必须把它当成「整体终止」，不能吞掉：sha256_of(nullptr,0,out) 这种静默
// 降级会让两条路径在同一个不支持的格式上都退化成同一个空摘要，diff_frames
// 因此永远判「相等」，恰恰是本层要防的「该红时不红」。
bool hash_video_frame(const Frame& f, uint8_t out[32], std::string* error) {
    const auto pix_fmt = static_cast<AVPixelFormat>(f.pix_fmt());
    const int  w       = f.width();
    const int  h       = f.height();
    if (w <= 0 || h <= 0 || pix_fmt < 0) {
        if (error != nullptr) {
            *error = "invalid video frame metadata (w=" + std::to_string(w) +
                      " h=" + std::to_string(h) +
                      " fmt=" + std::to_string(static_cast<int>(f.pix_fmt())) + ")";
        }
        return false;
    }

    const uint8_t* src_data[4]     = {f.plane(0), f.plane(1), f.plane(2), f.plane(3)};
    const int      src_linesize[4] = {f.stride(0), f.stride(1), f.stride(2), f.stride(3)};

    uint8_t* tight[4]          = {nullptr, nullptr, nullptr, nullptr};
    int      tight_linesize[4] = {0, 0, 0, 0};
    const int size = av_image_alloc(tight, tight_linesize, w, h, pix_fmt, /*align=*/1);
    if (size < 0) {
        if (error != nullptr) {
            const char* name = av_get_pix_fmt_name(pix_fmt);
            *error = "unsupported pix_fmt for digest (" + std::string(name ? name : "?") +
                      "): av_image_alloc: " + av_err(size);
        }
        return false;
    }

    av_image_copy(tight, tight_linesize, src_data, src_linesize, pix_fmt, w, h);
    sha256_of(tight[0], size, out);
    av_freep(&tight[0]);
    return true;
}

// 音频哈希：按 nb_samples × channels × av_get_bytes_per_sample(fmt) 取，
// 不哈希整个 buffer。
//
// 音频跟视频的「padding」性质不同，不需要视频那种逐行剥离：
//   - planar（每个声道各自一个平面）：每个平面的有效数据是从头开始连续的
//     nb_samples × bytes_per_sample 字节，AVFrame 的音频 buffer 分配
//     （av_samples_get_buffer_size）只会在每个平面的*末尾*补对齐 padding，
//     不会像视频那样在每一行之间插洞——所以只要从平面起点截断到有效长度，
//     天然就避开了尾部 padding，不需要「stride」概念。
//   - packed（所有声道交织进 data[0]）：同理，有效数据是从头开始连续的
//     nb_samples × channels × bytes_per_sample 字节。
// 两种情况加总起来的有效字节数都是 nb_samples × channels ×
// bytes_per_sample，只是 planar 时分成 channels 份、每份独立截断。
//
// 返回 false 的语义跟 hash_video_frame 一样：调用方按整体终止处理，不
// 静默退化成空摘要。
bool hash_audio_frame(const Frame& f, uint8_t out[32], std::string* error) {
    const auto fmt        = static_cast<AVSampleFormat>(f.sample_fmt());
    const int  nb_samples = f.nb_samples();
    const int  channels   = f.channels();
    const int  bytes      = av_get_bytes_per_sample(fmt);
    if (nb_samples <= 0 || channels <= 0 || bytes <= 0) {
        if (error != nullptr) {
            *error = "invalid audio frame metadata (nb_samples=" + std::to_string(nb_samples) +
                      " channels=" + std::to_string(channels) +
                      " bytes_per_sample=" + std::to_string(bytes) + ")";
        }
        return false;
    }

    if (!av_sample_fmt_is_planar(fmt)) {
        // packed：所有声道已经交织进 data[0] 一块连续内存，不需要拼接，
        // 直接对有效长度一次性求哈希。
        const int32_t total_bytes = nb_samples * channels * bytes;
        sha256_of(f.plane(0), total_bytes, out);
        return true;
    }

    // planar：每个声道各自一个平面。`Frame::plane(i)` 底层是
    // `AVFrame::data[i]`，超过 AV_NUM_DATA_POINTERS（8）就只能走
    // `AVFrame::extended_data`——`Frame` 没有暴露这条路径（src/media/frame.h
    // 是平台无关的公开缝，暂不为这个边缘情况加接口），继续按 data[i] 读
    // 会在 8 声道以上（7.1 环绕声正好卡在边界上）静默把第 9 条及以后的
    // 声道摘要成全 0——同 hash_video_frame 的失败哲学，报错而不是悄悄
    // 漏掉一部分数据。
    if (channels > AV_NUM_DATA_POINTERS) {
        if (error != nullptr) {
            *error = "planar audio channels(" + std::to_string(channels) +
                      ") exceeds AV_NUM_DATA_POINTERS(" +
                      std::to_string(AV_NUM_DATA_POINTERS) +
                      "); Frame 未暴露 extended_data，无法安全求摘要";
        }
        return false;
    }

    // 跟视频哈希同一个手法（选项 a）——把各平面的有效字节拼进一块连续
    // scratch buffer，再一次性求哈希，而不是给 sha256_of 加一个增量版本
    // （packet_digest_internal.h 目前只有一次性接口）。这里不用
    // av_image_copy 那一套（那是图像专用 API），手动拼接即可——每个平面
    // 已经是连续的、不带行间 padding。
    const int32_t plane_bytes = nb_samples * bytes;
    std::vector<uint8_t> scratch(static_cast<size_t>(plane_bytes) * static_cast<size_t>(channels));
    for (int ch = 0; ch < channels; ++ch) {
        const uint8_t* p = f.plane(ch);
        if (p == nullptr) {
            // channels 已经 <= AV_NUM_DATA_POINTERS，这里理应总能取到平面
            // 指针；真的拿到 nullptr 说明帧本身有问题，同样报错而不是
            // 悄悄把这个声道摘要成全 0。
            if (error != nullptr) {
                *error = "planar audio channel " + std::to_string(ch) + " has null plane";
            }
            return false;
        }
        std::memcpy(scratch.data() + static_cast<size_t>(ch) * static_cast<size_t>(plane_bytes),
                    p, static_cast<size_t>(plane_bytes));
    }
    sha256_of(scratch.data(), static_cast<int32_t>(scratch.size()), out);
    return true;
}

}  // namespace

namespace detail {

bool digest_of_frame(const Frame& f, int32_t track_index, FrameDigest* out, std::string* error) {
    out->track_index = track_index;
    out->pts_us       = f.pts_us();
    out->duration_us  = f.duration_us();
    if (f.is_video()) {
        out->fmt    = f.pix_fmt();
        out->width  = f.width();
        out->height = f.height();
        return hash_video_frame(f, out->data_sha256, error);
    }
    out->fmt         = f.sample_fmt();
    out->sample_rate = f.sample_rate();
    out->channels    = f.channels();
    out->nb_samples  = f.nb_samples();
    return hash_audio_frame(f, out->data_sha256, error);
}

}  // namespace detail

namespace {
using detail::digest_of_frame;

// 参照路径的解码上下文：一条流一个（非音视频流为 nullptr，跟 Pipeline
// 「不托管的轨」语义一致——它们的包直接丢弃，不参与解码）。
struct RefTrack {
    AVCodecContext* ctx       = nullptr;
    AVRational      time_base = {0, 1};
    bool            is_video  = false;
};

void close_ref_tracks(std::vector<RefTrack>& tracks) {
    for (RefTrack& t : tracks) {
        if (t.ctx != nullptr) avcodec_free_context(&t.ctx);
    }
}

// 排空一个解码器当前已经缓冲好、可以直接 receive 出来的所有帧，按接收到的
// 顺序 append 进 out.frames。track_index 用容器里的流下标——跟 Pipeline
// 的 track_index 编号规则相同（都是 AVStream 数组下标），两条路径的
// FrameDigest::track_index 才能直接比。
//
// max_frames 检查放在这个内循环里而不是只放在 run_decode 的外层包读循环——
// I7：一次 send() 可能吐出不止一帧（B 帧重排序、某些音频解码器
// 一包多帧），只在外层判 max_frames 会让参照路径在达到阈值之前的最后一个
// 包上「超发」，跟被测路径（Pipeline 每次 step() 至多产一帧，卡得死）不
// 对称——当前 fixture（H.264/AAC，每包至多出一帧）没有复现，但换一种
// 编码就会变成两条路径帧数不一致的、无法归因的假红。*stopped_early 让
// 调用方（run_decode）知道该提前收尾（比如跳过 flush）。
bool drain_ready_frames(AVCodecContext* ctx, AVRational tb, bool is_video,
                         int32_t track_index, const DecodeOptions& opt,
                         DecodeResult& out, bool* stopped_early) {
    for (;;) {
        if (opt.max_frames >= 0 &&
            static_cast<int64_t>(out.frames.size()) >= opt.max_frames) {
            *stopped_early = true;
            return true;
        }
        AVFrame* raw = av_frame_alloc();
        if (raw == nullptr) {
            out.error_stage = "av_frame_alloc";
            out.averror     = AVERROR(ENOMEM);
            return false;
        }
        const int rc = avcodec_receive_frame(ctx, raw);
        if (rc == 0) {
            Frame f = Frame::from_av(raw, tb, is_video);
            FrameDigest d;
            std::string err;
            if (!digest_of_frame(f, track_index, &d, &err)) {
                out.error_stage = "digest_of_frame: " + err;
                out.averror     = AVERROR_UNKNOWN;
                return false;
            }
            out.frames.push_back(std::move(d));
            continue;
        }
        av_frame_free(&raw);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        out.error_stage = "avcodec_receive_frame: " + av_err(rc);
        out.averror     = rc;
        return false;
    }
}

// 两条路径共用的主体：fmt 已 open_input 成功，从这里开始建各轨解码器、
// 读包、送包、排空。
void run_decode(AVFormatContext* fmt, const DecodeOptions& opt, DecodeResult& out) {
    int rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        out.averror     = rc;
        out.error_stage = "find_stream_info: " + av_err(rc);
        return;
    }

    std::vector<RefTrack> tracks(fmt->nb_streams);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        // 挑轨道用 codec_type，不用别的判据——跟 pipeline.cpp::create_common
        // 同一条纪律（demuxer.h 顶部警示：字幕/数据轨的 is_video 也是
        // false，不能拿来当音频判据）。字幕/数据/附件轨的 ctx 留空
        // （不托管），它们的包在下面读取循环里直接丢弃。
        if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (codec == nullptr) continue;   // 该轨终止（不支持的编码），其它轨继续
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (ctx == nullptr) continue;
        if (avcodec_parameters_to_context(ctx, par) < 0) {
            avcodec_free_context(&ctx);
            continue;
        }
        ctx->pkt_timebase  = fmt->streams[i]->time_base;
        // 跟 ffmpeg_video_decoder.cpp / ffmpeg_audio_decoder.cpp 同一条纪律：
        // 显式钉死单线程，否则多线程解码器内部帧完成顺序会跟机器有关，
        // 参照路径本身就不再是确定性的——而 reference_decode_is_deterministic
        // 这条用例的全部意义就是「参照路径读两遍必须完全一致」。
        ctx->thread_count  = 1;
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            continue;   // 该轨终止，其它轨继续——跟 Pipeline 的分级一致
        }
        tracks[i].ctx       = ctx;
        tracks[i].time_base = fmt->streams[i]->time_base;
        tracks[i].is_video  = (par->codec_type == AVMEDIA_TYPE_VIDEO);
    }

    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) {
        close_ref_tracks(tracks);
        out.averror     = AVERROR(ENOMEM);
        out.error_stage = "av_packet_alloc";
        return;
    }

    bool stopped_early = false;
    for (;;) {
        if (opt.max_frames >= 0 &&
            static_cast<int64_t>(out.frames.size()) >= opt.max_frames) {
            stopped_early = true;
            break;
        }
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            if (rc != AVERROR_EOF) {
                out.averror     = rc;
                out.error_stage = "av_read_frame: " + av_err(rc);
                av_packet_free(&pkt);
                close_ref_tracks(tracks);
                return;
            }
            break;   // 正常 EOF，去下面统一 flush
        }

        const int32_t si = pkt->stream_index;
        // static_cast<std::size_t> 而不是让 si（有符号）直接去下标 vector：
        // 跟 pipeline.cpp::step() 里 `&tracks_[static_cast<std::size_t>(ti)]`
        // 同一处理——先判 si>=0 再转成 size_t，避免 -Wsign-conversion。
        if (si < 0 || static_cast<std::size_t>(si) >= tracks.size()) {
            av_packet_unref(pkt);
            continue;
        }
        const std::size_t ti = static_cast<std::size_t>(si);
        if (tracks[ti].ctx == nullptr) {
            av_packet_unref(pkt);
            continue;   // 未托管的轨（字幕/数据/不支持的编码），跟 Pipeline 一样直接丢包
        }

        const int send_rc = avcodec_send_packet(tracks[ti].ctx, pkt);
        av_packet_unref(pkt);
        if (send_rc != 0) {
            if (send_rc == AVERROR(EAGAIN) || send_rc == AVERROR_EOF) {
                // 契约违反：正常流程里每次 send 前都已经把 receive 排空到
                // NeedInput/Eof（跟 drive_decoder 同一份契约），走到这里
                // 说明前面某处逻辑有洞，按整体终止处理而不是当成坏包吞掉。
                out.averror     = send_rc;
                out.error_stage = "avcodec_send_packet: " + av_err(send_rc);
                av_packet_free(&pkt);
                close_ref_tracks(tracks);
                return;
            }
            // 单包可跳过：跟 FFmpegVideoDecoder::send / FFmpegAudioDecoder::send
            // 同一条分级——某个包解码失败是最轻一级的错误，计数、跳过、继续，
            // 不终止整条解码。
            ++out.skipped_packets;
            continue;
        }

        if (!drain_ready_frames(tracks[ti].ctx, tracks[ti].time_base, tracks[ti].is_video,
                                 si, opt, out, &stopped_early)) {
            av_packet_free(&pkt);
            close_ref_tracks(tracks);
            return;
        }
        if (stopped_early) break;
    }
    av_packet_free(&pkt);

    if (!stopped_early) {
        // flush：跟 Pipeline::step() 的 flush 分支同一件事——送一个
        // nullptr 包，把解码器内部缓冲的帧（B 帧重排序等）排空。
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].ctx == nullptr) continue;
            const int send_rc = avcodec_send_packet(tracks[i].ctx, nullptr);
            if (send_rc != 0 && send_rc != AVERROR_EOF) {
                out.averror     = send_rc;
                out.error_stage = "avcodec_send_packet(flush): " + av_err(send_rc);
                close_ref_tracks(tracks);
                return;
            }
            if (!drain_ready_frames(tracks[i].ctx, tracks[i].time_base, tracks[i].is_video,
                                     static_cast<int32_t>(i), opt, out, &stopped_early)) {
                close_ref_tracks(tracks);
                return;
            }
            if (stopped_early) break;   // flush 途中也达到 max_frames：其它轨不必再 flush
        }
    }

    close_ref_tracks(tracks);
}

}  // namespace

DecodeResult decode_reference(const std::string& path, const DecodeOptions& opt) {
    DecodeResult out;
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        out.averror     = AVERROR(ENOMEM);
        out.error_stage = "avformat_alloc_context";
        return out;
    }
    fmt->probesize            = opt.probesize;
    fmt->max_analyze_duration = opt.analyzeduration;

    const int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        // 失败时 avformat_open_input 已经把 fmt 释放并置空，不要再 free
        // （跟 packet_digest.cpp::demux_file 同一条纪律）。
        out.averror     = rc;
        out.error_stage = "open_input: " + av_err(rc);
        return out;
    }
    run_decode(fmt, opt, out);
    avformat_close_input(&fmt);
    return out;
}

DecodeResult decode_pipeline(Pipeline& pipeline, const DecodeOptions& opt) {
    DecodeResult out;
    bool digest_failed = false;

    // digest_of_frame 失败（不支持的 pix_fmt/声道数等）按整体终止处理，
    // 不静默吞掉——同 drain_ready_frames 里的处理。用一个标志位而不是
    // 直接从 lambda 里 return：下面两处调用点（主循环、Blocked 排空）
    // 都嵌在各自的小循环里，直接 return 出不了 decode_pipeline 本身，
    // 标志位配合外层 for(;;) 顶部的检查更简单。
    auto push_frame_digest = [&](const Frame& f, int32_t track_index) {
        if (digest_failed) return;
        FrameDigest d;
        std::string err;
        if (!digest_of_frame(f, track_index, &d, &err)) {
            out.error_stage = "digest_of_frame: " + err;
            out.averror     = AVERROR_UNKNOWN;
            digest_failed   = true;
            return;
        }
        out.frames.push_back(std::move(d));
    };

    // 兜底排空：正常路径下每次 DecodedFrame 都立刻 pop，FrameQueue 从不
    // 堆积，Blocked 理论上不会因为帧队列满而出现。但 Pipeline 头文件顶部
    // 的契约写得很清楚——Blocked 时任何一条受管轨的队列不被消费，整条
    // 管线就停在那——为了不管调用方传进来的 PipelineConfig 有多苛刻都不
    // 会挂死，这里遇到 Blocked 就轮询所有受管轨尝试 pop，而不是假设
    // 「不会走到这个分支」。
    //
    // 注意：这里用 ti（Pipeline::tracks() 的下标）直接当
    // FrameDigest::track_index，依赖的假设是「Pipeline 的 track_index
    // 编号 == AVStream 下标，且跟主循环 StepOutcome::track_index 是同一
    // 套编号」——目前 pipeline.cpp 确实是这么实现的（tracks_ 按
    // fmt->nb_streams 1:1 建立）。一旦这条假设不再成立，Blocked 路径产出
    // 的 track_index 会跟主循环产出的对不上，表现为「场景 E 假红且看不出
    // 原因」——如果以后改了 Pipeline 的轨道编号规则，这里要跟着改。
    auto drain_all_tracks = [&](Pipeline& p) -> bool {
        bool drained_any = false;
        for (int32_t ti = 0; ti < static_cast<int32_t>(p.tracks().size()); ++ti) {
            // 跟上面的注释是同一个假设——把
            // "结构保证"变成"被检查的结构保证"，而不是只在注释里断言它
            // 成立。这里不能用 tiny_test 的 REQUIRE（本 TU 不依赖它），
            // 走跟本函数其余分支同一条纪律：写 error_stage、整体终止。
            if (p.tracks()[static_cast<std::size_t>(ti)].index != ti) {
                out.error_stage = "drain_all_tracks: track index 映射假设不成立(ti=" +
                    std::to_string(ti) + ", tracks()[ti].index=" +
                    std::to_string(p.tracks()[static_cast<std::size_t>(ti)].index) + ")";
                return drained_any;
            }
            for (;;) {
                if (digest_failed) return drained_any;
                // 跟 drain_ready_frames 同一条 I7 修法：max_frames 检查
                // 放进这个内循环，而不是只放在外层 for(;;)——一次 Blocked
                // 排空可能连续吐出好几帧，只在外层判会越过阈值。
                if (opt.max_frames >= 0 &&
                    static_cast<int64_t>(out.frames.size()) >= opt.max_frames) {
                    return drained_any;
                }
                auto f = p.pop_frame(ti);
                if (!f.has_value()) break;
                push_frame_digest(*f, ti);
                drained_any = true;
            }
        }
        return drained_any;
    };

    for (;;) {
        if (digest_failed) break;
        if (opt.max_frames >= 0 &&
            static_cast<int64_t>(out.frames.size()) >= opt.max_frames) {
            break;
        }
        const StepOutcome o = pipeline.step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            // opt.lazy_pop 为真时故意不在这里
            // pop——这是「产出即排空」驱动纪律本身，正是它让下面的 Blocked
            // 分支（drain_all_tracks）在全部既有用例下从未被走到过一次
            // （实测：短路 drain_all_tracks 直接 return true，16/16
            // 全绿）。让这一帧继续留在 FrameQueue 里，逼着队列真的堆到
            // capacity，Blocked 分支与 drain_all_tracks 才有机会被真实
            // 覆盖到。
            if (!opt.lazy_pop) {
                auto f = pipeline.pop_frame(o.track_index);
                if (f.has_value()) push_frame_digest(*f, o.track_index);
            }
            continue;
        }
        if (o.kind == StepOutcome::Kind::DemuxedPacket) continue;
        if (o.kind == StepOutcome::Kind::Eof) break;
        if (o.kind == StepOutcome::Kind::Error) {
            out.averror     = static_cast<int>(o.status);
            out.error_stage = "pipeline step: Error(status=" + std::to_string(o.status) + ")";
            break;
        }
        // Blocked：见上面 drain_all_tracks 的注释。如果排空之后什么都没
        // 拿到，说明管线真的卡死了（比如调用方给的 PipelineConfig 本身
        // 就矛盾），不能继续 for(;;) 空转——按整体终止上报而不是挂死。
        //
        // 不需要额外判 digest_failed 再决定要不要覆盖 error_stage：
        // drain_all_tracks 内循环里 drained_any 在每次 pop_frame 成功后
        // 无条件置 true（不管随后 push_frame_digest 是否失败），所以
        // digest_failed 一旦在本次调用里被置位，drain_all_tracks 必然
        // 已经返回 true，走不到这个分支；这里的 error_stage 赋值不会
        // 覆盖 push_frame_digest 已经写好的失败原因。
        if (!drain_all_tracks(pipeline)) {
            if (opt.blocked_waits) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            out.error_stage = "pipeline blocked with nothing to drain";
            out.averror     = -1;
            break;
        }
        // 见 DecodeResult::blocked_drain_rounds 顶部注释——
        // 这是唯一守着 lazy_pop 这个开关本身的可观测量,走到这里说明这次
        // 调用真的经历了一轮"step() 报 Blocked → drain_all_tracks 排空"。
        ++out.blocked_drain_rounds;
    }

    out.skipped_packets = pipeline.skipped_packets();
    return out;
}

std::string diff_frames(const DecodeResult& e, const DecodeResult& a) {
    // 「谁先失败」这段前置检查跟 packet_digest.cpp::diff_report 共用，见
    // packet_digest_internal.h::diff_failure_hardlines 顶部注释——这是
    // 实测到「两份平行逻辑会漂移」之后
    // 抽出来的，抽出来之前 diff_report 里那段解释 averror 冗余检查的长
    // 注释在这里就漏抄了一次。
    if (auto hardline = detail::diff_failure_hardlines(
            e.error_stage, e.averror, a.error_stage, a.averror)) {
        return *hardline;
    }
    // 硬底线：参照侧空就说明这次比对本身没测到任何东西，不能算通过。
    // DecodeResult 只有 frames 这一层空集合语义（跟 DemuxResult 的
    // streams/packets 两层不同），这条检查留在这里、不进共享助手。
    if (e.frames.empty()) {
        return "参照路径没有解出任何帧，比对无意义";
    }

    std::ostringstream o;
    const size_t n = e.frames.size() < a.frames.size() ? e.frames.size() : a.frames.size();
    for (size_t i = 0; i < n; ++i) {
        if (e.frames[i] == a.frames[i]) continue;
        const FrameDigest& x = e.frames[i];
        const FrameDigest& y = a.frames[i];
        o << "frame #" << i << " 不同:\n"
          << "  参照 track=" << x.track_index << " pts=" << x.pts_us
          << " duration=" << x.duration_us << " fmt=" << x.fmt
          << " size=" << x.width << "x" << x.height
          << " rate=" << x.sample_rate << " ch=" << x.channels
          << " nb_samples=" << x.nb_samples
          << " sha=" << hex32(x.data_sha256) << "\n"
          << "  实际 track=" << y.track_index << " pts=" << y.pts_us
          << " duration=" << y.duration_us << " fmt=" << y.fmt
          << " size=" << y.width << "x" << y.height
          << " rate=" << y.sample_rate << " ch=" << y.channels
          << " nb_samples=" << y.nb_samples
          << " sha=" << hex32(y.data_sha256);
        return o.str();
    }
    if (e.frames.size() != a.frames.size()) {
        o << "帧数量不同: 参照 " << e.frames.size() << ", 实际 " << a.frames.size()
          << "（前 " << n << " 个相同）";
        return o.str();
    }
    if (e.skipped_packets != a.skipped_packets) {
        o << "skipped_packets 不同: 参照 " << e.skipped_packets
          << ", 实际 " << a.skipped_packets;
        return o.str();
    }
    return {};
}

}  // namespace syp::probe
