// frame_digest.h — 把一次解码过程压成可比对的帧摘要序列，是 packet_digest.h
// 在解码维度的延伸。
//
// 差分比对的参照物同样是 FFmpeg 自己：同一份文件，一条走 libavformat +
// libavcodec 直接解码（不经过我们任何组件），一条走
// AvioBridge → Demuxer → PacketQueue → Decoder → FrameQueue（Pipeline），
// 逐帧比对。像素/采样数据也比——只比 pts/尺寸的话，「解码器吐出的画面内容
// 本身错了」这类缺陷会漏掉。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/pipeline.h"

namespace syp::probe {

// 音视频统一用一个结构体：视频帧上 sample_rate/channels/nb_samples 保持默认
// 0，音频帧上 width/height 保持默认 0 —— 跟 demuxer.h::TrackInfo「音频为 0 /
// 视频为 0」的既有风格一致，不为两种媒体类型各开一个结构体。
struct FrameDigest {
    int32_t track_index    = 0;
    int64_t pts_us         = 0;
    int64_t duration_us    = 0;
    int32_t fmt             = 0;   // 视频: AVPixelFormat；音频: AVSampleFormat
    int32_t width           = 0;   // 音频为 0
    int32_t height          = 0;   // 音频为 0
    int32_t sample_rate     = 0;   // 视频为 0
    int32_t channels        = 0;   // 视频为 0
    int32_t nb_samples      = 0;   // 视频为 0
    uint8_t data_sha256[32] = {};  // 像素/采样数据本体（剥掉 padding 之后）的摘要

    bool operator==(const FrameDigest&) const = default;
};

struct DecodeResult {
    std::vector<FrameDigest> frames;
    int64_t     skipped_packets = 0;
    std::string error_stage;      // 非空表示在哪一步失败了
    int         averror          = 0;
    // 这个字段唯一的用途是**守住 DecodeOptions::
    // lazy_pop 这个开关本身**，不是给调用方看的业务数据——变异体
    // 证实：把 lazy_pop 为真时的行为悄悄改回"仍然立即 pop"（开关形同
    // 虚设），场景 E 的 9 条用例全绿，因为 ref_by_track 相等这个判据对
    // "是否真的走了 Blocked/drain_all_tracks"完全不敏感——那次修复的
    // 覆盖成果本身没有任何独立的可观测量守着,哪天这个开关悄悄失效,
    // drain_all_tracks 会退回 0 命中的死代码而没有任何用例会有反应。
    // decode_pipeline() 每次真正走到 Blocked 分支、调用一次
    // drain_all_tracks 且成功排出了东西，这个字段就 ++——不是"解码器
    // 内部 Blocked 了多少次"这种业务语义,只是"这次调用是否真的经历过
    // Blocked 排空路径,经历了几次"这个纯粹的驱动方式度量。diff_frames()
    // 故意不比这个字段：它是驱动方式（要不要 lazy_pop、Pipeline 队列多
    // 大）的函数，不是解码正确性的一部分——参照路径（decode_reference）
    // 根本不会填它，比了只会在"测试本地排空"跟"库排空"两条用不同驱动
    // 方式跑同一份文件的场景里制造假红。
    int64_t     blocked_drain_rounds = 0;
};

// probesize / analyzeduration 与 packet_digest.h::DemuxOptions 同一份取值来源
// （syp::media::kDefaultProbeSize / kDefaultMaxAnalyzeDurationUs）：两条路径
// 探测深度不一致会让逐帧比对假红，这是此前已经踩过、demuxer.h 顶部专门写了
// 警示的坑，这里原样沿用而不是另起一份。
struct DecodeOptions {
    int64_t probesize       = syp::media::kDefaultProbeSize;
    int64_t analyzeduration = syp::media::kDefaultMaxAnalyzeDurationUs;
    // -1 = 解到 Eof 为止；>=0 时解出这么多帧（跨所有轨累计）就提前停止，
    // 只用于让跑得慢的场景用例可控——不是契约的一部分。
    int64_t max_frames      = -1;
    // 仅测试用，默认关闭：为真时 decode_pipeline() 见到 DecodedFrame 不
    // 立刻 pop_frame()，逼着 Blocked 分支（以及它带的排空逻辑
    // drain_all_tracks）真正被走到。本函数「产出即
    // 排空」的驱动纪律下，Blocked 分支与 drain_all_tracks 是全量 ctest
    // 下 0 次命中的死代码——见 decode_pipeline() 实现里对这个字段的
    // 使用位置，以及 tests/test_decode_e2e.cpp 场景 E 的说明。为真时
    // out.frames 的全局顺序不代表任何有意义的跨轨交织顺序（drain_all_
    // tracks 按轨号升序整条排空，不是按产出的真实时间顺序），调用方必须
    // 按 track_index 分组后再比对，不能直接拿去跟 decode_reference() 的
    // 输出做 diff_frames() 那种逐位比较。
    bool    lazy_pop         = false;
    // 线程模式（PipelineConfig::demux_thread）专用，默认关：Blocked 且没有可排空
    // 的帧时不判"管线卡死"，让出 200 微秒再试——线程模式下 Blocked 只表示加载线程这一刻
    // 还没把包送到。真卡死由调用方的看门狗兜底。
    bool    blocked_waits    = false;
};

// 参照路径：直接用 libavformat + libavcodec 解码，不经过本项目任何组件。
DecodeResult decode_reference(const std::string& path, const DecodeOptions& opt);

// 被测路径：驱动调用方已经创建好的 Pipeline 直到 Eof/Error，或达到
// max_frames。opt.lazy_pop 为假（默认）时，每次 step() 返回 DecodedFrame
// 都立刻 pop_frame() 排空——Pipeline 的契约（pipeline.h 顶部注释）是
// 「Blocked 时任何一条受管轨的 FrameQueue 满了都要靠调用方 pop_frame
// 排空才能继续」，本函数遇到 Blocked 会兜底轮询所有受管轨排空，但正常
// 路径下「产出即排空」已经让 FrameQueue 不会堆积到 Blocked——opt.lazy_pop
// 为真时故意不这么做，见该字段注释。
DecodeResult decode_pipeline(syp::media::Pipeline& pipeline, const DecodeOptions& opt);

// 返回空串表示全等；否则是人可读的第一处差异描述。语义与硬底线跟
// packet_digest.cpp 的 diff_report 同构：参照路径失败即非空、被测路径
// 失败即非空、参照侧零帧即非空——不存在「两边都失败所以相等」这回事。
std::string diff_frames(const DecodeResult& expected, const DecodeResult& actual);

}  // namespace syp::probe
