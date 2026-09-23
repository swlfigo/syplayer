// test_rate_limiter.cpp — 令牌桶的纯逻辑（假时钟、不起线程）与线程模式（真实时钟）。
#include "tiny_test.h"

#include <dl/rate_limiter.h>
#include <syplayer/syp_net.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

using syp::dl::Clock;
using syp::dl::RateClass;
using syp::dl::RateLimiter;

namespace {
struct FakeClock {
    int64_t t = 1000;   // 不从 0 起，避免把"未初始化的 last"误当成合法时刻
    static int64_t now(void* c) { return static_cast<FakeClock*>(c)->t; }
    Clock clock() { return Clock{&now, this}; }
};
}  // namespace

TEST_CASE(zero_rate_admits_everything_and_ignores_debits) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    CHECK(rl.admit(RateClass::Playing));
    CHECK(rl.admit(RateClass::Preload));
    rl.debit(int64_t{1} << 40);
    CHECK(rl.admit(RateClass::Preload));
    CHECK_EQ(rl.max_segment_bytes(), INT64_MAX);
}

TEST_CASE(turning_limit_on_starts_from_a_full_bucket) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);                       // 容量 = 1000 字节（1 秒额度）
    CHECK_EQ(rl.balance_for_test(), 1000);
    CHECK_EQ(rl.max_segment_bytes(), 1000);
}

TEST_CASE(debit_can_go_negative_and_refills_linearly) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(3000);                          // 1000 - 3000 = -2000
    CHECK_EQ(rl.balance_for_test(), -2000);
    fc.t += 500;                             // +1000 B/s × 0.5 s = +500
    CHECK_EQ(rl.balance_for_test(), -1500);
}

TEST_CASE(refill_is_capped_at_one_second_of_rate) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);                          // 0
    fc.t += 60000;                           // 空闲 60 秒也只补满到容量
    CHECK_EQ(rl.balance_for_test(), 1000);
}

// 【精度】500 B/s 每毫秒只补 0.5 字节。若按"字节"整数累加，每 1ms 查一次
// 会永远补 0。实现必须按"毫字节"累计（这是余额公式的要求）。
TEST_CASE(small_rates_accumulate_fractional_bytes_across_frequent_polls) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(500);
    rl.debit(500);                           // 0
    for (int i = 0; i < 1000; ++i) {         // 1000 次、每次 1ms
        fc.t += 1;
        (void)rl.balance_for_test();
    }
    CHECK_EQ(rl.balance_for_test(), 500);    // 500 B/s × 1 s = 500
}

TEST_CASE(playing_is_admitted_above_zero_preload_only_above_half_capacity) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);                       // 容量 1000，保留线 500
    rl.debit(700);                           // 余额 300：Playing 过、Preload 不过
    CHECK(rl.admit(RateClass::Playing));
    CHECK(!rl.admit(RateClass::Preload));
    rl.debit(300);                           // 余额 0：两类都不过（需要 > 0）
    CHECK(!rl.admit(RateClass::Playing));
    CHECK(!rl.admit(RateClass::Preload));
    fc.t += 501;                             // 余额 501：两类都过
    CHECK(rl.admit(RateClass::Playing));
    CHECK(rl.admit(RateClass::Preload));
}

TEST_CASE(lowering_rate_clamps_balance_but_keeps_debt) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);                       // 余额 1000
    rl.set_rate(200);                        // 夹到新容量 200
    CHECK_EQ(rl.balance_for_test(), 200);
    rl.debit(700);                           // -500
    rl.set_rate(100);                        // 负余额保留
    CHECK_EQ(rl.balance_for_test(), -500);
}

TEST_CASE(set_rate_clamps_negative_to_zero_and_huge_to_max) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(-5);
    CHECK_EQ(rl.rate(), 0);
    rl.set_rate(INT64_MAX);
    CHECK_EQ(rl.rate(), RateLimiter::kMaxRate);
}

TEST_CASE(armed_subscription_fires_once_when_its_threshold_is_crossed) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1500);                          // -500
    int fired = 0;
    auto sub = rl.subscribe([&] { ++fired; });
    sub->arm(RateClass::Playing);
    // 需要余额 > 0：-500 + 1000·t/1000 > 0 ⇒ t > 500ms ⇒ 最早 501ms 后
    CHECK_EQ(rl.next_due_ms_for_test(), fc.t + 501);
    fc.t += 500;
    rl.pump_for_test();
    CHECK_EQ(fired, 0);
    fc.t += 1;
    rl.pump_for_test();
    CHECK_EQ(fired, 1);
    fc.t += 1000;
    rl.pump_for_test();
    CHECK_EQ(fired, 1);                      // 回调前已自动解除 arm
    CHECK_EQ(rl.next_due_ms_for_test(), -1);
}

TEST_CASE(preload_subscription_waits_for_the_reserve_line) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);                          // 0
    auto sub = rl.subscribe([] {});
    sub->arm(RateClass::Preload);
    // 需要余额 > 500：1000·t/1000 > 500 ⇒ t = 501
    CHECK_EQ(rl.next_due_ms_for_test(), fc.t + 501);
}

TEST_CASE(callback_runs_outside_the_limiter_lock) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);
    bool admitted_inside = false;
    std::unique_ptr<RateLimiter::Subscription> sub;
    sub = rl.subscribe([&] { admitted_inside = rl.admit(RateClass::Playing); });
    sub->arm(RateClass::Playing);
    fc.t += 10;
    rl.pump_for_test();                      // 回调里再调 admit：持锁回调就会自锁
    CHECK(admitted_inside);
}

TEST_CASE(turning_limit_off_fires_every_armed_subscription) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(5000);
    int a = 0, b = 0;
    auto sa = rl.subscribe([&] { ++a; });
    auto sb = rl.subscribe([&] { ++b; });
    sa->arm(RateClass::Playing);
    sb->arm(RateClass::Preload);
    rl.set_rate(0);
    rl.pump_for_test();
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 1);
}

TEST_CASE(destroyed_subscription_is_never_called) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);
    int fired = 0;
    auto sub = rl.subscribe([&] { ++fired; });
    sub->arm(RateClass::Playing);
    sub.reset();
    fc.t += 5000;
    rl.pump_for_test();
    CHECK_EQ(fired, 0);
}

// 【同批多个订阅相互析构】同一批到点的 A、B：A 的回调里析构 B（例如释放了 B 所属的
// Scheduler）。契约只禁止在**自己的**回调里析构，所以这是合法用法——B 注销
// 之后绝不能再被回调。若 in_cb / cb_thread 整批标记，B 的析构会以为"当前
// 线程正在回调我"而不等，随后派发照样调 B->wake()：悬挂的 this。
TEST_CASE(subscription_destroyed_by_an_earlier_callback_in_the_same_batch_is_skipped) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);                          // 0
    int a = 0, b = 0;
    std::unique_ptr<RateLimiter::Subscription> sb;
    auto sa = rl.subscribe([&] { ++a; sb.reset(); });   // 先订阅 ⇒ 同批里先回调
    sb = rl.subscribe([&] { ++b; });
    sa->arm(RateClass::Playing);
    sb->arm(RateClass::Playing);
    fc.t += 10;                              // 余额 10 > 0：两者同批到点
    rl.pump_for_test();
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 0);
    CHECK(sb == nullptr);
}

// 【回调抛异常】吞掉、丢这一次唤醒；同批其余订阅照常回调；
// 抛异常那个的 in_cb 已复位，所以它的析构不会挂起。
TEST_CASE(throwing_callback_is_swallowed_and_does_not_block_others_or_destruction) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);                          // 0
    int thrown = 0, b = 0;
    auto sa = rl.subscribe([&] { ++thrown; throw std::runtime_error("boom"); });  // 同批先回调
    auto sb = rl.subscribe([&] { ++b; });
    sa->arm(RateClass::Playing);
    sb->arm(RateClass::Playing);
    fc.t += 10;
    rl.pump_for_test();                      // 不得把异常抛出来
    CHECK_EQ(thrown, 1);
    CHECK_EQ(b, 1);
    sa.reset();                              // in_cb 若没复位，这里永久挂起
    CHECK(sa == nullptr);
}

// 钉住防自锁那条路径：在自己的回调里析构自己的订阅，
// 派发必须照常返回（析构见 cb_thread 是当前线程就不等）。
TEST_CASE(destroying_own_subscription_inside_its_callback_does_not_self_deadlock) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);                          // 0
    int fired = 0;
    std::unique_ptr<RateLimiter::Subscription> sub;
    sub = rl.subscribe([&] { ++fired; sub.reset(); });
    sub->arm(RateClass::Playing);
    fc.t += 10;
    rl.pump_for_test();                      // 自己等自己 ⇒ 这里死锁（ctest 超时）
    CHECK_EQ(fired, 1);
    CHECK(sub == nullptr);
}

// 已 armed 时再 arm 以最新类别为准（这条语义会被依赖：
// 调度器刚从 Preload 改成 Playing，按旧类别等会醒晚）。
TEST_CASE(re_arming_with_a_different_class_uses_the_latest_class) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(1000);                          // 余额 0
    auto sub = rl.subscribe([] {});
    sub->arm(RateClass::Preload);
    // Preload 需 > 500 B：1000·t/1000 > 500 ⇒ ⌊500000/1000⌋ + 1 = 501
    CHECK_EQ(rl.next_due_ms_for_test(), fc.t + 501);
    sub->arm(RateClass::Playing);
    // Playing 需 > 0：⌊(0 − 0)/1000⌋ + 1 = 1
    CHECK_EQ(rl.next_due_ms_for_test(), fc.t + 1);
}

// 【深度欠账跨越 >1 秒的空档仍须线性恢复】若每次刷新都把 Δt 夹到 1000ms，
// 一次隔了 2.5 秒的刷新只补 1 秒额度，欠账恢复被拖慢（唤醒线程睡到算好的
// 到点时刻醒来会发现"还没到"，反复重睡）。夹取应当夹"补到满桶所需的时长"，
// 而不是固定的 1 秒。
TEST_CASE(deep_debt_refills_linearly_across_gaps_longer_than_one_second) {
    FakeClock fc;
    RateLimiter rl(fc.clock(), false);
    rl.set_rate(1000);
    rl.debit(4000);                          // 1000 - 4000 = -3000
    fc.t += 2500;                            // +1000 B/s × 2.5 s = +2500 ⇒ -500
    CHECK_EQ(rl.balance_for_test(), -500);
    fc.t += 5000;                            // -500 + 5000 = 4500 ⇒ 夹到容量 1000
    CHECK_EQ(rl.balance_for_test(), 1000);
}

// 线程模式（真实时钟）。上限 2 秒只是防挂死；正常应在约 100ms 内回调。
TEST_CASE(wake_thread_calls_back_without_manual_pump) {
    RateLimiter rl(syp::dl::system_clock(), true);
    rl.set_rate(10000);                      // 10 KB/s
    rl.debit(11000);                         // -1000 ⇒ 约 100ms 后越过 0
    std::atomic<int> fired{0};
    auto sub = rl.subscribe([&] { fired.fetch_add(1); });
    sub->arm(RateClass::Playing);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fired.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 再等 50ms：若回调前没解除 arm，线程会接着重复回调，这里才能看出来。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(fired.load(), 1);
}

// 析构与一次正在进行的回调竞争：析构必须等回调返回（TSan 覆盖）。
TEST_CASE(subscription_destructor_waits_for_an_in_flight_callback) {
    RateLimiter rl(syp::dl::system_clock(), true);
    rl.set_rate(1000000);
    rl.debit(1000000 + 1000);                // 约 1ms 后越过 0
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    auto sub = rl.subscribe([&] {
        entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        finished.store(true);
    });
    sub->arm(RateClass::Playing);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(entered.load());
    sub.reset();                             // 必须阻塞到回调返回
    CHECK(finished.load());
}

// fail-open 把速率改回 0 之后，没有任何线程会再去调
// fire_due()（调用方只有 thread_main 与 pump_for_test）——Scheduler 没有
// 驱动线程兜底（前提：唯一的恢复路径就是限速器的唤醒回调），
// 所以"rate_ = v 到 rate_ = 0 之间那扇窗口里因为被拒绝而 arm() 登记的订阅"
// 必须在 fail-open 时被同步补一次派发，否则永久卡住。这里测的是一个更简单、
// 效果等价的形状：**任何已经 armed 的订阅**，只要 fail-open 发生，就必须被
// 同步唤醒一次——不需要真的去重现"debit 恰好在两次加锁之间到达"那个窄
// 窗口，armed 状态本身就是判据。
TEST_CASE(fail_open_wakes_already_armed_subscribers_synchronously) {
    RateLimiter rl(syp::dl::system_clock(), true);
    std::atomic<int> fired{0};
    auto sub = rl.subscribe([&] { fired.fetch_add(1); });
    sub->arm(RateClass::Playing);          // 此刻 R == 0，尚未起线程，没人理它
    rl.force_next_thread_start_failure_for_test();
    rl.set_rate(1000000);                  // 触发线程创建，注入失败 ⇒ fail-open
    CHECK_EQ(fired.load(), 1);             // fail-open 内同步补跑的那次 fire_due 唤醒了它
    CHECK_EQ(rl.rate(), 0);
}

// 唤醒线程创建失败时 fail-open：set_rate 是 noexcept，
// std::thread 构造可能抛 std::system_error，接住之后必须把速率主动改回 0——
// 不然被拒绝的调用方永远等不到一个不存在的线程的回调，等价于"播放永久卡住"。
// 用测试钩子强制下一次线程创建抛异常，不碰真实系统资源。
TEST_CASE(fail_open_when_wake_thread_fails_to_start) {
    RateLimiter rl(syp::dl::system_clock(), true);
    rl.force_next_thread_start_failure_for_test();
    rl.set_rate(1000000);            // 触发一次线程创建尝试，注入的异常被接住
    CHECK_EQ(rl.rate(), 0);          // fail-open：主动改回 0，不是维持"限速但没人醒"
    CHECK(rl.admit(RateClass::Playing));
    CHECK(rl.admit(RateClass::Preload));
    // 确认真的没有起线程：进程仍然存活、后续调用不挂死（若 catch 被去掉，
    // set_rate 里的异常会穿透 noexcept 边界 std::terminate，本用例连同整个
    // 测试进程一起崩掉——ctest 会看到非零/信号退出而不是一行 FAIL，这就是
    // 这条变异的红法）。
    rl.set_rate(0);
}

// C API（syp_net.h）走进程单例：用例结束必须复位为 0，用 RAII 守卫（照
// RateGuard 体例，见 tests/test_preloader.cpp）——即便本用例里都是
// CHECK_EQ、不会提前 return，仍按本仓纪律统一走 RAII，不靠"手写复位在最后
// 一行"这种容易被后续编辑漏掉的写法。
namespace {
struct RateGuard {
    RateGuard() = default;
    ~RateGuard() { syp_rate_limit_set(0); }
    RateGuard(const RateGuard&)            = delete;
    RateGuard& operator=(const RateGuard&) = delete;
};
}  // namespace

TEST_CASE(c_api_round_trips_through_the_process_singleton) {
    RateGuard guard;
    syp_rate_limit_set(123456);
    CHECK_EQ(syp_rate_limit_get(), 123456);
    CHECK_EQ(RateLimiter::instance().rate(), 123456);   // 读消费者
    syp_rate_limit_set(-1);                              // 负数 ⇒ 0（不限）
    CHECK_EQ(syp_rate_limit_get(), 0);
    syp_rate_limit_set(INT64_MAX);                       // 夹到 kMaxRate
    CHECK_EQ(syp_rate_limit_get(), RateLimiter::kMaxRate);
}

int main() { return tiny_test_main(); }
