// fake_hw_backend.h —— 可配置的假硬件后端，测平台无关的硬解闸门逻辑。
#pragma once

#include "media/hw/hw_decode_backend.h"

namespace syp::test {

class FakeHwBackend final : public syp::media::hw::IHwDecodeBackend {
public:
    bool          supports_result = true;
    syp_status    prepare_result  = SYP_OK;
    AVPixelFormat fmt             = AV_PIX_FMT_VIDEOTOOLBOX;
    mutable int   supports_calls  = 0;
    int           prepare_calls   = 0;

    bool supports(const AVCodecParameters&) const noexcept override {
        ++supports_calls;
        return supports_result;
    }
    syp_status prepare(AVCodecContext*, const AVCodec**) override {
        ++prepare_calls;
        return prepare_result;
    }
    AVPixelFormat hw_pix_fmt() const noexcept override { return fmt; }
    const char*   name() const noexcept override { return "fake"; }
};

}  // namespace syp::test
