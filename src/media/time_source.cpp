#include "media/time_source.h"

#include "media/audio_sink.h"

extern "C" {
#include <libavutil/avutil.h>
}

namespace syp::media {

SystemClock::SystemClock() : anchor_(Clock::now()) {}

int64_t SystemClock::now_us() const noexcept {
    if (paused_) return base_us_;
    const auto d = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - anchor_);
    return base_us_ + static_cast<int64_t>(static_cast<double>(d.count()) * speed_);
}

void SystemClock::set_base(int64_t base_us) noexcept {
    base_us_ = base_us;
    anchor_  = Clock::now();
}

void SystemClock::set_speed(double speed) noexcept {
    // 先把已走过的时间按旧 speed 结算进 base_，再换比例——
    // 否则改 speed 会把过去那段也按新比例重算，时间发生跳变。
    base_us_ = now_us();
    anchor_  = Clock::now();
    speed_   = speed;
}

void SystemClock::pause() noexcept {
    if (paused_) return;
    base_us_ = now_us();
    paused_  = true;
}

void SystemClock::resume() noexcept {
    if (!paused_) return;
    anchor_ = Clock::now();
    paused_ = false;
}

int64_t AudioClock::now_us() const noexcept {
    if (sink_ == nullptr) return AV_NOPTS_VALUE;
    return sink_->played_us();
}

}  // namespace syp::media
