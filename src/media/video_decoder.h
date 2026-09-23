// video_decoder.h — 视频解码器的纯虚接口：架构文档「缝 ②」。
//
// 目前只有一个实现 FFmpegVideoDecoder。硬解**没有**在这条缝上另加
// VTDecoder：硬解是 FFmpegVideoDecoder 的硬解模式
// （VideoDecodeMode::Hardware），经平台无关的 hw::IHwDecodeBackend 注入平台
// 后端（Apple 是 FFmpeg VideoToolbox hwaccel），本接口一个字没改。本文件与
// src/media/ 的其它文件一样必须保持平台无关：不含 ObjC/CoreVideo/Metal 类型。
//
// ── send()/receive() 的契约：EAGAIN/EOF 不是错误 ──────────────────────
// FFmpeg 的 avcodec_send_packet / avcodec_receive_frame 是一对
// send/receive 状态机，三个返回值不代表「出错」：
//   - send_packet 返回 AVERROR(EAGAIN)：内部缓冲已满，调用方必须先把
//     receive() 排空、再重试 send。
//   - receive_frame 返回 AVERROR(EAGAIN)：内部没有可输出的帧，需要更多
//     输入（继续 send）。
//   - receive_frame 返回 AVERROR_EOF：该解码器已经完全排空（flush 之后）。
// 把这三者当错误处理是这类代码最常见的写法错误。
//
// 本接口把处理 send() 端 EAGAIN 的责任交给调用方（Pipeline）：
// Pipeline 保证每次 send() 之前，上一轮的 receive() 循环已经把
// NeedInput/Eof 都取到底。在这个前提下，send() 内部见到
// AVERROR(EAGAIN) 属于契约违反（调用方没有遵守「send 前先排空」），
// 实现会把它当错误处理、返回非 SYP_OK ——不会静默吞掉或重试。
// 如果将来有调用方不满足这个前提（比如直接拿本类当库用，不经
// Pipeline），会在这里踩坑，所以特别写在这里。
//
// ── 单包可跳过 vs 该轨终止 vs 整体终止 ──────────────────────────────
// send() 见到「非 EAGAIN/EOF 的负值」（真实码流里常见的畸形/损坏包）
// 属于三级错误分类里最轻的一级——单包可跳过：计入 skipped_packets()、
// 返回 SYP_OK，解码继续。open() 失败属于「该轨终止」，判定与处理在
// Pipeline，不在这个类里。
//
// skipped_packets() 本级只负责计数，不做升级判定：多大的累计值算「这条
// 轨值得放弃」是个产品决策（跟码流质量、网络状况、用户能接受的画面损坏
// 程度都有关），现在没有真实数据支撑定一个阈值，留给以后有真实播放
// 数据时再定——不在这里（也不在 Pipeline 里）预先编一个数字进代码。
#pragma once

#include <cstdint>

#include <syplayer/syp_types.h>

#include "media/frame.h"

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

namespace syp::media {

class IVideoDecoder {
public:
    // 复用同一枚举而不是给音频解码器另定义一份的理由见
    // ffmpeg_audio_decoder.h 顶部注释。
    enum class Receive { Frame, NeedInput, Eof, Error };

    virtual ~IVideoDecoder() = default;

    // 打开解码器。par/time_base 通常取自
    // demuxer->raw()->streams[i]->codecpar / ->time_base。
    // 失败返回非 SYP_OK，且不留半开状态（不必调用方再手动清理）。
    //
    // 接口契约（不是实现细节）：
    //   1. par->codec_type 必须与本解码器的角色匹配（视频实现要求
    //      AVMEDIA_TYPE_VIDEO），不匹配返回 SYP_ERR_INVALID_ARG——否则
    //      Pipeline 传错轨（比如把音频轨的 codecpar 递给了视频解码器）
    //      时，底层解码器可能照样能找到、能 open、能 send/receive，只是
    //      产出的 Frame::is_video() 与实际内容矛盾，且不会在这里报错。
    //   2. open() 可以在同一实例上重复调用（更换编解码器/参数，或者
    //      「该轨终止」后 Pipeline 想原地重试）：每次调用等价于先释放
    //      旧的解码器状态、再重新走一遍初始化，调用方不需要销毁重建
    //      整个 IVideoDecoder 实例。硬解模式同样遵守——avcodec_free_context
    //      连同挂在上面的 hw_device_ctx（VT session 由 FFmpeg 随之销毁）一起
    //      释放，硬解拒绝的闩也随之复位。
    // 音频解码器（FFmpegAudioDecoder，不继承本接口）的 open() 遵守同一份
    // 契约，见 ffmpeg_audio_decoder.h。
    virtual syp_status open(const AVCodecParameters* par, AVRational time_base) = 0;

    // 送入一个待解码包。pkt 为 nullptr 表示 flush/EOF 信号（对应
    // avcodec_send_packet(ctx, nullptr)）。
    //
    // 契约：调用方必须保证上一次 send() 之后已经把 receive() 循环到
    // NeedInput/Eof——见本文件顶部「send()/receive() 的契约」。违反这条
    // 契约时 send() 内部见到的 AVERROR(EAGAIN) 会被当错误返回。
    //
    // 单包解码失败（非 EAGAIN/EOF 的负值）不是致命错误：计入
    // skipped_packets()、返回 SYP_OK，调用方应当继续送下一个包。
    virtual syp_status send(const AVPacket* pkt) = 0;

    // 从解码器取出一帧。NeedInput 表示要先 send 更多输入；Eof 表示已经
    // 排空（flush 之后终会走到这里）；Error 表示解码器内部错误。
    virtual Receive receive(Frame* out) = 0;

    // 丢弃解码器内部状态（seek 后必调）。之后可以继续 send/receive，不
    // 带任何旧状态。
    virtual void flush() = 0;

    // 自 open() 以来因「单包可跳过」而丢弃的包数。
    virtual int64_t skipped_packets() const = 0;

    // 追帧：打开时跳过非参考帧（AVDISCARD_NONREF），关闭时恢复（AVDISCARD_DEFAULT）。
    // 可在两次 send 之间随时切换；open() 复位为关闭。软解与 hwaccel 硬解都在解码器层生效。
    virtual void set_skip_nonref(bool on) noexcept = 0;
};

}  // namespace syp::media
