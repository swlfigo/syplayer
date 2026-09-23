// test_health_ticker.cpp — HealthTicker：订阅、到点回调、懒启动线程、fail-open
#include "tiny_test.h"

#include <dl/clock.h>
#include <dl/health_ticker.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

using syp::dl::Clock;
using syp::dl::HealthTicker;

namespace {
struct FakeClock {
    int64_t t = 0;
    static int64_t now(void* ctx) { return static_cast<FakeClock*>(ctx)->t; }
    Clock clock() { return Clock{&now, this}; }
};
}  // namespace

TEST_CASE(arm_then_pump_before_due_does_not_fire) {
    FakeClock clk;
    HealthTicker tk(clk.clock(), /*start_thread=*/false);
    int n = 0;
    auto sub = tk.subscribe([&] { ++n; });
    sub->arm_after(1000);
    CHECK_EQ(tk.next_due_ms_for_test(), 1000);
    clk.t = 999;
    tk.pump_for_test();
    CHECK_EQ(n, 0);
}

TEST_CASE(fires_once_at_due_then_disarms) {
    FakeClock clk;
    HealthTicker tk(clk.clock(), false);
    int n = 0;
    auto sub = tk.subscribe([&] { ++n; });
    sub->arm_after(1000);
    clk.t = 1000;
    tk.pump_for_test();
    CHECK_EQ(n, 1);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    clk.t = 5000;
    tk.pump_for_test();
    CHECK_EQ(n, 1);
}

TEST_CASE(rearm_keeps_earliest_due) {
    FakeClock clk;
    HealthTicker tk(clk.clock(), false);
    auto sub = tk.subscribe([] {});
    sub->arm_after(1000);
    sub->arm_after(3000);
    CHECK_EQ(tk.next_due_ms_for_test(), 1000);
    sub->arm_after(200);
    CHECK_EQ(tk.next_due_ms_for_test(), 200);
}

TEST_CASE(negative_delay_is_due_now) {
    FakeClock clk;
    clk.t = 50;
    HealthTicker tk(clk.clock(), false);
    int n = 0;
    auto sub = tk.subscribe([&] { ++n; });
    sub->arm_after(-7);
    CHECK_EQ(tk.next_due_ms_for_test(), 50);
    tk.pump_for_test();
    CHECK_EQ(n, 1);
}

TEST_CASE(huge_delay_is_clamped) {
    // now_ms() + delay_ms 理论上会在 delay 接近 INT64_MAX 时溢出；health_ticker.cpp
    // 把 delay 上夹到 kMaxDelayMs = 86'400'000ms（24 小时）。手算期望值：
    // due = clk.t + 86'400'000，不是 clk.t + INT64_MAX。
    FakeClock clk;
    clk.t = 100;
    HealthTicker tk(clk.clock(), false);
    auto sub = tk.subscribe([] {});
    sub->arm_after(INT64_MAX);
    CHECK_EQ(tk.next_due_ms_for_test(), int64_t{100} + 86'400'000);
}

TEST_CASE(unsubscribed_is_not_fired) {
    FakeClock clk;
    HealthTicker tk(clk.clock(), false);
    int n = 0;
    auto sub = tk.subscribe([&] { ++n; });
    sub->arm_after(10);
    sub.reset();
    clk.t = 100;
    tk.pump_for_test();
    CHECK_EQ(n, 0);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
}

TEST_CASE(callback_may_rearm_itself) {
    FakeClock clk;
    HealthTicker tk(clk.clock(), false);
    int n = 0;
    std::unique_ptr<HealthTicker::Subscription> sub;
    sub = tk.subscribe([&] { ++n; sub->arm_after(1000); });
    sub->arm_after(1000);
    clk.t = 1000;
    tk.pump_for_test();
    CHECK_EQ(n, 1);
    CHECK_EQ(tk.next_due_ms_for_test(), 2000);   // 回调里 arm：1000 + 1000
}

TEST_CASE(one_callback_destroying_a_sibling_skips_it) {
    // 同批两个到点订阅，A 的回调里析构 B：B 不得再被回调（逐个标 in_cb）。
    FakeClock clk;
    HealthTicker tk(clk.clock(), false);
    int nb = 0;
    std::unique_ptr<HealthTicker::Subscription> b;
    auto a = tk.subscribe([&] { b.reset(); });
    b = tk.subscribe([&] { ++nb; });
    a->arm_after(0);
    b->arm_after(0);
    tk.pump_for_test();
    CHECK_EQ(nb, 0);
}

TEST_CASE(dtor_waits_for_inflight_fire) {
    HealthTicker tk(syp::dl::system_clock(), /*start_thread=*/true);
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    auto sub = tk.subscribe([&] {
        entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        finished = true;
    });
    sub->arm_after(0);
    for (int i = 0; i < 400 && !entered; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(entered.load());
    sub.reset();                       // 必须等回调返回
    CHECK(finished.load());
}

TEST_CASE(lazy_thread_fires_with_real_clock) {
    HealthTicker tk(syp::dl::system_clock(), true);
    std::atomic<int> n{0};
    auto sub = tk.subscribe([&] { ++n; });
    sub->arm_after(20);
    for (int i = 0; i < 400 && n.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_EQ(n.load(), 1);
}

TEST_CASE(thread_start_failure_disables_ticker) {
    HealthTicker tk(syp::dl::system_clock(), true);
    tk.force_next_thread_start_failure_for_test();
    std::atomic<int> n{0};
    auto sub = tk.subscribe([&] { ++n; });
    sub->arm_after(0);                 // 懒启动失败 ⇒ disabled，此后 arm 为空操作
    CHECK(tk.disabled_for_test());
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    sub->arm_after(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(n.load(), 0);
}

TEST_CASE(warm_up_failure_disables_ticker_and_arm_is_noop) {
    // Controller 补充：Scheduler 会在构造期（不持锁）调 warm_up() 把懒启动
    // 提前完成，这样之后持锁调用的 arm_after 不会再走到可能同步打日志的
    // 线程创建分支。这里钉住 warm_up 本身也会触发同一条 fail-open 路径。
    HealthTicker tk(syp::dl::system_clock(), true);
    tk.force_next_thread_start_failure_for_test();
    tk.warm_up();
    CHECK(tk.disabled_for_test());
    std::atomic<int> n{0};
    auto sub = tk.subscribe([&] { ++n; });
    sub->arm_after(0);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(n.load(), 0);
}

int main() { return tiny_test_main(); }
