// health_ticker.cpp — 设计与锁纪律见 health_ticker.h 顶部注释。
#include "health_ticker.h"

#include <syplayer/syp_types.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace syp::dl {

// 定义在 source_bridge.cpp（同一静态库、同一命名空间）。
void log_msg(syp_log_level lvl, const char* tag, const char* msg);

namespace {
// 单次睡眠上限：早醒无害（醒来重算），避免超大毫秒数换算纳秒溢出。
constexpr int64_t kMaxSleepMs = 1000;

// arm_after(delay_ms) 的 delay 上夹（24 小时）。now_ms() + delay 理论上会在
// 调用方传入接近 INT64_MAX 的 delay 时溢出 int64；挂死/慢任务检查间隔实际
// 以秒计，没有业务场景需要排到比这更远，夹一个远超实际用途的上限换绝不
// 溢出，比证明"调用方永远不会传超大值"更便宜。
constexpr int64_t kMaxDelayMs = 86'400'000;
}  // namespace

// 【故意泄漏，永不析构】理由同 RateLimiter::instance()。
HealthTicker& HealthTicker::instance() {
    static HealthTicker* p = new HealthTicker(system_clock(), true);
    return *p;
}

HealthTicker::HealthTicker(Clock clock, bool start_thread)
    : clock_(clock), start_thread_(start_thread) {}

HealthTicker::~HealthTicker() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

HealthTicker::Subscription::Subscription(HealthTicker* owner, std::shared_ptr<Sub> sub) noexcept
    : owner_(owner), sub_(std::move(sub)) {}

std::unique_ptr<HealthTicker::Subscription> HealthTicker::subscribe(std::function<void()> fire) {
    auto s  = std::make_shared<Sub>();
    s->fire = std::move(fire);
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs_.push_back(s);
    }
    return std::unique_ptr<Subscription>(new Subscription(this, std::move(s)));
}

void HealthTicker::ensure_thread() noexcept {
    if (!start_thread_ || disabled_.load(std::memory_order_relaxed)) return;
    // call_once 的可调用体一旦抛出，标准保证 once_flag 不会被置位为"已完成"，
    // 所以并发的另一次 ensure_thread 可能会在同一个 once_flag 上重新尝试并
    // 成功——这里不重试（见头文件），但"这次失败该不该 fail-open"必须看
    // 线程到底有没有跑起来，不是看这次调用有没有抛。做法镜像
    // RateLimiter::set_rate（见 rate_limiter.cpp）。
    bool failed = false;
    try {
        std::call_once(thread_once_, [this] {
            if (force_thread_start_failure_for_test_.exchange(false, std::memory_order_relaxed)) {
                throw std::runtime_error("forced thread-start failure (test)");
            }
            thread_ = std::thread([this] { thread_main(); });
            // 只有真正把线程跑起来的这一方才置位。std::once_flag 本身不可
            // 查询"是否成功过"，判断"该不该 fail-open"必须问这个。
            std::lock_guard<std::mutex> lk(mu_);
            thread_started_ = true;
        });
    } catch (...) {
        failed = true;
    }
    if (!failed) return;
    bool did_fail_open = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 并发的另一次 ensure_thread 可能已经用同一个 call_once 重试成功、
        // 线程真的在跑了——这时候线程存在，不该被这次失败的调用关掉检测
        // （否则就是"线程在，检测却被关掉"，还会多打一条无意义的 WARN）。
        if (!thread_started_) {
            disabled_.store(true, std::memory_order_relaxed);
            for (auto& s : subs_) s->armed = false;
            did_fail_open = true;
        }
    }
    if (did_fail_open) {
        // 不重试（见头文件）。锁外打日志（日志回调可能重入）。
        log_msg(SYP_LOG_WARN, "health",
                "ticker thread failed to start; bad-task detection disabled (fail-open)");
    }
}

void HealthTicker::warm_up() noexcept { ensure_thread(); }

// 【持 Scheduler::mu_ 调用】ensure_thread 会起线程但不回调、不等待任何
// Scheduler 的锁；起线程失败的日志回调是唯一的外部调用。
// Scheduler 构造时先 warm_up()，之后的 arm 不会再走到这里。
void HealthTicker::Subscription::arm_after(int64_t delay_ms) noexcept {
    owner_->ensure_thread();
    if (owner_->disabled_.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lk(owner_->mu_);
        if (sub_->dead) return;
        const int64_t clamped = std::clamp<int64_t>(delay_ms, 0, kMaxDelayMs);
        const int64_t due     = owner_->now_ms() + clamped;
        if (!sub_->armed || due < sub_->due_ms) sub_->due_ms = due;
        sub_->armed = true;
    }
    owner_->cv_.notify_all();
}

HealthTicker::Subscription::~Subscription() {
    std::unique_lock<std::mutex> lk(owner_->mu_);
    auto& v = owner_->subs_;
    v.erase(std::remove(v.begin(), v.end(), sub_), v.end());
    sub_->armed = false;
    sub_->dead  = true;
    if (sub_->in_cb && sub_->cb_thread != std::this_thread::get_id()) {
        owner_->cb_done_.wait(lk, [this] { return !sub_->in_cb; });
    }
}

int64_t HealthTicker::next_due_locked() const noexcept {
    int64_t best = -1;
    for (const auto& s : subs_) {
        if (!s->armed) continue;
        if (best < 0 || s->due_ms < best) best = s->due_ms;
    }
    return best;
}

// 逐个标 in_cb 的理由见 RateLimiter::fire_due 注释（同批互相析构的 UAF）。
void HealthTicker::fire_due() {
    std::vector<std::shared_ptr<Sub>> due;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t now = now_ms();
        for (const auto& s : subs_) {
            if (!s->armed || s->due_ms > now) continue;
            s->armed = false;
            due.push_back(s);
        }
    }
    const auto self = std::this_thread::get_id();
    for (const auto& s : due) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (s->dead) continue;
            s->in_cb     = true;
            s->cb_thread = self;
        }
        bool threw = false;
        try {
            s->fire();
        } catch (...) {
            threw = true;   // 吞掉：丢一次检查，下一次 arm 会补
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            s->in_cb = false;
        }
        cb_done_.notify_all();
        if (threw) log_msg(SYP_LOG_WARN, "health", "tick callback threw; one check dropped");
    }
}

void HealthTicker::thread_main() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        const int64_t now = now_ms();
        const int64_t due = next_due_locked();
        if (due < 0) {
            cv_.wait(lk);
            continue;
        }
        if (due > now) {
            cv_.wait_for(lk, std::chrono::milliseconds(std::min(due - now, kMaxSleepMs)));
            continue;
        }
        lk.unlock();
        fire_due();
        lk.lock();
    }
}

void HealthTicker::pump_for_test() { fire_due(); }

int64_t HealthTicker::next_due_ms_for_test() {
    std::lock_guard<std::mutex> lk(mu_);
    return next_due_locked();
}

void HealthTicker::force_next_thread_start_failure_for_test() noexcept {
    force_thread_start_failure_for_test_.store(true, std::memory_order_relaxed);
}

bool HealthTicker::disabled_for_test() const noexcept {
    return disabled_.load(std::memory_order_relaxed);
}

}  // namespace syp::dl
