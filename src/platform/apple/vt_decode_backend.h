// vt_decode_backend.h —— IHwDecodeBackend 的 Apple 实现：经 FFmpeg hwaccel 调 VideoToolbox。
//
// 严格硬件：FFmpeg 对 H.264 用 RequireHardwareAcceleratedVideoDecoder，
// 对 HEVC 只用 EnableHardwareAcceleratedVideoDecoder（videotoolbox.c:845-849，后者允许 VT
// 内部静默走软件）。不改 FFmpeg 源码，只能在 supports() 里先问系统
// VTIsHardwareDecodeSupported()，不支持直接拒。
#pragma once

#include <cstdint>

#include "media/hw/hw_decode_backend.h"

namespace syp::platform {

// 纯函数，零系统依赖，单测穷举。profile 只在 pix_fmt 未知（AV_PIX_FMT_NONE）时用于排除
// HEVC Main10(2)/Rext(4)。
bool vt_supports(bool hwaccel_compiled_in, int32_t codec_id, int32_t pix_fmt,
                 int32_t profile, bool system_says_hw_supported) noexcept;

// 进程内单例。Catalyst slice 上 supports() 恒 false。
media::hw::IHwDecodeBackend& videotoolbox_decode_backend() noexcept;

// 本平台能否硬解 H.264 8-bit（demo 的能力查询；不需要具体流）。
bool videotoolbox_available_for_h264() noexcept;

}  // namespace syp::platform
