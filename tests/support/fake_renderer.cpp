#include "support/fake_renderer.h"

namespace syp::test {

syp_status FakeRenderer::present(const syp::media::Frame& f, int64_t due_in_us) {
    ++calls_;
    if (busy_every_ > 0 && calls_ % busy_every_ == 0) {
        busy_pts_.push_back(f.pts_us());
        return SYP_ERR_BUSY;
    }
    if (fail_every_ > 0 && calls_ % fail_every_ == 0) return SYP_ERR_IO;
    shown_.push_back({f.pts_us(), clock_ != nullptr ? clock_->now_us() : 0, due_in_us});
    return SYP_OK;
}

void FakeRenderer::set_source_geometry(int32_t sar_num, int32_t sar_den,
                                       int32_t rotation_deg) noexcept {
    sar_num_      = sar_num;
    sar_den_      = sar_den;
    rotation_deg_ = rotation_deg;
    ++geometry_calls_;
}

void FakeRenderer::set_gravity(syp::media::Gravity g) noexcept { gravity_ = g; }

}  // namespace syp::test
