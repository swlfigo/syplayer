// scheduler.cpp — 洞切分 / 并发 / 连接复用启发式；回调一律在锁外发出
#include "scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>

namespace syp::dl {
namespace {

constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();

// 自动切分的粒度下限，不是「发出去的请求至少 256 字节」。
//
// min_segment_size 配成 1 时，ceil(remaining/conc) 会按约 2/3 几何衰减，
// 最后 10 KB 打出一串 1 字节请求。HTTP 头已经几百字节，再切更小净负收益。
// 本下限刹住的是这条衰减：remaining（所有洞的总和）还大于 256 时，不再把
// 切分粒度压到 256 以下。256 小于现有用例里最小的预期分片（300 字节：
// N=900 / conc=3），不会改那些精确区间断言；默认 512 KiB 碰不到这条。
//
// 管不到的一路：实际请求还要裁到 holes.front() 的大小。seek 取消任务会把
// 缓存打成碎片，留下 <256B 甚至 1B 的单个洞；填这种洞只能发等大的请求
// （多取会违反「绝不重下已有数据」）。无碎片时最小请求远大于 256。
constexpr int64_t kNoSplitBelow = 256;

int64_t sat_add(int64_t a, int64_t b) noexcept {
    if (b > 0 && a > kI64Max - b) return kI64Max;
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
        return std::numeric_limits<int64_t>::min();
    }
    return a + b;
}

int32_t clamp_pos_i32(int32_t v) noexcept {
    return v <= 0 ? 1 : v;
}

bool is_terminal_state(DLTaskState s) noexcept {
    return s == DLTaskState::Done || s == DLTaskState::Failed
        || s == DLTaskState::Canceled;
}

// 超时配置 ≤ 0（例如调用方没填、"用后端默认"）时按 DLTaskConfig 默认值算，
// 不让 0 把挂死阈值压成"只剩宽限"而秒杀正常任务。
int64_t effective_timeout(int32_t v, int32_t dflt) noexcept {
    return v > 0 ? static_cast<int64_t>(v) : static_cast<int64_t>(dflt);
}

// 当前线程上 health_check() 的嵌套深度（>0 ⇒ 在 HealthTicker 回调路径
// 上）。按线程而不是按 Scheduler 记：health_check 发出的用户回调可能重入
// **另一个** Scheduler 的公开方法，那一个的 reap 同样不能在 ticker 线程上跑。
thread_local int tl_health_depth = 0;

struct HealthDepthScope {
    HealthDepthScope() noexcept { ++tl_health_depth; }
    ~HealthDepthScope() { --tl_health_depth; }
    HealthDepthScope(const HealthDepthScope&)            = delete;
    HealthDepthScope& operator=(const HealthDepthScope&) = delete;
};

// 判慢：speed × ratio < baseline，不溢出。
// speed_bps() 在极端输入下会饱和到 INT64_MAX（bytes_per_sec 的 q > kMax/1000
// 分支），不能假定乘积放得下，所以不写裸乘法。守卫**不改判定边界**：ratio ≥ 2
// （构造期夹过）、baseline ≥ 0；speed > ⌊INT64_MAX / ratio⌋ 时 speed ≥
// ⌊INT64_MAX / ratio⌋ + 1 ⇒ speed × ratio > INT64_MAX ≥ baseline，数学上本来
// 就"不慢"。其余情况乘积 ≤ INT64_MAX，按原式整数比较。选乘法式而不是
// speed < baseline / ratio 的除法式：原式等价于 speed < ⌈baseline / ratio⌉，
// 除法式截断成 ⌊⌋，baseline 不整除时恰好少判 speed = ⌊baseline / ratio⌋ 这一个
// 值，与原始公式不等价；用例的手算按本式。
bool slower_than_baseline(int64_t speed, int32_t ratio, int64_t baseline) noexcept {
    const int64_t r = static_cast<int64_t>(ratio);
    if (r <= 0) return false;
    if (speed > kI64Max / r) return false;
    return speed * r < baseline;
}

int64_t ceil_div_nonneg(int64_t a, int32_t b) noexcept {
    if (b <= 0) return a;
    const int64_t d = static_cast<int64_t>(b);
    return a / d + ((a % d) != 0 ? 1 : 0);
}

}  // namespace

void Scheduler::HeaderCopy::rebuild_ptrs() {
    name_c.clear();
    value_c.clear();
    name_c.reserve(names.size());
    value_c.reserve(values.size());
    for (size_t i = 0; i < names.size(); ++i) {
        name_c.push_back(names[i].c_str());
        value_c.push_back(values[i].c_str());
    }
}

syp_headers Scheduler::HeaderCopy::view() const noexcept {
    syp_headers h{};
    h.names  = name_c.empty() ? nullptr : name_c.data();
    h.values = value_c.empty() ? nullptr : value_c.data();
    h.count  = static_cast<int32_t>(names.size());
    return h;
}

void Scheduler::HeaderCopy::assign(const syp_headers* h) {
    names.clear();
    values.clear();
    name_c.clear();
    value_c.clear();
    if (h == nullptr || h->count <= 0 || h->names == nullptr || h->values == nullptr) {
        return;
    }
    names.reserve(static_cast<size_t>(h->count));
    values.reserve(static_cast<size_t>(h->count));
    for (int32_t i = 0; i < h->count; ++i) {
        names.emplace_back(h->names[i] ? h->names[i] : "");
        values.emplace_back(h->values[i] ? h->values[i] : "");
    }
    rebuild_ptrs();
}

Scheduler::Scheduler(const syp_http_backend* backend, Clock clock,
                     SchedulerConfig cfg, SchedulerCallbacks cb)
    : backend_(backend), clock_(clock), cfg_(cfg), cb_(cb),
      limiter_(cfg.limiter != nullptr ? cfg.limiter : &RateLimiter::instance()),
      rate_class_(cfg.rate_class) {
    if (cfg_.max_concurrent_tasks < 0) cfg_.max_concurrent_tasks = 0;
    if (cfg_.min_segment_size < 0) cfg_.min_segment_size = 0;
    if (cfg_.segment_size_hint < 0) cfg_.segment_size_hint = 0;
    if (cfg_.reuse_max_remaining_bytes < 0) cfg_.reuse_max_remaining_bytes = 0;
    if (cfg_.connect_estimate_max_ms < 0) cfg_.connect_estimate_max_ms = 0;
    if (cfg_.max_consecutive_errors < 0) cfg_.max_consecutive_errors = 0;
    // 订阅一次，之后只 arm。回调在限速器锁外、可能在唤醒线程上执行；
    // 此刻 started_ 为假，提前到来的唤醒会在 request_schedule() 里早退。
    auto sub = limiter_->subscribe([this] { request_schedule(); });

    // 健康检查订阅，与限速订阅同一写法。回调在 ticker 线程上；
    // 此刻 started_ 为假，health_check 会早退。
    ticker_ = cfg_.health.ticker != nullptr ? cfg_.health.ticker : &HealthTicker::instance();
    if (cfg_.health.check_interval_ms <= 0) cfg_.health.check_interval_ms = 1000;
    if (cfg_.health.stall_grace_ms < 0) cfg_.health.stall_grace_ms = 0;
    if (cfg_.health.slow_ratio < 2) cfg_.health.slow_ratio = 2;
    if (cfg_.health.slow_strikes < 1) cfg_.health.slow_strikes = 1;
    // 不持任何锁时把 ticker 的懒启动提前做完：之后
    // schedule()/health_check() 持 mu_ 调的 arm_after 只剩"线程已在跑 / 已
    // disabled"的快路径，不会在持锁时走到线程创建失败分支里的 log_msg
    // （用户日志回调重入下载层公开接口会自锁 mu_）。检测关闭时不碰 ticker：
    // 不为一个永远不 arm 的订阅去起进程级线程。
    std::unique_ptr<HealthTicker::Subscription> hsub;
    if (cfg_.health.enabled) {
        ticker_->warm_up();
        hsub = ticker_->subscribe([this] { health_check(); });
    }

    std::lock_guard<std::mutex> g(mu_);
    sub_ = std::move(sub);
    health_sub_ = std::move(hsub);
}

bool Scheduler::dtor_idle_locked() const noexcept {
    if (in_schedule_ > 0) return false;
    for (const auto& s : slots_) {
        if (s && s->in_cb > 0) return false;
    }
    // 已被摘出 slots_、停在 deferred_release_ 里的 Slot 仍可能收到迟到的
    // on_finished（SlotCbGuard 照样记 in_cb），析构同样要等它离开回调栈。
    for (const auto& s : deferred_release_) {
        if (s && s->in_cb > 0) return false;
    }
    return true;
}

Scheduler::~Scheduler() {
    // 第一件事、**析构时不持任何锁**——注销限速订阅（避免下面两种自锁）。
    // ~Subscription 会等一次正在执行的唤醒回调返回，而回调在
    // request_schedule() → schedule() 里要拿 mu_、还可能经 on_idle 进上层
    // 的锁；持任何一把都可能死锁。
    // 指针在 mu_ 下移出、放锁后才析构：别的线程上的后端回调可能正走到
    // schedule() 的 sub_->arm()（它在 mu_ 下判空），裸 reset 会与之数据竞争。
    // 移出之后 schedule() 见空不再 arm；之后也不再有唤醒进来。
    // 健康检查订阅同理（回调 health_check 同样要拿 mu_、还可能经
    // schedule() 发用户回调），与限速订阅一起在 mu_ 下移出、放锁后析构，
    // 都先于下面的 stop / cancel。移出之后 schedule()/health_check() 见空
    // 不再 arm。
    std::unique_ptr<RateLimiter::Subscription>  dying_sub;
    std::unique_ptr<HealthTicker::Subscription> dying_health;
    {
        std::lock_guard<std::mutex> g(mu_);
        dying_sub    = std::move(sub_);
        dying_health = std::move(health_sub_);
    }
    dying_health.reset();
    dying_sub.reset();
    {
        std::lock_guard<std::mutex> g(mu_);
        stopped_ = true;
        paused_  = true;
        fatal_   = true;
    }
    // 锁外 cancel + 拆 Slot：DLTask 析构会等自己的回调结束；若此时持着 mu_，
    // 回调里再锁 mu_ 就死锁。
    cancel_all_tasks();
    std::vector<std::shared_ptr<Slot>> dying;
    {
        std::unique_lock<std::mutex> lk(mu_);
        // 与 ~DLTask 相同：wait_for 超时只打诊断，继续等。用户 on_data 在
        // 另一线程阻塞时，yield 忙等会占满一核；cv 让析构线程睡着。
        // 误用（在用户回调里析构本对象）会卡在这里周期性打日志，而不是
        // 满载空转——仍然不要这么做。
        while (!cv_.wait_for(lk, std::chrono::seconds(2),
                             [this] { return dtor_idle_locked(); })) {
            int ncb = 0;
            for (const auto& s : slots_) {
                if (s) ncb += s->in_cb;
            }
            for (const auto& s : deferred_release_) {
                if (s) ncb += s->in_cb;
            }
            std::fprintf(stderr,
                "syp_scheduler: dtor still waiting in_schedule=%d in_cb_sum=%d "
                "slots=%zu\n",
                in_schedule_, ncb, slots_.size());
        }
        dying.swap(slots_);
        for (auto& d : deferred_release_) dying.push_back(std::move(d));
        deferred_release_.clear();
    }
    dying.clear();
}

void Scheduler::start(std::string url, const syp_headers* headers,
                      int64_t total_length, const HoleSet& already_cached) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (started_ || stopped_) return;
        started_ = true;
        url_ = std::move(url);
        extra_headers_.assign(headers);
        total_length_ = total_length;
        cached_ = already_cached;
        received_ = already_cached;  // 启动时磁盘上已有的，不必再交付
        idle_armed_ = true;  // 启动时目标已齐也要通知一次空闲
    }
    schedule();
    reap_except();
}

void Scheduler::set_read_position(int64_t pos) {
    bool do_sched = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_) return;
        if (pos < 0) pos = 0;
        if (read_pos_ == pos) return;
        read_pos_ = pos;
        do_sched = started_;
    }
    if (do_sched) schedule();
    reap_except();
}

void Scheduler::set_target_end(int64_t end) {
    bool do_sched = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_) return;
        if (end < 0) end = -1;
        if (target_end_ == end) return;
        target_end_ = end;
        do_sched = started_;
    }
    if (do_sched) schedule();
    reap_except();
}

void Scheduler::notify_persisted(Range r) {
    bool do_sched = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_) return;
        cached_.add(r);
        received_.add(r);
        do_sched = started_;
    }
    if (do_sched) schedule();
    reap_except();
}

void Scheduler::pause() {
    std::vector<std::shared_ptr<Slot>> to_cancel;
    SlotRelease rel{this, to_cancel};   // 用户回调可能在 health 路径上调 pause
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_) return;
        paused_ = true;
        for (auto& s : slots_) {
            if (!s || s->dead || !s->task) continue;
            s->dead = true;
            to_cancel.push_back(s);
        }
    }
    for (auto& s : to_cancel) s->task->cancel();
    reap_except();
}

void Scheduler::resume() {
    bool do_sched = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_ || fatal_) return;
        if (!paused_) return;
        paused_ = false;
        idle_armed_ = true;
        do_sched = started_;
    }
    if (do_sched) schedule();
    reap_except();
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> g(mu_);
        stopped_ = true;
        paused_  = true;
    }
    cancel_all_tasks();
    reap_except();
}

int32_t Scheduler::active_task_count() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return live_count_locked();
}

HoleSet Scheduler::cached_ranges() const {
    std::lock_guard<std::mutex> g(mu_);
    return cached_;
}

HoleSet Scheduler::inflight_ranges() const {
    std::lock_guard<std::mutex> g(mu_);
    HoleSet out;
    for (const auto& s : slots_) {
        if (!s || s->dead || !s->task) continue;
        const auto st = s->task->state();
        if (is_terminal_state(st)) continue;
        int64_t from = s->wanted.start;
        if (s->started) {
            from = s->task->next_offset();
            if (from < s->wanted.start) from = s->wanted.start;
        }
        int64_t to = s->wanted.end;
        if (total_length_ >= 0 && to > total_length_) to = total_length_;
        if (from < to) out.add(Range{from, to});
    }
    return out;
}

int32_t Scheduler::consecutive_errors() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return consecutive_errors_;
}

int32_t Scheduler::stall_kills() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return stall_kills_;
}

int32_t Scheduler::slow_kills() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return slow_kills_;
}

int64_t Scheduler::baseline_bps_for_test() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return baseline_bps_;
}

void Scheduler::arm_health_locked() noexcept {
    // 有在途任务才登记。被限速卡住时没有在途任务 ⇒ 不 arm ⇒
    // 等待时长不计入任何判定。health_sub_ 为空 = 检测关闭或析构
    // 已开始。已 armed 不重复登记：arm_after 本身取较早者，重复调用结果
    // 一样，只是每次 schedule() 都去拿 ticker 的锁、notify 线程没有必要。
    if (!cfg_.health.enabled || !health_sub_ || health_armed_) return;
    if (stopped_ || fatal_ || paused_ || !started_) return;
    if (live_count_locked() <= 0) return;
    health_armed_ = true;
    health_sub_->arm_after(cfg_.health.check_interval_ms);
}

void Scheduler::health_check() {
    HealthDepthScope depth;              // 先于 to_cancel 构造 ⇒ 晚于它的释放析构
    std::vector<std::shared_ptr<Slot>> to_cancel;
    SlotRelease rel{this, to_cancel};
    PendingEmit e;
    bool do_sched = false;
    bool do_cancel_all = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        // 这次 arm 已经兑现（ticker 回调前自动解除 arm）。先清，下面任何
        // 早退都不 re-arm；之后 resume()/新任务的 schedule() 尾检会重新登记。
        health_armed_ = false;
        if (stopped_ || fatal_ || paused_ || !started_ || !cfg_.health.enabled) return;
        // 时钟只在真有候选时读：没有在途任务时（刚下完、被限速卡住）不去碰
        // 注入的时钟——测试里的假时钟可能正被另一线程改写。
        bool    have_now = false;
        int64_t now = 0;
        // 冷却按**本轮开始时**判定（与 now 同时取）：本轮替换设的冷却从下一轮起
        // 生效。否则同一轮里排在被替换者后面的 Slot 会因"刚进冷却"被清掉
        // strike、还把慢样本喂进基线——与"每轮至多替换一个只挡替换、不挡累计
        // strike"和"吃过 strike 的样本不进基线"都相悖。
        bool    cooling = false;
        // 每轮至多替换一个慢任务。只挡替换，不挡 strike 累计。
        bool    replaced_this_round = false;
        // 记一次超时失败（持 mu_）；达到 max_consecutive_errors ⇒ 置 fatal_ 并备好
        // on_error(SYP_ERR_TIMEOUT, 0)，返回 true 让调用处 break，随后走下面
        // do_cancel_all 的 cancel_all_tasks()。挂死与"零速慢替换"共用。
        auto count_timeout_error = [&]() -> bool {
            if (consecutive_errors_ < std::numeric_limits<int32_t>::max()) {
                ++consecutive_errors_;
            }
            if (cfg_.max_consecutive_errors > 0
                && consecutive_errors_ >= cfg_.max_consecutive_errors) {
                fatal_ = true;
                e.error = true;
                e.err_st = SYP_ERR_TIMEOUT;
                e.err_http = 0;
                return true;
            }
            return false;
        };
        for (auto& s : slots_) {
            // !started：schedule() 已建槽、还没在锁外 start()，这次尝试尚未
            // 开始，不存在"卡了多久"。
            if (!s || s->dead || !s->task || !s->started) continue;
            const DLTaskState st = s->task->state();
            if (st != DLTaskState::Connecting && st != DLTaskState::Receiving) continue;
            // ── 挂死 ──
            // ref = max(本次尝试起点, 最近一次交付字节)：重试会刷新起点；
            // 收数据阶段按最近进展算，不按起点（否则长下载会被误杀）。
            const int64_t started_at = s->task->attempt_started_ms();
            const int64_t progress   = s->task->last_progress_ms();
            if (started_at < 0) continue;
            if (!have_now) {
                now = clock_.now_ms(clock_.ctx);
                have_now = true;
                cooling = now < slow_cooldown_until_ms_;
            }
            const int64_t ref = progress > started_at ? progress : started_at;
            // 有效读超时：挂死阈值（Receiving）与"零速慢替换是否计错"共用这一把尺子。
            const int64_t read_to = effective_timeout(cfg_.task.read_timeout_ms, 15000);
            // 连接超时 ≤ 0 时先借读超时、再退 10000——与 Apple 后端一致
            // （apple_http_backend.mm：connect_timeout_ms==0 时连接阶段借用
            // read_timeout_ms）。否则 connect=0、read=30000 的配置
            // 会在后端自己还在等的 12 秒处被调度器抢先杀掉。
            const int32_t connect_dflt =
                cfg_.task.read_timeout_ms > 0 ? cfg_.task.read_timeout_ms : 10000;
            const int64_t timeout = (st == DLTaskState::Connecting)
                ? effective_timeout(cfg_.task.connect_timeout_ms, connect_dflt)
                : read_to;
            const int64_t limit = timeout + static_cast<int64_t>(cfg_.health.stall_grace_ms);
            if (now - ref >= limit) {
                s->dead = true;
                s->killed_by_health = true;
                to_cancel.push_back(s);
                if (stall_kills_ < std::numeric_limits<int32_t>::max()) ++stall_kills_;
                // 挂死等价于一次超时失败。被杀的任务稍后
                // on_task_finished(CANCELED) 走 canceled 分支，不会再计一次。
                if (count_timeout_error()) break;
                continue;
            }
            // ── 慢 ──
            // 下面三条 continue 是"这次量不出来"，strike 原样保留，不算"不慢"。
            if (st != DLTaskState::Receiving) continue;
            if (now - started_at < static_cast<int64_t>(cfg_.task.speed_window_ms)) continue;
            const std::optional<int64_t> speed = s->task->speed_bps();
            if (!speed.has_value()) continue;
            // 不支持 Range 时换连接要从 0 重下整份文件；基线为 0 = 还
            // 没有参照；冷却期内不判。三者任一 ⇒ 本次"不慢"。
            const bool may_judge = range_state_ != RangeState::Unsupported
                && baseline_bps_ > 0 && !cooling;
            bool slow_now = false;
            if (may_judge
                && slower_than_baseline(*speed, cfg_.health.slow_ratio, baseline_bps_)) {
                // 快下完的不值得换：按当前速度的剩余时间要超过建连估计上限，
                // 与 should_wait_locked 同一把尺子（speed == 0 视为无穷）。
                const int64_t remaining = slot_remaining_locked(*s);
                slow_now = (*speed == 0)
                    || (remaining > kI64Max / 1000)
                    || ((1000 * remaining) / *speed
                        > static_cast<int64_t>(cfg_.connect_estimate_max_ms));
            }
            if (slow_now) {
                if (s->slow_strikes < std::numeric_limits<int32_t>::max()) ++s->slow_strikes;
            } else {
                s->slow_strikes = 0;           // 连续才算
            }
            if (s->slow_strikes >= cfg_.health.slow_strikes && !replaced_this_round) {
                s->dead = true;
                s->killed_by_health = true;
                to_cancel.push_back(s);
                if (slow_kills_ < std::numeric_limits<int32_t>::max()) ++slow_kills_;
                baseline_bps_ /= 2;            // 乘法衰减
                slow_cooldown_until_ms_ =
                    sat_add(now, static_cast<int64_t>(cfg_.task.speed_window_ms));
                replaced_this_round = true;
                // 慢替换默认不计 consecutive_errors_：换一条更好的连接。
                // 零速（整整一个窗口零字节）照样走慢路径**快速替换**，但只有静默
                // 已达有效读超时（now − ref ≥ read_to，与挂死同一定义、不含宽限）
                // 才按超时计错，而不是零速一律计错：一律
                // 计错时，生产默认（SourceBridge 把 max_consecutive_errors 设成
                // max_retries = 3、并发 3、窗口 3000）下"连着但不走数据"三条任务
                // 每隔约 4 秒各被换一次、各计一次 ⇒ 约 12 秒 fatal，无视用户的
                // read_timeout_ms；HLS 里 fatal 的分片源还会被静默跳过。
                // 有界性不丢：替换任务若一直收不到字节，要么在 Connecting 挂死
                // （计错），要么基线减半到 0 之后停止判慢、轮到 Receiving 挂死（计错）。
                // 被杀的任务稍后 on_task_finished 只重调度，不会再计一次（killed_by_health）。
                if (*speed == 0 && now - ref >= read_to && count_timeout_error()) break;
                continue;
            }
            // 健康样本进基线：EWMA α = 1/8。吃过 strike 的（含本轮
            // 因"已替换过一个"而留着的）不进，免得慢任务把基线拉低、自己合法化。
            // speed、baseline 都在 [0, INT64_MAX]，差与和都不溢出。
            if (*speed > 0 && s->slow_strikes == 0) {
                baseline_bps_ = baseline_bps_ == 0
                    ? *speed
                    : baseline_bps_ + (*speed - baseline_bps_) / 8;
            }
        }
        do_cancel_all = fatal_;
        do_sched = !fatal_ && !to_cancel.empty();
        // 没杀人：按"有在途"原样再 arm。杀了人：交给下面那轮 schedule()
        // 的尾检（补发的新任务在途 ⇒ 由它 arm）。
        if (!do_cancel_all && !do_sched) arm_health_locked();
    }
    // 锁外 cancel：同步桩上 cancel 会当场回 on_finished，再拿 mu_。
    for (auto& s : to_cancel) s->task->cancel();
    emit(e);
    if (do_cancel_all) {
        // 与 on_task_finished 的 fatal 路径同构。
        cancel_all_tasks();
    } else if (do_sched) {
        // 空出来的区间在 occupied_locked() 里已不算占用（dead），本轮就会
        // 补发，从 next_offset 续、不重下已收字节。尾检里会重新 arm。
        schedule();
    }
    // 不 reap，也不在这里放掉最后一个 Slot 引用（刻意的）：本函数跑在全进程
    // 唯一的 ticker 线程上，而对 cancel 也不回调的违约后端（非目标场景）
    // 会让 ~DLTask 永久等终态——卡住的是 ticker 线程，所有 Scheduler 的检测
    // 一起失效。所以：
    //   · reap_except 在 health 路径上直接早退（含用户回调重入的公开方法）；
    //   · 本函数、schedule()、cancel_all_tasks()、pause() 锁外持有的 Slot
    //     引用经 SlotRelease 转进 deferred_release_——另一线程的 reap 可能已
    //     把 Slot 从 slots_ 摘掉，那时这些局部引用就是最后的持有者。
    // 死 Slot 与 deferred_release_ 留给下一次非 health 路径上的公开方法退出
    // 时 reap（与既有路径一致），或 ~Scheduler。
}

bool Scheduler::on_health_path() noexcept { return tl_health_depth > 0; }

void Scheduler::release_slots(std::vector<std::shared_ptr<Slot>>& v) noexcept {
    if (v.empty()) return;
    if (on_health_path()) {
        // schedule() 里的两个 SlotRelease 在 guard.release_locked()
        // 之后才析构，这里再拿 mu_ 与尾检那条"交还 in_schedule_ 之后不得再
        // 碰成员（~Scheduler 空隙里 UAF）"的警告看似冲突，但在 health 路径上
        // 是安全的：~Scheduler 第一步 dying_health.reset() 会等正在跑的
        // health_check 返回——本调用链整段都在那次回调里，析构方此时还没走到
        // 拆成员那一步；而非 health 路径根本不拿锁（下面 else 分支就地 clear）。
        // 公开方法（含 request_schedule）与析构并发本来就是契约禁止的。
        //
        // deferred_release_ 里不全是死 Slot——schedule() 的 to_start
        // 转进来的是**活的**新任务的引用（它同时还在 slots_ 里）。reap_except
        // 清空 deferred_release_ 时对它们只是引用计数减一，不会析构；真正的
        // 最后持有者仍是 slots_，等它 dead 之后才按常规路径拆。
        try {
            std::lock_guard<std::mutex> g(mu_);
            deferred_release_.reserve(deferred_release_.size() + v.size());
            for (auto& s : v) deferred_release_.push_back(std::move(s));
        } catch (...) {
            // 分配失败：退回就地释放（最坏情况即修复前的行为）。
        }
    }
    v.clear();
}

int32_t Scheduler::deferred_release_count_for_test() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return static_cast<int32_t>(deferred_release_.size());
}

bool Scheduler::range_supported() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    // 语义与三态化之前一致：只有「确认不支持」才返回 false。Unknown
    // （尚未探明）返回 true——它对应旧实现的初值 true。
    return range_state_ != RangeState::Unsupported;
}

void Scheduler::cb_on_data(void* ctx, int64_t offset, const uint8_t* data, int32_t len) {
    auto* slot = static_cast<Slot*>(ctx);
    if (slot == nullptr || slot->owner == nullptr) return;
    slot->owner->on_task_data(slot, offset, data, len);
}

void Scheduler::cb_on_total(void* ctx, int64_t total) {
    auto* slot = static_cast<Slot*>(ctx);
    if (slot == nullptr || slot->owner == nullptr) return;
    slot->owner->on_task_total(slot, total);
}

void Scheduler::cb_on_validators(void* ctx, const char* etag, const char* last_modified) {
    auto* slot = static_cast<Slot*>(ctx);
    if (slot == nullptr || slot->owner == nullptr) return;
    slot->owner->on_task_validators(slot, etag, last_modified);
}

void Scheduler::cb_on_finished(void* ctx, syp_status st, int32_t http_status) {
    auto* slot = static_cast<Slot*>(ctx);
    if (slot == nullptr || slot->owner == nullptr) return;
    slot->owner->on_task_finished(slot, st, http_status);
}

void Scheduler::on_task_data(Slot* slot, int64_t offset, const uint8_t* data, int32_t len) {
    if (data == nullptr || len <= 0) return;
    const int64_t n64 = static_cast<int64_t>(len);
    // 记账：网络上到了就算，不论下面是否去重丢弃、是否已 stop。
    // debit 只取限速器叶子锁、不阻塞、不回调。
    limiter_->debit(n64);
    if (offset < 0 || n64 > kI64Max - offset) return;
    const int64_t end = offset + n64;

    SlotCbGuard cb_guard(slot);

    std::vector<Range> deliver;
    SchedulerCallbacks local{};
    bool drop = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        drop = stopped_;
        if (!drop) {
            HoleSet have = cached_;
            for (const Range& r : received_.ranges()) have.add(r);
            deliver = have.holes_in(Range{offset, end});
            received_.add(Range{offset, end});
            local = cb_;
        }
    }

    if (!drop && local.on_data != nullptr) {
        for (const Range& d : deliver) {
            const int64_t skip = d.start - offset;
            const int64_t nbytes = d.size();
            if (skip < 0 || nbytes <= 0 || skip > n64) continue;
            if (nbytes > n64 - skip) continue;
            local.on_data(local.ctx, d.start, data + static_cast<size_t>(skip),
                          static_cast<int32_t>(nbytes));
        }
    }
}

void Scheduler::on_task_total(Slot* slot, int64_t total) {
    SlotCbGuard cb_guard(slot);
    PendingEmit e;
    bool do_sched = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        // 先翻 Range 状态再看总长：这两件事都由同一个 on_response 触发，
        // 翻在前面，下面那次 schedule() 就能直接按放开后的并发建槽。
        // 注意不能只靠下面 total 那段带 schedule——start() 已经给过总长时
        // （重启续下）那段是不触发的，并发会永远卡在 1。
        if (!stopped_ && refresh_range_state_locked()) {
            do_sched = started_ && !paused_ && !fatal_;
        }
        if (!stopped_ && !total_notified_ && total >= 0) {
            total_notified_ = true;
            if (total_length_ < 0) {
                total_length_ = total;
                do_sched = do_sched || (started_ && !paused_ && !fatal_);
            } else if (total_length_ != total) {
                // 与已有总长不一致：当源文件变化。本步不实现 etag 校验，
                // 只在尚未得知总长时采纳。已有值保持不变。
            }
            e.total = true;
            e.total_value = total_length_ >= 0 ? total_length_ : total;
        }
    }
    emit(e);
    if (do_sched) schedule();
}

void Scheduler::on_task_validators(Slot* slot, const char* etag, const char* last_modified) {
    SlotCbGuard cb_guard(slot);
    PendingEmit e;
    bool do_sched = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        // 206 缺 Content-Range、后端也没给出 total 时 on_total_length 不会
        // 被调用（notify_total 为假），这条是那种响应上唯一还会到达调度器
        // 的 on_response 回声——Range 确认得在这里也翻一次，否则并发会
        // 一直卡在 1。
        if (!stopped_ && refresh_range_state_locked()) {
            do_sched = started_ && !paused_ && !fatal_;
        }
        if (!stopped_ && !validators_notified_) {
            validators_notified_ = true;
            e.validators = true;
            e.etag = etag ? etag : "";
            e.last_modified = last_modified ? last_modified : "";
        }
    }
    emit(e);
    if (do_sched) schedule();
}

void Scheduler::on_task_finished(Slot* slot, syp_status st, int32_t http_status) {
    SlotCbGuard cb_guard(slot);

    PendingEmit e;
    bool do_sched = false;
    bool do_fallback = false;
    bool do_cancel_all = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        slot->dead = true;

        // health_check 杀掉的任务，杀的时候已经计过（或按规则
        // 不计）一次；它在锁外 cancel 生效之前以别的失败码结束（竞态）时，
        // 那次失败与"被杀"是同一件事，按 CANCELED 处理、不再计错。
        const bool canceled = (st == SYP_ERR_CANCELED)
            || (slot->killed_by_health && st != SYP_OK);
        const bool success  = (st == SYP_OK);

        if (stopped_ || fatal_) {
            // stop() 之后不再向用户回调，也不再调度。
        } else if (canceled) {
            do_sched = !paused_;
        } else {
            const bool no_range_signal =
                (st == SYP_ERR_RANGE_UNSUPPORTED)
                || (http_status == 200 && !is_whole_file_locked(slot->wanted));

            if (no_range_signal) {
                if (cfg_.allow_no_range_fallback) {
                    if (range_state_ != RangeState::Unsupported) {
                        range_state_ = RangeState::Unsupported;
                        do_fallback = true;
                    }
                    consecutive_errors_ = 0;
                    do_sched = !paused_;
                } else {
                    fatal_ = true;
                    e.error = true;
                    e.err_st = SYP_ERR_RANGE_UNSUPPORTED;
                    e.err_http = 0;
                }
            } else if (success) {
                consecutive_errors_ = 0;
                do_sched = !paused_;
            } else {
                if (consecutive_errors_ < std::numeric_limits<int32_t>::max()) {
                    ++consecutive_errors_;
                }
                if (cfg_.max_consecutive_errors > 0
                    && consecutive_errors_ >= cfg_.max_consecutive_errors) {
                    fatal_ = true;
                    e.error = true;
                    e.err_st = st;
                    e.err_http = http_status;
                } else {
                    do_sched = !paused_;
                }
            }
        }
        do_cancel_all = fatal_;
    }

    emit(e);

    if (do_fallback) {
        enter_no_range_fallback();
    } else if (do_cancel_all) {
        cancel_all_tasks();
    } else if (do_sched) {
        schedule();
    }
    // 不在这里 reap：DLTask::finish 在 on_finished 返回后还要碰 this，
    // 嵌套回调里 reap 别的刚 finish 的任务同样会 UAF。死 Slot 留到
    // 公开方法退出（所有 in_cb==0）再拆。
}

bool Scheduler::refresh_range_state_locked() noexcept {
    if (range_state_ != RangeState::Unknown) return false;
    for (const auto& s : slots_) {
        if (!s || !s->task) continue;
        if (!s->task->range_confirmed()) continue;
        range_state_ = RangeState::Supported;
        return true;
    }
    return false;
}

void Scheduler::enter_no_range_fallback() {
    // 不支持 Range：并发锁成 1，丢掉所有非「从 0 拉到尾」的任务，再开一条
    // [0, eof)。已交付的前缀由 on_task_data 用 received_ 去重，避免 200 整包
    // 再走一遍时重复回调。
    cancel_all_tasks();
    schedule();
}

void Scheduler::cancel_all_tasks() {
    std::vector<std::shared_ptr<Slot>> to_cancel;
    SlotRelease rel{this, to_cancel};   // fatal 路径可能在 health 路径上
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& s : slots_) {
            if (!s || !s->task) continue;
            s->dead = true;
            to_cancel.push_back(s);
        }
    }
    for (auto& s : to_cancel) s->task->cancel();
}

void Scheduler::emit(const PendingEmit& e) {
    SchedulerCallbacks local;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_) return;
        local = cb_;
    }
    if (e.total && local.on_total_length != nullptr) {
        local.on_total_length(local.ctx, e.total_value);
    }
    if (e.validators && local.on_validators != nullptr) {
        local.on_validators(local.ctx, e.etag.c_str(), e.last_modified.c_str());
    }
    if (e.error && local.on_error != nullptr) {
        local.on_error(local.ctx, e.err_st, e.err_http);
    }
    if (e.idle && local.on_idle != nullptr) {
        local.on_idle(local.ctx);
    }
}

void Scheduler::reap_except() {
    // ticker 线程上不拆任何 DLTask（见 health_check 末尾注释）。
    if (on_health_path()) return;
    std::vector<std::shared_ptr<Slot>> dying;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (const auto& s : slots_) {
            if (s && s->in_cb > 0) return;  // 任意回调栈上都不拆
        }
        // deferred_release_ 里的 Slot 已不在 slots_，但迟到的
        // on_finished（cancel 之后才回）仍会给它记 in_cb；那条回调栈上的公开
        // 方法（stop/pause/notify_persisted…）走到这里若照样拆，就是在 X 自己
        // 的回调栈上析构 X（~DLTask 见同线程在回调里不等 ⇒ 返回时 UAF）。
        for (const auto& s : deferred_release_) {
            if (s && s->in_cb > 0) return;
        }
        for (auto& d : deferred_release_) dying.push_back(std::move(d));
        deferred_release_.clear();
        auto it = slots_.begin();
        while (it != slots_.end()) {
            Slot* s = it->get();
            if (s && s->dead) {
                dying.push_back(std::move(*it));
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
    }
    dying.clear();
}

Range Scheduler::target_locked() const noexcept {
    int64_t left = read_pos_ < 0 ? 0 : read_pos_;
    int64_t right = target_end_;
    if (right < 0) {
        right = (total_length_ >= 0) ? total_length_ : kI64Max;
    }
    if (total_length_ >= 0 && right > total_length_) right = total_length_;
    if (left > right) left = right;
    return Range{left, right};
}

HoleSet Scheduler::occupied_locked() const {
    HoleSet taken = cached_;
    for (const Range& r : received_.ranges()) taken.add(r);
    for (const auto& s : slots_) {
        if (!s || s->dead || !s->task) continue;
        const auto st = s->task->state();
        if (is_terminal_state(st)) continue;
        // start() 之前 next_offset 仍是 0，必须用 wanted.start，否则会把
        // [1000,2000) 的未启动任务记成占用 [0,2000)。
        int64_t from = s->wanted.start;
        if (s->started) {
            from = s->task->next_offset();
            if (from < s->wanted.start) from = s->wanted.start;
        }
        int64_t to = s->wanted.end;
        if (total_length_ >= 0 && to > total_length_) to = total_length_;
        if (from < to) taken.add(Range{from, to});
    }
    return taken;
}

int32_t Scheduler::live_count_locked() const noexcept {
    int32_t n = 0;
    for (const auto& s : slots_) {
        if (!s || s->dead || !s->task) continue;
        const auto st = s->task->state();
        if (!is_terminal_state(st)) ++n;
    }
    return n;
}

int32_t Scheduler::max_tasks_locked() const noexcept {
    // 只有**确认支持 Range**（见过 206）才放开并发。
    //
    // 旧实现是 `if (!range_supported_) return 1;` + 初值 true，等于把
    // 「尚未探明」当成「支持」——首个 200 响应头里的 Content-Length 一到
    // on_total_length，并发立刻放开到 max_concurrent_tasks，在「知道总长」
    // 与「确认不支持 Range」之间的窗口里抢跑发出多条各自请求整份文件的
    // 连接。
    //
    // 「拿到 Content-Length」不等于「支持 Range」：首个请求是
    // `Range: bytes=0-`，200 + Content-Length 既可能是"不支持所以给全量"，
    // 也可能是"支持但你要的就是全量"，二者在首个响应上不可分。206 可以分。
    //
    // 代价（对绝大多数支持 Range 的正常源）：探测请求与其余并发请求之间
    // 多串行一个响应头的往返。首字节延迟不变——锁 1 时那条任务的 wanted
    // 是 [read_pos, eof)，起点与放开并发时第一个分片的起点完全相同。
    if (range_state_ != RangeState::Supported) return 1;
    return planned_tasks_locked();
}

int32_t Scheduler::planned_tasks_locked() const noexcept {
    if (range_state_ == RangeState::Unsupported) return 1;
    // 总长未知时无法按公式切分，开多条会把 INT64_MAX 拆出重叠/溢出的区间。
    // 等 on_total_length 之后再按正常并发重调度。
    if (total_length_ < 0) return 1;
    if (cfg_.max_concurrent_tasks <= 0) return 1;
    return cfg_.max_concurrent_tasks;
}

int64_t Scheduler::segment_size_locked(int64_t remaining) const noexcept {
    const int64_t seg = segment_size_unlimited_locked(remaining);
    // 分片上限 = min(原上限, max(R × 1 秒, kNoSplitBelow))。
    // 一片不超过 1 秒的额度，否则单个大分片在途时欠账太深、其它源长时间
    // 拿不到准入；但不低于切分下限——R 很小时切出一串几字节的请求，HTTP
    // 头比数据还大。R == 0 时 cap 为 INT64_MAX，原样返回。
    //
    // 【no-Range 的行为变化】本上限覆盖本函数的全部出口，所以限速非 0 且
    // 总长未知时，首个探测请求从"开放区间 [pos, EOF)"变为闭区间
    // [pos, pos + cap)。对不支持 Range 的源，这意味着服务端回 200 整文件 →
    // 走既有 no-Range 降级 → 重发，**多一次 200 往返**，且那次 200 的响应体
    // 到达时同样记账（debit），会把余额扣深一截。端到端用例
    // no_range_source_under_a_limit_still_completes（test_preloader）钉住
    // "仍能完整、正确地读完"。
    const int64_t cap = limiter_->max_segment_bytes();
    if (cap < kI64Max) return std::min(seg, std::max(cap, kNoSplitBelow));
    return seg;
}

int64_t Scheduler::segment_size_unlimited_locked(int64_t remaining) const noexcept {
    if (remaining <= 0) return 0;
    const int64_t min_sz = cfg_.min_segment_size;
    // 剩余不足下限：不硬切，一个任务下完。这是「整段剩余已经很小」，
    // 不是「每个洞都至少这么大」——单个碎片洞小于下限时，调用方拿到
    // 的仍是洞本身的长度（schedule() 里 take = min(seg, hole.size())）。
    if (min_sz > 0 && remaining <= min_sz) return remaining;
    if (remaining <= kNoSplitBelow) return remaining;

    // 用 planned 而不是 max：探测窗口里连接数锁 1，但分片必须按最终并发
    // 数切，否则探测任务会独占整个目标窗口（见 planned_tasks_locked 注释）。
    const int32_t conc = clamp_pos_i32(planned_tasks_locked());
    int64_t seg = ceil_div_nonneg(remaining, conc);
    if (min_sz > 0 && seg < min_sz) seg = min_sz;
    if (seg < kNoSplitBelow) seg = kNoSplitBelow;
    const int64_t hint = cfg_.segment_size_hint;
    // 优先级（从硬到软）：
    //   1. remaining：永不超出；
    //   2. max(min_segment_size, kNoSplitBelow)：切分粒度下限；
    //   3. segment_size_hint：上界。hint==0 不设限。hint 低于第 2 档时
    //      有效区间为空，忽略 hint（与「hint < min 则忽略」同一条规则，
    //      避免 hint=10 把已经刹在 256 的粒度又压回去）。
    // 下限管的是 HTTP 头开销，不是「用户想切多碎就多碎」。1 字节请求
    // 只来自碎片洞那一路，不走 hint。
    if (hint > 0 && hint >= min_sz && hint >= kNoSplitBelow && seg > hint) {
        seg = hint;
    }
    if (seg > remaining) seg = remaining;
    return seg;
}

bool Scheduler::is_whole_file_locked(Range w) const noexcept {
    if (w.start != 0) return false;
    if (w.end == kI64Max) return true;
    if (total_length_ >= 0 && w.end >= total_length_) return true;
    return false;
}

int64_t Scheduler::slot_remaining_locked(const Slot& s) const {
    if (!s.task) return 0;
    const int64_t from = s.task->next_offset();
    int64_t to = s.wanted.end;
    if (to == kI64Max) {
        if (total_length_ < 0) return kI64Max;  // 未知 → 视为极大，启发式走「不等」
        to = total_length_;
    }
    if (total_length_ >= 0 && to > total_length_) to = total_length_;
    if (from >= to) return 0;
    return to - from;
}

bool Scheduler::should_wait_locked() const {
    // 连接复用启发式。还有一条可选判据「估计时间 < 平均建连耗时 * 0.7」，
    // 本步不实现：DLTask 不暴露建连耗时，没有数据来源。补的位置是 DLTask
    // 在 Connecting→Receiving 记下耗时、Scheduler 维护滑动平均之后，把
    // `estimate_ms > avg_conn_ms * 7/10` 接进下面这条链的末尾。不要凭空
    // 编一个平均建连耗时。
    for (const auto& s : slots_) {
        if (!s || s->dead || !s->task) continue;
        const auto st = s->task->state();
        if (is_terminal_state(st)) continue;

        const int64_t remaining = slot_remaining_locked(*s);
        if (remaining <= 0) continue;
        if (remaining > cfg_.reuse_max_remaining_bytes) continue;

        const std::optional<int64_t> speed = s->task->speed_bps();
        // 三态：nullopt（估不出）和 0（真停滞）都不等，禁止当除数。
        if (!speed.has_value()) continue;
        if (*speed <= 0) continue;

        // estimate_ms = 1000 * remaining / speed。整数除法（截断），不要先转 float。
        int64_t estimate_ms = 0;
        if (remaining > kI64Max / 1000) {
            estimate_ms = kI64Max / *speed;
        } else {
            estimate_ms = (1000 * remaining) / *speed;
        }
        if (estimate_ms > cfg_.connect_estimate_max_ms) continue;

        return true;  // 等这个快下完的任务腾出槽
    }
    return false;
}

bool Scheduler::should_stop_slot_locked(const Slot& s) const {
    if (!s.task) return true;
    const auto st = s.task->state();
    if (is_terminal_state(st)) return true;

    const Range tgt = target_locked();
    // 完全在目标窗口之外（seek 走了，或 target_end 收窄）。
    if (s.wanted.end <= tgt.start || s.wanted.start >= tgt.end) return true;

    // 负责的区间已经全部在已缓存里（别的任务抢先 / notify_persisted）。
    const int64_t from = s.task->next_offset();
    int64_t to = s.wanted.end;
    if (total_length_ >= 0 && to > total_length_) to = total_length_;
    if (from >= to) return true;
    if (cached_.contains(Range{from, to})) return true;
    if (cached_.contains(s.wanted)) return true;
    return false;
}

DLTaskConfig Scheduler::task_cfg_for_spawn() const {
    // 透传超时/重试/速度窗口。Range 降级由 Scheduler 统一做：任务级若允许
    // 200 降级，每个非 0 起点的任务都会各自吞一份整包响应，调度器无法
    // 「只留从 0 开始的一条」。所以这里强制 false。
    DLTaskConfig t = cfg_.task;
    t.allow_no_range_fallback = false;
    return t;
}

void Scheduler::schedule() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (in_schedule_ > 0) {
            schedule_again_ = true;
            return;
        }
        in_schedule_ = 1;
        schedule_again_ = false;
    }
    InScheduleGuard guard(this);

    // 钩子指针必须在 release_locked() 之前、同一临界区内读出。清零
    // in_schedule_ 之后再锁 mu_ 读成员，会在 ~Scheduler 的空隙里 UAF。
    void (*hook)(void*) = nullptr;
    void* hook_ctx = nullptr;

    for (;;) {
        std::vector<std::shared_ptr<Slot>> to_cancel;
        std::vector<std::shared_ptr<Slot>> to_start;
        // health 路径上（health_check → schedule）不让本轮成为 Slot 的最后持有者。
        // 它们在尾检 guard.release_locked() 之后才析构、会再拿一次 mu_——为何
        // 不违反尾检注释里"交还之后不得再碰成员"的约束，见 release_slots。
        SlotRelease rel_cancel{this, to_cancel};
        SlotRelease rel_start{this, to_start};
        PendingEmit e;

        {
            std::lock_guard<std::mutex> g(mu_);
            if (stopped_ || fatal_ || !started_) {
                // 不在这里 break：要走到尾检，和交还 in_schedule_ 同一把锁。
            } else {
                if (live_count_locked() > 0) idle_armed_ = true;

                for (auto& s : slots_) {
                    if (!s || s->dead || !s->task) continue;
                    if (should_stop_slot_locked(*s)) {
                        s->dead = true;
                        to_cancel.push_back(s);
                    }
                }
            }
        }

        for (auto& s : to_cancel) s->task->cancel();

        {
            std::lock_guard<std::mutex> g(mu_);
            if (stopped_ || fatal_ || paused_ || !started_) {
                // paused / 已停：不新开任务。
            } else {
                // 兜底：上面两条回调之外的任何一次重调度（seek、
                // notify_persisted、任务结束……）都在这里重新看一眼 Range
                // 状态，不依赖某一条回调路径一定被走到。
                refresh_range_state_locked();
                const Range tgt = target_locked();
                const int32_t cap = max_tasks_locked();

                // 分片大小按本轮开始时的剩余长度算一次，后面切出来的任务
                // 共用。每切一片就重算 ceil(剩余/并发) 会越切越小，三个
                // 槽填不满「剩余/并发」那一层含义。
                int64_t seg = 0;
                bool    have_seg = false;
                bool    need_arm = false;   // 本轮被限速拒过

                while (live_count_locked() < cap) {
                    const HoleSet taken = occupied_locked();
                    const std::vector<Range> holes = taken.holes_in(tgt);
                    if (holes.empty()) break;

                    if (should_wait_locked()) break;

                    Range work{};
                    if (range_state_ == RangeState::Unsupported) {
                        // 服务端不认 Range：只能从 0 拉到尾。已交付前缀靠
                        // on_task_data 用 received_ 去重，不能改成从洞起点要。
                        const int64_t eof =
                            total_length_ >= 0 ? total_length_ : kI64Max;
                        work = Range{0, eof};
                        if (work.empty()) break;
                    } else {
                        if (!have_seg) {
                            // 分片大小按「目标窗口里还没到手的总量」算，
                            // **不减在途任务占的区间**。减了的话，探测请求
                            // （Range 探明之前并发锁 1 时先发的那一条）会把
                            // remaining 削掉一段，随后放开并发切出来的分片
                            // 比不带探测时更小更碎，同一份文件要多发好几条
                            // 请求。不减在途，探测前后切出来的分片完全一致。
                            HoleSet got;
                            for (const Range& r : cached_.ranges()) got.add(r);
                            for (const Range& r : received_.ranges()) got.add(r);
                            int64_t remaining = 0;
                            bool unbounded = false;
                            for (const Range& h : got.holes_in(tgt)) {
                                if (h.end == kI64Max || h.size() < 0) {
                                    unbounded = true;
                                    break;
                                }
                                remaining = sat_add(remaining, h.size());
                            }
                            if (unbounded) remaining = kI64Max;
                            seg = segment_size_locked(remaining);
                            have_seg = true;
                        }
                        const Range hole = holes.front();
                        int64_t take = hole.size();
                        if (seg > 0 && take > seg) take = seg;
                        if (take <= 0) break;
                        const int64_t work_end = sat_add(hole.start, take);
                        work = Range{hole.start,
                                     work_end > hole.end ? hole.end : work_end};
                        if (work.empty()) break;
                    }

                    // 限速准入。已在途的不受影响，只管新发分片。
                    // 放在洞与分片都算完之后：只有"真有东西要发"才问，
                    // 目标已齐时不会平白 arm 一次唤醒。被拒时目标里仍有
                    // 洞，下面的 idle 判定（holes_in(tgt).empty()）因此不会
                    // 误报空闲。
                    if (!limiter_->admit(rate_class_)) {
                        need_arm = true;
                        break;
                    }

                    auto slot = std::make_shared<Slot>();
                    slot->owner = this;
                    slot->id = next_slot_id_++;
                    slot->wanted = work;
                    DLTaskCallbacks tcb{
                        slot.get(),
                        &Scheduler::cb_on_data,
                        &Scheduler::cb_on_total,
                        &Scheduler::cb_on_validators,
                        &Scheduler::cb_on_finished,
                    };
                    slot->task = std::make_unique<DLTask>(
                        backend_, clock_, task_cfg_for_spawn(), tcb);
                    idle_armed_ = true;
                    to_start.push_back(slot);
                    slots_.push_back(std::move(slot));
                }

                // 被拒则登记唤醒：没有这一步，在途任务全结束后既没有数据
                // 到达、也没有任务结束来再触发调度，会永久卡住。
                // arm 只取限速器叶子锁、不等待，持 mu_ 调用安全。
                // sub_ 为空 = 析构已开始，不再登记。
                if (need_arm && sub_) {
                    if (test_before_arm_ != nullptr) test_before_arm_(test_before_arm_ctx_);
                    sub_->arm(rate_class_);
                    // arm 之后复查一次。fail-open（唤醒线程起不来、
                    // R 被置回 0）若恰好落在上面 admit 被拒与这次 arm 之间，它的
                    // 同步派发已经错过了本订阅，且此后再无派发者。arm 取限速器
                    // mu_，与 fail-open 的 rate_=0 形成 happens-before：要么 arm
                    // 早于 fail-open 的收集（会被那次派发叫醒），要么晚于它（这里
                    // 的复查读到 R==0）。复查通过就置 schedule_again_——此处仍在
                    // mu_ 临界区内，本轮尾检会看到它、再跑一轮把活发出去。
                    // 有唤醒线程的正常路径下 arm 的 notify 已覆盖此窗口，这行纯为
                    // fail-open 路径；它顶多让正常路径多跑一轮空调度，不改准入结果。
                    if (limiter_->admit(rate_class_)) schedule_again_ = true;
                }

                if (live_count_locked() == 0) {
                    const HoleSet taken = occupied_locked();
                    if (taken.holes_in(tgt).empty() && idle_armed_) {
                        idle_armed_ = false;
                        e.idle = true;
                    }
                }
            }
        }

        for (auto& s : to_start) {
            syp_headers hv{};
            std::string url_copy;
            Range wanted{};
            bool skip = false;
            int32_t hdr_n = 0;
            {
                std::lock_guard<std::mutex> g(mu_);
                if (stopped_ || fatal_ || paused_ || s->dead) {
                    s->dead = true;
                    skip = true;
                } else {
                    url_copy = url_;
                    hv = extra_headers_.view();
                    hdr_n = hv.count;
                    wanted = s->wanted;
                    s->started = true;
                }
            }
            if (skip) continue;
            // start 在锁外：同步桩的 create/start 不回调，但以后的后端可能会。
            s->task->start(std::move(url_copy), hdr_n > 0 ? &hv : nullptr, wanted);
        }

        emit(e);

        {
            std::lock_guard<std::mutex> g(mu_);
            // 「没有待重跑」和「交还 in_schedule_」必须同一临界区：
            // 若先 break 放锁、再由守卫析构清零，中间窗口里另一线程
            // 会看到 in_schedule_==1 只置 schedule_again_ 就返回，
            // 随后守卫把 in_schedule_ 清 0，这趟调度永久丢失。
            if (!schedule_again_ || stopped_ || fatal_) {
                // 有在途任务就登记一次健康检查。
                arm_health_locked();
                hook = test_after_schedule_loop_;
                hook_ctx = test_after_schedule_loop_ctx_;
                guard.release_locked();
                break;
            }
            schedule_again_ = false;
        }
    }

    if (hook != nullptr) hook(hook_ctx);
}

void Scheduler::request_schedule() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_ || !started_) return;
    }
    schedule();
    reap_except();
}

void Scheduler::set_rate_class(RateClass c) {
    {
        std::lock_guard<std::mutex> g(mu_);
        rate_class_ = c;
    }
    // 放锁后重跑一轮：Preload → Playing 的升级应立即按新阈值准入，而不是
    // 等下一次数据到达 / 任务结束 / 唤醒。request_schedule 自己判 stopped_ /
    // started_。
    request_schedule();
}

RateClass Scheduler::rate_class() const {
    std::lock_guard<std::mutex> g(mu_);
    return rate_class_;
}

void Scheduler::set_test_hook_after_schedule_loop(void (*fn)(void*), void* ctx) {
    std::lock_guard<std::mutex> g(mu_);
    test_after_schedule_loop_ = fn;
    test_after_schedule_loop_ctx_ = ctx;
}

void Scheduler::set_test_hook_before_arm(void (*fn)(void*), void* ctx) {
    std::lock_guard<std::mutex> g(mu_);
    test_before_arm_ = fn;
    test_before_arm_ctx_ = ctx;
}

}  // namespace syp::dl
