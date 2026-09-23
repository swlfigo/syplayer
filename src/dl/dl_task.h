// dl_task.h — 一条连接、一个字节区间的下载任务
#pragma once

#include "clock.h"
#include "hole_set.h"

#include <syplayer/syp_http.h>
#include <syplayer/syp_types.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace syp::dl {

enum class DLTaskState { Idle, Connecting, Receiving, Done, Failed, Canceled };

struct DLTaskConfig {
    // 两个超时字段只原样填进 syp_http_request；DLTask 自己不计时，见类注释。
    int32_t connect_timeout_ms      = 10000;
    int32_t read_timeout_ms         = 15000;
    int32_t max_redirects           = 8;      // 超过 → SYP_ERR_TOO_MANY_REDIRECTS
    // 连续**无进展**的重试上限：推进过字节的尝试会把预算清零，所以一条
    // 反复断流但一直在前进的连接能下完（有界：每次至少推进 1 字节）。
    int32_t max_retries             = 3;
    int32_t speed_window_ms         = 3000;  // 短于 50ms 的陷阱见 speed_bps() 注释
    bool    allow_no_range_fallback = true;
};

struct DLTaskCallbacks {
    void* ctx;
    // 收到数据。offset 是**绝对文件偏移**。保证：偏移严格递增且连续，不重不漏。
    void (*on_data)(void* ctx, int64_t offset, const uint8_t* data, int32_t len);
    // 首次解析出资源总长度
    void (*on_total_length)(void* ctx, int64_t total);
    // 首次拿到校验信息。etag / last-modified 任一可为空串；两者都缺失时仍会
    // 回调一次（两个空串）。回调了不等于有值，调用方必须自己看字符串。
    void (*on_validators)(void* ctx, const char* etag, const char* last_modified);
    // 终态，**恰好回调一次**
    void (*on_finished)(void* ctx, syp_status st, int32_t http_status);
};

// 单连接 Range 下载。
//
// 状态机：
//   Idle → Connecting          start()
//   Connecting → Receiving     2xx 且开始交付字节
//   Connecting → Connecting    失败后重试（新请求从 next_offset 续传）
//   Receiving  → Connecting    同上
//   Connecting/Receiving → Done/Failed/Canceled   终态，不再迁出
//   Idle → Canceled            start 之前 cancel / 析构
//
// 超时：
//   DLTask 自己不计时。cfg_ 的 connect_timeout_ms / read_timeout_ms 只是原样
//   填进 syp_http_request，交给后端。超时判定与上报是后端的责任：到期后必须
//   回调 on_complete(SYP_ERR_TIMEOUT, 0)。DLTask 把该错误当可重试处理。
//   后端不守约（永不回调）时，任务不会自行超时——由 Scheduler 的坏任务检测
//   兜底（超时 + 宽限后判挂死、cancel 并重发，见 scheduler.h
//   HealthConfig）；后端连 cancel 也不回调时仍收不回。
//
// 取消与后端契约：
//   create 已返回 handle、start 尚未调用时若已取消，DLTask 会直接
//   backend_->cancel(h) 而不调 start。依赖 syp_http.h：无论是否 start 过，
//   cancel 之后后端必须恰好回调一次 on_complete(SYP_ERR_CANCELED, 0)。
//   pin 期间 on_complete 已到、handle 尚未 destroy 时，cancel() 仍可能再
//   调 backend_->cancel。公开头没写这条；后端须把 complete 之后的 cancel
//   当 no-op（不得补发第二次 on_complete）。
//
// 线程安全：
//   后端可在任意线程回调 syp_response_sink；同一请求的回调串行（见 syp_http.h）。
//   所有可变状态由 mu_ 保护。state / next_offset / received_bytes / speed_bps /
//   redirect_count / attempt_count 可与回调、cancel 并发调用。
//   调用用户回调（on_data / on_total_length / on_validators / on_finished）时
//   **不持有 mu_**，因此用户在回调里调 cancel() 或读状态不会死锁。
//   cancel() 可在任意线程、可重入、可在用户回调里调用。
//   析构约定：
//     - 析构**可以**与后端回调并发（内部会等到终态且无用户回调在途）；
//     - 析构**不得**与 start() 并发（C++ 对象生命周期的通用约束；
//       issue_request 在 create 前后有无锁窗口，与 start 并发会 UAF）；
//     - 析构**可以**与 cancel() 并发。
//   不要在用户回调里析构本对象（同线程等自己返回会卡死）。
class DLTask {
public:
    DLTask(const syp_http_backend* backend, Clock clock,
           DLTaskConfig cfg, DLTaskCallbacks cb);
    ~DLTask();

    DLTask(const DLTask&)            = delete;
    DLTask& operator=(const DLTask&) = delete;
    DLTask(DLTask&&)                 = delete;
    DLTask& operator=(DLTask&&)      = delete;

    // wanted 为半开区间；wanted.end == INT64_MAX 表示"要到文件尾"
    void start(std::string url, const syp_headers* headers, Range wanted);
    void cancel();   // 可在任意线程、可重入、可在回调里调

    // 测试用：cancel() 已取出 handle（并 pin）之后、调用 backend_->cancel
    // 之前。用来注入「on_complete 已经 destroy」与「cancel 还握着旧指针」
    // 的交错。钩子里再调 cancel() 会重入；测试侧应一次即清。
    void set_test_hook_after_cancel_take_handle(void (*fn)(void*), void* ctx);

    DLTaskState state() const noexcept;
    int64_t next_offset() const noexcept;      // 下一个还没收到的绝对偏移
    int64_t received_bytes() const noexcept;
    // 近期平均速度，字节/秒。三态：
    //
    //   返回       条件 / 含义
    //   nullopt    speed_window_ms <= 0；或 span 不足门槛；或本任务从未
    //              收到过任何字节。估不出来，调用方走「还没有估计」分支。
    //   0          span 已够长、已经收到过数据、但当前窗口内无字节。
    //              真停滞，该换任务了。0 的前提是「已经收到过数据」：
    //              一个字节都没到过谈不上「测出速率是 0」。
    //   >0         正常测量值
    //
    // 调用方拿到 nullopt 时不得编造一个默认速度去除。刚建连、仍在
    // Connecting、received_bytes == 0 时是 nullopt，不是 0——否则调度器
    // 会把刚起步 50ms 的任务当停滞踢掉。
    //
    // 公式：
    //   left  = max(now_ms - speed_window_ms, task_start_ms)
    //   span  = now_ms - left
    //   sum   = 时刻 >= left 的样本字节合计
    //   speed = sum * 1000 / span
    //           （仅当 span >= min(kMinSpeedSpanMs, window)）
    // 左端点是 start() 时采的任务起始时刻，重试不重置，再按窗口封顶。
    // 分子的字节和分母的时间同一基准：起播不把速率摊到整个窗口上（避免
    // 低报），也不从最早样本的时刻起算（否则该样本的字节进了 sum、获取
    // 它的时间却没进 span，N 个样本大约高报 N/(N-1) 倍）。经过时间达到
    // 窗口宽度后，与「sum * 1000 / window」一致（稳态）。
    //
    // 重试不清空速度样本、也不重置左端点。窗口（speed_window_ms）自己
    // 会把过期样本淘汰，不需要额外清空；3 秒前那条死连接的样本会自己
    // 过期。重试造成的空档会如实拉低平均值——这是对的：一个反复断流
    // 的任务确实就是慢，调度器应该看到这一点。
    //
    // span 不足 min(kMinSpeedSpanMs, window) 时返回 nullopt。
    // kMinSpeedSpanMs（50ms，定义见 dl_task.cpp）避免单块数据在 1ms 内
    // 到达被放大成数十 MB/s 的假尖峰（51200 字节 / 1ms = 51,200,000 B/s），
    // 调度器用 remaining_bytes / speed 会严重低估剩余时间。门槛取
    // window 的较小值：span 被窗口封顶，窗口配成 1..49ms 时永远够不到
    // 50ms，若不取小值估计器会静默永久失效（配 speed_window_ms=20
    // 永远 nullopt）。窗口短于 50ms 等于调用方接受更短、更吵的测量区间。
    std::optional<int64_t> speed_bps() const noexcept;
    int32_t redirect_count() const noexcept;
    int32_t attempt_count() const noexcept;    // 含首次，重试各 +1

    // 坏任务检测的输入。均受 mu_ 保护，可与回调并发读。
    // attempt_started_ms：本次尝试 issue_request 的时刻（重试各更新一次）；
    //   从未发出过请求为 -1。**计时从请求真正发出算起**——被限速拒绝准入
    //   时根本没有 DLTask，那段等待不会被算进任何任务的时长。
    // last_progress_ms：最近一次交付字节的时刻，跨尝试保留；从未收到为 -1。
    //   响应头、重定向不算进展。
    int64_t attempt_started_ms() const noexcept;
    int64_t last_progress_ms() const noexcept;

    // 本任务是否收到过 206。**这是「服务端支持 Range」的唯一硬证据**：
    // 请求发出去的是 `Range: bytes=<start>-`（见 issue_request 与各后端），
    // 支持 Range 的服务端对它回 206，不支持的（忽略 Range 头的）回 200。
    // 反过来不成立——`start == 0` 的 200 只说明"这次给了全量"，既可能是
    // 不支持 Range，也可能是服务端选择忽略 Range 头，所以它**只**是
    // 「尚未确认」，不是「确认不支持」（确认不支持要靠 start > 0 的 200，
    // 那条走 SYP_ERR_RANGE_UNSUPPORTED）。
    // 一经置位不再清除（重试/重定向都不清）。可与回调并发调用。
    bool range_confirmed() const noexcept;

private:
    struct HeaderCopy {
        std::vector<std::string> names;
        std::vector<std::string> values;
        std::vector<const char*> name_c;
        std::vector<const char*> value_c;

        void rebuild_ptrs();
        syp_headers view() const noexcept;
        void assign(const syp_headers* h);
    };

    struct SpeedSample {
        int64_t t_ms  = 0;
        int32_t bytes = 0;
    };

    static bool is_terminal(DLTaskState s) noexcept;

    static void sink_on_response(void* ctx, int32_t http_status,
                                 const syp_headers* headers,
                                 int64_t content_length, int64_t total_length);
    static void sink_on_data(void* ctx, const uint8_t* data, int32_t len);
    static bool sink_on_redirect(void* ctx, const char* new_url);
    static void sink_on_complete(void* ctx, syp_status status, int32_t http_status);

    int64_t now_ms() const noexcept;
    void    issue_request();          // 不持锁：create 后释放锁再 start
    void    finish(syp_status st, int32_t http_status);
    bool    range_satisfied_locked() const noexcept;
    bool    can_retry_locked() const noexcept;
    void    record_speed_locked(int32_t n) noexcept;
    std::optional<int64_t> speed_bps_locked() const noexcept;
    void    notify_meta_unlocked(int64_t total, const std::string& etag,
                                 const std::string& last_modified,
                                 bool have_total, bool have_validators);

    // 把 handle 交给 destroy。若仍有 cancel() 的 pin，只记 pending，返回
    // nullptr——unpin 到 0 时再真正交出。调用方必须在锁外 destroy。
    syp_http_request_handle* take_handle_for_destroy_locked() noexcept;
    syp_http_request_handle* unpin_handle_locked() noexcept;

    // cancel() 占用 handle_refs_ 的 RAII：钩子 / backend_->cancel 若抛，
    // 析构仍 unpin（必要时锁外 destroy），避免 handle_refs_ 永远 > 0
    // 把 ~DLTask 卡死。正常路径在持 mu_ 时 release_locked()。
    struct HandlePinGuard {
        DLTask* self;
        bool    released = false;
        explicit HandlePinGuard(DLTask* s) : self(s) {}
        syp_http_request_handle* release_locked() {
            if (released) return nullptr;
            released = true;
            return self->unpin_handle_locked();
        }
        ~HandlePinGuard() {
            if (released) return;
            syp_http_request_handle* destroy_h = nullptr;
            {
                std::lock_guard<std::mutex> g(self->mu_);
                if (released) return;
                destroy_h = self->unpin_handle_locked();
            }
            if (destroy_h != nullptr && self->backend_ != nullptr
                && self->backend_->destroy != nullptr) {
                self->backend_->destroy(destroy_h);
            }
        }
        HandlePinGuard(const HandlePinGuard&)            = delete;
        HandlePinGuard& operator=(const HandlePinGuard&) = delete;
    };

    const syp_http_backend* backend_;
    Clock                   clock_;
    DLTaskConfig            cfg_;
    DLTaskCallbacks         cb_;
    syp_response_sink       sink_{};

    mutable std::mutex      mu_;
    std::condition_variable cv_;

    DLTaskState state_            = DLTaskState::Idle;
    std::string url_;
    HeaderCopy  extra_headers_;
    Range       wanted_{};
    int64_t     next_offset_      = 0;
    int64_t     received_bytes_   = 0;
    int32_t     redirect_count_   = 0;
    int32_t     attempt_count_    = 0;
    // 连续**无进展**的尝试数（含当前这次）：一次尝试让 next_offset_ 往前推进过
    // 就清零。重试预算看的是它，不是 attempt_count_——见 can_retry_locked()。
    int32_t     stalled_attempts_ = 0;
    int64_t     total_length_     = -1;
    int64_t     body_file_pos_    = 0;   // 服务端响应体下一字节对应的绝对偏移
    int64_t     attempt_range_start_ = 0;
    int64_t     task_start_ms_       = 0;  // start() 时采一次；重试不重置
    int64_t     attempt_started_ms_  = -1;  // 见 attempt_started_ms()
    int64_t     last_progress_ms_    = -1;  // 见 last_progress_ms()
    int32_t     last_http_status_ = 0;
    syp_status  last_error_       = SYP_OK;
    bool        user_canceled_    = false;
    bool        finished_emitted_ = false;
    bool        has_fatal_        = false;  // 本轮已判定致命，丢数据等 on_complete
    bool        drop_body_        = false;
    bool        range_confirmed_  = false;  // 收到过 206；见 range_confirmed()
    bool        total_notified_   = false;
    bool        validators_notified_ = false;
    int         in_user_callback_ = 0;
    std::thread::id callback_thread_{};

    syp_http_request_handle* handle_ = nullptr;
    int  handle_refs_ = 0;                 // cancel() 取出后、backend_->cancel 返回前
    bool handle_destroy_pending_ = false;  // on_complete 想 destroy，但还有 pin

    void (*test_after_cancel_take_handle_)(void*) = nullptr;
    void*   test_after_cancel_take_handle_ctx_    = nullptr;

    std::deque<SpeedSample>  speed_samples_;
};

namespace detail {

// Content-Range 是不可信输入。解析失败 / 长度为 * 时 total = -1。
// first/last 是 HTTP 的闭区间；无效时为 -1。
struct ParsedContentRange {
    bool    valid = false;
    int64_t first = -1;
    int64_t last  = -1;
    int64_t total = -1;
};

ParsedContentRange parse_content_range(std::string_view header) noexcept;

}  // namespace detail

}  // namespace syp::dl
