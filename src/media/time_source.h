// time_source.h — 主时钟抽象与两个实现。
//
// 三个实现：AudioClock（音频主时钟）、SystemClock（无音频时的退路）、
// 以及测试里的 FakeClock（tests/support/fake_clock.h）。TrackPlayer 只认接口。
#pragma once

#include <chrono>
#include <cstdint>

namespace syp::media {

class IAudioSink;

class TimeSource {
public:
    virtual ~TimeSource() = default;
    // 当前播放位置，媒体时间轴，微秒。暂停时冻结。
    virtual int64_t now_us() const noexcept = 0;
};

class SystemClock final : public TimeSource {
public:
    SystemClock();

    int64_t now_us() const noexcept override;

    void set_base(int64_t base_us) noexcept;

    // 合法范围 [0.5, 2.0]（与允许的播放速率范围一致）。范围校验的责任
    // 在调用方 TrackPlayer::set_speed，本类刻意不做防御性
    // 检查——传 0 或负数不会崩，但会悄悄破坏这个类的基本假设：speed = 0
    // 让时钟彻底冻结，但 paused() 仍报 false，外部观察者分不出"暂停"和
    // "速度为零"；speed 为负数会让 now_us() 真的往回走（时间倒流），
    // 破坏"暂停之外单调不减"这条隐含契约。
    void set_speed(double speed) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    bool paused() const noexcept { return paused_; }

private:
    using Clock = std::chrono::steady_clock;
    int64_t             base_us_  = 0;
    Clock::time_point   anchor_{};       // 上一次基准被重设的真实时刻
    double              speed_    = 1.0;
    bool                paused_   = false;
};

class AudioClock final : public TimeSource {
public:
    explicit AudioClock(IAudioSink* sink) noexcept : sink_(sink) {}
    int64_t now_us() const noexcept override;

private:
    IAudioSink* sink_;   // 不持有所有权；生命周期由 TrackPlayer 保证
};

}  // namespace syp::media
