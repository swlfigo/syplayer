// rate_limiter.h — 进程全局下行限速：令牌桶，播放优先
//
// 【模型】一个桶，速率 R（字节/秒），容量 = R × 1 秒。数据**到达时**记账
// （debit，可把余额扣成负数——在途请求内部全速，欠的账按 R 线性还），
// **调度侧准入**（admit）：余额不够就不发新分片。R == 0 表示不限，此时
// admit 恒真、debit 是空操作（热路径只多一次原子读）。
//
// 【两类阈值】Playing 需要余额 > 0；Preload 需要余额 > 容量的一半。预加载
// 只用播放用剩的额度，余额高于半桶时才能**开始新请求**。
// 注意这不是"永远留出半桶"：一次调度可连续放行多片，字节到达后才扣账，
// 余额可被一次预加载突发扣到远低于 0。
//
// 【毫字节记账】余额以"毫字节"（字节 × 1000）存。R 字节/秒 × Δt 毫秒 恰好
// 是 R × Δt 毫字节，没有除法、没有舍入——500 B/s 每毫秒补 0.5 字节，按整字节
// 累加的话每 1ms 查一次就永远补 0（test_rate_limiter 有专门用例钉住）。
// kMaxRate = 2^40 保证 R × 1000（容量）与单次扣账 × 1000 都远在 int64 之内。
//
// 【唤醒是订阅式的，不是一次性 Waiter】见 Subscription 的注释。
//
// 【锁的纪律】
//   · RateLimiter::mu_ 是**叶子锁**：持有它时不得取任何其它锁、不得调用
//     订阅回调。回调（Scheduler 重跑调度，会拿 Scheduler::mu_）一律在 mu_
//     **之外**执行，因此不可能与 Scheduler / SourceBridge / CacheStore 的
//     锁成环。
//   · 调用 set_rate / debit / admit / arm / subscribe 时可以持有调用方自己的
//     锁（它们只拿 mu_ 这一把叶子锁、从不回调）。
//   · **~Subscription 是唯一会阻塞等待回调的地方**：析构时不得持有回调会去
//     拿的任何锁，也不得在自己的回调里析构（后者做了防御，见析构注释）。
//   · 唤醒回调 wake 抛出的异常会被**吞掉**（记一行 WARN 日志），代价是丢这
//     一次唤醒——下一次 arm 或数据到达会再调度。不吞的话，进程全局唯一的
//     唤醒线程里逃出一个异常就是 std::terminate；
//     在 pump_for_test 里逃出则 in_cb 永不复位、之后的析构永久挂起。
#pragma once

#include "clock.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace syp::dl {

enum class RateClass { Playing, Preload };

class RateLimiter {
    struct Sub;   // 订阅的内部状态，定义见下方 private 段

public:
    static constexpr int64_t kMaxRate = int64_t{1} << 40;   // 字节/秒上限，超出夹到它

    // 进程单例，永不析构（理由见 .cpp 的 instance()）。唤醒线程在 R 第一次
    // 被设为非 0 时懒启动。
    static RateLimiter& instance();

    // 测试用：独立实例，注入时钟。start_thread 为假时不起唤醒线程，由用例
    // 调 pump_for_test() 手动推进；为真时与单例同样懒启动，析构时 notify + join。
    RateLimiter(Clock clock, bool start_thread);
    ~RateLimiter();

    RateLimiter(const RateLimiter&)            = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    // 0 = 不限。<0 → 0；>kMaxRate → kMaxRate。立即生效：
    //   0 → 非 0：满桶起算（刚开闸不应立刻让所有人等）；
    //   非 0 → 非 0：先按旧速率刷新，再把余额夹到新容量（负余额保留）；
    //   → 0：之后 admit 恒真，armed 订阅全部到点。
    // 任何取值都会 notify 唤醒线程重算。
    //
    // 唤醒线程懒启动失败时 fail-open（速率改回 0，详见
    // .cpp）。此时没有唤醒线程能代为派发，本函数会**在调用它的这个线程上
    // 同步跑一次 fire_due()**，直接调已 armed 订阅的 wake 回调（例如
    // `Scheduler::request_schedule`）。这条路径下调用方不得持有任何回调会去
    // 取的锁——正常情况（线程起成功，或本来就没起线程创建）没有这条限制，
    // 因为派发要么发生在异步的唤醒线程上，要么根本不发生。
    void    set_rate(int64_t bytes_per_sec) noexcept;
    int64_t rate() const noexcept;

    // 数据到达时记账。不阻塞、不唤醒任何人；R == 0 时为空操作。负数忽略。
    void debit(int64_t bytes) noexcept;

    // 准入判定。R == 0 恒真。
    bool admit(RateClass cls) noexcept;

    // 分片上限：R == 0 → INT64_MAX；否则 = 容量（R × 1 秒）= R。
    int64_t max_segment_bytes() const noexcept;

    // 订阅：每个调用方（每个 Scheduler）**构造时订阅一次**，之后只 arm()。
    //   arm(cls)：登记"下次 cls 可准入时回调我"。已 armed 时只把类别更新为
    //     最新的 cls（调度器可能刚改过类别，按旧类别等会醒晚）。
    //   限速器在回调**之前**自动解除 arm（一次 arm 至多一次回调）。
    //   析构即注销，且**会等待**正在执行中的那次回调返回，因此回调捕获的
    //   this 不会悬挂。必须先于 RateLimiter 析构。
    //
    // 【为什么是订阅式、不是"每次被拒创建一个一次性 Waiter"】准入发生在
    //   Scheduler::schedule() 持有 Scheduler::mu_ 时；唤醒回调走
    //   request_schedule() 要拿同一把 mu_。
    //   ① 若持 mu_ 析构 Waiter，它等的回调正卡在拿 mu_ ⇒ 死锁；
    //   ② 若回调自己跑进 schedule() 发现不必再等、去析构自己的 Waiter
    //      ⇒ 自己等自己。
    //   订阅对象只在 ~Scheduler 开头、不持任何锁时析构，①② 都不存在。
    class Subscription {
    public:
        void arm(RateClass cls) noexcept;
        ~Subscription();

        Subscription(const Subscription&)            = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&&)                 = delete;
        Subscription& operator=(Subscription&&)      = delete;

    private:
        friend class RateLimiter;
        Subscription(RateLimiter* owner, std::shared_ptr<Sub> sub) noexcept;

        RateLimiter*         owner_;
        std::shared_ptr<Sub> sub_;
    };
    std::unique_ptr<Subscription> subscribe(std::function<void()> wake);

    // ── 测试钩子 ──
    // 以当前时钟回调所有到点的 armed 订阅。只用于 start_thread == false 的
    // 实例（fire_due 假定同一时刻只有一个线程在派发）。
    void    pump_for_test();
    int64_t balance_for_test();       // 刷新后的余额，单位：字节（向下取整）
    int64_t next_due_ms_for_test();   // 最早到点的绝对时刻（ms）；无 armed 订阅返回 -1

    // 让下一次触发线程创建的 set_rate 强制失败（不碰真实系统资源），用于覆盖
    // fail-open 路径：只消费一次（下一次真正尝试创建线程时
    // 若这次没抛，flag 已经被清掉，之后的 set_rate 会正常起线程）。
    // 这是公开成员：谁调它、调的是哪个实例，就影响哪个
    // 实例——包括对 `instance()` 单例调用同样生效。默认 `false`，生产代码里
    // 没有任何调用点，只有测试会设它；不是"调了也不影响单例"。
    void force_next_thread_start_failure_for_test() noexcept;

private:
    struct Sub {
        std::function<void()> wake;          // 构造后不变，锁外调用
        // 以下字段全部受 RateLimiter::mu_ 保护
        bool            armed = false;
        RateClass       cls   = RateClass::Playing;
        bool            dead  = false;       // 已注销（~Subscription 置位），fire_due 见之跳过
        bool            in_cb = false;       // 正在锁外执行**这一个**订阅的 wake
        std::thread::id cb_thread{};         // in_cb 为真时，执行回调的线程
    };

    int64_t now_ms() const noexcept { return clock_.now_ms(clock_.ctx); }

    // ── 以下 *_locked 都要求调用方持有 mu_ ──
    void    refresh_locked(int64_t now) noexcept;
    int64_t threshold_mb_locked(RateClass cls) const noexcept;
    int64_t due_ms_locked(RateClass cls, int64_t now) const noexcept;  // 需先 refresh
    int64_t next_due_locked(int64_t now) noexcept;                     // 含 refresh；-1 = 无

    void fire_due();      // pump_for_test 与唤醒线程共用；不得持 mu_ 调用
    void thread_main();

    const Clock clock_;
    const bool  start_thread_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;         // 唤醒线程：arm / set_rate / 析构 notify
    std::condition_variable cb_done_;    // ~Subscription 等 in_cb 归零

    // rate_ 的写入都在 mu_ 下；原子只是为了 R == 0 快路径的无锁读。
    std::atomic<int64_t> rate_{0};
    int64_t balance_mb_ = 0;             // 余额，毫字节；受 mu_ 保护
    int64_t last_ms_    = 0;             // 上次刷新的时刻；受 mu_ 保护
    bool    stop_       = false;         // 受 mu_ 保护
    std::vector<std::shared_ptr<Sub>> subs_;   // 受 mu_ 保护

    std::once_flag thread_once_;
    std::thread    thread_;   // 只在 set_rate 的 call_once 里写、析构里 join

    // 线程是否真的起来了——只由 call_once 里成功完成
    // `thread_ = std::thread(...)` 的那一方置真，受 mu_ 保护。std::call_once
    // 的 once_flag 本身不可查询，而"这次 set_rate 该不该 fail-open"必须问的
    // 是"线程到底有没有在跑"，不是"这次调用有没有抛异常"——并发的另一次
    // set_rate 可能在同一个 once_flag 上重试并成功，这个标志就是用来分辨
    // 这两种情况的依据。
    bool thread_started_ = false;

    // 测试钩子专用，默认 false 对生产路径零影响（一次原子读）。
    std::atomic<bool> force_thread_start_failure_for_test_{false};
};

}  // namespace syp::dl
