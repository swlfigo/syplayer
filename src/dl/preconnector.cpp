// preconnector.cpp — 设计、回收与并发约束见 preconnector.h 顶部注释。
#include "preconnector.h"

#include "dl_task.h"
#include "source_bridge.h"   // current_http_backend()

#include <atomic>
#include <utility>

namespace syp::dl {

// 定义在 source_bridge.cpp（同一静态库、同一命名空间）。source_bridge.h 里
// 也有声明；这里照 health_ticker.cpp 的写法再写一遍，读者不用跳文件确认。
void log_msg(syp_log_level lvl, const char* tag, const char* msg);

namespace {

constexpr const char* kTag = "preconnect";

// 预连接的 DLTask 配置：不重试、不测速，其余同默认。
DLTaskConfig preconnect_config() noexcept {
    DLTaskConfig cfg;
    cfg.connect_timeout_ms = 10000;
    cfg.read_timeout_ms    = 15000;
    cfg.max_retries        = 0;
    cfg.max_redirects      = 8;
    cfg.speed_window_ms    = 0;
    return cfg;
}

const syp_http_backend* current_backend_thunk(void* /*ctx*/) {
    return current_http_backend();
}

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

// 显式端口：非空、全十进制、1..65535。空串由调用方按缺省处理。
bool parse_port(std::string_view s, int32_t& out) noexcept {
    if (s.empty() || s.size() > 5) return false;
    int32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<int32_t>(c - '0');
    }
    if (v < 1 || v > 65535) return false;
    out = v;
    return true;
}

}  // namespace

namespace detail {

std::string origin_key(std::string_view url) {
    const size_t sep = url.find("://");
    if (sep == std::string_view::npos) return {};
    const std::string scheme = to_lower(url.substr(0, sep));
    int32_t port = 0;
    if (scheme == "http") {
        port = 80;
    } else if (scheme == "https") {
        port = 443;
    } else {
        return {};
    }

    std::string_view rest = url.substr(sep + 3);
    // authority 止于第一个 '/'、'?'、'#'
    const size_t auth_end = rest.find_first_of("/?#");
    std::string_view auth = rest.substr(0, auth_end);
    // 去 userinfo：取最后一个 '@' 之后（密码里可能出现 '@' 的非法写法也按最后一个切）
    const size_t at = auth.rfind('@');
    if (at != std::string_view::npos) auth = auth.substr(at + 1);

    std::string_view host;
    std::string_view port_part;
    bool has_port = false;
    if (!auth.empty() && auth.front() == '[') {
        // IPv6 字面量：[addr] 或 [addr]:port
        const size_t rb = auth.find(']');
        if (rb == std::string_view::npos || rb == 1) return {};
        host = auth.substr(0, rb + 1);
        std::string_view after = auth.substr(rb + 1);
        if (!after.empty()) {
            if (after.front() != ':') return {};
            has_port  = true;
            port_part = after.substr(1);
        }
    } else {
        const size_t colon = auth.find(':');
        host = auth.substr(0, colon);
        if (colon != std::string_view::npos) {
            has_port  = true;
            port_part = auth.substr(colon + 1);
        }
    }
    if (host.empty()) return {};
    if (has_port && !port_part.empty()) {
        if (!parse_port(port_part, port)) return {};
    }

    std::string key = scheme;
    key += "://";
    key += to_lower(host);
    key += ':';
    key += std::to_string(port);
    return key;
}

}  // namespace detail

// 回调 ctx 指向的记录。
//
// 成员顺序：task 放最后，Entry 析构时它**最先**析构——~DLTask 会等全部用户
// 回调返回，那之后才轮到回调写过的原子标志与 task_raw 结束生命周期。
//
// 回调里**只用 task_raw，不碰 task**：libc++ 的 ~unique_ptr() 走 reset()，
// 先把内部指针置空、再调 deleter。所以 ~DLTask 等在途回调结束的那段时间里，
// task 已经读出 nullptr；回调若读 task 就是数据竞争 + 空指针解引用。
// task_raw 在 start() 之前赋值、此后不再改，回调读它没有竞争。
struct Preconnector::Entry {
    std::atomic<bool> done{false};       // on_finished 已回调
    std::atomic<bool> got_data{false};   // 已收到首块数据
    // start() 已返回。回收条件是 done && start_returned：后端可以在 start()
    // 里同步回调直至 on_finished，此时 done 已真但发起线程仍在 DLTask::start
    // 里；另一线程的 preconnect() 若据 done 就析构它，就是 dl_task.h 禁止的
    // "析构与 start() 并发"。
    std::atomic<bool> start_returned{false};
    DLTask* task_raw = nullptr;          // 不可变别名，回调专用（见上）
    std::unique_ptr<DLTask> task;        // 所有权；只由持有 Entry 的线程读写
};

// 【故意泄漏，永不析构】理由同 RateLimiter::instance()：进程退出时各静态
// 对象的析构顺序不可控，后端 / 日志回调可能先没了；而且析构要等在途请求
// 结束，退出路径上不该为一个尽力而为的功能阻塞。
Preconnector& Preconnector::instance() {
    static Preconnector* p = new Preconnector(&current_backend_thunk, nullptr, system_clock());
    return *p;
}

Preconnector::Preconnector(const syp_http_backend* (*backend_fn)(void*), void* backend_ctx,
                           Clock clock)
    : backend_fn_(backend_fn), backend_ctx_(backend_ctx), clock_(clock) {}

Preconnector::~Preconnector() {
    std::vector<std::unique_ptr<Entry>> all;
    {
        std::lock_guard<std::mutex> g(mu_);
        all.swap(entries_);
    }
    // 先全部 cancel，再逐个析构：各任务并行收尾，而不是一个等完再 cancel 下一个。
    for (auto& e : all) {
        if (e && e->task_raw != nullptr) e->task_raw->cancel();
    }
    all.clear();   // ~DLTask 等终态且无回调在途
}

int64_t Preconnector::now_ms() const noexcept {
    if (clock_.now_ms == nullptr) return 0;
    return clock_.now_ms(clock_.ctx);
}

int32_t Preconnector::inflight_locked() const noexcept {
    int32_t n = 0;
    for (const auto& e : entries_) {
        if (!e->done.load(std::memory_order_acquire)) ++n;
    }
    return n;
}

// happens-before：entry->task_raw 在 preconnect() 里于 start() **之前**赋值
// 完成，回调只可能在 start() 之后发生（DLTask 在 start 里才 create/start
// 后端请求），所以这里读到的一定是已构造好的对象。
void Preconnector::cb_on_data(void* ctx, int64_t /*offset*/, const uint8_t* /*data*/,
                              int32_t /*len*/) {
    auto* e = static_cast<Entry*>(ctx);
    if (e->got_data.exchange(true, std::memory_order_acq_rel)) return;
    // 只掐断**非 206** 的响应（服务端不支持 Range、回 200 全量）：不掐就会
    // 把整个文件拉完。206 时 1 字节 body 本身就是完整响应，任务
    // 自然以 OK 结束；此时再 cancel，URLSession 可能把连接拆掉而不是还回
    // 连接池——正好毁掉预连接要的东西。
    // range_confirmed() 取 DLTask::mu_；DLTask 调用户回调时不持 mu_
    // （dl_task.h 线程安全一节），这里不会自锁。206 的判定在 on_response 里
    // 置位，先于任何 on_data。
    if (!e->task_raw->range_confirmed()) {
        e->task_raw->cancel();   // DLTask::cancel 允许在回调里调
    }
}

void Preconnector::cb_on_finished(void* ctx, syp_status /*st*/, int32_t /*http_status*/) {
    auto* e = static_cast<Entry*>(ctx);
    e->done.store(true, std::memory_order_release);
}

bool Preconnector::preconnect(std::string_view url, const syp_headers* headers) {
    const std::string key = detail::origin_key(url);
    if (key.empty()) {
        // 不记 url 本身：查询串里可能带签名。
        log_msg(SYP_LOG_DEBUG, kTag, "skip: not an http(s) url or no host");
        return false;
    }
    const syp_http_backend* be = (backend_fn_ != nullptr) ? backend_fn_(backend_ctx_) : nullptr;
    if (be == nullptr) {
        log_msg(SYP_LOG_DEBUG, kTag, "skip: no http backend registered");
        return false;
    }

    // 锁外、登记之前先拷好 url：这里抛 bad_alloc 不会留下任何半截状态。
    std::string url_copy(url);

    std::vector<std::unique_ptr<Entry>> dying;
    Entry* fresh = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        const int64_t now = now_ms();

        // ① 回收已结束的任务：移到局部，锁外析构（~DLTask 会等回调结束，
        //    回调里要写 done——虽然不取 mu_，也不在锁里做可能阻塞的事）。
        size_t keep = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            Entry* e = entries_[i].get();
            if (e->done.load(std::memory_order_acquire)
                && e->start_returned.load(std::memory_order_acquire)) {
                dying.push_back(std::move(entries_[i]));
            } else {
                if (keep != i) entries_[keep] = std::move(entries_[i]);
                ++keep;
            }
        }
        entries_.resize(keep);

        // ② 清过期去重条目：满 30 秒（>=）即过期，保证表不无界增长。
        for (auto it = last_sent_.begin(); it != last_sent_.end();) {
            if (now - it->second >= kDedupeWindowMs) {
                it = last_sent_.erase(it);
            } else {
                ++it;
            }
        }

        // ③ 去重、上限
        if (last_sent_.count(key) != 0 || inflight_locked() >= kMaxInflight) {
            fresh = nullptr;
        } else {
            auto e = std::make_unique<Entry>();
            DLTaskCallbacks cb{};
            cb.ctx             = e.get();
            cb.on_data         = &Preconnector::cb_on_data;
            cb.on_total_length = nullptr;
            cb.on_validators   = nullptr;
            cb.on_finished     = &Preconnector::cb_on_finished;
            e->task = std::make_unique<DLTask>(be, clock_, preconnect_config(), cb);
            e->task_raw = e->task.get();
            // 先登记去重再入表：入表（vector 扩容）抛了就撤掉去重条目，
            // 不会留下一个永远不 start、永远占着在途名额的 Entry。
            last_sent_[key] = now;
            try {
                entries_.push_back(std::move(e));
            } catch (...) {
                last_sent_.erase(key);
                throw;
            }
            fresh = entries_.back().get();
        }
    }
    dying.clear();   // 锁外析构已结束的 DLTask

    if (fresh == nullptr) return false;
    // 锁外 start：后端可能同步回调。fresh 此刻不会被别的线程回收
    // （start_returned 仍为假），也不会被析构（析构不与本函数并发）。
    try {
        fresh->task_raw->start(std::move(url_copy), headers, Range{0, 1});
    } catch (...) {
        // start 里能抛的只有拷贝请求头的分配（此时任务仍是 Idle）：cancel
        // 让它立即以 CANCELED 结束（done 置真），再标记 start 已返回，下一次
        // 调用即可回收——否则这个 Entry 永远占着一个在途名额。异常照常上抛，
        // 由 C 入口兜底。
        fresh->task_raw->cancel();
        fresh->start_returned.store(true, std::memory_order_release);
        throw;
    }
    fresh->start_returned.store(true, std::memory_order_release);
    return true;
}

int32_t Preconnector::inflight_for_test() const {
    std::lock_guard<std::mutex> g(mu_);
    return inflight_locked();
}

int32_t Preconnector::retained_for_test() const {
    std::lock_guard<std::mutex> g(mu_);
    return static_cast<int32_t>(entries_.size());
}

int32_t Preconnector::dedupe_entries_for_test() const {
    std::lock_guard<std::mutex> g(mu_);
    return static_cast<int32_t>(last_sent_.size());
}

}  // namespace syp::dl
