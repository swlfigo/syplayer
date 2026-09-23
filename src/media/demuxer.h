// demuxer.h — AVFormatContext 的薄封装：吃 AVIOContext 或本地 file: 路径，
// 吐 AVPacket 并带上轨道索引。
//
// probesize / max_analyze_duration 显式钉死为 kDefaultProbeSize /
// kDefaultMaxAnalyzeDurationUs（本文件下方），tools/syp_probe/
// packet_digest.h 的 DemuxOptions 默认值引用这两个常量——两条路径探测
// 深度不同会导致「我们的管线 vs FFmpeg 直接解码」的逐帧差分假红，这是
// 此前已经踩过的坑。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <syplayer/syp_types.h>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace syp::media {

// probesize / max_analyze_duration 的唯一定义处。tools/syp_probe/
// packet_digest.h 的 DemuxOptions 默认值引用这两个常量，而不是各写一份——
// 验收标准是「我们的管线 vs FFmpeg 直接解码」的逐帧差分，两处一旦漂移，
// 探测深度不同会产生假红，且症状是「差分比对莫名不等」，极难查到根因。
inline constexpr int64_t kDefaultProbeSize            = 5 * 1024 * 1024;
inline constexpr int64_t kDefaultMaxAnalyzeDurationUs = 5000000;

// 把 FFmpeg 时长字段归一化成调用方能安全使用的微秒数：AV_NOPTS_VALUE
// （即 INT64_MIN）与任何负值都收敛到 0。
//
// 连负值一起收的理由：duration_us 对调用方的隐含契约是「非负的微秒数，
// 0 表示不知道」——FFmpeg 的 duration 字段正常只会是有效值或
// AV_NOPTS_VALUE，但畸形文件、损坏的时间戳表等边缘情况理论上可能算出
// AV_NOPTS_VALUE 以外的负数。这类负数不该被当成「真实但离谱的时长」透传
// 给下游：调用方（seek 条、进度条、时长展示）几乎必然假设这个值非负，
// 一旦拿到负数去做减法/百分比计算，后果通常比「显示成 0（未知）」更糟。
// 真正的异常应该走 syp_status / ReadResult::Error 这两条专职通道，不
// 应该靠这个字段的符号位去传递——所以这里选择统一压平，不放行任何负值。
//
// 抽成纯函数是为了能脱离素材测试：真实触发场景（直播源、chunked 传输、
// 长度未知的流）做不成 fixture，而这段逻辑本身与素材无关，是纯粹的字段
// 映射。见 tests/test_demuxer.cpp::normalize_duration_handles_nopts。
inline constexpr int64_t normalize_duration(int64_t raw) noexcept {
    return (raw == AV_NOPTS_VALUE || raw < 0) ? 0 : raw;
}

// 警告：`!is_video` 不等于「是音频轨」。字幕/数据/附件轨的 is_video 也是
// false，但 width/height/sample_rate/channels 全是 0——解码器选型必须用
// `codec_type == AVMEDIA_TYPE_AUDIO`（经 raw()->streams[i]->codecpar）
// 做音频判据，不能把 !is_video 当音频判据，否则字幕轨会被误当音频轨，
// 拿着 sample_rate=0/channels=0 去初始化解码器。
struct TrackInfo {
    int32_t     index       = 0;
    int32_t     codec_id    = 0;   // AVCodecID 的整数值
    bool        is_video    = false;
    AVRational  time_base   = {0, 1};
    int32_t     width       = 0;   // 音频为 0
    int32_t     height      = 0;   // 音频为 0
    int32_t     sample_rate = 0;   // 视频为 0
    int32_t     channels    = 0;   // 视频为 0，走 codecpar->ch_layout.nb_channels
    int64_t     duration_us = 0;
    // AV_DISPOSITION_ATTACHED_PIC：这条"视频轨"其实是一张封面图（专辑封面
    // 之类），只有一个采样、不是真正的视频内容。
    //
    // 为什么放在 TrackInfo 而不是给 Pipeline 新增一个访问器：调用方
    // （TrackPlayer::create()）要在"选哪条视频轨"这一步就用到它，而
    // Pipeline 的 tracks() 直接转发 Demuxer::tracks()——在这里加一个字段，
    // Pipeline 一个字都不用改（能不改就不改）。这也不违反
    // track_player.cpp 里那条"别给 TrackInfo 加 sample_fmt"的约束：那条的
    // 理由是 TrackInfo 只承载"打开解码器需要什么"，而 sample_fmt 只有音频
    // **输出**路径用得上；attached_pic 恰恰是"这条轨是什么"的一部分，跟
    // is_video/width/height 同一性质，是选轨判据而不是输出细节。
    bool        attached_pic = false;
    // 这条流被调用方设成了 AVDISCARD_ALL（HLS 选轨：未选中的 variant）。
    //
    // 【为什么必须有】AVDISCARD_ALL **不会**把流从 AVFormatContext::streams
    // 里移除——它只是让 demuxer 不再产出这条流的包。于是 Pipeline 仍会
    // 托管它、TrackPlayer 的"第一条 is_video"仍然看得见它，可能绑上一条
    // 永远不会有帧的轨：画面永远黑、无错误、无降级。与 attached_pic 是
    // 同一性质的"这条轨是什么"的选轨判据，所以放在同一个地方。
    //
    // 【什么时候被填】build_tracks() 里读 st->discard。调用方必须在
    // open_prepared() 的 after_open 回调里改完 st->discard——那个调用点
    // （find_stream_info 之后、build_tracks 之前）是契约的一部分，晚一步
    // 这个字段就全是 false。
    bool        discard = false;

    // ---- 显示几何 ----
    // 放在 TrackInfo 而不是给 Pipeline 新增访问器：与 attached_pic / discard
    // 同一性质——"这条轨是什么"，不是输出细节（见本结构体上方 is_video 那段）。
    int32_t sar_num = 0;   // sample_aspect_ratio；任一分量 <= 0 表示未知，按 1:1
    int32_t sar_den = 0;
    // 为得到正立画面需要**顺时针**施加的角度，已规整到 {0,90,180,270}。
    // 不是 av_display_rotation_get() 的原样返回值——换算（含符号）只在
    // build_tracks() 里做一次，符号错误因此只可能出现在一个地方。
    int32_t rotation_deg = 0;
};

class Demuxer {
public:
    enum class ReadResult { Packet, Eof, Error };

    ~Demuxer();

    Demuxer(const Demuxer&)            = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    // 打开本地文件（file: 路径）。失败返回 nullptr 并写 *err。
    static std::unique_ptr<Demuxer> open_file(const std::string& path, syp_status* err);

    // 打开调用方提供的 AVIOContext——不接管 ctx 的所有权，析构时不会关闭它。
    static std::unique_ptr<Demuxer> open_avio(AVIOContext* ctx, syp_status* err);

    // 接管一个调用方**已经配置好** io_open/io_close2/opaque/protocol_whitelist
    // 的 AVFormatContext，用 url 打开它。
    //
    // 与 open_avio 的区别：那条路是「调用方给一条字节流，我们不管它的生命
    // 周期」；这条路是「调用方给一个半成品 AVFormatContext，我们接管它」——
    // HLS 需要后者，因为 io_open 必须在 avformat_open_input **之前**装好
    // （hls 解封装器在 read_header 里就要打开子播放列表，hls.c:2165）。
    //
    // opts：原样递给 avformat_open_input 的第四个参数，可为 nullptr。
    // **不是可有可无的**：HlsSession 那两个选项（http_persistent=0 /
    // allowed_extensions=ALL）是 hls 解封装器的 AVOption，只能经这条路
    // 进去。漏掉 http_persistent=0 不是"静默降级"而是**进程 abort**：
    // keepalive 分支（hls.c:708/815）会对我们的自定义 AVIOContext 做
    // ffio_geturlcontext + av_assert0(uc)（hls.c:646）。
    // avformat_open_input 会把未被消费的条目写回
    // *opts，所有权仍归调用方。
    //
    // after_open：在 avformat_find_stream_info() **之后**、build_tracks()
    // **之前**调用，可为空。调用点固定在这里是因为 HLS 的选轨
    // 要改 st->discard，而 build_tracks() 读的正是 open 之后的流表——
    // 晚一步的话 TrackInfo 已经按未选轨的状态建好了。
    //
    // 成功后 Demuxer 拥有 prepared，析构时 avformat_close_input。
    // 失败时 prepared 已被 avformat_open_input 释放，调用方不得再碰。
    static std::unique_ptr<Demuxer> open_prepared(
        AVFormatContext* prepared,
        const std::string& url,
        AVDictionary** opts,
        const std::function<void(AVFormatContext*)>& after_open,
        syp_status* err);

    const std::vector<TrackInfo>& tracks() const noexcept { return tracks_; }
    int64_t duration_us() const noexcept { return duration_us_; }

    // 成功（Packet）时 *out 归调用方所有，须 av_packet_free。
    ReadResult read(AVPacket** out, int32_t* track_index);

    syp_status seek(int64_t ts_us);

    // 供解码器取 codecpar：d->raw()->streams[i]->codecpar。
    AVFormatContext* raw() const noexcept { return fmt_; }

private:
    Demuxer() = default;

    void build_tracks();

    AVFormatContext*       fmt_          = nullptr;
    int64_t                duration_us_  = 0;
    std::vector<TrackInfo> tracks_;
};

}  // namespace syp::media
