// health_ticker.h — 坏任务检测的进程级计时源
//
// Scheduler 纯事件驱动：一个挂死的下载任务不产生任何回调，没有外部计时源
// 就永远不会被看见。HealthTicker 是全进程一份的"到点叫我"
// 服务：每个 Scheduler 构造时订阅一次，有在途任务时 arm_after 一次，到点在
// ticker 线程上回调（→ Scheduler::health_check）。
//
// 订阅模型照抄 RateLimiter::Subscription，两种自锁的分析
// 见 rate_limiter.h 的 Subscription 注释，这里同样成立：
//   ① 订阅只在 ~Scheduler 开头、不持任何锁时析构；
//   ② 回调里析构自己的订阅不等自己（按线程 id 判）。
// 回调在 ticker 的 mu_ 之外执行，可以随意调 arm_after。
//
// 线程：第一次 arm_after 时懒启动。起不来（std::thread 抛）⇒ fail-open：
// 置 disabled、记一次 WARN，此后 arm_after 为空操作——检测是兜底，不是正确
// 性前提，下载行为退回引入该机制之前的状态。不重试：与限速不同，这里
// 没有"等唤醒才能继续"的调用方，关掉就是最安全的终态。
//
// warm_up()：Scheduler 构造时（不持任何锁）主动调一次，把懒启动提前到
// 构造期完成。之后 arm_after 只是"线程已经在跑/已经 disabled"的快路径，
// 不会再走到 ensure_thread 里可能同步调日志回调的分支——Scheduler 后续都
// 是持 mu_ 调 arm_after，日志回调重入会死锁（见 .cpp 里 arm_after 的注释）。
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

class HealthTicker {
    struct Sub;

public:
    // 进程单例，永不析构（理由同 RateLimiter::instance()）。
    static HealthTicker& instance();

    // 测试用独立实例。start_thread 为假时不起线程，由用例调 pump_for_test()。
    HealthTicker(Clock clock, bool start_thread);
    ~HealthTicker();

    HealthTicker(const HealthTicker&)            = delete;
    HealthTicker& operator=(const HealthTicker&) = delete;

    class Subscription {
    public:
        // 登记"delay_ms 毫秒后（按 ticker 自己的时钟）回调我一次"。负数按 0；
        // 上夹到 kMaxDelayMs（.cpp，24 小时）——now_ms() + delay 理论上可能
        // 溢出 int64（调用方传入接近 INT64_MAX 的 delay_ms 时），没有业务场景
        // 需要真排到那么远，夹一个远超实际用途（挂死/慢任务检查间隔以秒计）
        // 的上限，换绝不溢出。
        // 已 armed 时取较早者。回调之前自动解除 arm。只取 ticker 的叶子锁、
        // 不等待、不回调 ⇒ 持 Scheduler::mu_ 调用安全——但这一条只在
        // warm_up() 已经调过之后成立：第一次懒启动若失败，会在**这次**调用
        // 里同步走 log_msg（见 .cpp），此时不得持有任何锁。Scheduler 保证
        // 构造期（不持锁）先调一次 warm_up()，之后的 arm_after 才真正安全。
        // 传相对延迟而不是绝对时刻：调用方与 ticker 的时钟可以不同源（单例
        // 用系统时钟，Scheduler 可能注入假时钟）。
        void arm_after(int64_t delay_ms) noexcept;
        ~Subscription();

        Subscription(const Subscription&)            = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&&)                 = delete;
        Subscription& operator=(Subscription&&)      = delete;

    private:
        friend class HealthTicker;
        Subscription(HealthTicker* owner, std::shared_ptr<Sub> sub) noexcept;

        HealthTicker*        owner_;
        std::shared_ptr<Sub> sub_;
    };
    std::unique_ptr<Subscription> subscribe(std::function<void()> fire);

    // 主动把懒启动提前完成。调用方不得持有任何锁（可能同步调日志回调）。
    // 之后的 arm_after 都是快路径（线程已起/已 disabled），不会再触发线程
    // 创建，因此可以安全地在持锁时调用。见头部注释。
    void warm_up() noexcept;

    // ── 测试钩子 ──
    void    pump_for_test();
    int64_t next_due_ms_for_test();
    // 让下一次懒启动强制失败（只消费一次）。默认 false，生产代码无调用点。
    void    force_next_thread_start_failure_for_test() noexcept;
    bool    disabled_for_test() const noexcept;

private:
    struct Sub {
        std::function<void()> fire;          // 构造后不变，锁外调用
        // 以下受 HealthTicker::mu_ 保护
        bool            armed  = false;
        int64_t         due_ms = 0;
        bool            dead   = false;
        bool            in_cb  = false;
        std::thread::id cb_thread{};
    };

    int64_t now_ms() const noexcept { return clock_.now_ms(clock_.ctx); }
    int64_t next_due_locked() const noexcept;   // -1 = 无
    void    ensure_thread() noexcept;           // 不得持 mu_ 调用
    void    fire_due();                         // 不得持 mu_ 调用
    void    thread_main();

    const Clock clock_;
    const bool  start_thread_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::condition_variable cb_done_;
    bool stop_ = false;                              // 受 mu_ 保护
    std::vector<std::shared_ptr<Sub>> subs_;         // 受 mu_ 保护

    std::once_flag    thread_once_;
    std::thread       thread_;
    std::atomic<bool> disabled_{false};
    std::atomic<bool> force_thread_start_failure_for_test_{false};

    // 【镜像 RateLimiter 的写法】只有真正把线程跑起来的那一次 ensure_thread
    // 才置位，受 mu_ 保护。std::once_flag 本身不可查询"是否成功过"——并发的
    // 另一次 ensure_thread 可能在同一个 once_flag 上重试并成功，判断"这次
    // 失败该不该 fail-open"必须问这个标志，不是问"这次调用有没有抛"。
    // 见 .cpp 里 ensure_thread 失败分支的注释。
    bool thread_started_ = false;
};

}  // namespace syp::dl
