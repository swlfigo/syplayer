// rate_limiter.cpp — 令牌桶本体。设计与锁纪律见 rate_limiter.h 顶部注释。
#include "rate_limiter.h"

#include <syplayer/syp_types.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace syp::dl {

// 定义在 source_bridge.cpp（同一静态库、同一命名空间），与 cache_store.h 同法前置声明。
void log_msg(syp_log_level lvl, const char* tag, const char* msg);

namespace {
// 余额下限（毫字节）。正常用法远到不了（单次扣账 ≤ INT32_MAX 字节），它只
// 保证无论扣多少次，"容量 − 余额"与"(阈值 − 余额) / R"都不会溢出 int64：
// 容量 ≤ 2^40 × 1000 < 2^50，2^50 + 2^62 < 2^63。
constexpr int64_t kMinBalanceMb = -(int64_t{1} << 62);

// 唤醒线程单次睡眠的上限。早醒无害（醒来会重算），而把毫秒数原样交给
// wait_for 在"欠账极深、R 极小"时可能大到让 chrono 换算成纳秒时溢出。
constexpr int64_t kMaxSleepMs = 1000;
}  // namespace

// 【故意泄漏，永不析构】静态析构期去 join 一个可能正在回调某个已被销毁的
// Scheduler 的线程，是经典的静态析构顺序陷阱；进程退出时这份
// 清理没有任何价值，唤醒线程随进程结束即可。
RateLimiter& RateLimiter::instance() {
    static RateLimiter* p = new RateLimiter(system_clock(), true);
    return *p;
}

RateLimiter::RateLimiter(Clock clock, bool start_thread)
    : clock_(clock), start_thread_(start_thread) {}

RateLimiter::~RateLimiter() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

// ── 余额 ──────────────────────────────────────────────────────────────

// 惰性补充：balance += R × Δt（字节/秒 × 毫秒 = 毫字节），夹到容量 R × 1000。
// 【Δt 的夹取】下界 0（时钟不回退也照样防）；上界是"补到满桶所需的时长"
// ⌈(容量 − 余额) / R⌉，而**不是**固定的 1000ms——固定夹 1 秒会让一次隔了
// 超过 1 秒的刷新少补，深度欠账（余额 < 0）就不再按 R 线性恢复，唤醒线程
// 睡到算好的到点时刻醒来会发现"还没到"而反复重睡。按所需时长夹取同样杜绝
// R × Δt 溢出：落在夹取分支之外时 R × Δt < 容量 − 余额 + R。
void RateLimiter::refresh_locked(int64_t now) noexcept {
    const int64_t r  = rate_.load(std::memory_order_relaxed);
    const int64_t dt = now - last_ms_;
    last_ms_ = now;
    if (r == 0 || dt <= 0) return;
    const int64_t cap = r * 1000;
    if (balance_mb_ >= cap) {
        balance_mb_ = cap;
        return;
    }
    const int64_t need    = cap - balance_mb_;               // > 0
    const int64_t dt_full = need / r + (need % r != 0 ? 1 : 0);
    if (dt >= dt_full) {
        balance_mb_ = cap;
    } else {
        balance_mb_ += r * dt;
    }
}

// 阈值（毫字节）：Playing 需要余额 > 0；Preload 需要余额 > 容量的一半
// = R × 1000 / 2 = R × 500。
int64_t RateLimiter::threshold_mb_locked(RateClass cls) const noexcept {
    if (cls == RateClass::Playing) return 0;
    return rate_.load(std::memory_order_relaxed) * 500;
}

// 到点时刻：余额 B、阈值 T、速率 R（毫字节/毫秒 = R 字节/秒）。
//   B > T ⇒ 现在；否则 now + ⌊(T − B) / R⌋ + 1。
// "+1"保证严格越过：到那一刻余额 ≥ B + R·⌊(T−B)/R⌋ + R > B + (T−B) = T。
// R == 0 ⇒ 不限，现在就到点。
int64_t RateLimiter::due_ms_locked(RateClass cls, int64_t now) const noexcept {
    const int64_t r = rate_.load(std::memory_order_relaxed);
    if (r == 0) return now;
    const int64_t t = threshold_mb_locked(cls);
    if (balance_mb_ > t) return now;
    return now + (t - balance_mb_) / r + 1;
}

int64_t RateLimiter::next_due_locked(int64_t now) noexcept {
    refresh_locked(now);
    int64_t best = -1;
    for (const auto& s : subs_) {
        if (!s->armed) continue;
        const int64_t d = due_ms_locked(s->cls, now);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

// ── 公开接口 ──────────────────────────────────────────────────────────

void RateLimiter::set_rate(int64_t bytes_per_sec) noexcept {
    const int64_t v = std::clamp<int64_t>(bytes_per_sec, 0, kMaxRate);
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t now = now_ms();
        const int64_t old = rate_.load(std::memory_order_relaxed);
        if (old == 0) {
            // 0 → 非 0：满桶起算。0 → 0：什么都不变。
            if (v != 0) balance_mb_ = v * 1000;
            last_ms_ = now;
        } else {
            // 先按旧速率把余额结算到现在，再换速率。
            refresh_locked(now);
            if (v != 0) balance_mb_ = std::min(balance_mb_, v * 1000);   // 负余额保留
        }
        rate_.store(v, std::memory_order_relaxed);
    }
    if (start_thread_ && v != 0) {
        // 【fail-open】call_once：并发的首次 set_rate 只起一个
        // 线程；std::thread 构造可能抛 std::system_error（系统线程资源耗尽等），
        // 本函数是 noexcept，不接住就是 std::terminate。
        //
        // 接住之后不能当作"限速仍然生效"：唤醒线程起不来，被 admit() 拒绝、
        // 等着 arm() 之后被回调的调用方永远等不到那次回调——等价于"播放永久
        // 卡住"。所以失败时**主动把速率改回 0**：R == 0 时
        // admit 是无锁快路径、恒真，不再依赖任何线程；限速是可选优化，宁可
        // 直接关掉也不能拖着播放一起死。
        //
        // call_once 的可调用体一旦抛出，标准保证 once_flag 不会被置位为"已完成"，
        // 所以下一次 set_rate(非 0) 会重新尝试起线程——这里选**重试**而不是
        // "失败一次就永久禁用限速"：线程创建失败多半是瞬时的资源紧张，没有
        // 理由让它对这个进程的余生都失效。
        bool thread_start_failed = false;
        try {
            std::call_once(thread_once_, [this] {
                // 测试钩子：只消费一次，不影响真实线程创建路径。
                if (force_thread_start_failure_for_test_.exchange(false, std::memory_order_relaxed)) {
                    throw std::runtime_error("forced thread-start failure (test)");
                }
                thread_ = std::thread([this] { thread_main(); });
                // 只有真正把线程跑起来的这一方才置位。
                // std::once_flag 本身不可查询"是否成功过"，判断"该不该
                // fail-open"必须问这个、不是问"这次调用有没有抛"——并发的
                // 另一次 set_rate 可能在同一个 once_flag 上重试并成功。
                std::lock_guard<std::mutex> lk(mu_);
                thread_started_ = true;
            });
        } catch (...) {
            thread_start_failed = true;
        }
        if (thread_start_failed) {
            bool did_fail_open = false;
            {
                // rate_ 的写入纪律：都在 mu_ 下（见成员声明处的注释），这里不能
                // 只用原子 store 图省事。
                std::lock_guard<std::mutex> lk(mu_);
                // 并发的另一次 set_rate 可能已经用同一个
                // call_once 重试成功、线程真的在跑了——这时候线程存在，不该
                // 被这次失败的调用清零，否则就是"线程在，限速却被关掉"。
                if (!thread_started_) {
                    rate_.store(0, std::memory_order_relaxed);
                    did_fail_open = true;
                }
            }
            if (did_fail_open) {
                // log_msg 会同步调用户装的日志回调；回调里
                // 如果再调 syp_rate_limit_set，会在同一个线程上重入 mu_——
                // mu_ 是非递归锁，这是真死锁。必须在上面那个 lock_guard 放锁
                // 之后才调。
                log_msg(SYP_LOG_WARN, "ratelimit",
                        "wake thread failed to start; rate limit disabled (fail-open)");
                // 唤醒线程没起来，之后不会再有任何线程去调
                // fire_due()（调用方只有 thread_main 与 pump_for_test）。刚才
                // 从"rate_ = v"到"rate_ = 0"这两次加锁之间是有窗口的：别的
                // 线程在那个窗口里看到的是旧的非 0 速率，一次 debit 把余额
                // 扣空、随后 admit() 被拒、调用方 arm() 登记等待——如果那正好
                // 是最后一个在途任务的最后一次事件，就再也没有别的触发点能
                // 唤醒它了（"播放永久卡住"）。所以这里必须
                // 在**当前线程上同步**补跑一次 fire_due()：R 已经改回 0，
                // fire_due 的"unlimited"分支会让所有已 armed 的订阅立即到点。
                //
                // fire_due() 不是 noexcept（内部 std::vector::push_back 收集
                // due 列表可能抛 bad_alloc），必须接住——否则又是一次穿透
                // noexcept 边界的 std::terminate。
                //
                // 调用方须知：这次 fire_due() 会在调用 set_rate 的这个线程上
                // 直接跑已 armed 订阅的 wake 回调（例如
                // Scheduler::request_schedule），是同步的，不是经唤醒线程异步
                // 派发——调用方（包括 syp_rate_limit_set）此刻不能持有任何
                // 回调会去取的锁。
                try {
                    fire_due();
                } catch (...) {
                }
            }
        }
    }
    cv_.notify_all();
}

void RateLimiter::force_next_thread_start_failure_for_test() noexcept {
    force_thread_start_failure_for_test_.store(true, std::memory_order_relaxed);
}

int64_t RateLimiter::rate() const noexcept {
    return rate_.load(std::memory_order_relaxed);
}

void RateLimiter::debit(int64_t bytes) noexcept {
    if (bytes <= 0) return;
    if (rate_.load(std::memory_order_relaxed) == 0) return;   // 热路径：一次原子读
    std::lock_guard<std::mutex> lk(mu_);
    if (rate_.load(std::memory_order_relaxed) == 0) return;   // 与 set_rate(0) 竞争
    refresh_locked(now_ms());
    const int64_t b = std::min(bytes, kMaxRate);
    balance_mb_ = std::max(balance_mb_ - b * 1000, kMinBalanceMb);
}

bool RateLimiter::admit(RateClass cls) noexcept {
    if (rate_.load(std::memory_order_relaxed) == 0) return true;
    std::lock_guard<std::mutex> lk(mu_);
    if (rate_.load(std::memory_order_relaxed) == 0) return true;
    refresh_locked(now_ms());
    return balance_mb_ > threshold_mb_locked(cls);
}

int64_t RateLimiter::max_segment_bytes() const noexcept {
    const int64_t r = rate_.load(std::memory_order_relaxed);
    return r == 0 ? INT64_MAX : r;   // 容量 = R × 1 秒
}

// ── 订阅 ──────────────────────────────────────────────────────────────

RateLimiter::Subscription::Subscription(RateLimiter* owner, std::shared_ptr<Sub> sub) noexcept
    : owner_(owner), sub_(std::move(sub)) {}

std::unique_ptr<RateLimiter::Subscription> RateLimiter::subscribe(std::function<void()> wake) {
    auto s  = std::make_shared<Sub>();
    s->wake = std::move(wake);
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs_.push_back(s);
    }
    return std::unique_ptr<Subscription>(new Subscription(this, std::move(s)));
}

void RateLimiter::Subscription::arm(RateClass cls) noexcept {
    {
        std::lock_guard<std::mutex> lk(owner_->mu_);
        sub_->armed = true;
        sub_->cls   = cls;
    }
    owner_->cv_.notify_all();
}

// 析构 = 注销 + 等在途回调返回。逐条对照两种自锁：
//   ① "持着回调要拿的锁析构 ⇒ 回调卡在那把锁上、析构卡在等回调"：这是调用方
//      契约（订阅只在 ~Scheduler 开头、不持任何锁时析构），限速器这边能做的
//      是保证**自己**不参与成环——等待用的是 cb_done_ 在 mu_ 上的 wait，wait
//      期间 mu_ 是放开的；而 fire_due 调 wake 时从不持 mu_，所以回调可以随意
//      调 admit / arm / debit，不会被这里的等待卡住。
//   ② "在自己的回调里析构 ⇒ 自己等自己"：in_cb 的记录里带着回调线程 id，
//      若就是当前线程则**不等**直接返回。这是防御：正确用法不应如此。此时
//      回调尚未返回，但 Sub 由 fire_due 手里的 shared_ptr 保活，回调返回后
//      fire_due 复位 in_cb 不会碰到已释放的内存。
//      这条判据之所以安全，前提是 in_cb / cb_thread **只标在正在执行的那一个
//      订阅上**（见 fire_due）：若整批一起标，同批 A 的回调里析构 B 时 B 也
//      显示"当前线程在回调它"⇒ 不等、直接返回 ⇒ 随后 fire_due 照样调
//      B->wake()，悬挂的 this。
//   注销一律置 dead：已被 fire_due 挑成候选、但还没轮到的订阅，轮到时见
//   dead 就跳过——注销之后绝不会再被回调。
RateLimiter::Subscription::~Subscription() {
    std::unique_lock<std::mutex> lk(owner_->mu_);
    auto& v = owner_->subs_;
    v.erase(std::remove(v.begin(), v.end(), sub_), v.end());
    sub_->armed = false;
    sub_->dead  = true;
    if (sub_->in_cb && sub_->cb_thread != std::this_thread::get_id()) {
        owner_->cb_done_.wait(lk, [this] { return !sub_->in_cb; });
    }
}

// 派发：锁内挑出到点的 armed 订阅作为候选并**先解除 arm**（一次 arm 至多一次
// 回调）；随后**逐个**处理，每个候选：
//   持锁 → 已注销（dead）就跳过；否则只给**这一个**置 in_cb / cb_thread → 放锁
//   → 调 wake（吞异常）→ 持锁复位 in_cb → notify cb_done_。
// 为什么逐个而不是整批标记 in_cb：见 ~Subscription 注释 ② 的后半段（同批互相
// 析构会落到 UAF）。为什么 notify 在每个之后：正在等这一个的析构方应当尽早放行。
void RateLimiter::fire_due() {
    std::vector<std::shared_ptr<Sub>> due;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t now = now_ms();
        refresh_locked(now);
        const bool unlimited = rate_.load(std::memory_order_relaxed) == 0;
        for (const auto& s : subs_) {
            if (!s->armed) continue;
            if (!unlimited && balance_mb_ <= threshold_mb_locked(s->cls)) continue;
            s->armed = false;
            due.push_back(s);
        }
    }
    const auto self = std::this_thread::get_id();
    for (const auto& s : due) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (s->dead) continue;           // 候选之后被注销：绝不再回调
            s->in_cb     = true;
            s->cb_thread = self;
        }
        bool threw = false;
        try {
            s->wake();                       // mu_ 之外
        } catch (...) {
            // 吞掉：丢这一次唤醒，下一次 arm 或数据到达会再调度。必须吞——
            // 否则 in_cb 不复位（析构永久挂起）、同批其余订阅收不到回调，
            // 在唤醒线程里更是 std::terminate。
            threw = true;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            s->in_cb = false;
        }
        cb_done_.notify_all();
        if (threw) log_msg(SYP_LOG_WARN, "ratelimit", "wake callback threw; one wakeup dropped");
    }
}

// 睡到最早一个 armed 订阅的到点时刻。debit 不 notify：睡眠期间的扣账把
// 阈值推后时，醒来重算即可（只会早醒、不会漏醒）；arm / set_rate / 析构
// 都 notify，令其立即重算。所有判定都在持 mu_ 时做，notify 不会丢。
void RateLimiter::thread_main() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        const int64_t now = now_ms();
        const int64_t due = next_due_locked(now);
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

// ── 测试钩子 ──────────────────────────────────────────────────────────

void RateLimiter::pump_for_test() {
    fire_due();
}

int64_t RateLimiter::balance_for_test() {
    std::lock_guard<std::mutex> lk(mu_);
    refresh_locked(now_ms());
    // 向下取整（C++ 的整数除法向零取整，负数要修一下）。
    int64_t q = balance_mb_ / 1000;
    if (balance_mb_ % 1000 < 0) --q;
    return q;
}

int64_t RateLimiter::next_due_ms_for_test() {
    std::lock_guard<std::mutex> lk(mu_);
    return next_due_locked(now_ms());
}

}  // namespace syp::dl
