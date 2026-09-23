// apple_http_backend.mm — syp_http_backend 的 NSURLSession 实现
//
// 超时映射（NSURLSession 没有单独的连接超时，不能假装 1:1）：
//   真正执行契约的是每请求一条 GCD dispatch_source 计时器，不是 session 配置。
//   不用 dispatch_after：提交后无法取消，timer_gen_ 只能让旧 block 醒来变
//   no-op，block 本身仍活满整个 read_timeout_ms。默认 15s、高速下载每秒
//   几百个分片时会同时挂着几千个各持 weak_ptr 的待触发 block。
//   DISPATCH_SOURCE_TYPE_TIMER + dispatch_source_set_timer 重设同一条
//   source，不产生新对象；complete/destroy 时 cancel，立刻释放。
//   connect_timeout_ms
//     → 从 start() 起到 didReceiveResponse 的「到首字节」计时。
//       连不上、服务端不回包，都走这条。这不是 TCP SYN 超时。
//       若 connect_timeout_ms==0，连接阶段借用 read_timeout_ms，避免永远挂死。
//   read_timeout_ms
//     → 收到响应头后的空闲读超时，每次 didReceiveData 重置。
//       接近 timeoutIntervalForRequest 的语义（两次数据之间的静默），
//       但按请求独立，不绑在 session 配置上。
//   为什么不用 timeoutIntervalForRequest / ForResource 承载契约：
//     1. 这两个值在 NSURLSessionConfiguration 上，session 级，无法按请求不同。
//     2. timeoutIntervalForResource 实测 300ms 会拖到 ~1.2s 才触发，不适合作
//        短超时；它还是整段传输硬上限，大文件会误杀。
//     3. timeoutIntervalForRequest 本机 300ms 能触发（→ NSURLErrorTimedOut），
//        但一旦共用 session 就不能按请求配。
//   session 上把两个 interval 都设成 7 天，只当「计时器漏了」的背书。
//   NSURLRequest.timeoutInterval 一并写入较小的那个正超时（秒），但
//   NSURLSession 可能忽略它——不依赖。
//   超时后 cancel task，并以 SYP_ERR_TIMEOUT 回调（见 timed_out_ 标志，
//   因为 [task cancel] 给的是 NSURLErrorCancelled 而不是 TimedOut）。
//
// 线程模型：
//   共用一个 NSURLSession，delegateQueue 是并发 NSOperationQueue
//   （maxConcurrentOperationCount = default，即系统并发）。
//   同一请求的回调串行靠 Handle::emit_mu_：所有 sink 调用都在这把锁里，
//   包括 on_complete。并发下载不会被排成一条队。
//   代价：每次回调多一次 mutex；换来的是 N 个请求可以同时收数据。
//   若改成 queue.maxConcurrentOperationCount=1，实现更简单，但所有
//   请求的 delegate 回调全局串行，并发下载会被这一个队列卡住。
//
// cancel 在 start 之前：
//   create() 只分配 Handle，不建 NSURLSessionTask。
//   start() 才 dataTaskWithRequest + resume。
//   因此 cancel-before-start 根本碰不到 NSURLSession，必须自己合成
//   恰好一次 on_complete(SYP_ERR_CANCELED, 0)。
//   本机实测：对一个已 create、从未 resume 的 task 调 cancel，仍会走
//   didCompleteWithError(NSURLErrorCancelled / -999)。这是本机行为，
//   不是契约保证——resume 之前我们仍然自己合成；若 NSURLSession 后来
//   又回调，completed_ 挡住第二次。
//   resume() 返回后才置 resume_called_：此后 cancel 只 [task cancel]，
//   等 didComplete。resume 之前一律自己合成。
//
// 生命周期：
//   Handle 是 enable_shared_from_this，live_ 与 by_task_ 各持一份 shared_ptr。
//   syp_http_request_handle* 是 Handle* 裸指针，调用方通过 create/destroy 拥有。
//   NSURLSession 持有 Delegate 与 DataTask；DataTask 不持有 Handle。
//   Delegate 用 taskIdentifier 查 by_task_ 拿到 shared_ptr。
//   destroy 把 destroyed_ 置位、等到 inflight_==0（同线程回调内调用则不等），
//   再从两张表抹掉。之后不再回调。从未 start 的 destroy 不回调。
//   Handle 析构时 ARC 释放 DataTask 强引用。

#import <Foundation/Foundation.h>

#include "apple_http_backend.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <functional>   // std::hash<std::thread::id>：destroy 诊断里打印线程 id
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
struct Owner;
struct Handle;
}

@interface SypHttpDelegate : NSObject <NSURLSessionDataDelegate>
- (instancetype)initWithOwner:(Owner*)owner;
@end

namespace {

struct Handle;

struct Owner {
    syp_http_backend table{};
    NSURLSession* session = nil;
    SypHttpDelegate* delegate = nil;
    NSOperationQueue* queue = nil;

    std::mutex mu;
    std::unordered_map<Handle*, std::shared_ptr<Handle>> live;
    std::unordered_map<NSUInteger, std::shared_ptr<Handle>> by_task;

    void init_session();
    std::shared_ptr<Handle> handle_for_task(NSUInteger tid);
    void register_live(const std::shared_ptr<Handle>& h);
    void register_task(NSUInteger tid, const std::shared_ptr<Handle>& h);
    void forget(Handle* h, bool has_tid, NSUInteger tid);
};

struct Handle : std::enable_shared_from_this<Handle> {
    Owner* owner = nullptr;
    syp_response_sink sink{};
    std::string url;
    std::vector<std::pair<std::string, std::string>> extra_headers;
    int64_t range_start = 0;
    int64_t range_end   = -1;
    int32_t connect_timeout_ms = 0;
    int32_t read_timeout_ms    = 0;

    std::mutex state_mu_;
    std::mutex emit_mu_;
    // 嵌套顺序只允许 timer_cfg_mu_ → state_mu_，禁止反过来。
    // GCD set_event_handler / set_timer / resume / cancel 只在持有
    // timer_cfg_mu_、且不持有 state_mu_ 时调用：handler 里的 on_timer
    // 要抢 state_mu_，set_event_handler 可能等当前 handler 跑完。
    std::mutex timer_cfg_mu_;
    std::condition_variable cv_;

    bool started_        = false;
    bool resume_called_  = false;
    bool canceled_       = false;
    bool timed_out_      = false;
    bool completed_      = false;
    bool destroyed_      = false;
    bool got_response_   = false;
    bool in_callback_    = false;
    int  inflight_       = 0;
    int32_t http_status_ = 0;
    std::atomic<uint64_t> timer_gen_{0};
    std::thread::id callback_thread_{};

    // 方案 B：
    // 「本线程是否正持有本 Handle 的一个 inflight」的记账。destroy() 的逃生口
    // 用它做结构性判据 —— 任何一条进了 begin_inflight() 的路径都自动武装，
    // 不依赖谁记得去设 in_callback_。
    //
    // 必须是 per-Handle（这是 Handle 的成员），不能是全局的「线程 -> 是否在回调里」：
    // delegate 跑在共享的并发 NSOperationQueue 上，GCD 会复用线程，同一条线程
    // 在不同时刻服务不同 Handle。全局记账会串味 —— 线程 T 为 Handle A 记上的账
    // 会让 Handle B 的 destroy() 误判「调用者就在我的回调帧里」而跳过本该做的等待。
    // per-Handle + begin/end 成对增减 ⇒ T 服务 B 时，A 的表里早已没有 T。
    //
    // 用计数不用布尔：同一条线程若将来出现「同一 Handle 上嵌套两层 inflight」
    // （今天不可达，见 begin_inflight 的注释），布尔会被内层的 end 抹掉，
    // 外层帧的逃生口随即失效 —— 那正是本次要根治的那一类缺陷。
    //
    // 只在 state_mu_ 下读写（begin/end_inflight 本来就要拿这把锁，不引入新锁、
    // 不改变锁序）。条目数 = 当前在本 handle 回调里的线程数，实测 0~2，
    // 线性扫描比哈希便宜；vector 容量在第一次 push_back 之后复用，稳态零分配。
    std::vector<std::pair<std::thread::id, int>> inflight_by_thread_;

    NSURLSessionDataTask* task_ = nil;
    NSUInteger task_id_ = 0;
    bool has_task_id_   = false;
    dispatch_source_t timer_ = nil;
    bool timer_resumed_ = false;  // 仅在 timer_cfg_mu_ 下读写

    ~Handle();

    void start();
    void cancel();
    void destroy();

    bool begin_inflight();
    void end_inflight();
    // 调用方必须持有 state_mu_。
    bool holds_inflight_on_this_thread_locked() const;

    void did_receive_response(NSHTTPURLResponse* resp,
                              NSURLSessionResponseDisposition* disp);
    void did_receive_data(NSData* data);
    bool will_redirect(NSURLRequest* new_req);
    void did_complete(NSError* error);

    void arm_timer(int32_t ms);
    void bump_timer();
    void on_timer(uint64_t gen);
    dispatch_source_t steal_timer_locked();
    void cancel_timer_source(dispatch_source_t src);

    void emit_response(int32_t status, const syp_headers* headers,
                       int64_t content_length, int64_t total_length);
    void emit_data(const uint8_t* p, int32_t n);
    bool emit_redirect(const std::string& new_url);
    void emit_complete(syp_status st, int32_t http);
    bool finish_with_cancel_emit_locked(const syp_response_sink& s, bool force,
                                        __strong dispatch_source_t& src);
    void deliver_complete_locked(std::unique_lock<std::mutex>& lk,
                                 const syp_response_sink& s,
                                 syp_status st, int32_t http);
};

// ---------------------------------------------------------------- 错误码 / Content-Range

syp_status map_nsurl_error(NSError* error) {
    if (error == nil) return SYP_OK;
    if (![error.domain isEqualToString:NSURLErrorDomain]) return SYP_ERR_NETWORK;
    switch (error.code) {
    case NSURLErrorTimedOut:   return SYP_ERR_TIMEOUT;
    case NSURLErrorCancelled:  return SYP_ERR_CANCELED;
    default:                   return SYP_ERR_NETWORK;
    }
}

bool is_ws(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim_sv(std::string_view s) {
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

bool ascii_ieq(std::string_view a, std::string_view b) {
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

bool parse_u64_strict(std::string_view s, uint64_t& out) {
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

struct ParsedCR {
    bool valid = false;
    int64_t first = -1;
    int64_t last  = -1;
    int64_t total = -1;
};

ParsedCR parse_content_range(std::string_view header) {
    ParsedCR out;
    std::string_view s = trim_sv(header);
    if (s.size() < 5) return out;
    if (!ascii_ieq(s.substr(0, 5), "bytes")) return out;
    s.remove_prefix(5);
    if (s.empty() || !is_ws(s.front())) return out;
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);

    const size_t slash = s.find('/');
    if (slash == std::string_view::npos) return out;
    const std::string_view range = trim_sv(s.substr(0, slash));
    const std::string_view total = trim_sv(s.substr(slash + 1));
    if (range.empty() || total.empty()) return out;

    if (range == "*") {
        out.first = -1;
        out.last  = -1;
    } else {
        const size_t dash = range.find('-');
        if (dash == std::string_view::npos) return out;
        uint64_t u1 = 0, u2 = 0;
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

std::string ns_to_std(NSString* s) {
    if (s == nil) return {};
    const char* p = s.UTF8String;
    return p ? std::string(p) : std::string();
}

int32_t clamp_http_status(NSInteger sc) {
    if (sc > static_cast<NSInteger>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    if (sc < static_cast<NSInteger>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    return static_cast<int32_t>(sc);
}

NSTimeInterval smaller_positive_timeout_sec(int32_t a_ms, int32_t b_ms) {
    int32_t ms = 0;
    if (a_ms > 0) ms = a_ms;
    if (b_ms > 0 && (ms == 0 || b_ms < ms)) ms = b_ms;
    if (ms <= 0) return 60.0;
    return static_cast<NSTimeInterval>(ms) / 1000.0;
}

// ---------------------------------------------------------------- Owner

void Owner::init_session() {
    queue = [NSOperationQueue new];
    queue.name = @"syp.http.apple";
    queue.maxConcurrentOperationCount = NSOperationQueueDefaultMaxConcurrentOperationCount;

    delegate = [[SypHttpDelegate alloc] initWithOwner:this];

    NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest  = 7.0 * 24.0 * 3600.0;
    cfg.timeoutIntervalForResource = 7.0 * 24.0 * 3600.0;
    cfg.URLCache = nil;
    cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    cfg.HTTPCookieStorage = nil;
    cfg.HTTPShouldSetCookies = NO;
    cfg.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    cfg.HTTPMaximumConnectionsPerHost = 8;

    session = [NSURLSession sessionWithConfiguration:cfg
                                            delegate:delegate
                                       delegateQueue:queue];
}

std::shared_ptr<Handle> Owner::handle_for_task(NSUInteger tid) {
    std::lock_guard<std::mutex> g(mu);
    auto it = by_task.find(tid);
    if (it == by_task.end()) return nullptr;
    return it->second;
}

void Owner::register_live(const std::shared_ptr<Handle>& h) {
    std::lock_guard<std::mutex> g(mu);
    live[h.get()] = h;
}

void Owner::register_task(NSUInteger tid, const std::shared_ptr<Handle>& h) {
    std::lock_guard<std::mutex> g(mu);
    by_task[tid] = h;
}

void Owner::forget(Handle* h, bool has_tid, NSUInteger tid) {
    std::lock_guard<std::mutex> g(mu);
    live.erase(h);
    if (has_tid) by_task.erase(tid);
}

// ---------------------------------------------------------------- Handle inflight

bool Handle::holds_inflight_on_this_thread_locked() const {
    const std::thread::id self = std::this_thread::get_id();
    for (const auto& e : inflight_by_thread_) {
        if (e.first == self) return e.second > 0;
    }
    return false;
}

bool Handle::begin_inflight() {
    std::lock_guard<std::mutex> g(state_mu_);
    if (destroyed_ || completed_) return false;
    ++inflight_;
    // 同线程重入：今天不可达 —— begin_inflight 只有四条 delegate 回调调用，
    // NSOperationQueue 的一次 operation 不会在自己的线程上嵌套执行另一条，
    // 且 didReceiveResponse 的 completionHandler 在 end_inflight 之后才调。
    // 但计数的成本是一个 int，布尔一旦哪天真的嵌套就会静默失效，
    // 所以这里按计数记账（见成员声明处的注释）。
    const std::thread::id self = std::this_thread::get_id();
    for (auto& e : inflight_by_thread_) {
        if (e.first == self) {
            ++e.second;
            return true;
        }
    }
    inflight_by_thread_.emplace_back(self, 1);
    return true;
}

void Handle::end_inflight() {
    std::lock_guard<std::mutex> g(state_mu_);
    --inflight_;
    const std::thread::id self = std::this_thread::get_id();
    for (size_t i = 0; i < inflight_by_thread_.size(); ++i) {
        if (inflight_by_thread_[i].first != self) continue;
        if (--inflight_by_thread_[i].second <= 0) {
            // swap-and-pop：条目数极小，顺序无意义；pop_back 保留容量，
            // 稳态（0↔1 来回）不再分配。
            inflight_by_thread_[i] = inflight_by_thread_.back();
            inflight_by_thread_.pop_back();
        }
        break;
    }
    cv_.notify_all();
}

Handle::~Handle() {
    if (timer_ != nil) {
        dispatch_source_cancel(timer_);
        timer_ = nil;
    }
}

dispatch_source_t Handle::steal_timer_locked() {
    dispatch_source_t src = timer_;
    timer_ = nil;
    return src;
}

void Handle::cancel_timer_source(dispatch_source_t src) {
    if (src == nil) return;
    std::lock_guard<std::mutex> cfg(timer_cfg_mu_);
    dispatch_source_cancel(src);
}

void Handle::arm_timer(int32_t ms) {
    if (ms <= 0) return;
    dispatch_source_t src = nil;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (completed_ || destroyed_) return;
        if (timer_ == nil) {
            // 新建时保持 suspended，set_timer 之后再 resume，避免默认
            // fire 时间 0 立刻触发。
            timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
        }
        src = timer_;
    }
    // generation 的分配与 set_timer 同在 timer_cfg_mu_ 下：另一个
    // arm_timer / bump_timer 必须等本次配完 source 才能再 ++timer_gen_。
    // 不能在持有 state_mu_ 时调 GCD（on_timer 要抢同一把锁）。
    uint64_t gen = 0;
    std::lock_guard<std::mutex> cfg(timer_cfg_mu_);
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (completed_ || destroyed_) return;
        if (timer_ != src) return;  // 已被 steal
        gen = ++timer_gen_;
    }
    if (dispatch_source_testcancel(src) != 0) return;
    std::weak_ptr<Handle> weak = shared_from_this();
    dispatch_source_set_event_handler(src, ^{
        auto sp = weak.lock();
        if (sp) sp->on_timer(gen);
    });
    const int64_t nsec = static_cast<int64_t>(ms) * 1000000LL;
    dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, nsec),
                              DISPATCH_TIME_FOREVER, 0);
    if (!timer_resumed_) {
        dispatch_resume(src);
        timer_resumed_ = true;
    }
}

void Handle::bump_timer() {
    std::lock_guard<std::mutex> cfg(timer_cfg_mu_);
    std::lock_guard<std::mutex> g(state_mu_);
    ++timer_gen_;
}

void Handle::on_timer(uint64_t gen) {
    NSURLSessionDataTask* t = nil;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (gen != timer_gen_.load(std::memory_order_relaxed)
            || completed_ || destroyed_) return;
        timed_out_ = true;
        t = task_;
    }
    if (t != nil) [t cancel];
    emit_complete(SYP_ERR_TIMEOUT, 0);
}

void Handle::start() {
    bool synth = false;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (destroyed_ || completed_ || started_) return;
        started_ = true;
        if (canceled_) synth = true;
    }
    if (synth) {
        emit_complete(SYP_ERR_CANCELED, 0);
        return;
    }

    NSString* url_ns = [[NSString alloc] initWithBytes:url.data()
                                                length:url.size()
                                              encoding:NSUTF8StringEncoding];
    NSURL* nsurl = url_ns != nil ? [NSURL URLWithString:url_ns] : nil;
    if (nsurl == nil) {
        emit_complete(SYP_ERR_INVALID_ARG, 0);
        return;
    }

    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsurl];
    req.HTTPMethod = @"GET";
    req.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    req.timeoutInterval = smaller_positive_timeout_sec(connect_timeout_ms,
                                                       read_timeout_ms);

    for (const auto& hv : extra_headers) {
        if (hv.first.empty()) continue;
        NSString* k = [[NSString alloc] initWithBytes:hv.first.data()
                                               length:hv.first.size()
                                             encoding:NSUTF8StringEncoding];
        NSString* v = [[NSString alloc] initWithBytes:hv.second.data()
                                               length:hv.second.size()
                                             encoding:NSUTF8StringEncoding];
        if (k != nil) [req setValue:(v != nil ? v : @"") forHTTPHeaderField:k];
    }

    char range_buf[80];
    if (range_end < 0) {
        std::snprintf(range_buf, sizeof(range_buf),
                      "bytes=%" PRId64 "-", range_start);
    } else {
        std::snprintf(range_buf, sizeof(range_buf),
                      "bytes=%" PRId64 "-%" PRId64, range_start, range_end);
    }
    NSString* range_ns = [[NSString alloc] initWithUTF8String:range_buf];
    [req setValue:range_ns forHTTPHeaderField:@"Range"];
    // 缓存按字节偏移分片拼接，必须拿原始字节：NSURLSession 默认自动带
    // Accept-Encoding: gzip 并透明解压，但 expectedContentLength/Content-Range
    // 仍是压缩后的长度——下载层按 623 字节记账、交出去的却是解压后的前 623
    // 字节，播放列表被截断，FFmpeg 报 Invalid data（twimg HLS 实测）。放在
    // extra_headers 之后，调用方不能覆盖掉。
    [req setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];

    NSURLSessionDataTask* t = [owner->session dataTaskWithRequest:req];
    const NSUInteger tid = t.taskIdentifier;
    owner->register_task(tid, shared_from_this());

    bool cancel_now = false;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        task_ = t;
        task_id_ = tid;
        has_task_id_ = true;
        if (canceled_ || destroyed_) cancel_now = true;
    }
    if (cancel_now) {
        [t cancel];
        emit_complete(SYP_ERR_CANCELED, 0);
        return;
    }

    [t resume];

    bool cancel_after = false;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        resume_called_ = true;
        if (canceled_ || destroyed_) cancel_after = true;
    }
    if (cancel_after) {
        [t cancel];
        // resume 已返回：didComplete 会来。若已 completed_ 则那次是 no-op。
        return;
    }

    int32_t first_ms = connect_timeout_ms;
    if (first_ms <= 0) first_ms = read_timeout_ms;
    arm_timer(first_ms);
}

void Handle::cancel() {
    NSURLSessionDataTask* t = nil;
    bool synth = false;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        canceled_ = true;
        if (completed_ || destroyed_) return;
        t = task_;
        // resume() 返回前 NSURLSession 不保证 didComplete；自己合成。
        //
        // 但不能在「本线程正在本 handle 的回调里」时合成：emit_response /
        // emit_data / emit_redirect 全程持有 emit_mu_，而 emit_complete 又要
        // 拿同一把（非递归）锁 —— 同线程重入即 UB。
        // start() 在 [t resume] 返回之后才置 resume_called_（见上面两处），
        // 这中间的窄窗口里 didReceiveResponse / willPerformHTTPRedirection
        // 可以落地，sink 在回调里调 cancel 就会踩到。
        // 此时不合成是安全的：外层收尾 finish_with_cancel_emit_locked() 见到
        // canceled_ 必定合成恰好一次 on_complete（见那里的注释）。
        // 从 on_complete 里调 cancel 则更早——completed_ 已置位，上面就 return 了。
        //
        // 判据与 destroy() 用同一个并集，理由相同：in_callback_ 只覆盖记得
        // 记账的路径，而「本线程持有本 Handle 的 inflight」是结构性事实 ——
        // 走到这里的 delegate 线程必然是经由某个 emit_*（emit_mu_ 已被本线程
        // 持有）才回到 sink、再回到这里的，重入 emit_complete 就是 UB。
        const bool in_own_callback =
            holds_inflight_on_this_thread_locked() ||
            (in_callback_ && callback_thread_ == std::this_thread::get_id());
        if (!resume_called_ && !in_own_callback) synth = true;
    }
    if (t != nil) [t cancel];
    if (synth) emit_complete(SYP_ERR_CANCELED, 0);
}

void Handle::destroy() {
    NSURLSessionDataTask* t = nil;
    dispatch_source_t src = nil;
    bool has_tid = false;
    NSUInteger tid = 0;
    {
        std::unique_lock<std::mutex> lk(state_mu_);
        destroyed_ = true;
        ++timer_gen_;
        src = steal_timer_locked();
        t = task_;
        task_ = nil;
        has_tid = has_task_id_;
        tid = task_id_;
        has_task_id_ = false;
        // 契约（syp_http.h）：destroy 前调用方已收到 on_complete。
        // 正常路径 inflight_ 已在 end_inflight 里降到 0，这里的 wait
        // 只等可能还在 begin_inflight/end_inflight 窗口里的 delegate。
        // 同线程回调内调用则跳过，避免等自己返回 —— 允许 sink 在 on_complete
        // 里直接 destroy（dl 层就是这么用的）。
        //
        // 逃生口的判据是两条的并集，缺一不可：
        //
        //  (B) 本线程正持有本 Handle 的一个 inflight。这是**结构性事实**：
        //      只要 destroy 的调用者所在的这条栈上有一层 begin_inflight()，
        //      它要等的 inflight_ 里就有一份是它自己下方那一帧持有的，
        //      等待在定义上不可能被满足。四条 delegate 回调（以及将来任何
        //      新加的、无论有没有记得设 in_callback_）都自动落在这一条里。
        //  (A') in_callback_ && callback_thread_ == 本线程。B 覆盖不到
        //      **不经过 begin_inflight 的 sink 投递路径**，今天有三条：
        //      Handle::cancel() 的 cancel-before-resume 合成、Handle::start()
        //      的 cancel-before-start 合成、on_timer() 的超时合成 ——
        //      它们都在调用方/GCD 定时器线程上直接调 emit_complete()，
        //      栈上没有任何 inflight。sink 在那次 on_complete 里 destroy
        //      时只有这一条判据能救。所以 in_callback_/callback_thread_
        //      不能因为有了 B 就删掉。
        //
        // 调用方违约或 sink 卡死时会一直等。不能超时后继续往下析构：
        // forget 掉 live_ 之后 inflight 回调会 UAF。超时只打诊断，然后继续等。
        const bool own_inflight = holds_inflight_on_this_thread_locked();
        const bool in_own_callback =
            in_callback_ && callback_thread_ == std::this_thread::get_id();
        const bool same_thread = own_inflight || in_own_callback;
        if (!same_thread) {
            const std::hash<std::thread::id> tid_hash{};
            const unsigned long long self_id =
                static_cast<unsigned long long>(tid_hash(std::this_thread::get_id()));
            while (!cv_.wait_for(lk, std::chrono::seconds(2),
                                 [this] { return inflight_ == 0 && !in_callback_; })) {
                const unsigned long long cb_id =
                    static_cast<unsigned long long>(tid_hash(callback_thread_));
                std::fprintf(stderr,
                    "syp_apple_http: destroy still waiting inflight=%d in_callback=%d "
                    "own_inflight=0 this_thread=%llu callback_thread=%llu"
                    "（等的是别的线程的回调帧：本线程既不持有本 handle 的 inflight，"
                    "也不是 in_callback_ 记的那条线程。若这两个 id 相同，说明有一条"
                    "既不走 begin_inflight()、也没记 in_callback_ 的 sink 投递路径——"
                    "那是自死锁）\n",
                    inflight_, static_cast<int>(in_callback_), self_id, cb_id);
            }
        }
    }
    cancel_timer_source(src);
    if (t != nil) [t cancel];
    if (owner != nullptr) owner->forget(this, has_tid, tid);
}

// on_complete 的唯一投递点：置 in_callback_/callback_thread_ → 放 state_mu_
// 调 sink.on_complete → 重新拿锁清零 → notify。emit_complete() 与
// finish_with_cancel_emit_locked() 两条路径共用这一份记账，避免改一处漏一处。
//
// 调用约定：调用方持有 emit_mu_，并持有 lk（state_mu_），且已经在**同一个
// 临界区里**把 completed_ 置好 —— 置位与武装记账不能分成两次加锁，否则中间
// 那道缝里别的线程的 destroy() 会看到「completed_ 已置、in_callback_ 未置、
// inflight_ 为 0」而径直返回，随后我们再去碰 sink，就违反了「destroy 返回后
// 后端不得再触碰 sink」。返回时 lk 仍持有锁。
//
// 记账为什么必须跨过 s.on_complete：契约允许 sink 在 on_complete 里同线程调
// destroy（dl 层的 DLTask::sink_on_complete 就是这么做的），destroy() 的逃生口
// 靠 in_callback_/callback_thread_ 识别「调用者就在本帧里」。曾经的写法是先
// in_callback_ = false 再合成 on_complete，逃生口没被武装，destroy() 于是去等
// 自己下方那一帧持有的 inflight_ —— 单线程自死锁。
// （走 begin_inflight 的 delegate 路径另有方案 B 的判据兜底，但 cancel()/
// start()/on_timer() 三条合成路径栈上没有 inflight，只有这份记账能救。）
void Handle::deliver_complete_locked(std::unique_lock<std::mutex>& lk,
                                     const syp_response_sink& s,
                                     syp_status st, int32_t http) {
    in_callback_ = true;
    callback_thread_ = std::this_thread::get_id();
    lk.unlock();
    if (s.on_complete != nullptr) {
        s.on_complete(s.ctx, st, http);
    }
    lk.lock();
    in_callback_ = false;
    cv_.notify_all();
}

void Handle::emit_complete(syp_status st, int32_t http) {
    syp_response_sink s{};
    dispatch_source_t src = nil;
    {
        std::lock_guard<std::mutex> emit(emit_mu_);
        std::unique_lock<std::mutex> lk(state_mu_);
        if (completed_ || destroyed_) return;
        completed_ = true;
        // complete/destroy 在 state_mu_ 下 ++timer_gen_ 并 steal。
        // 不能在持有 state_mu_ 时再拿 timer_cfg_mu_（与 arm_timer 的
        // timer_cfg_mu_ → state_mu_ 相反，会死锁）。completed_ 已置位，
        // 即便 arm_timer 在本锁与 set_timer 之间配上旧 gen，on_timer
        // 见 completed_ 直接返回，随后 cancel_timer_source 拆掉 source。
        ++timer_gen_;
        src = steal_timer_locked();
        s = sink;
        deliver_complete_locked(lk, s, st, http);
    }
    cancel_timer_source(src);
}

// on_response / on_data / on_redirect 返回后的统一收尾：若回调期间被取消
// （或 sink 拒绝了重定向，force=true），就在同一条回调栈上合成恰好一次
// on_complete(SYP_ERR_CANCELED)；否则只是退出回调记账。
//
// 调用约定：调用方已持有 emit_mu_、且不持有 state_mu_，并且已经把
// in_callback_/callback_thread_ 置成当前线程（emit_* 的入口做的）。
// 返回是否合成了 on_complete；被偷走的计时器 source 从 src 带出（由调用方在
// emit_mu_ 之外 cancel_timer_source）。
bool Handle::finish_with_cancel_emit_locked(const syp_response_sink& s, bool force,
                                            __strong dispatch_source_t& src) {
    std::unique_lock<std::mutex> lk(state_mu_);
    if (!((force || canceled_) && !completed_ && !destroyed_)) {
        // 没有要合成的终态：照常退出回调记账，唤醒可能在等的 destroy。
        in_callback_ = false;
        cv_.notify_all();
        return false;
    }
    completed_ = true;
    canceled_  = true;
    ++timer_gen_;
    src = steal_timer_locked();
    // 与 emit_complete() 共用同一份记账：in_callback_ 跨过 on_complete
    // 保持为 true（调用方已置好，这里再置一次是幂等的），返回后才清零。
    deliver_complete_locked(lk, s, SYP_ERR_CANCELED, 0);
    return true;
}

void Handle::emit_response(int32_t status, const syp_headers* headers,
                           int64_t content_length, int64_t total_length) {
    syp_response_sink s{};
    dispatch_source_t src = nil;
    {
        std::lock_guard<std::mutex> emit(emit_mu_);
        {
            std::lock_guard<std::mutex> g(state_mu_);
            if (completed_ || destroyed_ || canceled_) return;
            in_callback_ = true;
            callback_thread_ = std::this_thread::get_id();
            s = sink;
        }
        if (s.on_response != nullptr) {
            s.on_response(s.ctx, status, headers, content_length, total_length);
        }
        finish_with_cancel_emit_locked(s, /*force=*/false, src);
    }
    cancel_timer_source(src);
}

void Handle::emit_data(const uint8_t* p, int32_t n) {
    syp_response_sink s{};
    dispatch_source_t src = nil;
    {
        std::lock_guard<std::mutex> emit(emit_mu_);
        {
            std::lock_guard<std::mutex> g(state_mu_);
            if (completed_ || destroyed_ || canceled_) return;
            in_callback_ = true;
            callback_thread_ = std::this_thread::get_id();
            s = sink;
        }
        if (s.on_data != nullptr && p != nullptr && n > 0) {
            s.on_data(s.ctx, p, n);
        }
        finish_with_cancel_emit_locked(s, /*force=*/false, src);
    }
    cancel_timer_source(src);
}

bool Handle::emit_redirect(const std::string& new_url) {
    syp_response_sink s{};
    bool follow = true;
    bool do_complete = false;
    dispatch_source_t src = nil;
    {
        std::lock_guard<std::mutex> emit(emit_mu_);
        {
            std::lock_guard<std::mutex> g(state_mu_);
            if (completed_ || destroyed_ || canceled_) return false;
            in_callback_ = true;
            callback_thread_ = std::this_thread::get_id();
            s = sink;
        }
        if (s.on_redirect != nullptr) {
            follow = s.on_redirect(s.ctx, new_url.c_str());
        }
        do_complete = finish_with_cancel_emit_locked(s, /*force=*/!follow, src);
        // sink 返回 true 但期间被取消：也不能跟随。
        if (do_complete) follow = false;
    }
    cancel_timer_source(src);
    if (do_complete) {
        NSURLSessionDataTask* t = nil;
        {
            std::lock_guard<std::mutex> g(state_mu_);
            t = task_;
        }
        if (t != nil) [t cancel];
    }
    return follow;
}

void Handle::did_receive_response(NSHTTPURLResponse* resp,
                                  NSURLSessionResponseDisposition* disp) {
    *disp = NSURLSessionResponseAllow;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (completed_ || destroyed_ || canceled_) {
            *disp = NSURLSessionResponseCancel;
            return;
        }
        got_response_ = true;
        http_status_ = clamp_http_status(resp.statusCode);
    }
    bump_timer();
    if (read_timeout_ms > 0) arm_timer(read_timeout_ms);

    int32_t status = 0;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        status = http_status_;
    }

    std::vector<std::string> names;
    std::vector<std::string> values;
    std::vector<const char*> name_c;
    std::vector<const char*> value_c;
    NSDictionary* fields = resp.allHeaderFields;
    if (fields != nil) {
        names.reserve(static_cast<size_t>(fields.count));
        values.reserve(static_cast<size_t>(fields.count));
        for (id key in fields) {
            NSString* ks = [key isKindOfClass:[NSString class]]
                               ? (NSString*)key
                               : [key description];
            id obj = fields[key];
            NSString* vs = [obj isKindOfClass:[NSString class]]
                               ? (NSString*)obj
                               : [obj description];
            names.push_back(ns_to_std(ks));
            values.push_back(ns_to_std(vs));
        }
    }
    name_c.reserve(names.size());
    value_c.reserve(values.size());
    for (size_t i = 0; i < names.size(); ++i) {
        name_c.push_back(names[i].c_str());
        value_c.push_back(values[i].c_str());
    }
    syp_headers hdrs{};
    hdrs.names  = name_c.empty() ? nullptr : name_c.data();
    hdrs.values = value_c.empty() ? nullptr : value_c.data();
    hdrs.count  = static_cast<int32_t>(names.size());

    // 兜底：服务端无视 Accept-Encoding: identity 仍压缩时，NSURLSession 交出的是解压后
    // 的字节，响应里的长度（Content-Length / Content-Range）描述的却是压缩表示——两者
    // 对不上，一律按"长度未知"上报，不能拿去记账。
    bool encoded = false;
    if (fields != nil) {
        for (NSString* k in fields) {
            if ([k caseInsensitiveCompare:@"Content-Encoding"] != NSOrderedSame) continue;
            id obj = fields[k];
            NSString* v = [obj isKindOfClass:[NSString class]] ? (NSString*)obj : [obj description];
            NSString* t = [v stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            encoded = t.length > 0 && [t caseInsensitiveCompare:@"identity"] != NSOrderedSame;
            break;
        }
    }

    int64_t content_length = -1;
    const long long expected = resp.expectedContentLength;
    if (!encoded && expected != NSURLResponseUnknownLength && expected >= 0) {
        content_length = static_cast<int64_t>(expected);
    }

    int64_t total_length = -1;
    NSString* cr = nil;
    if (fields != nil && !encoded) {
        cr = fields[@"Content-Range"];
        if (cr == nil) {
            for (NSString* k in fields) {
                if ([k caseInsensitiveCompare:@"Content-Range"] == NSOrderedSame) {
                    id obj = fields[k];
                    cr = [obj isKindOfClass:[NSString class]] ? (NSString*)obj
                                                              : [obj description];
                    break;
                }
            }
        }
    }
    if (cr != nil) {
        const ParsedCR parsed = parse_content_range(ns_to_std(cr));
        if (parsed.valid) {
            total_length = parsed.total;
            if (content_length < 0 && parsed.first >= 0 && parsed.last >= parsed.first) {
                content_length = parsed.last - parsed.first + 1;
            }
        }
    }

    emit_response(status, &hdrs, content_length, total_length);
}

void Handle::did_receive_data(NSData* data) {
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (completed_ || destroyed_ || canceled_) return;
    }
    if (read_timeout_ms > 0) arm_timer(read_timeout_ms);

    const uint8_t* p = static_cast<const uint8_t*>(data.bytes);
    NSUInteger left = data.length;
    while (left > 0) {
        {
            std::lock_guard<std::mutex> g(state_mu_);
            if (completed_ || destroyed_ || canceled_) return;
        }
        const int32_t n = (left > 1u << 30) ? (1 << 30) : static_cast<int32_t>(left);
        emit_data(p, n);
        p += static_cast<size_t>(n);
        left -= static_cast<NSUInteger>(n);
    }
}

bool Handle::will_redirect(NSURLRequest* new_req) {
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (completed_ || destroyed_ || canceled_) return false;
    }
    const std::string u = ns_to_std(new_req.URL.absoluteString);
    return emit_redirect(u);
}

void Handle::did_complete(NSError* error) {
    syp_status st = SYP_OK;
    int32_t http = 0;
    {
        std::lock_guard<std::mutex> g(state_mu_);
        if (completed_ || destroyed_) return;
        if (canceled_) {
            st = SYP_ERR_CANCELED;
            http = 0;
        } else if (timed_out_) {
            st = SYP_ERR_TIMEOUT;
            http = 0;
        } else if (error != nil) {
            st = map_nsurl_error(error);
            http = 0;
        } else if (http_status_ >= 400) {
            st = SYP_ERR_HTTP_STATUS;
            http = http_status_;
        } else {
            st = SYP_OK;
            http = 0;
        }
    }
    emit_complete(st, http);
}

// ---------------------------------------------------------------- trampolines

syp_http_request_handle* trampoline_create(void* ctx,
                                           const syp_http_request* req,
                                           const syp_response_sink* sink) {
    auto* owner = static_cast<Owner*>(ctx);
    if (owner == nullptr || req == nullptr || sink == nullptr) return nullptr;
    auto h = std::make_shared<Handle>();
    h->owner = owner;
    h->sink  = *sink;
    h->url   = req->url ? req->url : "";
    h->range_start = req->range_start;
    h->range_end   = req->range_end;
    h->connect_timeout_ms = req->connect_timeout_ms;
    h->read_timeout_ms    = req->read_timeout_ms;
    if (req->headers.names != nullptr && req->headers.values != nullptr) {
        for (int32_t i = 0; i < req->headers.count; ++i) {
            const char* n = req->headers.names[i];
            const char* v = req->headers.values[i];
            h->extra_headers.emplace_back(n ? n : "", v ? v : "");
        }
    }
    owner->register_live(h);
    return reinterpret_cast<syp_http_request_handle*>(h.get());
}

std::shared_ptr<Handle> lock_live(Owner* owner, syp_http_request_handle* raw) {
    if (owner == nullptr || raw == nullptr) return nullptr;
    auto* p = reinterpret_cast<Handle*>(raw);
    std::lock_guard<std::mutex> g(owner->mu);
    auto it = owner->live.find(p);
    if (it == owner->live.end()) return nullptr;
    return it->second;
}

void trampoline_start(syp_http_request_handle* raw) {
    // backend_ctx 在 table 上；从 Handle 取 owner。
    auto* p = reinterpret_cast<Handle*>(raw);
    if (p == nullptr || p->owner == nullptr) return;
    auto h = lock_live(p->owner, raw);
    if (!h) return;
    h->start();
}

void trampoline_cancel(syp_http_request_handle* raw) {
    auto* p = reinterpret_cast<Handle*>(raw);
    if (p == nullptr || p->owner == nullptr) return;
    auto h = lock_live(p->owner, raw);
    if (!h) return;
    h->cancel();
}

void trampoline_destroy(syp_http_request_handle* raw) {
    auto* p = reinterpret_cast<Handle*>(raw);
    if (p == nullptr || p->owner == nullptr) return;
    auto h = lock_live(p->owner, raw);
    if (!h) return;
    h->destroy();
}

}  // namespace

@implementation SypHttpDelegate {
    Owner* _owner;
}

- (instancetype)initWithOwner:(Owner*)owner {
    self = [super init];
    if (self) _owner = owner;
    return self;
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
didReceiveResponse:(NSURLResponse*)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session;
    NSURLSessionResponseDisposition disp = NSURLSessionResponseAllow;
    auto h = _owner != nullptr ? _owner->handle_for_task(task.taskIdentifier) : nullptr;
    if (h && h->begin_inflight()) {
        NSHTTPURLResponse* http = [response isKindOfClass:[NSHTTPURLResponse class]]
                                      ? (NSHTTPURLResponse*)response
                                      : nil;
        if (http != nil) {
            h->did_receive_response(http, &disp);
        }
        h->end_inflight();
    }
    completionHandler(disp);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data {
    (void)session;
    auto h = _owner != nullptr ? _owner->handle_for_task(task.taskIdentifier) : nullptr;
    if (h && h->begin_inflight()) {
        h->did_receive_data(data);
        h->end_inflight();
    }
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
willPerformHTTPRedirection:(NSHTTPURLResponse*)response
        newRequest:(NSURLRequest*)request
 completionHandler:(void (^)(NSURLRequest* _Nullable))completionHandler {
    (void)session;
    (void)response;
    BOOL follow = YES;
    auto h = _owner != nullptr ? _owner->handle_for_task(task.taskIdentifier) : nullptr;
    if (h && h->begin_inflight()) {
        follow = h->will_redirect(request) ? YES : NO;
        h->end_inflight();
    }
    completionHandler(follow ? request : nil);
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
didCompleteWithError:(NSError*)error {
    (void)session;
    auto h = _owner != nullptr ? _owner->handle_for_task(task.taskIdentifier) : nullptr;
    if (h && h->begin_inflight()) {
        h->did_complete(error);
        h->end_inflight();
    }
}

@end

const syp_http_backend* syp_apple_http_backend(void) {
    static std::once_flag once;
    static Owner* owner = nullptr;
    static syp_http_backend table{};
    std::call_once(once, [] {
        owner = new Owner();
        owner->init_session();
        table.backend_ctx = owner;
        table.create  = trampoline_create;
        table.start   = trampoline_start;
        table.cancel  = trampoline_cancel;
        table.destroy = trampoline_destroy;
    });
    return &table;
}
