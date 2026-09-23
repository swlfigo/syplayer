// ffmpeg_video_decoder.h — IVideoDecoder 的 FFmpeg 软解/硬解实现。
//
// 硬解模式：由调用方显式传入 VideoDecodeMode::Hardware + 一个
// hw::IHwDecodeBackend，本类不自动兜底。"不兜底"由四件事共同保证：
//   1. get_format 回调只接受 backend 的硬件格式，否则返回 AV_PIX_FMT_NONE；
//   2. 回调一旦返回 NONE 就闩住（hw_rejected_），此后 send() 返回
//      SYP_ERR_NOT_IMPLEMENTED、receive() 返回 Error，直到下一次 open()；
//   3. open() 在硬解模式把 ctx->pix_fmt 置 NONE——否则 H.264 在 hwaccel 初始化
//      失败后会拿 codecpar 带进来的 yuv420p 当"当前格式"绕过 get_format 继续软解；
//   4. receive() 输出闸：硬解模式下任何 format != hw_pix_fmt() 的帧都丢弃并报
//      Error（同样闩住）——不依赖 FFmpeg 内部何时调用 get_format。
#pragma once

#include <cstdint>

#include "media/hw/hw_decode_backend.h"
#include "media/video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace syp::media {

// get_format 闸门的纯逻辑：candidates（以 AV_PIX_FMT_NONE 结尾）里有 want 就返回 want，
// 否则返回 AV_PIX_FMT_NONE——不从候选里挑软件格式。它只是"不兜底"四件事之一，单靠它
// 挡不住静默软解（见本文件顶部）。
AVPixelFormat choose_hw_format(const AVPixelFormat* candidates, AVPixelFormat want) noexcept;

class FFmpegVideoDecoder final : public IVideoDecoder {
public:
    // mode == Hardware 时 backend 必须活过本实例；为空或不支持 → open() 返回
    // SYP_ERR_NOT_IMPLEMENTED。Software 模式完全忽略 backend。
    explicit FFmpegVideoDecoder(VideoDecodeMode mode = VideoDecodeMode::Software,
                                hw::IHwDecodeBackend* backend = nullptr) noexcept
        : mode_(mode), backend_(backend) {}
    ~FFmpegVideoDecoder() override;

    FFmpegVideoDecoder(const FFmpegVideoDecoder&)            = delete;
    FFmpegVideoDecoder& operator=(const FFmpegVideoDecoder&) = delete;

    syp_status open(const AVCodecParameters* par, AVRational time_base) override;
    syp_status send(const AVPacket* pkt) override;
    Receive    receive(Frame* out) override;
    void       flush() override;
    int64_t    skipped_packets() const override { return skipped_packets_; }
    void       set_skip_nonref(bool on) noexcept override;

    // 最近一次 open() 成功且工作在硬解模式（硬解被拒后仍为 true；拒绝体现为
    // send()/receive() 报错，Pipeline 据此把该轨标记 failed）。
    bool hardware() const noexcept { return ctx_ != nullptr && mode_ == VideoDecodeMode::Hardware; }

    // 当前是否处于跳非参考帧模式（只读，测试与 Pipeline 查询用）。
    bool skip_nonref() const noexcept { return skip_nonref_; }

private:
    void close() noexcept;
    static AVPixelFormat get_format_cb(AVCodecContext* ctx, const AVPixelFormat* fmts);

    VideoDecodeMode       mode_;
    hw::IHwDecodeBackend* backend_;
    AVCodecContext*       ctx_             = nullptr;
    AVRational            time_base_       = {0, 1};
    int64_t               skipped_packets_ = 0;
    // 硬解闸门闩：get_format 拒绝或 receive() 见到非硬件帧后置位，open()/close() 复位。
    bool                  hw_rejected_     = false;
    // 追帧：是否跳过非参考帧。open()/close() 复位为 false。
    bool                  skip_nonref_     = false;
};

}  // namespace syp::media
