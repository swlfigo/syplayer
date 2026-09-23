#import "platform/apple/vt_decode_backend.h"

#import <VideoToolbox/VideoToolbox.h>

#include <syp_ffmpeg_features.h>

extern "C" {
#include <libavcodec/defs.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace syp::platform {

bool vt_supports(bool hwaccel_compiled_in, int32_t codec_id, int32_t pix_fmt,
                 int32_t profile, bool system_says_hw_supported) noexcept {
    if (!hwaccel_compiled_in || !system_says_hw_supported) return false;
    if (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_HEVC) return false;
    if (pix_fmt == AV_PIX_FMT_YUV420P || pix_fmt == AV_PIX_FMT_YUVJ420P) return true;
    if (pix_fmt != AV_PIX_FMT_NONE) return false;   // 已知且不是 8-bit 4:2:0
    // 格式未知：H.264 放行（VT 的 Require 标志会在 session 创建时兜住非常规 profile），
    // HEVC 按 profile 排除 Main10 / Rext。其它未知格式的 profile 一律放行——真不支持时
    // VT 会在 session 创建（ff_videotoolbox_common_init）时拒绝，那条路径由
    // FFmpegVideoDecoder 的 get_format 闩 + 输出闸兜成该轨报错，不会静默软解。
    if (codec_id == AV_CODEC_ID_HEVC &&
        (profile == AV_PROFILE_HEVC_MAIN_10 || profile == AV_PROFILE_HEVC_REXT)) {
        return false;
    }
    return true;
}

namespace {

bool hwaccel_compiled_for(int32_t codec_id) noexcept {
    if (codec_id == AV_CODEC_ID_H264) return SYP_FFMPEG_H264_VIDEOTOOLBOX_HWACCEL != 0;
    if (codec_id == AV_CODEC_ID_HEVC) return SYP_FFMPEG_HEVC_VIDEOTOOLBOX_HWACCEL != 0;
    return false;
}

bool system_supports(int32_t codec_id) noexcept {
    if (codec_id == AV_CODEC_ID_H264) return VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
    if (codec_id == AV_CODEC_ID_HEVC) return VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC);
    return false;
}

class VtBackend final : public media::hw::IHwDecodeBackend {
public:
    bool supports(const AVCodecParameters& par) const noexcept override {
        return vt_supports(hwaccel_compiled_for(par.codec_id), par.codec_id, par.format,
                           par.profile, system_supports(par.codec_id));
    }
    syp_status prepare(AVCodecContext* ctx, const AVCodec** /*codec*/) override {
        AVBufferRef* dev = nullptr;
        if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0) {
            return SYP_ERR_NOT_IMPLEMENTED;
        }
        ctx->hw_device_ctx = dev;   // 所有权交给 ctx，avcodec_free_context 时释放
        return SYP_OK;
    }
    AVPixelFormat hw_pix_fmt() const noexcept override { return AV_PIX_FMT_VIDEOTOOLBOX; }
    const char*   name() const noexcept override { return "videotoolbox"; }
};

}  // namespace

media::hw::IHwDecodeBackend& videotoolbox_decode_backend() noexcept {
    // 有意泄漏：函数内 static 对象会在 exit() 时析构，而进程退出期间别的静态对象/
    // 仍在跑的解码线程可能还持有这个引用（非拥有指针，见 PipelineConfig::hw_backend）。
    // 无状态单例，泄漏零成本。
    static auto* backend = new VtBackend;
    return *backend;
}

bool videotoolbox_available_for_h264() noexcept {
    return vt_supports(hwaccel_compiled_for(AV_CODEC_ID_H264), AV_CODEC_ID_H264,
                       AV_PIX_FMT_YUV420P, 100, system_supports(AV_CODEC_ID_H264));
}

}  // namespace syp::platform
