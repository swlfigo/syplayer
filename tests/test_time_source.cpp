#include "media/time_source.h"
#include "media/audio_sink.h"
#include "support/fake_clock.h"
#include "tiny_test.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <thread>

using syp::media::AudioClock;
using syp::media::IAudioSink;
using syp::media::SystemClock;

TEST_CASE(system_clock_advances_with_real_time) {
    SystemClock c;
    const int64_t t0 = c.now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const int64_t t1 = c.now_us();
    // 只断言"确实走了"与"没有暴走"，不断言精确值——这是真实时钟。
    CHECK(t1 - t0 >= 20000);
    CHECK(t1 - t0 <  500000);
}

TEST_CASE(system_clock_freezes_while_paused) {
    SystemClock c;
    c.pause();
    const int64_t t0 = c.now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_EQ(c.now_us(), t0);            // 冻结期间一动不动
    CHECK(c.paused());

    c.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const int64_t t2 = c.now_us();
    // 恢复后继续走，且暂停那 30ms 没有被算进去（留足余量：< 暂停+运行的总和）
    CHECK(t2 > t0);
    CHECK(t2 - t0 < 55000);
}

TEST_CASE(system_clock_scales_by_speed) {
    SystemClock c;
    c.set_speed(2.0);
    const int64_t t0 = c.now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t elapsed = c.now_us() - t0;
    // 2 倍速下媒体时间走得比真实时间快
    CHECK(elapsed > 60000);
}

TEST_CASE(system_clock_set_base_takes_effect_immediately) {
    SystemClock c;
    c.set_base(7'000'000);
    const int64_t t = c.now_us();
    CHECK(t >= 7'000'000);
    CHECK(t <  7'100'000);
}

TEST_CASE(system_clock_speed_change_does_not_jump_time) {
    SystemClock c;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t before = c.now_us();
    c.set_speed(2.0);
    const int64_t after = c.now_us();
    // 换速度的瞬间时间不能跳：before 已经走过的那段必须按旧速率保留
    CHECK(after >= before);
    CHECK(after - before < 5000);
}

TEST_CASE(system_clock_resume_while_not_paused_is_noop) {
    SystemClock c;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t before = c.now_us();
    c.resume();                      // 从未暂停过，这应当是空操作
    const int64_t after = c.now_us();
    CHECK(after >= before);
    CHECK(after - before < 5000);    // 判别力在这半条：清零会摔穿下界
}

TEST_CASE(system_clock_pause_after_running_freezes_elapsed_value) {
    SystemClock c;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    c.pause();
    const int64_t frozen = c.now_us();
    // 判别力在下界：忘了结算的缺陷版本会冻结在 0，而不是已经走过的那段。
    CHECK(frozen >= 150000);
    CHECK(frozen <  400000);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_EQ(c.now_us(), frozen);    // 冻结期间真的一动不动
}

namespace {
// 只为这条用例存在的最小 sink：played_us() 直接返回注入值。
class StubSink final : public IAudioSink {
public:
    syp_status open(int32_t, int32_t, int32_t) override { return SYP_OK; }
    bool       write(const syp::media::Frame&) override { return true; }
    int64_t    played_us() const noexcept override { return played_; }
    void       pause() override {}
    void       resume() override {}
    void       flush(int64_t) override {}
    void       set_speed(double) override {}   // 这条用例不关心倍速，空实现
    bool       failed() const noexcept override { return failed_; }
    bool       output_drained() const noexcept override { return true; }   // AudioClock 不读它

    int64_t played_ = 0;
    bool    failed_ = false;
};
}  // namespace

TEST_CASE(audio_clock_forwards_sink_position) {
    StubSink s;
    s.played_ = 1'234'000;
    AudioClock c(&s);
    CHECK_EQ(c.now_us(), int64_t{1'234'000});
}

TEST_CASE(audio_clock_passes_through_nopts) {
    // sink 还没开或已失败时报 AV_NOPTS_VALUE，时钟必须原样透传，
    // 不能悄悄变成 0 —— 0 是一个合法的播放位置，混淆了调用方就分不出
    // 「在开头」和「时钟不可用」。
    StubSink s;
    s.played_ = AV_NOPTS_VALUE;
    AudioClock c(&s);
    CHECK_EQ(c.now_us(), int64_t{AV_NOPTS_VALUE});
}

TEST_CASE(audio_clock_nullptr_sink_reports_nopts) {
    // sink 指针本身就是空（TrackPlayer 还没接上 sink 的窗口期），
    // 时钟必须报"不可用"，不能解引用空指针，也不能悄悄报 0。
    AudioClock c(nullptr);
    CHECK_EQ(c.now_us(), int64_t{AV_NOPTS_VALUE});
}

int main() { return tiny_test_main(); }
