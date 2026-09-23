// scheduler.h — 洞→分片切分、并发决策、重调度
#pragma once

#include "clock.h"
#include "dl_task.h"
#include "health_ticker.h"
#include "hole_set.h"
#include "rate_limiter.h"

#include <syplayer/syp_http.h>
#include <syplayer/syp_types.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace syp::dl {

// 坏任务检测。内部旋钮，不进 syp_config。
struct HealthConfig {
    bool          enabled           = true;
    int32_t       check_interval_ms = 1000;   // ≤ 0 按 1000
    int32_t       stall_grace_ms    = 2000;   // 挂死阈值 = 对应超时 + 本值；< 0 按 0
    int32_t       slow_ratio        = 4;      // speed * slow_ratio < baseline ⇒ 慢；< 2 按 2
    int32_t       slow_strikes      = 2;      // 连续几次检查慢才判；< 1 按 1
    // nullptr = HealthTicker::instance()。非空时调用方保证它活得比本
    // Scheduler 久（测试注入不起线程的独立实例用）。
    HealthTicker* ticker            = nullptr;
};

struct SchedulerConfig {
    int32_t max_concurrent_tasks      = 3;
    int64_t min_segment_size          = 512 * 1024;
    // 0 = 自动。上界；低于 min_segment_size 或内部切分下限 kNoSplitBelow
    // 时忽略（有效区间为空）。不能用来把粒度压到下限以下。
    int64_t segment_size_hint         = 0;
    int64_t reuse_max_remaining_bytes = 1 << 20;
    int32_t connect_estimate_max_ms   = 2000;
    int32_t max_consecutive_errors    = 5;
    bool    allow_no_range_fallback   = true;
    DLTaskConfig task;                            // 透传给每个 DLTask（见类注释）
    // 限速：发新分片前按 rate_class 向 limiter 申请准入；
    // 数据到达时记账。limiter 为 nullptr 时用进程单例 RateLimiter::instance()；
    // 非空时调用方保证它活得比本 Scheduler 久（测试注入独立实例用）。
    RateClass    rate_class = RateClass::Playing;
    RateLimiter* limiter    = nullptr;
    // 坏任务检测（挂死兜底 + 慢任务替换）。
    HealthConfig health;
};

struct SchedulerCallbacks {
    void* ctx;
    // 收到数据（绝对文件偏移）。**调用方负责落盘**，落稳后再调 notify_persisted。
    void (*on_data)(void* ctx, int64_t offset, const uint8_t* data, int32_t len);
    void (*on_total_length)(void* ctx, int64_t total);
    void (*on_validators)(void* ctx, const char* etag, const char* last_modified);
    // 不可恢复错误（可恢复的内部重试，不走这里）
    void (*on_error)(void* ctx, syp_status st, int32_t http_status);
    // 目标区间已全部下完，当前无事可做。边沿触发：只在「有事可做转为
    // 无事可做」时回调一次；seek 到另一段已齐的窗口不会再刷。
    void (*on_idle)(void* ctx);
};

// 多连接 Range 调度：目标窗口 − 已缓存 − 在途 = 待下载的洞，再按并发上限切分。
//
// 目标窗口是 [read_pos, target_end)。seek 改的是窗口左端，所以原先落在读位置
// 左侧的任务会变成「完全在目标之外」被停掉——这是「seek 后立刻转向新位置」
// 的实现，而不是另做一套优先级队列。
//
// 线程安全：
//   DLTask 可从后端任意线程回调。所有可变状态由 mu_ 保护。
//   调用用户回调（on_data / on_total_length / on_validators / on_error /
//   on_idle）时**不持有 mu_**，因此用户在回调里调 pause/stop/notify_persisted
//   / set_read_position 不会死锁。
//   调用 DLTask::start / cancel 时也不持有 mu_：cancel 在同步桩上会立刻
//   回 on_finished，再拿 mu_ 会死锁。
//   析构约定：
//     - 析构**可以**与后端回调并发（先 stop，再在锁外拆掉 DLTask，
//       DLTask 自己会等到终态且无用户回调在途）；
//     - 析构**不得**与 start() 等公开方法并发；
//     - 不要在用户回调里析构本对象（同线程等自己返回会卡死）；
//     - **不得在持有任何用户回调（on_idle 等）或限速唤醒会去拿的锁时析构**：
//       析构第一步注销限速订阅，会等在途的唤醒回调返回，而唤醒回调走
//       request_schedule() → schedule()，要拿 mu_、还可能发 on_idle。
//       与"析构会等在途后端回调"是同一条约束。
//       析构第一步同时注销限速订阅与健康检查订阅，二者的回调都
//       走 mu_（health_check 还可能经 schedule() 发 on_idle / on_error）。
//     - **不得在 ticker 回调链上（health_check 触发的用户回调里）析构任何
//       Scheduler**：析构末尾拆 Slot 会在当前线程上跑 ~DLTask，对 cancel
//       不回调的后端会永久等终态、冻住全进程唯一的 ticker 线程；析构的是
//       本对象时还会在 ~Subscription 里等自己（同"不要在用户回调里析构"）。
//
// DLTask 生命周期：
//   完成回调里创建新 DLTask 是安全的（另一个对象）。**禁止**在当前任务的
//   on_finished / on_data 里 unique_ptr.reset 掉它自己——DLTask::finish
//   在用户回调返回后还要碰 this。死任务延后到「当前不在该任务回调栈上」
//   再析构（公开方法退出，或别的任务的回调里 reap 非当前者）。
class Scheduler {
public:
    Scheduler(const syp_http_backend* backend, Clock clock,
              SchedulerConfig cfg, SchedulerCallbacks cb);
    ~Scheduler();

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&)                 = delete;
    Scheduler& operator=(Scheduler&&)      = delete;

    // url/headers 固定；already_cached 是启动时已在磁盘上的区间（来自 CacheIndex）
    void start(std::string url, const syp_headers* headers,
               int64_t total_length,            // 未知传 -1
               const HoleSet& already_cached);

    // 播放位置变化 / seek。调度器据此决定先补哪个洞。
    void set_read_position(int64_t pos);
    // 想要下载的窗口右端（比如"当前位置往后 N 字节"）。-1 表示到文件尾。
    void set_target_end(int64_t end);

    // 调用方把 on_data 的字节落稳后调这个，调度器才认为该区间已缓存。
    void notify_persisted(Range r);

    void pause();
    void resume();
    void stop();                  // 停掉所有任务，之后不再调度

    // 无条件再跑一轮 schedule()。set_read_position / set_target_end 在值没变
    // 时早退，不会触发调度；read() 等窗口没动的路径要靠这条把丢掉的调度捡回来。
    // 【调用方不得持有任何回调会去拿的锁】schedule() 在**调用线程上**同步跑，
    // 可能在该线程上触发 on_idle / on_error 等回调（SourceBridge 的回调会拿
    // SourceBridge::mu_）。SourceBridge 一律锁内拷出 Scheduler*、登记
    // in_public_、锁外调（见 apply_window / set_rate_class）。
    void request_schedule();

    // 改准入类别（Preload ↔ Playing），下一次 schedule() 起生效——本调用
    // 放锁后立刻 request_schedule() 一次，升级为 Playing 不必等下一次唤醒。
    // 已在途的任务不受影响（限速只管新准入）。
    // 【调用方不得持有任何回调会去拿的锁】理由同 request_schedule()：放锁后
    // 那一轮 schedule() 在调用线程上同步跑，可能触发 on_idle / on_error。
    void      set_rate_class(RateClass c);
    // 测试缝：读的就是 schedule() 准入时用的那个成员。
    RateClass rate_class() const;
    // 测试缝：**构造时**传进来的类别（cfg_.rate_class）。cfg_ 构造后不再写，
    // set_rate_class 也不动它，所以不取锁。用来与时序无关地断言"开源那一刻
    // 给的是什么类别"——rate_class() 会被事后的 set_rate_class 改掉。
    RateClass initial_rate_class_for_test() const noexcept { return cfg_.rate_class; }

    // 测试用：schedule() 主循环退出之后、函数返回之前调用。
    // 钩子指针在尾检临界区内、release_locked() 之前读出；调用在锁外，
    // 只用局部变量，不再碰成员。钩子里再调 schedule() 会重入；测试侧应一次即清。
    void set_test_hook_after_schedule_loop(void (*fn)(void*), void* ctx);

    // 测试用：schedule() 里限速准入被拒之后、sub_->arm() 之前
    // 调用，用来把 fail-open（R 被置回 0）精确地打进这个窗口。默认空，生产
    // 代码没有调用点。【在持 mu_ 时调用】钩子里不得碰本调度器的任何接口
    // （会自锁），也不得做任何会同步回调 wake 的事——只许对**不起唤醒线程**
    // 的独立限速器调 set_rate（那条路径只取限速器叶子锁、不回调）。
    void set_test_hook_before_arm(void (*fn)(void*), void* ctx);

    // 供测试与上层观测
    int32_t active_task_count() const noexcept;
    HoleSet cached_ranges() const;         // 已缓存（含启动时传入的）
    HoleSet inflight_ranges() const;       // 正在下载中的区间
    int32_t consecutive_errors() const noexcept;
    bool    range_supported() const noexcept;  // 是否已确认服务端支持 Range

    // 测试缝 / 观测：累计判挂死、判慢而掐掉的尝试数；慢任务基线（B/s）。
    int32_t stall_kills() const noexcept;
    int32_t slow_kills() const noexcept;
    int64_t baseline_bps_for_test() const noexcept;
    // 测试缝：health 路径上延后释放、尚未被 reap 的 Slot 引用数。
    int32_t deferred_release_count_for_test() const noexcept;

private:
    // shared_ptr：pause / schedule / cancel_all 在锁下收集待 cancel 的 Slot，
    // 放锁后才 cancel()。这空隙里另一线程 reap_except 会拆掉 unique_ptr Slot
    // → UAF。拷一份 shared_ptr 出来让 Slot/DLTask 活到 cancel 返回。
    // 不用 in_cb 让路：pause/schedule 并不在该 Slot 的回调栈上，in_cb 帮不上。
    struct Slot {
        Scheduler*            owner = nullptr;
        uint64_t              id    = 0;
        Range                 wanted{};
        std::unique_ptr<DLTask> task;
        bool                  dead    = false;  // 已结束/已决定停，待 reap
        bool                  started = false;
        int                   in_cb   = 0;     // 正在该 Slot 的用户回调栈上
        int32_t               slow_strikes = 0;  // 连续判慢次数（受 mu_ 保护）
        // 被 health_check 判坏杀掉（挂死或慢替换，受 mu_ 保护）。
        // 杀的决定在锁内、cancel 在锁外，其间任务可能先以别的失败码结束
        // （竞态），那次失败不是新的一次、不该再计 consecutive_errors_——
        // on_task_finished 见此标记把任何非 OK 终态都当 CANCELED 处理。
        bool                  killed_by_health = false;
    };

    struct HeaderCopy {
        std::vector<std::string> names;
        std::vector<std::string> values;
        std::vector<const char*> name_c;
        std::vector<const char*> value_c;

        void rebuild_ptrs();
        syp_headers view() const noexcept;
        void assign(const syp_headers* h);
    };

    struct PendingEmit {
        bool     idle  = false;
        bool     error = false;
        syp_status err_st = SYP_OK;
        int32_t  err_http = 0;
        bool     total = false;
        int64_t  total_value = -1;
        bool     validators = false;
        std::string etag;
        std::string last_modified;
    };

    static void cb_on_data(void* ctx, int64_t offset, const uint8_t* data, int32_t len);
    static void cb_on_total(void* ctx, int64_t total);
    static void cb_on_validators(void* ctx, const char* etag, const char* last_modified);
    static void cb_on_finished(void* ctx, syp_status st, int32_t http_status);

    void on_task_data(Slot* slot, int64_t offset, const uint8_t* data, int32_t len);
    void on_task_total(Slot* slot, int64_t total);
    void on_task_validators(Slot* slot, const char* etag, const char* last_modified);
    void on_task_finished(Slot* slot, syp_status st, int32_t http_status);

    void schedule();
    // HealthTicker 回调入口（ticker 线程上执行；锁约束同 request_schedule）。
    // 判挂死 / 判慢 → 锁外 cancel → 未 fatal 则 schedule() 补发；否则按"有在途"
    // 再 arm。**不 reap**：见 .cpp 末尾注释。
    void health_check();
    // 持 mu_ 调用：有在途任务、未 armed、未停/未暂停/未 fatal 且检测开启时
    // arm_after(check_interval_ms) 一次。schedule() 尾检与 health_check() 共用。
    void arm_health_locked() noexcept;
    void emit(const PendingEmit& e);
    void reap_except();

    // 本线程是否正在某个 health_check() 里（= HealthTicker 的回调线程上，
    // 含它同步引出的 cancel → on_finished → schedule()、用户回调重入的公开方法）。
    static bool on_health_path() noexcept;
    // 放掉一批锁外用过的 Slot 引用。平时就地 clear；在 health 路径上则在 mu_
    // 下转进 deferred_release_，**不让 ticker 线程成为 Slot 的最后持有者**：
    // 另一线程的 reap_except 可能已把它从 slots_ 摘掉，此处一放就会在 ticker
    // 线程上跑 ~DLTask——对 cancel 不回调的后端会永久等终态，全进程唯一的
    // ticker 线程随之冻住，所有 Scheduler 的检测一起失效。
    void release_slots(std::vector<std::shared_ptr<Slot>>& v) noexcept;
    // 作用域退出时调 release_slots（覆盖 break / 异常出口）。
    struct SlotRelease {
        Scheduler* self;
        std::vector<std::shared_ptr<Slot>>& v;
        SlotRelease(Scheduler* s, std::vector<std::shared_ptr<Slot>>& vec) noexcept
            : self(s), v(vec) {}
        ~SlotRelease() { self->release_slots(v); }
        SlotRelease(const SlotRelease&)            = delete;
        SlotRelease& operator=(const SlotRelease&) = delete;
    };
    void cancel_all_tasks();
    void enter_no_range_fallback();

    // Unknown → Supported：任一在途任务收到过 206 就翻。返回 true 表示
    // 本次调用真的翻了（调用方据此触发一次重调度，把并发放开）。
    // 只在持 mu_ 时调用；内部会取 DLTask::mu_，锁序与 live_count_locked()
    // 一致（Scheduler::mu_ → DLTask::mu_），DLTask 侧不持自己的锁回调。
    bool refresh_range_state_locked() noexcept;

    Range   target_locked() const noexcept;
    HoleSet occupied_locked() const;
    int32_t live_count_locked() const noexcept;
    int32_t max_tasks_locked() const noexcept;
    // 「切分时假定的并发数」。与 max_tasks_locked（同时最多几条连接在跑）
    // 分开：探测窗口里连接数锁 1，但分片仍按 max_concurrent_tasks 切，
    // 探测请求因此只占第一个分片，其余的洞留给 206 落地后建的任务。
    // 不分开的话探测任务会把整个目标窗口占满（occupied_locked 按
    // wanted.end 记账），holes_in(tgt) 恒空，并发永远放不开。
    int32_t planned_tasks_locked() const noexcept;
    // 限速前的分片大小（原逻辑）；segment_size_locked 在它上面再夹限速的
    // 分片上限。拆开是为了让上限管到原逻辑的**每一个**出口（含"剩余不足
    // min_segment_size 就一片下完"那条早退）。
    int64_t segment_size_unlimited_locked(int64_t remaining) const noexcept;
    int64_t segment_size_locked(int64_t remaining) const noexcept;
    bool    should_wait_locked() const;
    bool    should_stop_slot_locked(const Slot& s) const;
    bool    is_whole_file_locked(Range w) const noexcept;
    int64_t slot_remaining_locked(const Slot& s) const;
    DLTaskConfig task_cfg_for_spawn() const;

    struct SlotCbGuard {
        Slot* slot;
        explicit SlotCbGuard(Slot* s) : slot(s) {
            std::lock_guard<std::mutex> g(slot->owner->mu_);
            ++slot->in_cb;
        }
        ~SlotCbGuard() {
            std::lock_guard<std::mutex> g(slot->owner->mu_);
            --slot->in_cb;
            slot->owner->cv_.notify_all();
        }
        SlotCbGuard(const SlotCbGuard&)            = delete;
        SlotCbGuard& operator=(const SlotCbGuard&) = delete;
    };

    // schedule() 占用 in_schedule_ 的 RAII：任何退出路径（含 emit 用户回调抛
    // 异常、容器分配失败）都清零并唤醒析构等待。不要在「已在 schedule 中」
    // 那条早退路径上构造——那条只置 schedule_again_，并未占用标志。
    // 正常退出必须在持 mu_ 时调用 release_locked()，把「没有待重跑」和
    // 「交还 in_schedule_」放进同一临界区；析构见 released 则不再清零。
    struct InScheduleGuard {
        Scheduler* self;
        bool       released = false;
        explicit InScheduleGuard(Scheduler* s) : self(s) {}
        void release_locked() {
            if (released) return;
            released = true;
            self->in_schedule_ = 0;
            self->cv_.notify_all();
        }
        ~InScheduleGuard() {
            if (released) return;
            std::lock_guard<std::mutex> g(self->mu_);
            if (released) return;
            self->in_schedule_ = 0;
            self->cv_.notify_all();
        }
        InScheduleGuard(const InScheduleGuard&)            = delete;
        InScheduleGuard& operator=(const InScheduleGuard&) = delete;
    };

    bool dtor_idle_locked() const noexcept;

    const syp_http_backend* backend_;
    Clock                   clock_;
    SchedulerConfig         cfg_;
    SchedulerCallbacks      cb_;

    mutable std::mutex mu_;
    std::condition_variable cv_;  // in_schedule_ / in_cb 归零时 notify；析构 wait

    std::string url_;
    HeaderCopy  extra_headers_;
    int64_t     total_length_ = -1;
    int64_t     read_pos_     = 0;
    int64_t     target_end_   = -1;  // -1 = 到文件尾
    HoleSet     cached_;
    HoleSet     received_;           // 本会话已交付（含未 persist）

    bool started_          = false;
    bool paused_           = false;
    bool stopped_          = false;
    bool fatal_            = false;
    // 三态。二态（bool，初值 true）会把
    // 「还没探明」当成「支持」，Content-Length 一到并发就放开到
    // max_concurrent_tasks，抢跑发出多条各自请求整份文件的连接。
    //   Unknown     还没见过任何 206。并发锁 1（只发一条探测请求），但
    //               仍按洞发正常的 Range 请求——「未确认支持」不等于
    //               「确认不支持」，不能据此就退化成整份重拉。
    //   Supported   见过 206。放开到 max_concurrent_tasks。
    //   Unsupported 见过硬信号（SYP_ERR_RANGE_UNSUPPORTED，或 start>0 的
    //               200）。并发锁 1 且只发 [0, eof)。
    enum class RangeState : uint8_t { Unknown, Supported, Unsupported };
    RangeState range_state_ = RangeState::Unknown;
    // 边沿触发 on_idle：只在「有事可做 → 无事可做」时回调一次。
    // true = 曾经有过活任务/启动后尚未通知；变空闲时发一次并清掉。
    bool idle_armed_       = false;
    bool total_notified_   = false;
    bool validators_notified_ = false;
    int32_t consecutive_errors_ = 0;

    uint64_t next_slot_id_   = 1;
    int      in_schedule_    = 0;
    bool     schedule_again_ = false;

    void (*test_after_schedule_loop_)(void*) = nullptr;
    void*   test_after_schedule_loop_ctx_    = nullptr;
    void (*test_before_arm_)(void*)          = nullptr;   // 受 mu_ 保护
    void*   test_before_arm_ctx_             = nullptr;

    std::vector<std::shared_ptr<Slot>> slots_;

    // ── 限速 ──
    // limiter_ 构造后不变。rate_class_ 受 mu_ 保护。
    // sub_：构造函数末尾订阅一次，之后只在 schedule() 被拒时 arm()（持 mu_
    // 调用是安全的：arm 只取限速器的叶子锁、不等待、不回调）。sub_ 这个
    // 指针本身也受 mu_ 保护：~Scheduler 在 mu_ 下把它移出、放锁后才析构
    // （析构会等在途唤醒回调，回调要拿 mu_），schedule() 在 mu_ 下判空后 arm。
    RateLimiter*                                  limiter_;
    RateClass                                     rate_class_;
    std::unique_ptr<RateLimiter::Subscription>    sub_;

    // ── 坏任务检测 ──
    // ticker_ 构造后不变。health_sub_ 与 sub_ 同一套约定：构造函数末尾在 mu_
    // 下装入，~Scheduler 在 mu_ 下移出、放锁后析构（析构会等在途的
    // health_check 返回，回调要拿 mu_）；schedule()/health_check() 在 mu_ 下
    // 判空后 arm_after（warm_up 之后只取 ticker 叶子锁、不等待、不回调）。
    // 其余成员都受 mu_ 保护。
    HealthTicker*                                 ticker_ = nullptr;
    std::unique_ptr<HealthTicker::Subscription>   health_sub_;
    bool    health_armed_  = false;   // 已 arm 未回调，避免每次 schedule 都去 notify
    int32_t stall_kills_   = 0;
    int32_t slow_kills_    = 0;
    // 慢任务基线（B/s）：健康样本的 EWMA（α = 1/8），每替换一次减半。
    // 0 = 还没有样本，不判慢。
    int64_t baseline_bps_  = 0;
    // 慢任务替换的冷却截止时刻（本调度器时钟）：替换后一个速度窗口内不判慢。
    int64_t slow_cooldown_until_ms_ = 0;
    // health 路径上延后释放的 Slot 引用（见 release_slots）。受 mu_ 保护；
    // reap_except（非 health 路径、无 in_cb 时）与 ~Scheduler 在锁外清掉。
    std::vector<std::shared_ptr<Slot>> deferred_release_;
};

}  // namespace syp::dl
