// fake_clock.h — 可注入的假时钟。测试用例直接拨它，不 sleep、不碰真实时间。
#pragma once

#include "media/time_source.h"

namespace syp::test {

class FakeClock final : public syp::media::TimeSource {
public:
    int64_t now_us() const noexcept override { return now_; }
    void set(int64_t us) noexcept { now_ = us; }
    void advance(int64_t us) noexcept { now_ += us; }

private:
    int64_t now_ = 0;
};

}  // namespace syp::test
