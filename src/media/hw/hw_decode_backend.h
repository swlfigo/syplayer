// hw_decode_backend.h —— 视频硬解后端的平台无关接口。
//
// 硬解统一经 FFmpeg：Apple 是 hwaccel（挂 hw_device_ctx，输出 AV_PIX_FMT_VIDEOTOOLBOX），
// Android 将来是独立包装解码器（h264_mediacodec，需替换 AVCodec + 设 JNI）——prepare()
// 带 const AVCodec** 就是给后者留的口子。本头不得 include 任何平台头。
#pragma once

#include <syplayer/syp_types.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace syp::media {

// 由调用方显式选择，不自动兜底。
enum class VideoDecodeMode { Software, Hardware };

}  // namespace syp::media

namespace syp::media::hw {

class IHwDecodeBackend {
public:
    virtual ~IHwDecodeBackend() = default;

    // 当前平台/设备能否硬解该流（codec + 位深都要看）。纯查询，无副作用，可重复调用。
    virtual bool supports(const AVCodecParameters& par) const noexcept = 0;

    // avcodec_open2 之前调。成功后 *codec 可能被替换。失败时不得留下任何挂在 ctx 上的资源。
    virtual syp_status prepare(AVCodecContext* ctx, const AVCodec** codec) = 0;

    // get_format 回调唯一接受的像素格式。
    virtual AVPixelFormat hw_pix_fmt() const noexcept = 0;

    // 诊断用，如 "videotoolbox"。
    virtual const char* name() const noexcept = 0;
};

}  // namespace syp::media::hw
