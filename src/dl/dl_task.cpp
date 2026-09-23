// dl_task.cpp — 单连接 Range 任务：重定向/重试续传/状态码分类；回调一律在锁外发出
#include "dl_task.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace syp::dl {
namespace {

bool ascii_ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

// 只接受无符号十进制；溢出或空串失败。避免 strtoll 的 ERANGE/locale。
bool parse_u64_strict(std::string_view s, uint64_t& out) noexcept {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        const uint64_t d = static_cast<uint64_t>(ch - '0');
        if (v > (std::numeric_limits<uint64_t>::max() - d) / 10u) return false;
        v = v * 10u + d;
    }
    out = v;
    return true;
}

const char* header_get(const syp_headers* h, const char* name) noexcept {
    if (h == nullptr || name == nullptr || h->names == nullptr || h->values == nullptr) {
        return nullptr;
    }
    for (int32_t i = 0; i < h->count; ++i) {
        if (h->names[i] == nullptr) continue;
        if (ascii_ieq(h->names[i], name)) return h->values[i];
    }
    return nullptr;
}

// 可重试 vs 致命：一处判定，on_response / on_complete 共用。
//
// HTTP 状态码：
//   200 / 206           成功（200 在 wanted.start>0 且不允许降级时除外）
//   408 / 429 / 5xx     可重试
//   416                 致命（SYP_ERR_CONTENT_CHANGED）
//   其他 4xx 等         致命（SYP_ERR_HTTP_STATUS）
//
// syp_status：
//   SYP_ERR_TIMEOUT     可重试（超时由后端判定并回调，见 dl_task.h）
//   SYP_ERR_NETWORK     可重试
//   SYP_ERR_IO          可重试（规格分类表未列；本地/传输 IO 常是瞬时的，
//                       这里按可重试处理）
//   其余负错误码        致命
enum class HttpClass {
    Full200,
    Partial206,
    Unsatisfiable416,
    Retryable,
    Fatal,
};

HttpClass classify_http_status(int32_t s) noexcept {
    if (s == 200) return HttpClass::Full200;
    if (s == 206) return HttpClass::Partial206;
    if (s == 416) return HttpClass::Unsatisfiable416;
    if (s == 408 || s == 429) return HttpClass::Retryable;
    if (s >= 500 && s <= 599) return HttpClass::Retryable;
    return HttpClass::Fatal;
}

bool status_is_retryable(syp_status st, int32_t http) noexcept {
    if (st == SYP_ERR_TIMEOUT || st == SYP_ERR_NETWORK || st == SYP_ERR_IO) {
        return true;
    }
    if (st == SYP_ERR_HTTP_STATUS) {
        return classify_http_status(http) == HttpClass::Retryable;
    }
    return false;
}

int64_t clamp_nonneg_i32(int32_t v) noexcept {
    return v < 0 ? 0 : static_cast<int64_t>(v);
}

// 速度估计的默认最短测量区间（毫秒）。低于这个门槛，单块数据在 1ms 内
// 到达会把吞吐放大约 1000 倍（51200 字节 → 51 MB/s）。50ms 是一次弱网
// 重试步长 / 播放器事件循环 tick 的量级：够滤掉假尖峰，又不把
// 100 KiB / 100ms 这种合法的启动估计推迟到窗口稳态。
// 实际门槛是 min(kMinSpeedSpanMs, window)：span 被窗口封顶，窗口 < 50ms
// 时永远够不到本常量，不取小值估计器会永久静默。
constexpr int64_t kMinSpeedSpanMs = 50;

int64_t window_cutoff(int64_t now, int64_t window) noexcept {
    if (window <= 0) return now;
    if (now < std::numeric_limits<int64_t>::min() + window) {
        return std::numeric_limits<int64_t>::min();
    }
    return now - window;
}

// sum 字节在 span_ms 毫秒内 → 字节/秒。不除零；sum*1000 饱和到 int64 max。
int64_t bytes_per_sec(int64_t sum, int64_t span_ms) noexcept {
    if (sum <= 0 || span_ms <= 0) return 0;
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    const int64_t q = sum / span_ms;
    const int64_t r = sum % span_ms;
    if (q > kMax / 1000) return kMax;
    const int64_t out = q * 1000;
    // span 被窗口（int32）封顶，r < span，r*1000 不溢出 int64。
    const int64_t frac = (r * 1000) / span_ms;
    if (out > kMax - frac) return kMax;
    return out + frac;
}

}  // namespace

namespace detail {

ParsedContentRange parse_content_range(std::string_view header) noexcept {
    ParsedContentRange out;
    std::string_view s = trim(header);
    if (s.size() < 5) return out;
    if (!ascii_ieq(s.substr(0, 5), "bytes")) return out;
    s.remove_prefix(5);
    if (s.empty() || !is_ws(s.front())) return out;
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);

    const size_t slash = s.find('/');
    if (slash == std::string_view::npos) return out;
    const std::string_view range = trim(s.substr(0, slash));
    const std::string_view total = trim(s.substr(slash + 1));
    if (range.empty() || total.empty()) return out;

    if (range == "*") {
        out.first = -1;
        out.last  = -1;
    } else {
        const size_t dash = range.find('-');
        if (dash == std::string_view::npos) return out;
        uint64_t u1 = 0;
        uint64_t u2 = 0;
        if (!parse_u64_strict(range.substr(0, dash), u1)) return out;
        if (!parse_u64_strict(range.substr(dash + 1), u2)) return out;
        if (u1 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return out;
        if (u2 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return out;
        if (u1 > u2) return out;
        out.first = static_cast<int64_t>(u1);
        out.last  = static_cast<int64_t>(u2);
    }

    if (total == "*") {
        out.total = -1;
    } else {
        uint64_t t = 0;
        if (!parse_u64_strict(total, t)) return out;
        if (t > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return out;
        out.total = static_cast<int64_t>(t);
    }
    out.valid = true;
    return out;
}

}  // namespace detail

void DLTask::HeaderCopy::rebuild_ptrs() {
    name_c.clear();
    value_c.clear();
    name_c.reserve(names.size());
    value_c.reserve(values.size());
    for (size_t i = 0; i < names.size(); ++i) {
        name_c.push_back(names[i].c_str());
        value_c.push_back(values[i].c_str());
    }
}

syp_headers DLTask::HeaderCopy::view() const noexcept {
    syp_headers h{};
    h.names  = name_c.empty() ? nullptr : name_c.data();
    h.values = value_c.empty() ? nullptr : value_c.data();
    h.count  = static_cast<int32_t>(names.size());
    return h;
}

void DLTask::HeaderCopy::assign(const syp_headers* h) {
    names.clear();
    values.clear();
    if (h == nullptr || h->count <= 0 || h->names == nullptr || h->values == nullptr) {
        rebuild_ptrs();
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

bool DLTask::is_terminal(DLTaskState s) noexcept {
    return s == DLTaskState::Done || s == DLTaskState::Failed || s == DLTaskState::Canceled;
}

DLTask::DLTask(const syp_http_backend* backend, Clock clock,
               DLTaskConfig cfg, DLTaskCallbacks cb)
    : backend_(backend), clock_(clock), cfg_(cfg), cb_(cb) {
    if (cfg_.connect_timeout_ms < 0) cfg_.connect_timeout_ms = 0;
    if (cfg_.read_timeout_ms < 0) cfg_.read_timeout_ms = 0;
    if (cfg_.max_redirects < 0) cfg_.max_redirects = 0;
    if (cfg_.max_retries < 0) cfg_.max_retries = 0;
    if (cfg_.speed_window_ms < 0) cfg_.speed_window_ms = 0;
    sink_.ctx         = this;
    sink_.on_response = &DLTask::sink_on_response;
    sink_.on_data     = &DLTask::sink_on_data;
    sink_.on_redirect = &DLTask::sink_on_redirect;
    sink_.on_complete = &DLTask::sink_on_complete;
}

DLTask::~DLTask() {
    cancel();
    std::unique_lock lk(mu_);
    const bool same_thread_in_cb =
        in_user_callback_ > 0 && callback_thread_ == std::this_thread::get_id();
    if (!same_thread_in_cb) {
        // 后端漏 on_complete 时这里会一直等（提前返回会 UAF）。超时只打诊断。
        // handle_refs_：另一线程的 cancel() 可能还握着 pin，等它 unpin
        // 再 destroy，避免与 backend_->cancel 交错释放。
        while (!cv_.wait_for(lk, std::chrono::seconds(2), [this] {
            return finished_emitted_ && in_user_callback_ == 0 && handle_refs_ == 0;
        })) {
            std::fprintf(stderr,
                "syp_dl_task: dtor still waiting state=%d finished_emitted=%d "
                "in_user_callback=%d user_canceled=%d has_fatal=%d handle=%p "
                "handle_refs=%d\n",
                static_cast<int>(state_),
                static_cast<int>(finished_emitted_),
                in_user_callback_,
                static_cast<int>(user_canceled_),
                static_cast<int>(has_fatal_),
                static_cast<void*>(handle_),
                handle_refs_);
        }
    }
    auto* h = take_handle_for_destroy_locked();
    lk.unlock();
    if (h != nullptr && backend_ != nullptr && backend_->destroy != nullptr) {
        backend_->destroy(h);
    }
}

int64_t DLTask::now_ms() const noexcept {
    if (clock_.now_ms == nullptr) return 0;
    return clock_.now_ms(clock_.ctx);
}

void DLTask::start(std::string url, const syp_headers* headers, Range wanted) {
    bool emit_invalid = false;
    bool emit_empty   = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (state_ != DLTaskState::Idle || finished_emitted_) return;
        if (wanted.start < 0) {
            emit_invalid = true;
        } else if (wanted.end != std::numeric_limits<int64_t>::max()
                   && wanted.end <= wanted.start) {
            emit_empty = true;
            url_ = std::move(url);
            extra_headers_.assign(headers);
            wanted_ = wanted;
            next_offset_ = wanted.start;
        } else {
            url_ = std::move(url);
            extra_headers_.assign(headers);
            wanted_ = wanted;
            next_offset_ = wanted.start;
            received_bytes_ = 0;
            state_ = DLTaskState::Connecting;
            task_start_ms_ = now_ms();
        }
    }
    if (emit_invalid) {
        finish(SYP_ERR_INVALID_ARG, 0);
        return;
    }
    if (emit_empty) {
        finish(SYP_OK, 0);
        return;
    }
    issue_request();
}

void DLTask::set_test_hook_after_cancel_take_handle(void (*fn)(void*), void* ctx) {
    std::lock_guard<std::mutex> g(mu_);
    test_after_cancel_take_handle_ = fn;
    test_after_cancel_take_handle_ctx_ = ctx;
}

syp_http_request_handle* DLTask::take_handle_for_destroy_locked() noexcept {
    if (handle_refs_ > 0) {
        handle_destroy_pending_ = true;
        return nullptr;
    }
    auto* h = handle_;
    handle_ = nullptr;
    handle_destroy_pending_ = false;
    return h;
}

syp_http_request_handle* DLTask::unpin_handle_locked() noexcept {
    if (handle_refs_ > 0) --handle_refs_;
    cv_.notify_all();
    if (handle_refs_ == 0 && handle_destroy_pending_) {
        return take_handle_for_destroy_locked();
    }
    return nullptr;
}

void DLTask::cancel() {
    syp_http_request_handle* h = nullptr;
    syp_http_request_handle* destroy_h = nullptr;
    bool emit_now = false;
    void (*hook)(void*) = nullptr;
    void* hook_ctx = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        user_canceled_ = true;
        if (finished_emitted_) return;
        if (handle_ == nullptr) {
            // 没有在途请求：start 之前，或两轮重试的间隙（间隙里 on_complete 会处理）。
            if (state_ == DLTaskState::Idle) emit_now = true;
        } else {
            h = handle_;
            ++handle_refs_;
            hook = test_after_cancel_take_handle_;
            hook_ctx = test_after_cancel_take_handle_ctx_;
            test_after_cancel_take_handle_ = nullptr;
            test_after_cancel_take_handle_ctx_ = nullptr;
        }
    }
    if (h != nullptr) {
        // 钩子 / backend_->cancel 都在锁外：后端可能同步回调 on_complete，
        // 再拿 mu_ 会死锁。pin 让 on_complete 把 destroy 推迟到 unpin。
        HandlePinGuard pin(this);
        if (hook != nullptr) hook(hook_ctx);
        if (backend_ != nullptr && backend_->cancel != nullptr) {
            backend_->cancel(h);
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            destroy_h = pin.release_locked();
        }
        if (destroy_h != nullptr && backend_ != nullptr
            && backend_->destroy != nullptr) {
            backend_->destroy(destroy_h);
        }
    } else if (emit_now) {
        finish(SYP_ERR_CANCELED, 0);
    }
}

DLTaskState DLTask::state() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return state_;
}

int64_t DLTask::next_offset() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return next_offset_;
}

int64_t DLTask::received_bytes() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return received_bytes_;
}

std::optional<int64_t> DLTask::speed_bps() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return speed_bps_locked();
}

int32_t DLTask::redirect_count() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return redirect_count_;
}

int32_t DLTask::attempt_count() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return attempt_count_;
}

int64_t DLTask::attempt_started_ms() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return attempt_started_ms_;
}

int64_t DLTask::last_progress_ms() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return last_progress_ms_;
}

bool DLTask::range_confirmed() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return range_confirmed_;
}

void DLTask::record_speed_locked(int32_t n) noexcept {
    if (n <= 0) return;
    const int64_t t = now_ms();
    last_progress_ms_ = t;
    speed_samples_.push_back(SpeedSample{t, n});
    const int64_t window = clamp_nonneg_i32(cfg_.speed_window_ms);
    const int64_t cutoff = window_cutoff(t, window);
    while (!speed_samples_.empty() && speed_samples_.front().t_ms < cutoff) {
        speed_samples_.pop_front();
    }
}

std::optional<int64_t> DLTask::speed_bps_locked() const noexcept {
    const int64_t window = clamp_nonneg_i32(cfg_.speed_window_ms);
    if (window <= 0) return std::nullopt;
    const int64_t t = now_ms();
    const int64_t cutoff = window_cutoff(t, window);
    // left = max(now - window, task_start_ms)。不用最早样本时刻：
    // 那个样本的字节在 sum 里，获取它的时间必须从任务起始算起。
    // 左端点是 start() 采的，重试不重置——否则 s.t_ms < left 会把重试前
    // 的样本排除掉，等于变相清空。
    const int64_t left = (cutoff > task_start_ms_) ? cutoff : task_start_ms_;
    int64_t sum = 0;
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    for (const SpeedSample& s : speed_samples_) {
        if (s.t_ms < left) continue;
        const int64_t b = static_cast<int64_t>(s.bytes);
        if (sum > kMax - b) sum = kMax;
        else sum += b;
    }
    int64_t span = 0;
    if (t >= left) {
        const uint64_t udiff = static_cast<uint64_t>(t) - static_cast<uint64_t>(left);
        span = (udiff > static_cast<uint64_t>(kMax)) ? kMax : static_cast<int64_t>(udiff);
    }
    if (span > window) span = window;
    const int64_t effective_min_span = std::min(kMinSpeedSpanMs, window);
    if (span < effective_min_span) return std::nullopt;
    // 一个字节都没到过，谈不上「测出速率是 0」，那是还没有估计。
    // 0 只留给「曾经收到过数据、当前窗口里一个字节都没有」的真停滞。
    if (received_bytes_ == 0) return std::nullopt;
    if (sum <= 0) return int64_t{0};
    return bytes_per_sec(sum, span);
}

bool DLTask::range_satisfied_locked() const noexcept {
    if (total_length_ >= 0 && next_offset_ >= total_length_) return true;
    if (wanted_.end != std::numeric_limits<int64_t>::max()
        && next_offset_ >= wanted_.end) {
        return true;
    }
    return false;
}

bool DLTask::can_retry_locked() const noexcept {
    if (user_canceled_ || finished_emitted_ || has_fatal_) return false;
    // 【预算按"连续无进展"计，不按总尝试数】弱网上一条连接反复被掐断、但
    // 每次都往前推进，是能下完的；按总数计会让大分片下着下着就失败。Apple
    // 后端在 -1005（连接丢失）时还会丢掉已缓冲、未交付给 delegate 的字节，
    // 交付量随负载随机——test_probe_e2e 场景 F 在并发负载下概率性变红，
    // 实测就是 9 次尝试、每次只交付 0~97000 字节中的随机一截，把总数预算
    // 耗尽。
    return stalled_attempts_ <= cfg_.max_retries;
}

void DLTask::issue_request() {
    syp_http_request req{};
    enum class Plan { Create, FinishOk, FinishCanceled, FinishInvalid };
    Plan plan = Plan::Create;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (finished_emitted_) return;
        if (user_canceled_) {
            plan = Plan::FinishCanceled;
        } else if (range_satisfied_locked()) {
            plan = Plan::FinishOk;
        } else if (backend_ == nullptr || backend_->create == nullptr) {
            plan = Plan::FinishInvalid;
        } else {
            ++attempt_count_;
            attempt_started_ms_ = now_ms();
            ++stalled_attempts_;
            state_ = DLTaskState::Connecting;
            // 不清空 speed_samples_，也不重置 task_start_ms_。窗口自己会
            // 淘汰过期样本；重试空档如实拉低平均值，调度器该看到「这任务慢」。
            drop_body_ = false;
            has_fatal_ = false;
            last_error_ = SYP_OK;
            last_http_status_ = 0;
            attempt_range_start_ = next_offset_;
            body_file_pos_ = next_offset_;
            req.url = url_.c_str();
            req.headers = extra_headers_.view();
            req.range_start = next_offset_;
            req.range_end = (wanted_.end == std::numeric_limits<int64_t>::max())
                                ? int64_t{-1}
                                : wanted_.end - 1;
            req.connect_timeout_ms = cfg_.connect_timeout_ms;
            req.read_timeout_ms    = cfg_.read_timeout_ms;
        }
    }
    if (plan == Plan::FinishCanceled) {
        finish(SYP_ERR_CANCELED, 0);
        return;
    }
    if (plan == Plan::FinishOk) {
        finish(SYP_OK, 0);
        return;
    }
    if (plan == Plan::FinishInvalid) {
        finish(SYP_ERR_INVALID_ARG, 0);
        return;
    }

    // create 的 req 指针指向本对象持有的 url/headers。create 不得回调。
    syp_http_request_handle* h =
        backend_->create(backend_->backend_ctx, &req, &sink_);

    bool need_start  = false;
    bool need_cancel = false;
    bool create_fail = false;
    bool retry_create = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (finished_emitted_) {
            create_fail = false;
            need_cancel = false;
            need_start = false;
        } else if (h == nullptr) {
            last_error_ = SYP_ERR_NETWORK;
            create_fail = true;
            retry_create = can_retry_locked();
        } else {
            handle_ = h;
            if (user_canceled_) need_cancel = true;
            else need_start = true;
        }
    }

    if (h != nullptr && !need_start && !need_cancel) {
        // 已终态，丢掉刚 create 的 handle。
        if (backend_->destroy != nullptr) backend_->destroy(h);
        return;
    }
    if (create_fail) {
        if (retry_create) {
            issue_request();
            return;
        }
        finish(SYP_ERR_NETWORK, 0);
        return;
    }
    if (need_cancel) {
        // 走 cancel() 的 pin 协议，避免与同步 on_complete/destroy 交错。
        cancel();
        return;
    }
    if (need_start && backend_->start != nullptr) backend_->start(h);
}

void DLTask::finish(syp_status st, int32_t http_status) {
    DLTaskCallbacks local{};
    {
        std::lock_guard<std::mutex> g(mu_);
        if (finished_emitted_) return;
        if (user_canceled_) {
            st = SYP_ERR_CANCELED;
            http_status = 0;
        }
        finished_emitted_ = true;
        if (st == SYP_OK) state_ = DLTaskState::Done;
        else if (st == SYP_ERR_CANCELED) state_ = DLTaskState::Canceled;
        else state_ = DLTaskState::Failed;
        last_error_ = st;
        last_http_status_ = http_status;
        local = cb_;
        callback_thread_ = std::this_thread::get_id();
        ++in_user_callback_;
        cv_.notify_all();
    }
    if (local.on_finished != nullptr) {
        local.on_finished(local.ctx, st, http_status);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        --in_user_callback_;
        cv_.notify_all();
    }
}

void DLTask::notify_meta_unlocked(int64_t total, const std::string& etag,
                                 const std::string& last_modified,
                                 bool have_total, bool have_validators) {
    DLTaskCallbacks local{};
    bool send_total = false;
    bool send_val   = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        local = cb_;
        if (have_total && !total_notified_ && total >= 0) {
            total_notified_ = true;
            send_total = true;
        }
        if (have_validators && !validators_notified_) {
            validators_notified_ = true;
            send_val = true;
        }
        if (send_total || send_val) {
            callback_thread_ = std::this_thread::get_id();
            ++in_user_callback_;
        }
    }
    if (send_total && local.on_total_length != nullptr) {
        local.on_total_length(local.ctx, total);
    }
    if (send_val && local.on_validators != nullptr) {
        local.on_validators(local.ctx, etag.c_str(), last_modified.c_str());
    }
    if (send_total || send_val) {
        std::lock_guard<std::mutex> g(mu_);
        --in_user_callback_;
        cv_.notify_all();
    }
}

void DLTask::sink_on_response(void* ctx, int32_t http_status,
                              const syp_headers* headers,
                              int64_t content_length, int64_t total_length) {
    auto* self = static_cast<DLTask*>(ctx);
    std::string etag;
    std::string lm;
    int64_t parsed_total = -1;
    bool notify_total = false;
    bool notify_val   = false;
    // 已判定丢弃响应体时要中止的句柄。锁内取、
    // 锁外调 backend_->cancel，取法与 cancel() 一致：先 ++handle_refs_ 打 pin。
    syp_http_request_handle* abort_h = nullptr;

    {
        std::lock_guard<std::mutex> g(self->mu_);
        if (self->finished_emitted_ || self->user_canceled_) return;

        self->last_http_status_ = http_status;
        const char* et = header_get(headers, "ETag");
        const char* lm_h = header_get(headers, "Last-Modified");
        if (et != nullptr) etag = et;
        if (lm_h != nullptr) lm = lm_h;

        const char* cr = header_get(headers, "Content-Range");
        if (cr != nullptr) {
            const auto parsed = detail::parse_content_range(cr);
            if (parsed.valid) {
                parsed_total = parsed.total;
                if (parsed.first >= 0) self->body_file_pos_ = parsed.first;
            }
        }

        const char* cl = header_get(headers, "Content-Length");
        int64_t parsed_cl = -1;
        if (cl != nullptr) {
            uint64_t u = 0;
            if (parse_u64_strict(cl, u)
                && u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                parsed_cl = static_cast<int64_t>(u);
            }
        }

        const HttpClass cls = classify_http_status(http_status);
        switch (cls) {
        case HttpClass::Partial206:
            // 206 = 服务端认了 Range 头。这是「支持 Range」的唯一硬证据，
            // 调度器据此才放开并发。
            self->range_confirmed_ = true;
            if (parsed_total < 0 && total_length >= 0) parsed_total = total_length;
            if (self->body_file_pos_ < 0) self->body_file_pos_ = self->attempt_range_start_;
            // Content-Range 缺失时，假定 body 从本次请求的 Range 起点开始。
            if (cr == nullptr) self->body_file_pos_ = self->attempt_range_start_;
            self->drop_body_ = false;
            break;
        case HttpClass::Full200:
            self->body_file_pos_ = 0;
            if (self->wanted_.start > 0 && !self->cfg_.allow_no_range_fallback) {
                self->has_fatal_ = true;
                self->last_error_ = SYP_ERR_RANGE_UNSUPPORTED;
                self->drop_body_ = true;
                // 光置 drop_body_ 不够：body 会照样在网线上跑完，
                // sink_on_data 只是把字节丢在地上，白付一整份文件的流量。
                // 这里主动掐断连接。
                // 用 cancel 不用 destroy：destroy 的契约是「调用前已收到
                // on_complete」，此刻还没有；而且 handle_ 是本对象自己在管
                // 的生命周期，在响应回调里 destroy 掉正在用的句柄会与
                // sink_on_complete/析构的 take_handle 路径抢释放。
                if (self->handle_ != nullptr && self->backend_ != nullptr
                    && self->backend_->cancel != nullptr) {
                    abort_h = self->handle_;
                    ++self->handle_refs_;
                }
            } else {
                // 200 的 Content-Length 就是资源总长。
                if (parsed_cl >= 0) parsed_total = parsed_cl;
                else if (content_length >= 0) parsed_total = content_length;
                self->drop_body_ = false;
            }
            break;
        case HttpClass::Unsatisfiable416:
            self->has_fatal_ = true;
            self->last_error_ = SYP_ERR_CONTENT_CHANGED;
            self->drop_body_ = true;
            break;
        case HttpClass::Retryable:
            self->last_error_ = SYP_ERR_HTTP_STATUS;
            self->drop_body_ = true;
            break;
        case HttpClass::Fatal:
            self->has_fatal_ = true;
            self->last_error_ = SYP_ERR_HTTP_STATUS;
            self->drop_body_ = true;
            break;
        }

        if (parsed_total >= 0 && self->total_length_ < 0) {
            self->total_length_ = parsed_total;
        }
        notify_total = (self->total_length_ >= 0);
        notify_val = (cls == HttpClass::Full200 || cls == HttpClass::Partial206);
        parsed_total = self->total_length_;
    }

    if (notify_total || notify_val) {
        self->notify_meta_unlocked(parsed_total, etag, lm, notify_total, notify_val);
    }

    // 放在最后：cancel 之后后端可能在本帧里就把 on_complete 合成出来，
    // 那条 on_complete 会走 has_fatal_ 分支发终态（仍是
    // SYP_ERR_RANGE_UNSUPPORTED，与不中止时完全一致），此后不该再碰 self
    // 做别的事。pin 的两个作用与 cancel() 里相同：让那条 on_complete 的
    // take_handle_for_destroy_locked() 推迟到 unpin，避免与本帧抢着释放
    // 句柄；同时挡住 ~DLTask（它等 handle_refs_ == 0）。
    if (abort_h != nullptr) {
        HandlePinGuard pin(self);
        self->backend_->cancel(abort_h);
        syp_http_request_handle* destroy_h = nullptr;
        {
            std::lock_guard<std::mutex> g(self->mu_);
            destroy_h = pin.release_locked();
        }
        if (destroy_h != nullptr && self->backend_->destroy != nullptr) {
            self->backend_->destroy(destroy_h);
        }
    }
}

void DLTask::sink_on_data(void* ctx, const uint8_t* data, int32_t len) {
    auto* self = static_cast<DLTask*>(ctx);
    if (data == nullptr || len <= 0) return;

    const uint8_t* deliver_ptr = nullptr;
    int32_t deliver_len = 0;
    int64_t deliver_off = 0;
    void (*cb)(void*, int64_t, const uint8_t*, int32_t) = nullptr;
    void* cb_ctx = nullptr;

    {
        std::lock_guard<std::mutex> g(self->mu_);
        if (self->finished_emitted_ || self->user_canceled_ || self->drop_body_) return;

        int64_t pos = self->body_file_pos_;
        const int64_t n64 = static_cast<int64_t>(len);
        // 先推进服务端 body 游标，再决定交付哪些字节。
        if (pos > std::numeric_limits<int64_t>::max() - n64) {
            self->has_fatal_ = true;
            self->last_error_ = SYP_ERR_NETWORK;
            self->drop_body_ = true;
            return;
        }
        self->body_file_pos_ = pos + n64;

        int32_t skip = 0;
        if (pos < self->next_offset_) {
            const int64_t gap = self->next_offset_ - pos;
            if (gap >= n64) return;  // 整段都是 200 降级时要丢掉的前缀
            skip = static_cast<int32_t>(gap);
            pos += gap;
        } else if (pos > self->next_offset_) {
            // 出现空洞：不能跳着交付。本轮当网络失败，由重试从 next_offset 再要。
            self->last_error_ = SYP_ERR_NETWORK;
            self->drop_body_ = true;
            return;
        }

        int32_t remain = len - skip;
        const uint8_t* p = data + skip;
        int64_t cap = static_cast<int64_t>(remain);
        if (self->wanted_.end != std::numeric_limits<int64_t>::max()) {
            const int64_t left = self->wanted_.end - self->next_offset_;
            if (left <= 0) return;
            if (cap > left) cap = left;
        }
        if (self->total_length_ >= 0) {
            const int64_t left = self->total_length_ - self->next_offset_;
            if (left <= 0) return;
            if (cap > left) cap = left;
        }
        if (cap <= 0) return;

        deliver_len = static_cast<int32_t>(cap);
        deliver_ptr = p;
        deliver_off = pos;
        self->next_offset_ += cap;
        self->received_bytes_ += cap;
        self->record_speed_locked(deliver_len);
        if (self->state_ == DLTaskState::Connecting) {
            self->state_ = DLTaskState::Receiving;
        }
        cb = self->cb_.on_data;
        cb_ctx = self->cb_.ctx;
        self->callback_thread_ = std::this_thread::get_id();
        ++self->in_user_callback_;
    }

    if (cb != nullptr && deliver_len > 0) {
        cb(cb_ctx, deliver_off, deliver_ptr, deliver_len);
    }
    {
        std::lock_guard<std::mutex> g(self->mu_);
        --self->in_user_callback_;
        self->cv_.notify_all();
    }
}

bool DLTask::sink_on_redirect(void* ctx, const char* new_url) {
    auto* self = static_cast<DLTask*>(ctx);
    std::lock_guard<std::mutex> g(self->mu_);
    if (self->finished_emitted_ || self->user_canceled_) return false;
    // 同一 URL 也照常计数，不做环路检测。
    ++self->redirect_count_;
    if (self->redirect_count_ > self->cfg_.max_redirects) {
        self->has_fatal_ = true;
        self->last_error_ = SYP_ERR_TOO_MANY_REDIRECTS;
        return false;
    }
    if (new_url != nullptr) self->url_ = new_url;
    return true;
}

void DLTask::sink_on_complete(void* ctx, syp_status status, int32_t http_status) {
    auto* self = static_cast<DLTask*>(ctx);

    syp_http_request_handle* old = nullptr;
    bool do_retry = false;
    bool do_finish = false;
    syp_status finish_st = status;
    int32_t finish_http = http_status;

    {
        std::lock_guard<std::mutex> g(self->mu_);
        old = self->take_handle_for_destroy_locked();
        if (self->finished_emitted_) {
            // 已经终态（例如析构抢先 finish 了 Idle），仍要 destroy handle。
        } else if (self->user_canceled_) {
            do_finish = true;
            finish_st = SYP_ERR_CANCELED;
            finish_http = 0;
        } else if (self->has_fatal_) {
            do_finish = true;
            finish_st = self->last_error_;
            finish_http = (finish_st == SYP_ERR_HTTP_STATUS) ? self->last_http_status_ : 0;
            if (finish_st == SYP_ERR_HTTP_STATUS && finish_http == 0) finish_http = http_status;
        } else if (self->range_satisfied_locked()) {
            do_finish = true;
            finish_st = SYP_OK;
            finish_http = self->last_http_status_;
        } else {
            syp_status err = status;
            int32_t err_http = http_status;
            if (status == SYP_OK) {
                // 服务端说完了但字节没够：短响应，可重试。
                err = SYP_ERR_NETWORK;
                err_http = 0;
            } else if (status == SYP_ERR_HTTP_STATUS) {
                const HttpClass cls = classify_http_status(http_status);
                if (cls == HttpClass::Unsatisfiable416) {
                    err = SYP_ERR_CONTENT_CHANGED;
                    err_http = 0;
                    self->has_fatal_ = true;
                } else if (cls == HttpClass::Retryable) {
                    err = SYP_ERR_HTTP_STATUS;
                    err_http = http_status;
                } else if (cls == HttpClass::Full200 || cls == HttpClass::Partial206) {
                    err = SYP_ERR_NETWORK;  // 2xx 但没下够
                    err_http = 0;
                } else {
                    err = SYP_ERR_HTTP_STATUS;
                    err_http = http_status;
                    self->has_fatal_ = true;
                }
            } else if (status == SYP_ERR_CANCELED && !self->user_canceled_) {
                // 后端因 on_redirect 返回 false 等原因取消。若已记了 TOO_MANY 走 fatal。
                if (self->last_error_ == SYP_ERR_TOO_MANY_REDIRECTS) {
                    err = SYP_ERR_TOO_MANY_REDIRECTS;
                    self->has_fatal_ = true;
                }
            }
            self->last_error_ = err;
            if (err_http != 0) self->last_http_status_ = err_http;
            // 这次尝试推进过 ⇒ 重试预算清零（见 can_retry_locked()）。
            if (self->next_offset_ > self->attempt_range_start_) self->stalled_attempts_ = 0;

            if (self->has_fatal_) {
                do_finish = true;
                finish_st = self->last_error_;
                finish_http = (finish_st == SYP_ERR_HTTP_STATUS) ? self->last_http_status_ : 0;
            } else if (status_is_retryable(err, self->last_http_status_)
                       || err == SYP_ERR_NETWORK || err == SYP_ERR_TIMEOUT) {
                if (self->can_retry_locked()) {
                    do_retry = true;
                } else {
                    do_finish = true;
                    finish_st = err;
                    finish_http = (err == SYP_ERR_HTTP_STATUS) ? self->last_http_status_ : 0;
                }
            } else {
                do_finish = true;
                finish_st = err;
                finish_http = (err == SYP_ERR_HTTP_STATUS) ? self->last_http_status_ : 0;
            }
        }
    }

    if (old != nullptr && self->backend_ != nullptr && self->backend_->destroy != nullptr) {
        self->backend_->destroy(old);
    }

    if (do_retry) {
        self->issue_request();
        return;
    }
    if (do_finish) {
        self->finish(finish_st, finish_http);
    }
}

}  // namespace syp::dl
