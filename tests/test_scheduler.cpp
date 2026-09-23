// test_scheduler.cpp — Scheduler 分片/并发/复用启发式/降级，全部走同步泵 + 假时钟
#include "tiny_test.h"
#include "support/stub_backend.h"
#include "support/watchdog.h"

#include <dl/clock.h>
#include <dl/health_ticker.h>
#include <dl/hole_set.h>
#include <dl/rate_limiter.h>
#include <dl/scheduler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using syp::dl::Clock;
using syp::dl::HoleSet;
using syp::dl::Range;
using syp::dl::Scheduler;
using syp::dl::SchedulerCallbacks;
using syp::dl::SchedulerConfig;
using syp::dl::test::StubBackend;
using syp::dl::test::synthetic_byte;

namespace syp::dl {

std::ostream& operator<<(std::ostream& os, const Range& r) {
    return os << "[" << r.start << ", " << r.end << ")";
}

}  // namespace syp::dl

namespace {

struct FakeClock {
    int64_t t = 0;
    static int64_t now(void* ctx) {
        return static_cast<FakeClock*>(ctx)->t;
    }
    Clock clock() { return Clock{&now, this}; }
    void advance(int64_t ms) { t += ms; }
};

Range req_range(const StubBackend::CapturedRequest& r, int64_t eof) {
    const int64_t s = r.range_start < 0 ? 0 : r.range_start;
    int64_t e = eof;
    if (r.range_end >= 0) {
        if (r.range_end >= std::numeric_limits<int64_t>::max()) e = eof;
        else e = r.range_end + 1;
    }
    return Range{s, e};
}

bool ranges_overlap(Range a, Range b) {
    const int64_t s = a.start > b.start ? a.start : b.start;
    const int64_t e = a.end < b.end ? a.end : b.end;
    return s < e;
}

bool any_overlap(const std::vector<Range>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        for (size_t j = i + 1; j < v.size(); ++j) {
            if (ranges_overlap(v[i], v[j])) return true;
        }
    }
    return false;
}

void check_stub(const StubBackend& s) {
    std::string msg;
    if (!s.contract_ok(&msg)) {
        tiny_test::fail(__FILE__, __LINE__, "stub.contract_ok()", msg);
    }
}

SchedulerConfig cfg_small() {
    SchedulerConfig c;
    c.max_concurrent_tasks = 1;
    c.min_segment_size = 1;
    c.segment_size_hint = 0;
    c.reuse_max_remaining_bytes = 1 << 20;
    c.connect_estimate_max_ms = 2000;
    c.max_consecutive_errors = 5;
    c.allow_no_range_fallback = true;
    c.task.max_retries = 3;
    c.task.speed_window_ms = 3000;
    c.task.allow_no_range_fallback = false;
    // 既有用例不关心坏任务检测，而默认 ticker 是进程单例（真实线程、
    // 真实时钟），会在同步桩用例里从另一条线程插进 health_check。关掉；
    // 检测用例显式打开并注入独立 ticker。
    c.health.enabled = false;
    return c;
}

StubBackend::Script script_ok(int64_t n) {
    StubBackend::Script s;
    s.resource_length = n;
    s.support_range = true;
    s.http_status = 206;
    s.etag = "\"e\"";
    s.last_modified = "Wed, 01 Jan 2020 00:00:00 GMT";
    s.chunk_size = 64;
    return s;
}

struct Harness {
    StubBackend stub;
    FakeClock   clk;
    Scheduler*  sched = nullptr;
    int64_t     file_n = 0;
    std::vector<uint8_t> hits;          // 每个偏移被 on_data 交付的次数
    std::vector<uint8_t> bytes;         // 按偏移存放，未交付为 0x00 哨兵不够，另用 hits
    int idle_n  = 0;
    int err_n   = 0;
    int data_n  = 0;
    int total_n = 0;
    int val_n   = 0;
    syp_status last_err = SYP_OK;
    int32_t    last_http = 0;
    int64_t    total = -1;
    std::string etag;
    std::string last_modified;
    bool persist = true;
    bool stopped_deliveries = false;
    // hits / bytes / 计数器 / sched：同一把锁。TracingBackend::check_create
    // 读 hits 也走这里，不能另用一把锁护同一份数据。
    mutable std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::atomic<int64_t> delivered{0};

    void set_scheduler(Scheduler* s) {
        std::lock_guard<std::mutex> g(wait_mu);
        sched = s;
    }

    void reset_hits(int64_t n) {
        file_n = n;
        hits.assign(static_cast<size_t>(n), 0);
        bytes.assign(static_cast<size_t>(n), 0);
    }

    static void on_data(void* ctx, int64_t offset, const uint8_t* data, int32_t len) {
        auto* h = static_cast<Harness*>(ctx);
        {
            std::lock_guard<std::mutex> g(h->wait_mu);
            ++h->data_n;
            if (h->stopped_deliveries) {
                tiny_test::fail(__FILE__, __LINE__, "on_data after stop()");
                return;
            }
            if (len <= 0 || data == nullptr) return;
            for (int32_t i = 0; i < len; ++i) {
                const int64_t off = offset + i;
                if (off < 0 || off >= h->file_n) {
                    tiny_test::fail(__FILE__, __LINE__, "on_data offset out of file",
                                    "off=" + std::to_string(off));
                    continue;
                }
                const auto u = static_cast<size_t>(off);
                if (h->hits[u] > 0) {
                    tiny_test::fail(__FILE__, __LINE__, "byte delivered twice",
                                    "off=" + std::to_string(off));
                }
                if (h->hits[u] < 255) ++h->hits[u];
                h->bytes[u] = data[static_cast<size_t>(i)];
            }
        }
        h->delivered.fetch_add(static_cast<int64_t>(len), std::memory_order_relaxed);
        if (h->persist) {
            Scheduler* s = nullptr;
            {
                std::lock_guard<std::mutex> g(h->wait_mu);
                s = h->sched;
            }
            if (s != nullptr) {
                s->notify_persisted(
                    Range{offset, offset + static_cast<int64_t>(len)});
            }
        }
    }

    static void on_total(void* ctx, int64_t total) {
        auto* h = static_cast<Harness*>(ctx);
        std::lock_guard<std::mutex> g(h->wait_mu);
        if (h->stopped_deliveries) {
            tiny_test::fail(__FILE__, __LINE__, "on_total_length after stop()");
            return;
        }
        h->total = total;
        ++h->total_n;
    }

    static void on_val(void* ctx, const char* etag, const char* lm) {
        auto* h = static_cast<Harness*>(ctx);
        std::lock_guard<std::mutex> g(h->wait_mu);
        if (h->stopped_deliveries) {
            tiny_test::fail(__FILE__, __LINE__, "on_validators after stop()");
            return;
        }
        h->etag = etag ? etag : "";
        h->last_modified = lm ? lm : "";
        ++h->val_n;
    }

    static void on_error(void* ctx, syp_status st, int32_t http) {
        auto* h = static_cast<Harness*>(ctx);
        std::lock_guard<std::mutex> g(h->wait_mu);
        if (h->stopped_deliveries) {
            tiny_test::fail(__FILE__, __LINE__, "on_error after stop()");
            return;
        }
        h->last_err = st;
        h->last_http = http;
        ++h->err_n;
    }

    static void on_idle(void* ctx) {
        auto* h = static_cast<Harness*>(ctx);
        {
            std::lock_guard<std::mutex> g(h->wait_mu);
            if (h->stopped_deliveries) {
                tiny_test::fail(__FILE__, __LINE__, "on_idle after stop()");
                return;
            }
            ++h->idle_n;
        }
        h->wait_cv.notify_all();
    }

    bool wait_idle_ge(int n, int timeout_ms = 8000) {
        std::unique_lock<std::mutex> lk(wait_mu);
        return wait_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [&] { return idle_n >= n; });
    }

    SchedulerCallbacks cbs() {
        return SchedulerCallbacks{this, &on_data, &on_total, &on_val,
                                  &on_error, &on_idle};
    }

    int64_t delivered_bytes() const {
        std::lock_guard<std::mutex> g(wait_mu);
        int64_t n = 0;
        for (uint8_t v : hits) if (v > 0) ++n;
        return n;
    }

    bool hits_exactly_once(Range r) const {
        std::lock_guard<std::mutex> g(wait_mu);
        if (r.start < 0 || r.end > file_n) return false;
        for (int64_t i = r.start; i < r.end; ++i) {
            if (hits[static_cast<size_t>(i)] != 1) return false;
        }
        return true;
    }

    bool hits_none(Range r) const {
        std::lock_guard<std::mutex> g(wait_mu);
        if (r.start < 0 || r.end > file_n) return false;
        for (int64_t i = r.start; i < r.end; ++i) {
            if (hits[static_cast<size_t>(i)] != 0) return false;
        }
        return true;
    }

    bool synthetic_ok(Range r) const {
        std::lock_guard<std::mutex> g(wait_mu);
        for (int64_t i = r.start; i < r.end; ++i) {
            if (hits[static_cast<size_t>(i)] == 0) continue;
            if (bytes[static_cast<size_t>(i)] != synthetic_byte(i)) return false;
        }
        return true;
    }
};

void pump_all(StubBackend& s) { s.pump_all(); }

// 探测窗口：首个请求发的是 `Range: bytes=<start>-`，
// 在它的 206 回来之前调度器不知道服务端支不支持 Range，并发锁在 1，只发
// 这一条探测请求；分片仍按 max_concurrent_tasks 切，所以探测请求的区间与
// 放开并发后第一个分片完全一致（见 Scheduler::planned_tasks_locked）。
// 凡是断「start() 之后立刻有几条请求」的用例都要先把探测响应泵出来。
//
// 同步桩到 on_response 恰好两步：Running→Responding（无回调）、
// Responding（发 on_response）。第三步起才是 body。
void pump_probe_response(StubBackend& s) {
    s.pump();
    s.pump();
}

std::vector<Range> all_requests(const StubBackend& s, int64_t eof) {
    std::vector<Range> v;
    const int n = s.request_count();
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) v.push_back(req_range(s.request_at(i), eof));
    return v;
}

bool request_covers(const StubBackend& s, int64_t eof, Range q) {
    if (q.empty()) return false;
    for (int i = 0; i < s.request_count(); ++i) {
        if (ranges_overlap(req_range(s.request_at(i), eof), q)) return true;
    }
    return false;
}

// 请求层 tracing：接管 create / on_complete，记录每个请求的存活窗口。
// 每次 create 时断言：新请求不覆盖已缓存字节、不与仍存活的请求相交。
// 调度层 on_task_data 会用 cached∪received 去重，交付 bitmap 看不到重叠。
struct TracingBackend {
    StubBackend* stub = nullptr;
    Harness*     harness = nullptr;
    HoleSet      already;
    int64_t      file_n = 0;
    syp_http_backend table{};

    struct TraceHandle {
        TracingBackend* tr = nullptr;
        syp_http_request_handle* inner = nullptr;
        syp_response_sink user{};
        Range range{};
        bool  live = true;
    };

    std::mutex mu;
    std::map<TraceHandle*, std::unique_ptr<TraceHandle>> owns;
    int overlap_n  = 0;
    int cachehit_n = 0;

    void install() {
        table.backend_ctx = this;
        table.create  = &TracingBackend::tr_create;
        table.start   = &TracingBackend::tr_start;
        table.cancel  = &TracingBackend::tr_cancel;
        table.destroy = &TracingBackend::tr_destroy;
    }

    const syp_http_backend* backend() const noexcept { return &table; }

    static Range range_of(const syp_http_request* req, int64_t eof) {
        StubBackend::CapturedRequest c;
        c.range_start = req->range_start;
        c.range_end   = req->range_end;
        return req_range(c, eof);
    }

    void check_create(Range nr) {
        if (nr.empty()) return;
        std::lock_guard<std::mutex> g(mu);
        for (auto& kv : owns) {
            TraceHandle* th = kv.first;
            if (th == nullptr || !th->live) continue;
            if (ranges_overlap(th->range, nr)) {
                ++overlap_n;
                tiny_test::fail(__FILE__, __LINE__,
                                "new request overlaps live request",
                                "new=" + tiny_test::stringify(nr)
                                    + " live=" + tiny_test::stringify(th->range));
            }
        }
        for (const Range& c : already.ranges()) {
            if (ranges_overlap(c, nr)) {
                ++cachehit_n;
                tiny_test::fail(__FILE__, __LINE__,
                                "new request overlaps already-cached",
                                "new=" + tiny_test::stringify(nr)
                                    + " cached=" + tiny_test::stringify(c));
            }
        }
        if (harness != nullptr) {
            // 锁顺序：tr->mu → wait_mu。on_data 只拿 wait_mu；wr_on_complete
            // 先放 tr->mu 再进用户回调。不要反过来。
            std::lock_guard<std::mutex> hg(harness->wait_mu);
            if (harness->file_n == file_n) {
                const int64_t lo = nr.start < 0 ? 0 : nr.start;
                int64_t hi = nr.end;
                if (hi > file_n) hi = file_n;
                for (int64_t i = lo; i < hi; ++i) {
                    if (harness->hits[static_cast<size_t>(i)] > 0) {
                        ++cachehit_n;
                        tiny_test::fail(__FILE__, __LINE__,
                                        "new request overlaps delivered byte",
                                        "off=" + std::to_string(i));
                        break;
                    }
                }
            }
        }
    }

    static void wr_on_response(void* ctx, int32_t http, const syp_headers* hdrs,
                               int64_t cl, int64_t tot) {
        auto* th = static_cast<TraceHandle*>(ctx);
        if (th->user.on_response != nullptr) {
            th->user.on_response(th->user.ctx, http, hdrs, cl, tot);
        }
    }
    static void wr_on_data(void* ctx, const uint8_t* data, int32_t len) {
        auto* th = static_cast<TraceHandle*>(ctx);
        if (th->user.on_data != nullptr) th->user.on_data(th->user.ctx, data, len);
    }
    static bool wr_on_redirect(void* ctx, const char* url) {
        auto* th = static_cast<TraceHandle*>(ctx);
        if (th->user.on_redirect != nullptr) {
            return th->user.on_redirect(th->user.ctx, url);
        }
        return true;
    }
    static void wr_on_complete(void* ctx, syp_status st, int32_t http) {
        auto* th = static_cast<TraceHandle*>(ctx);
        {
            std::lock_guard<std::mutex> g(th->tr->mu);
            th->live = false;
        }
        if (th->user.on_complete != nullptr) {
            th->user.on_complete(th->user.ctx, st, http);
        }
    }

    static syp_http_request_handle* tr_create(
        void* ctx, const syp_http_request* req, const syp_response_sink* sink) {
        auto* tr = static_cast<TracingBackend*>(ctx);
        if (req == nullptr || sink == nullptr) return nullptr;
        auto th = std::make_unique<TraceHandle>();
        th->tr = tr;
        th->user = *sink;
        th->range = range_of(req, tr->file_n);
        th->live = true;
        tr->check_create(th->range);

        syp_response_sink wrapped{};
        wrapped.ctx = th.get();
        wrapped.on_response = &wr_on_response;
        wrapped.on_data     = &wr_on_data;
        wrapped.on_redirect = &wr_on_redirect;
        wrapped.on_complete = &wr_on_complete;

        const syp_http_backend* inner = tr->stub->backend();
        th->inner = inner->create(inner->backend_ctx, req, &wrapped);
        if (th->inner == nullptr) return nullptr;

        TraceHandle* raw = th.get();
        {
            std::lock_guard<std::mutex> g(tr->mu);
            tr->owns[raw] = std::move(th);
        }
        return reinterpret_cast<syp_http_request_handle*>(raw);
    }

    static void tr_start(syp_http_request_handle* h) {
        auto* th = reinterpret_cast<TraceHandle*>(h);
        if (th == nullptr || th->inner == nullptr) return;
        th->tr->stub->backend()->start(th->inner);
    }
    static void tr_cancel(syp_http_request_handle* h) {
        auto* th = reinterpret_cast<TraceHandle*>(h);
        if (th == nullptr || th->inner == nullptr) return;
        th->tr->stub->backend()->cancel(th->inner);
    }
    static void tr_destroy(syp_http_request_handle* h) {
        auto* th = reinterpret_cast<TraceHandle*>(h);
        if (th == nullptr) return;
        if (th->inner != nullptr) th->tr->stub->backend()->destroy(th->inner);
        std::lock_guard<std::mutex> g(th->tr->mu);
        th->tr->owns.erase(th);
    }
};

}  // namespace

TEST_CASE(full_download_empty_cache) {
    constexpr int64_t N = 400;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 1;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_all(h.stub);
    CHECK_EQ(h.idle_n, 1);
    CHECK_EQ(h.err_n, 0);
    CHECK_EQ(h.total, N);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK(h.synthetic_ok(Range{0, N}));
    CHECK_EQ(sched.active_task_count(), 0);
    CHECK_EQ(sched.consecutive_errors(), 0);
    check_stub(h.stub);
}

TEST_CASE(does_not_redownload_already_cached) {
    constexpr int64_t N = 500;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 50;
    HoleSet cached;
    cached.add(Range{100, 250});
    cached.add(Range{400, 450});
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, cached);
    CHECK(!request_covers(h.stub, N, Range{100, 250}));
    CHECK(!request_covers(h.stub, N, Range{400, 450}));
    pump_all(h.stub);
    CHECK(h.hits_none(Range{100, 250}));
    CHECK(h.hits_none(Range{400, 450}));
    CHECK(h.hits_exactly_once(Range{0, 100}));
    CHECK(h.hits_exactly_once(Range{250, 400}));
    CHECK(h.hits_exactly_once(Range{450, N}));
    CHECK(h.synthetic_ok(Range{0, N}));
    CHECK_EQ(h.idle_n, 1);
    check_stub(h.stub);
}

TEST_CASE(concurrent_three_tasks_no_overlap) {
    constexpr int64_t N = 900;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 100;
    cfg.segment_size_hint = 0;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(h.stub.request_count(), 1);   // 探测窗口
    pump_probe_response(h.stub);
    CHECK_EQ(sched.active_task_count(), 3);
    CHECK_EQ(h.stub.request_count(), 3);
    const auto rs = all_requests(h.stub, N);
    CHECK(!any_overlap(rs));
    int64_t sum = 0;
    for (const Range& r : rs) sum += r.size();
    CHECK(sum > 0);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
}

TEST_CASE(segment_size_auto_ceil_div) {
    // remaining=3000, conc=3, min=1, hint=0 → ceil(1000)=1000，三段
    constexpr int64_t N = 3000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 1;
    cfg.segment_size_hint = 0;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    // 探测请求就是第一个分片本身：切分按 max_concurrent_tasks 算，
    // 与放开并发后完全一致。
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1000}));
    pump_probe_response(h.stub);
    CHECK_EQ(h.stub.request_count(), 3);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1000}));
    CHECK_EQ(req_range(h.stub.request_at(1), N), (Range{1000, 2000}));
    CHECK_EQ(req_range(h.stub.request_at(2), N), (Range{2000, 3000}));
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
}

TEST_CASE(segment_size_clamped_to_min) {
    // remaining=3000, conc=3, auto=1000 < min=2000 → 每片 2000，两任务
    constexpr int64_t N = 3000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 2000;
    cfg.segment_size_hint = 0;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_probe_response(h.stub);
    CHECK_EQ(h.stub.request_count(), 2);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 2000}));
    CHECK_EQ(req_range(h.stub.request_at(1), N), (Range{2000, 3000}));
    CHECK(!any_overlap(all_requests(h.stub, N)));
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
}

TEST_CASE(segment_size_clamped_to_hint) {
    // remaining=10MiB-scale 用小数：N=10000, conc=2, auto=5000, hint=1000, min=10
    constexpr int64_t N = 10000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 2;
    cfg.min_segment_size = 10;
    cfg.segment_size_hint = 1000;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_probe_response(h.stub);
    CHECK_EQ(h.stub.request_count(), 2);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1000}));
    CHECK_EQ(req_range(h.stub.request_at(1), N), (Range{1000, 2000}));
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
}

TEST_CASE(segment_size_remaining_below_min_not_split) {
    constexpr int64_t N = 100;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 512 * 1024;
    cfg.segment_size_hint = 0;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, N}));
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
}

TEST_CASE(seek_turns_to_nearby_hole) {
    constexpr int64_t N = 2000;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.chunk_size = 50;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 2;
    // hint 低于 kNoSplitBelow 会被忽略，分片变成 ceil(N/conc)=1000，
    // 第二条任务已经盖住 1600，seek 不会再开新请求。
    cfg.min_segment_size = 256;
    cfg.segment_size_hint = 256;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    const int before = h.stub.request_count();
    CHECK(before >= 1);
    // 先把靠近 0 的数据下一小段
    for (int i = 0; i < 8; ++i) h.stub.pump();
    const int64_t got = h.delivered_bytes();
    CHECK(got > 0);
    sched.set_read_position(1600);
    const int after = h.stub.request_count();
    CHECK(after > before);
    bool near_seek = false;
    for (int i = before; i < after; ++i) {
        const Range r = req_range(h.stub.request_at(i), N);
        if (r.start >= 1500) near_seek = true;
        // 已交付的区间不应被新请求再要
        for (int64_t off = 0; off < N; ++off) {
            if (h.hits[static_cast<size_t>(off)] == 0) continue;
            CHECK(!ranges_overlap(r, Range{off, off + 1}));
        }
    }
    CHECK(near_seek);
    sched.set_read_position(0);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK(h.synthetic_ok(Range{0, N}));
    check_stub(h.stub);
}

namespace {

struct ReuseSetup {
    static constexpr int64_t kSeg  = 200 * 1024;
    static constexpr int64_t kFile = 8 * kSeg;

    Harness h;
    SchedulerConfig cfg = cfg_small();
    std::unique_ptr<Scheduler> sched;

    ReuseSetup() {
        h.reset_hits(kFile);
        auto sc = script_ok(kFile);
        sc.chunk_size = 50 * 1024;
        h.stub.set_default_script(sc);
        cfg.max_concurrent_tasks = 2;
        cfg.min_segment_size = kSeg;
        cfg.segment_size_hint = kSeg;
        cfg.reuse_max_remaining_bytes = 1 << 20;
        cfg.connect_estimate_max_ms = 2000;
        cfg.task.speed_window_ms = 3000;
        sched = std::make_unique<Scheduler>(
            h.stub.backend(), h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(sched.get());
        sched->start("http://x/v", nullptr, kFile, HoleSet{});
        pump_probe_response(h.stub);   // 探测窗口，见 pump_probe_response
        REQUIRE(h.stub.request_count() == 2);
        const Range first = req_range(h.stub.request_at(0), kFile);
        sched->set_target_end(first.end);  // 停掉第二条
    }

    int expand_and_count() {
        const int before = h.stub.request_count();
        sched->set_target_end(-1);
        return h.stub.request_count() - before;
    }
};

void pump_until_received(Harness& h, int64_t min_bytes, int cap = 10000) {
    int guard = 0;
    while (h.delivered_bytes() < min_bytes && guard++ < cap) {
        if (!h.stub.pump()) break;
    }
}

}  // namespace

TEST_CASE(reuse_remaining_too_large_opens_new) {
    ReuseSetup s;
    // 几乎没下，remaining ≈ 200KiB... 不够 > 1MiB。改用更大分片的独立场景。
    s.h.set_scheduler(nullptr);
    s.sched.reset();
    constexpr int64_t kSeg = 2 * 1024 * 1024;
    constexpr int64_t kFile = 4 * kSeg;
    Harness h;
    h.reset_hits(kFile);
    auto sc = script_ok(kFile);
    sc.chunk_size = 1024;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 2;
    cfg.min_segment_size = kSeg;
    cfg.segment_size_hint = kSeg;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, kFile, HoleSet{});
    pump_probe_response(h.stub);   // 探测窗口，见 pump_probe_response
    CHECK_EQ(h.stub.request_count(), 2);
    const Range first = req_range(h.stub.request_at(0), kFile);
    CHECK(first.size() > (1 << 20));
    sched.set_target_end(first.end);
    pump_until_received(h, 1024);
    CHECK(h.delivered_bytes() > 0);
    CHECK(h.delivered_bytes() < first.size() - (1 << 20));
    const int before = h.stub.request_count();
    sched.set_target_end(-1);
    CHECK(h.stub.request_count() > before);  // remaining > 1MiB → 不等
    sched.set_read_position(0);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, kFile}));
    check_stub(h.stub);
}

TEST_CASE(reuse_speed_nullopt_opens_new) {
    ReuseSetup s;
    // 不泵数据：speed 仍是 nullopt
    const int opened = s.expand_and_count();
    CHECK(opened >= 1);
    s.sched->set_read_position(0);
    pump_all(s.h.stub);
    CHECK(s.h.hits_exactly_once(Range{0, ReuseSetup::kFile}));
    check_stub(s.h.stub);
}

TEST_CASE(reuse_speed_zero_opens_new) {
    ReuseSetup s;
    pump_until_received(s.h, 50 * 1024);
    CHECK(s.h.delivered_bytes() > 0);
    s.h.clk.advance(4000);  // 滑出 3000ms 窗口 → speed = 0
    const int opened = s.expand_and_count();
    CHECK(opened >= 1);
    s.sched->set_read_position(0);
    pump_all(s.h.stub);
    CHECK(s.h.hits_exactly_once(Range{0, ReuseSetup::kFile}));
    check_stub(s.h.stub);
}

TEST_CASE(reuse_estimate_over_2000_opens_new) {
    ReuseSetup s;
    pump_until_received(s.h, 50 * 1024);
    // remaining ≈ 150KiB。要 estimate > 2000，需要 speed < 150000*1000/2000 = 75000
    // 50KiB / span = 75000 → span = 50KiB*1000/75000 ≈ 666ms；再拉长 span。
    s.h.clk.advance(2000);
    // speed ≈ 50KiB * 1000 / 2000 = 25600 B/s
    // remaining ≈ 150KiB，estimate ≈ 1000*150000/25600 ≈ 5859 > 2000
    const int opened = s.expand_and_count();
    CHECK(opened >= 1);
    s.sched->set_read_position(0);
    pump_all(s.h.stub);
    CHECK(s.h.hits_exactly_once(Range{0, ReuseSetup::kFile}));
    check_stub(s.h.stub);
}

TEST_CASE(reuse_estimate_within_threshold_waits) {
    ReuseSetup s;
    pump_until_received(s.h, 100 * 1024);
    s.h.clk.advance(100);
    // speed ≈ 100KiB * 1000 / 100 = 1_024_000 B/s
    // remaining ≈ 100KiB，estimate ≈ 100ms < 2000，remaining < 1MiB → 等
    const int opened = s.expand_and_count();
    CHECK_EQ(opened, 0);
    CHECK_EQ(s.sched->active_task_count(), 1);
    s.sched->set_read_position(0);
    pump_all(s.h.stub);
    CHECK(s.h.hits_exactly_once(Range{0, ReuseSetup::kFile}));
    check_stub(s.h.stub);
}

TEST_CASE(range_fallback_allowed_single_from_zero) {
    constexpr int64_t N = 300;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.support_range = false;
    sc.http_status = 200;
    sc.chunk_size = 50;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 80;
    cfg.segment_size_hint = 80;
    cfg.allow_no_range_fallback = true;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    // 探测窗口：Range 探明之前只发一条。
    CHECK_EQ(h.stub.request_count(), 1);
    pump_all(h.stub);
    CHECK(h.stub.request_count() >= 2);
    CHECK_EQ(sched.range_supported(), false);
    CHECK(sched.active_task_count() <= 1);
    bool saw_from_zero = false;
    for (int i = 0; i < h.stub.request_count(); ++i) {
        if (h.stub.request_at(i).range_start == 0) saw_from_zero = true;
    }
    CHECK(saw_from_zero);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
}

// 探测窗口并发锁定这处已知问题的回归用例。
//
// 契约（syp_config.h 的 allow_no_range_fallback）承诺「服务端不支持 Range
// 时退回**单连接**全量下载」。首个请求发的是 `Range: bytes=0-`（见
// DLTask::issue_request 的 range_start=0 / range_end=-1，以及
// apple_http_backend.mm 里的 "bytes=%lld-"）——支持 Range 的服务端对它回
// 206，不支持的回 200，**首个响应就分得清**。所以在看到 206 之前并发必须
// 锁在 1，不能因为「拿到了 Content-Length」就放开到 max_concurrent_tasks。
//
// 修复前：start() 里总长已知 → max_tasks_locked() 直接返回 3，
// 一口气建 3 个槽；断言 request_count()==1 红（实际 3）。
TEST_CASE(no_range_source_stays_single_connection) {
    constexpr int64_t N = 300;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.support_range = false;
    sc.http_status = 200;
    sc.chunk_size = 50;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 80;
    cfg.segment_size_hint = 80;
    cfg.allow_no_range_fallback = true;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    // 首个响应还没到（同步桩 create 不回调）：Range 支持与否尚未探明，
    // 并发必须锁 1。
    CHECK_EQ(h.stub.request_count(), 1);

    int32_t peak_live = sched.active_task_count();
    while (h.stub.pump()) {
        const int32_t live = sched.active_task_count();
        if (live > peak_live) peak_live = live;
    }
    // 全程只有一条连接（这就是 LoopbackServer::peak_concurrent_requests()
    // 在端到端场景 E 里量的那个东西的单测版本）。
    CHECK_EQ(peak_live, 1);
    // 请求条数不是 1：不支持 Range 的源要三条**串行**请求才收敛——
    //   [0, 256)   探测（分片按 max_concurrent_tasks 切出来的第一片）
    //   [256, 300) 探测任务成功收尾后补的下一个洞；它的 200 才是硬信号
    //              （start > 0 → DLTask 判 SYP_ERR_RANGE_UNSUPPORTED 并
    //              掐断连接，即「响应体丢弃未及时中止连接」那处已知问题的路径）
    //   [0, 300)   降级后的单连接全量
    // 契约管的是"同时几条连接"，不是"一共发几次请求"，所以这里断的是
    // peak_live 而不是 request_count。
    CHECK(h.stub.request_count() <= 3);
    CHECK_EQ(req_range(h.stub.request_at(h.stub.request_count() - 1), N),
             (Range{0, N}));
    CHECK_EQ(sched.range_supported(), false);   // 确实走到了降级
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
}

// 正向对照：支持 Range 的源（206）在**首个响应到达之后**必须立刻放开到
// max_concurrent_tasks——锁 1 只覆盖「尚未探明」这个窗口，不是永久降速。
TEST_CASE(range_source_opens_concurrency_after_first_206) {
    constexpr int64_t N = 3000;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.chunk_size = 64;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 100;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(h.stub.request_count(), 1);   // 探测阶段
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1000}));
    // on_response(206) 一到，并发就该放开——不必等探测任务下完。
    pump_probe_response(h.stub);
    CHECK_EQ(h.stub.request_count(), 3);
    CHECK_EQ(sched.active_task_count(), 3);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
}

TEST_CASE(range_fallback_disallowed_reports_error) {
    constexpr int64_t N = 300;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.support_range = false;
    sc.http_status = 200;
    sc.chunk_size = 50;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 80;
    cfg.segment_size_hint = 80;
    cfg.allow_no_range_fallback = false;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_all(h.stub);
    CHECK_EQ(h.err_n, 1);
    CHECK_EQ(h.last_err, SYP_ERR_RANGE_UNSUPPORTED);
    CHECK_EQ(sched.active_task_count(), 0);
    const int nreq = h.stub.request_count();
    pump_all(h.stub);
    CHECK_EQ(h.stub.request_count(), nreq);  // 不再开新任务
    check_stub(h.stub);
}

TEST_CASE(reschedule_stops_task_fully_cached) {
    constexpr int64_t N = 600;
    Harness h;
    h.reset_hits(N);
    h.persist = false;  // 自己控制 cached
    auto sc = script_ok(N);
    sc.chunk_size = 30;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 2;
    cfg.min_segment_size = 200;
    cfg.segment_size_hint = 200;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_probe_response(h.stub);
    CHECK_EQ(h.stub.request_count(), 2);
    const Range first = req_range(h.stub.request_at(0), N);
    const Range second = req_range(h.stub.request_at(1), N);
    sched.notify_persisted(first);
    const HoleSet inf = sched.inflight_ranges();
    CHECK(!inf.contains(first));
    // 第二条还在
    bool second_inflight = false;
    for (const Range& r : inf.ranges()) {
        if (ranges_overlap(r, second)) second_inflight = true;
    }
    CHECK(second_inflight);
    h.persist = true;
    sched.set_read_position(0);
    pump_all(h.stub);
    // first 被标成已缓存，不应交付（persist=false 期间也没交付过）
    CHECK(h.hits_none(first));
    CHECK(h.hits_exactly_once(Range{first.end, N}));
    check_stub(h.stub);
}

TEST_CASE(consecutive_errors_stop_scheduling) {
    constexpr int64_t N = 400;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.timeout = true;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 1;
    cfg.min_segment_size = 100;
    cfg.max_consecutive_errors = 3;
    cfg.task.max_retries = 0;  // 一次失败就到 Scheduler
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_all(h.stub);
    CHECK_EQ(h.err_n, 1);
    CHECK_EQ(h.last_err, SYP_ERR_TIMEOUT);
    CHECK_EQ(sched.consecutive_errors(), 3);
    CHECK_EQ(sched.active_task_count(), 0);
    const int nreq = h.stub.request_count();
    CHECK_EQ(nreq, 3);
    pump_all(h.stub);
    CHECK_EQ(h.stub.request_count(), nreq);
    check_stub(h.stub);
}

TEST_CASE(on_idle_then_grow_target) {
    constexpr int64_t N = 800;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 2;
    cfg.min_segment_size = 50;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.set_target_end(200);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_all(h.stub);
    CHECK_EQ(h.idle_n, 1);
    CHECK(h.hits_exactly_once(Range{0, 200}));
    CHECK(h.hits_none(Range{200, N}));
    sched.set_target_end(500);
    CHECK(sched.active_task_count() > 0);
    pump_all(h.stub);
    CHECK_EQ(h.idle_n, 2);
    CHECK(h.hits_exactly_once(Range{0, 500}));
    CHECK(h.hits_none(Range{500, N}));
    check_stub(h.stub);
}

TEST_CASE(stop_suppresses_callbacks_and_new_tasks) {
    constexpr int64_t N = 500;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.chunk_size = 20;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 2;
    cfg.min_segment_size = 100;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    const int nreq = h.stub.request_count();
    sched.stop();
    h.stopped_deliveries = true;
    pump_all(h.stub);
    CHECK_EQ(h.stub.request_count(), nreq);
    CHECK_EQ(sched.active_task_count(), 0);
    check_stub(h.stub);
}

TEST_CASE(dtor_with_inflight_tasks) {
    constexpr int64_t N = 400;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 50;
    {
        Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(&sched);
        sched.start("http://x/v", nullptr, N, HoleSet{});
        CHECK(sched.active_task_count() > 0);
        h.set_scheduler(nullptr);
    }
    check_stub(h.stub);
}

TEST_CASE(randomized_cross_check) {
    // 审计规模 8×150。回归网 8 seed；Release 100 轮贴近审计，
    // Debug（含 ASan/TSan）50 轮，整份 ctest 仍在秒级。
    constexpr uint32_t kSeeds[] = {
        20260907u, 1u, 2u, 7u, 11u, 13u, 17u, 19u,
    };
#if defined(NDEBUG)
    constexpr int kRounds = 100;
#else
    constexpr int kRounds = 50;
#endif
    for (uint32_t seed : kSeeds) {
    std::mt19937 rng(seed);
    for (int round = 0; round < kRounds; ++round) {
        const int64_t N = static_cast<int64_t>(
            std::uniform_int_distribution<int>(64, 2048)(rng));
        Harness h;
        h.reset_hits(N);
        auto sc = script_ok(N);
        sc.chunk_size = static_cast<int32_t>(
            std::uniform_int_distribution<int>(1, 64)(rng));
        h.stub.set_default_script(sc);
        // 失败注入：只砸第 1 个请求，后续重试/新任务走不断的 default，
        // 否则每个请求都断会把 consecutive_errors 打满。
        if (rng() % 4 == 0) {
            auto br = sc;
            const int64_t cap = std::max<int64_t>(1, N / 4);
            br.break_after_bytes = static_cast<int64_t>(
                std::uniform_int_distribution<int>(
                    1, static_cast<int>(cap))(rng));
            h.stub.set_script_for_request(1, br);
        }
        h.stub.set_random_pump(true, seed + static_cast<uint32_t>(round));

        SchedulerConfig cfg = cfg_small();
        cfg.max_concurrent_tasks = static_cast<int32_t>(
            std::uniform_int_distribution<int>(1, 3)(rng));
        cfg.min_segment_size = static_cast<int64_t>(
            std::uniform_int_distribution<int>(1, 200)(rng));
        cfg.segment_size_hint =
            (rng() % 2 == 0)
                ? int64_t{0}
                : static_cast<int64_t>(
                      std::uniform_int_distribution<int>(50, 400)(rng));
        cfg.task.max_retries = 3;
        cfg.max_consecutive_errors = 16;

        HoleSet already;
        const int npre = std::uniform_int_distribution<int>(0, 3)(rng);
        for (int i = 0; i < npre; ++i) {
            const int64_t a = static_cast<int64_t>(
                std::uniform_int_distribution<int>(0, static_cast<int>(N))(rng));
            const int64_t b = static_cast<int64_t>(
                std::uniform_int_distribution<int>(0, static_cast<int>(N))(rng));
            if (a < b) already.add(Range{a, b});
        }

        TracingBackend tr;
        tr.stub = &h.stub;
        tr.harness = &h;
        tr.already = already;
        tr.file_n = N;
        tr.install();

        const int64_t total_arg = (rng() % 5 == 0) ? int64_t{-1} : N;

        Scheduler sched(tr.backend(), h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(&sched);
        sched.start("http://x/v", nullptr, total_arg, already);

        const int nseek = std::uniform_int_distribution<int>(0, 5)(rng);
        for (int s = 0; s < nseek; ++s) {
            const int steps = std::uniform_int_distribution<int>(0, 12)(rng);
            for (int i = 0; i < steps; ++i) {
                if (!h.stub.pump()) break;
            }
            const int64_t pos = static_cast<int64_t>(
                std::uniform_int_distribution<int>(0, static_cast<int>(N))(rng));
            sched.set_read_position(pos);
            if (rng() % 3 == 0) {
                const int64_t end = static_cast<int64_t>(
                    std::uniform_int_distribution<int>(
                        0, static_cast<int>(N))(rng));
                sched.set_target_end(end == 0 ? -1 : end);
            }
            if (rng() % 5 == 0) h.clk.advance(50);
        }

        // 收尾：回到全文件，把洞补完
        sched.set_read_position(0);
        sched.set_target_end(-1);
        pump_all(h.stub);

        CHECK_EQ(h.err_n, 0);
        CHECK_EQ(tr.overlap_n, 0);
        CHECK_EQ(tr.cachehit_n, 0);
        CHECK(h.hits_none(Range{0, 0}));  // 空区间 trivially
        for (int64_t i = 0; i < N; ++i) {
            const bool pre = already.contains(Range{i, i + 1});
            const uint8_t n = h.hits[static_cast<size_t>(i)];
            if (pre) {
                if (n != 0) {
                    tiny_test::fail(__FILE__, __LINE__,
                                    "pre-cached byte was delivered",
                                    "round=" + std::to_string(round)
                                        + " off=" + std::to_string(i));
                    break;
                }
            } else if (n != 1) {
                tiny_test::fail(__FILE__, __LINE__,
                                "target byte not delivered exactly once",
                                "round=" + std::to_string(round)
                                    + " off=" + std::to_string(i)
                                    + " hits=" + std::to_string(n));
                break;
            } else if (h.bytes[static_cast<size_t>(i)] != synthetic_byte(i)) {
                tiny_test::fail(__FILE__, __LINE__, "synthetic mismatch",
                                "round=" + std::to_string(round)
                                    + " off=" + std::to_string(i));
                break;
            }
        }
        check_stub(h.stub);
        h.set_scheduler(nullptr);
    }
    }  // seeds
}

TEST_CASE(on_idle_not_on_every_seek) {
    constexpr int64_t N = 400;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    pump_all(h.stub);
    CHECK_EQ(h.idle_n, 1);
    CHECK(h.hits_exactly_once(Range{0, N}));
    for (int i = 0; i < 50; ++i) {
        sched.set_read_position(static_cast<int64_t>((i * 7) % static_cast<int>(N)));
    }
    CHECK_EQ(h.idle_n, 1);  // 已齐窗口里 seek 不再刷 on_idle
    check_stub(h.stub);
}

TEST_CASE(hint_cannot_punch_through_split_floor) {
    // remaining 远大于 256 时，hint=10 不得把切分粒度压回 10 字节。
    constexpr int64_t N = 10000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 1;
    cfg.segment_size_hint = 10;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK(h.stub.request_count() >= 1);
    for (int i = 0; i < h.stub.request_count(); ++i) {
        CHECK(req_range(h.stub.request_at(i), N).size() >= 256);
    }
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
}

TEST_CASE(tail_below_floor_not_split) {
    // remaining=10, min=1, conc=3：没有尾部下限会切成 4+4+2。
    constexpr int64_t N = 10000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 1;
    HoleSet already;
    already.add(Range{0, N - 10});
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, already);
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{N - 10, N}));
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{N - 10, N}));
    CHECK(h.hits_none(Range{0, N - 10}));
    check_stub(h.stub);
}

namespace {

// 这几条 async_* 用例（连同下面 async_dtor_with_inflight_tasks）都用
// StubBackend::Mode::Async，走真实 worker 线程回调——正是"worker 线程在自己
// 回调栈上重入 destroy"那条死锁路径会经过的形状。之前没有看门狗兜底：
// Apple 后端一旦回归，
// 表现是整个套件零输出挂到 ctest TIMEOUT（45s），连挂在哪条用例都看不出来。
// 看门狗按用例名区分诊断，即使用例本体的等待循环也卡死不返回，看门狗自己的
// 线程仍会独立到点打印诊断/硬退出——不依赖用例内部逻辑还能不能跑到下一行。
//
// 硬超时沿用 window_advance_reaps_task_reentrant_destroy_from_worker_thread
// 那条同文件用例已验证过的取值（20000ms）：scheduler 目标 ctest TIMEOUT 是
// 45s，20000 留出的余量与那条用例的论证一致，不再重复计算。
constexpr int kAsyncWatchdogSoftMs = 4000;
constexpr int kAsyncWatchdogHardMs = 20000;
constexpr const char* kAsyncWatchdogHint =
    "本用例用 StubBackend::Mode::Async（真实 worker 线程）。卡住通常意味着"
    "调度器/DLTask 与后端 worker 线程之间出现了死锁，而不是下载慢。";

}  // namespace

TEST_CASE(async_full_download) {
    syp::test::Watchdog wd("async_full_download", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 512;
    Harness h;
    h.reset_hits(N);
    h.persist = false;
    h.stub.set_mode(StubBackend::Mode::Async);
    h.stub.set_default_script(script_ok(N));
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 64;
    {
        Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(&sched);
        sched.start("http://x/v", nullptr, N, HoleSet{});
        CHECK(h.wait_idle_ge(1));
        CHECK_EQ(h.err_n, 0);
        CHECK(h.hits_exactly_once(Range{0, N}));
        CHECK(h.synthetic_ok(Range{0, N}));
        CHECK_EQ(sched.active_task_count(), 0);
        h.set_scheduler(nullptr);
    }
    check_stub(h.stub);
}

TEST_CASE(async_concurrent_setters_during_download) {
    syp::test::Watchdog wd("async_concurrent_setters_during_download",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs,
                           kAsyncWatchdogHint);
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.persist = false;
    h.stub.set_mode(StubBackend::Mode::Async);
    auto sc = script_ok(N);
    sc.chunk_size = 32;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 128;
    {
        Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(&sched);
        sched.start("http://x/v", nullptr, N, HoleSet{});

        std::atomic<bool> stop{false};
        std::thread mut([&] {
            int64_t pos = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                sched.set_read_position(pos);
                sched.set_target_end((pos + 800) > N ? -1 : pos + 800);
                sched.notify_persisted(Range{0, 0});  // 空区间 no-op，只为走锁
                pos = (pos + 64) % N;
                std::this_thread::yield();
            }
        });

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (h.delivered.load(std::memory_order_relaxed) == 0
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);
        mut.join();
        sched.set_read_position(0);
        sched.set_target_end(-1);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            if (sched.active_task_count() == 0
                && h.delivered.load(std::memory_order_relaxed) >= N) {
                break;
            }
            std::this_thread::yield();
        }
        CHECK(h.hits_exactly_once(Range{0, N}));
        CHECK_EQ(h.err_n, 0);
        h.set_scheduler(nullptr);
    }
    check_stub(h.stub);
}

TEST_CASE(async_stop_mid_download) {
    syp::test::Watchdog wd("async_stop_mid_download", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 8192;
    Harness h;
    h.reset_hits(N);
    h.persist = false;
    h.stub.set_mode(StubBackend::Mode::Async);
    auto sc = script_ok(N);
    sc.chunk_size = 16;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 256;
    int nreq = 0;
    {
        Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(&sched);
        sched.start("http://x/v", nullptr, N, HoleSet{});
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (h.delivered.load(std::memory_order_relaxed) == 0
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        nreq = h.stub.request_count();
        sched.stop();
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (sched.active_task_count() > 0
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        CHECK_EQ(sched.active_task_count(), 0);
        CHECK_EQ(h.stub.request_count(), nreq);
        h.set_scheduler(nullptr);
    }
    CHECK_EQ(h.stub.request_count(), nreq);
    check_stub(h.stub);
}

TEST_CASE(schedule_callback_throw_clears_in_schedule) {
    // emit(on_idle) 在 schedule() 锁外调用用户回调。回调抛异常时，
    // in_schedule_ 必须被 RAII 清零，否则 ~Scheduler 会永久 wait。
    constexpr int64_t N = 200;
    StubBackend stub;
    stub.set_default_script(script_ok(N));
    FakeClock clk;

    struct Ctx {
        int idle_n = 0;
        bool throw_idle = true;
        static void on_idle(void* p) {
            auto* c = static_cast<Ctx*>(p);
            ++c->idle_n;
            if (c->throw_idle) {
                c->throw_idle = false;
                throw std::runtime_error("on_idle");
            }
        }
    } ctx;

    SchedulerCallbacks cb{};
    cb.ctx = &ctx;
    cb.on_idle = &Ctx::on_idle;

    SchedulerConfig cfg = cfg_small();
    HoleSet all;
    all.add(Range{0, N});

    auto sched = std::make_unique<Scheduler>(stub.backend(), clk.clock(), cfg, cb);

    bool threw = false;
    try {
        // 启动时目标已齐：schedule() 同步走 emit(idle)，不发请求。
        sched->start("http://x/v", nullptr, N, all);
    } catch (const std::runtime_error& ex) {
        threw = true;
        CHECK_EQ(std::string(ex.what()), std::string("on_idle"));
    }
    CHECK(threw);
    CHECK_EQ(ctx.idle_n, 1);

    // 守卫已把 in_schedule_ 清零：后续 schedule() 必须真正进入，
    // 而不是只把 schedule_again_ 置位后立刻返回。
    sched->pause();
    sched->resume();
    CHECK_EQ(ctx.idle_n, 2);
    CHECK_EQ(sched->active_task_count(), 0);

    // 析构不得因 in_schedule_ 卡死。放到旁路线程，超时则判失败，避免拖死整份 ctest。
    std::atomic<bool> dtor_done{false};
    std::thread dtor_thr([&] {
        sched.reset();
        dtor_done.store(true, std::memory_order_release);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!dtor_done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(dtor_done.load(std::memory_order_acquire));
    if (dtor_done.load(std::memory_order_acquire)) {
        dtor_thr.join();
    } else {
        tiny_test::fail(__FILE__, __LINE__,
                        "Scheduler dtor hung after on_idle threw from schedule()");
        dtor_thr.detach();
    }
    check_stub(stub);
}

TEST_CASE(async_dtor_with_inflight_tasks) {
    syp::test::Watchdog wd("async_dtor_with_inflight_tasks", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.persist = false;  // 析构时不要从 on_data 再进 Scheduler
    h.stub.set_mode(StubBackend::Mode::Async);
    auto sc = script_ok(N);
    sc.chunk_size = 16;
    // 确定性化：原来靠
    // "start() 之后 yield 轮询 2s 墙钟截止时间等 active_task_count() 变正"
    // 来断言"析构时确实有在途任务"，16 路并行压满机器时 async worker
    // 可能在测试线程第一次看到之前就已经跑完整个任务并被 reap_except() 收掉
    // ——active_task_count() 从未被测试线程观察到非零，断言偶发假红
    // （实测约 0.2%，1/480）。改用 StubBackend 已有的
    // pause_after_first_on_data_once + wait_paused() 闩锁：worker 线程交付
    // 第一块数据后卡在暂停点、不检查取消也不可能合成 on_complete，
    // 所以只要 wait_paused() 返回 true，"此刻确有一个任务真在途"就是机制上
    // 钉死的事实，不是运气——不会因为完成得快而漏判。
    sc.pause_after_first_on_data_once = true;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 128;
    {
        auto sched = std::make_unique<Scheduler>(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(sched.get());
        sched->start("http://x/v", nullptr, N, HoleSet{});

        const bool paused = h.stub.wait_paused(8000);
        CHECK(paused);
        if (!paused) {
            // worker 没到暂停点：可能这一轮真的没有任务被创建（比如切片
            // 逻辑变了），后面的断言无意义，也不能再碰仍可能被后台线程
            // 持有的对象。
            h.set_scheduler(nullptr);
            return;
        }
        // 确定性构造的前置状态：worker 卡在暂停点，任务不可能已完成，
        // active_task_count() > 0 是必然，不是撞出来的。
        CHECK(sched->active_task_count() > 0);

        // ~Scheduler 会走 ~DLTask 等 finished_emitted_，而 worker 还卡在我们
        // 的暂停闩锁上——必须让析构与 release_paused() 并发（同
        // window_advance_reaps_task_reentrant_destroy_from_worker_thread
        // 的 Reaper 是同一形状）：析构线程等 worker 收尾，测试主线程释放暂停
        // 放行 worker，两边都是谓词式等待，不要求先后顺序。
        std::atomic<bool> dtor_done{false};
        std::thread dtor_thr([&] {
            sched.reset();
            dtor_done.store(true, std::memory_order_release);
        });
        h.stub.release_paused();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!dtor_done.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (dtor_done.load(std::memory_order_acquire)) {
            dtor_thr.join();
        } else {
            tiny_test::fail(__FILE__, __LINE__,
                            "Scheduler dtor 没能在有界时间内返回（在途任务已"
                            "确定性钉死为真在途，不是撞运气没等到）");
            // 不能 join/继续往下走：dtor_thr 仍可能在摸 sched 指向的、这个
            // 作用域一旦返回就会被其它局部量拖着一起析构的对象。交给看门狗
            // 硬退出，FAIL 行已经先落地。
            dtor_thr.detach();
            wd.block_until_hard_exit("Scheduler::~Scheduler 没能在有界时间内返回");
        }
        h.set_scheduler(nullptr);
    }
    check_stub(h.stub);
}

TEST_CASE(schedule_again_not_lost_between_loop_exit_and_guard) {
    // 注入 B1：T1 尾检见 schedule_again_==false 后 break 放锁，T2 在
    // ~InScheduleGuard 清 in_schedule_ 之前调 schedule()，只把
    // schedule_again_ 置上就返回。窗口没被占用、也没人再调 schedule()。
    constexpr int64_t N = 400;
    Harness h;
    h.reset_hits(N);
    h.persist = false;
    auto sc = script_ok(N);
    sc.chunk_size = 64;
    h.stub.set_default_script(sc);
    SchedulerConfig cfg = cfg_small();
    cfg.min_segment_size = 100;
    cfg.segment_size_hint = 100;

    HoleSet already;
    already.add(Range{0, 100});

    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.set_target_end(100);
    sched.start("http://x/v", nullptr, N, already);
    CHECK_EQ(sched.active_task_count(), 0);
    CHECK_EQ(h.stub.request_count(), 0);

    struct Hook {
        Scheduler* s = nullptr;
        static void run(void* p) {
            auto* hk = static_cast<Hook*>(p);
            hk->s->set_test_hook_after_schedule_loop(nullptr, nullptr);
            hk->s->set_target_end(200);
        }
    } hook;
    hook.s = &sched;
    sched.set_test_hook_after_schedule_loop(&Hook::run, &hook);

    // 窗口没变也会进 schedule()，把钩子打在「循环已 break、守卫未析构」处。
    sched.notify_persisted(Range{0, 100});

    CHECK(sched.active_task_count() > 0);
    CHECK(h.stub.request_count() > 0);
    const auto inflight = sched.inflight_ranges();
    CHECK(inflight.contiguous_from(100) > 0);

    sched.stop();
    h.set_scheduler(nullptr);
    check_stub(h.stub);
}

// -------------------------------------------------------------- 思路 3（dl 层级）
//
// 上面所有用例都用同步泵（`pump()`）单步执行，交错完全由测试代码的调用
// 顺序钉死，不需要真线程。但这也意味着它们从没测过"后端在自己的异步
// 回调线程上，独立于任何调用方决定，合成 on_complete 并同线程 destroy"
// 这个真正触发过死锁的形状——test_apple_http_backend.cpp 的
// destroy_in_on_complete_after_* 系列覆盖的是"后端隔离测试"这一格；
// 这里补的是"真实 Scheduler + 真实 DLTask + 契约合规的异步后端"这一格。
//
// ⚠️ 顺序约束：下面这条用例必须留在本文件的最后。它一旦真的挂死
// （回归了本用例要守住的那条重入路径），Reaper 会 detach 而不是 join
// 卡住的线程（见 ~Reaper 的注释）——这依赖"之后不会再有别的用例构造新
// 对象跟它抢内存/竞态"，所以新用例一律加在它之前。
namespace {

// sched.set_read_position() 在本用例的交错里会阻塞在 ~DLTask 等
// finished_emitted_（见 dl_task.cpp 的析构函数），必须放到独立线程跑，
// 不能占用测试主线程——否则测试主线程既要跑它、又要 release_paused()，
// 自己等自己。
struct Reaper {
    Scheduler* sched = nullptr;
    int64_t    pos   = 0;

    std::mutex              mu;
    std::condition_variable cv;
    bool                    done = false;
    std::thread              th;

    void run() {
        th = std::thread([this] {
            sched->set_read_position(pos);
            std::lock_guard<std::mutex> g(mu);
            done = true;
            cv.notify_all();
        });
    }

    bool wait_done(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [this] { return done; });
    }

    // 只在确认 wait_done() 已经返回 true 时才 join——否则线程可能永久卡
    // 在 ~DLTask 里，join() 会把测试进程也拖死。没等到就 detach：不再等
    // 它，让测试能继续往下走（这是本文件最后一条用例，之后不会再有别的
    // 用例构造新对象与它抢内存/竞态）。
    ~Reaper() {
        if (!th.joinable()) return;
        bool ok = false;
        {
            std::lock_guard<std::mutex> g(mu);
            ok = done;
        }
        if (ok) th.join();
        else th.detach();
    }
};

// 本用例的安全网（照 test_apple_http_backend.cpp 那套：软超时打诊断、
// 硬超时 _exit，实现见 tests/support/watchdog.h）。
//
// 为什么这条用例非要不可：它一旦真的挂死（回归了要守住的那条重入路径），
// 卡住的 worker / reaper 线程还在 Scheduler / Harness / StubBackend 内部，
// 而这三个都是用例栈上的局部量 —— 用例提前 return 就会析构它们，留在后台
// 的线程随即踩到已释放的内存（真实 UAF）。有了看门狗才能在失败路径上选择
// 「不返回、交给硬超时 _exit」，也才敢往 StubBackend 里注入一个真会自死锁的
// 桩来做反向自检。
//
// 硬超时取 20000 而不是 apple 套件的 30000：scheduler 目标的 ctest TIMEOUT
// 是 45s，30000 叠上 block_until_hard_exit 的 5s 兜底就是 35s，离 45s 太近 ——
// 与 ctest TIMEOUT 撞看门狗那条教训同源。
// 本套件正常总耗时约 0.8s，20s 有 20 倍以上余量。
constexpr int kReentrantSoftMs = 4000;
constexpr int kReentrantWaitMs = 8000;
constexpr int kReentrantHardMs = 20000;

constexpr const char* kReentrantHint =
    "本用例要回归的形状是：worker 线程在自己的回调栈上合成 on_complete、"
    "同线程重入 backend->destroy()。卡住通常意味着 destroy 在等一个只有它自己"
    "返回后才可能满足的条件（自死锁），~DLTask 于是永远等不到 finished_emitted_。";

}  // namespace

TEST_CASE(window_advance_reaps_task_reentrant_destroy_from_worker_thread) {
    constexpr int64_t N = 4096;
    // 必须是本作用域的第一个局部量：这样它最后析构，整条析构链
    // （~Reaper → ~Scheduler → ~Harness/~StubBackend）都还在它的保护之下。
    syp::test::Watchdog wd("window_advance_reaps_task_reentrant_destroy_from_worker_thread",
                           kReentrantSoftMs, kReentrantHardMs, kReentrantHint);
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.chunk_size = 64;
    sc.pause_after_first_on_data_once = true;
    h.stub.set_mode(StubBackend::Mode::Async);
    h.stub.set_default_script(sc);

    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 1;
    cfg.min_segment_size = N;   // 不切片：单条任务扛下整个 [0, N)
    cfg.segment_size_hint = 0;

    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, N}));

    // 等 worker 线程真的卡在暂停点：已经把第一块数据交给 dl 层、还没
    // 检查是否被取消。此时任务确实处于 Receiving、next_offset() 非零，
    // 不是"还没开始"或"已经结束"——否则后面复现的就不是这条死锁的形状。
    const bool paused = h.stub.wait_paused(kReentrantWaitMs);
    CHECK(paused);
    if (!paused) return;   // worker 没到暂停点，后面的断言无意义
    CHECK(h.data_n >= 1);
    CHECK_EQ(sched.active_task_count(), 1);

    // 窗口推进到任务范围之外：should_stop_slot_locked() 判定"窗口已经
    // 不需要它"，schedule() 的 to_cancel 循环同步调 task->cancel()（这
    // 一步不依赖暂停：Async 桩的 cancel() 只置位、不合成）。随后
    // reap_except() 走到 ~DLTask，它会等 finished_emitted_——只有释放
    // 暂停、放 worker 线程跑完 on_data 收尾 → 在worker自己的线程上合成
    // on_complete → 同线程同步 destroy → finish()，这个条件才会满足。
    // 这条调用必须放到独立线程，否则会和"释放暂停"这一步互相等待。
    Reaper reaper;
    reaper.sched = &sched;
    reaper.pos   = N;
    reaper.run();

    // release_paused() 与 reaper 线程是否已经到达等待点无关——两边都是
    // 谓词式等待，不要求先后顺序。
    h.stub.release_paused();

    const bool reaped = reaper.wait_done(kReentrantWaitMs);
    CHECK(reaped);
    if (!reaped) {
        tiny_test::fail(__FILE__, __LINE__, "reaper.wait_done",
                        "set_read_position 没能在 8s 内返回——很可能是 ~DLTask "
                        "卡在等自己的回调帧（本用例要回归的正是这个形状）");
        // 不能 return：reaper / worker 线程仍卡在 Scheduler / StubBackend 内部，
        // 而这两个是本作用域的栈上局部量，往下走就会把它们析构掉（UAF）。
        // 交给看门狗硬超时 _exit()，FAIL 行已经先落地了。
        wd.block_until_hard_exit("set_read_position 没能在有界时间内返回");
    }

    CHECK_EQ(sched.active_task_count(), 0);
    // 契约自查：没有"complete 之后再回调"、没有"destroy 之前没 complete"、
    // cancel 计数与 complete 计数相等——同线程重入 destroy 没有破坏桩自己
    // 的记账。
    check_stub(h.stub);
    // 第一块数据确实到过 dl 层，不是被暂停机制悄悄吞掉了。
    CHECK(h.hits_exactly_once(Range{0, 64}));

    sched.stop();
    h.set_scheduler(nullptr);
}

// ── 限速接线 ──────────────────────────────────────────────────────
//
// 限速器一律用**独立实例**、不起唤醒线程，与 Harness 共用同一个 FakeClock，
// 经 SchedulerConfig::limiter 注入；由用例调 pump_for_test() 手动派发唤醒。
// 声明顺序 h → rl → sched：析构逆序，Scheduler（持 Subscription）先于
// RateLimiter 析构，RateLimiter（持时钟指针）先于 Harness 析构。
//
// 同步桩：pump_all() 在当前假时刻把所有在途请求一口气下完，因此一个分片
// 的字节全部在同一毫秒内记账——余额的变化可以精确手算。

namespace {

using syp::dl::RateClass;
using syp::dl::RateLimiter;

// 一轮：把在途的全下完 → 假时钟前进 step_ms → 派发到点的唤醒（唤醒里
// request_schedule() 会同步建新任务，下一轮的 pump_all 再把它下完）。
void rl_round(Harness& h, RateLimiter& rl, int64_t step_ms) {
    pump_all(h.stub);
    h.clk.t += step_ms;
    rl.pump_for_test();
}

}  // namespace

TEST_CASE(rate_limited_scheduler_is_woken_and_eventually_finishes) {
    // 钉死"永久卡住"：被拒后必须登记唤醒，否则没有任何
    // 在途任务、也没有任何数据到达来再触发调度。
    constexpr int64_t N = 64 * 1024;   // 65536
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    RateLimiter rl(h.clk.clock(), /*start_thread=*/false);
    rl.set_rate(16 * 1024);            // 16384 B/s，满桶 16384 起算
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 1;
    cfg.limiter = &rl;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    // 分片 = min(65536, max(16384, 256)) = 16384
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 16384}));

    pump_all(h.stub);
    // 余额 16384 − 16384 = 0，Playing 要求 > 0 ⇒ 被拒。此刻无在途任务、
    // 目标里还有洞 [16384, 65536)：不得误报 idle（洞还在，只是暂时不许发）。
    CHECK_EQ(h.delivered.load(), 16384);
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(sched.active_task_count(), 0);
    CHECK_EQ(h.idle_n, 0);
    // 已登记唤醒：余额 0 mB、阈值 0 ⇒ 到点 = 0 + (0 − 0) / 16384 + 1 = 1 ms
    CHECK_EQ(rl.next_due_ms_for_test(), 1);

    for (int i = 0; i < 200 && h.delivered.load() < N; ++i) {
        rl_round(h, rl, 100);
    }
    CHECK_EQ(h.delivered.load(), 65536);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK(h.synthetic_ok(Range{0, N}));
    CHECK_EQ(h.idle_n, 1);
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(rate_limited_scheduler_admits_no_more_than_rate_times_time_plus_slack) {
    constexpr int64_t N = 1024 * 1024;   // 1 MiB
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    RateLimiter rl(h.clk.clock(), /*start_thread=*/false);
    rl.set_rate(64 * 1024);              // 65536 B/s
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.limiter = &rl;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int i = 0; i < 20; ++i) rl_round(h, rl, 100);   // 推进 20 × 100 = 2000 ms
    CHECK_EQ(h.clk.t, 2000);

    int64_t sum = 0;
    for (const Range& r : all_requests(h.stub, N)) sum += r.size();
    // 上界 = R·T + 容量 + 在途上限 × 分片上限
    //      = 65536 × 2 + 65536 + 3 × 65536 = 131072 + 65536 + 196608 = 393216
    CHECK(sum <= 393216);
    // 精确值（同步桩下可手算）：t=0 满桶 65536，探测 [0,65536) 准入；206 到达
    // 后并发放开到 3，此时还没有数据到达、余额仍 65536 ⇒ 再准入两片
    // [65536,131072)、[131072,196608)。三片全部到达后余额 = 65536 − 196608
    // = −131072 字节；恢复到 > 0 需 131072 / 65536 = 2 s 再多 1 ms ⇒ 到点
    // 2001 ms，推进到 2000 ms 时尚未到点。故 sum = 3 × 65536 = 196608。
    CHECK_EQ(sum, 196608);
    CHECK_EQ(h.stub.request_count(), 3);
    CHECK_EQ(rl.next_due_ms_for_test(), 2001);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(segments_are_capped_to_one_second_of_rate) {
    // 用生产默认的 min_segment_size（512 KiB）：剩余 16384 ≤ 下限时
    // segment_size_locked 走"不切、一片下完"的早退分支——限速上限也必须
    // 管到那一路（分片上限 = min(原上限, max(R × 1 秒, 256))）。
    constexpr int64_t N = 16384;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    RateLimiter rl(h.clk.clock(), /*start_thread=*/false);
    rl.set_rate(4096);                   // 上限 = max(4096, 256) = 4096
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 512 * 1024;
    cfg.limiter = &rl;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int i = 0; i < 200 && h.delivered.load() < N; ++i) {
        rl_round(h, rl, 100);
    }
    CHECK_EQ(h.delivered.load(), 16384);
    const auto rs = all_requests(h.stub, N);
    // 16384 / 4096 = 4 片
    CHECK_EQ(rs.size(), size_t{4});
    for (const Range& r : rs) CHECK(r.size() <= 4096);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(segment_cap_never_goes_below_no_split_floor) {
    constexpr int64_t N = 1000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    RateLimiter rl(h.clk.clock(), /*start_thread=*/false);
    rl.set_rate(10);                     // 上限 = max(10, 256) = 256
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 1;
    cfg.limiter = &rl;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    // 每片 256 字节把余额扣到约 −246，按 10 B/s 恢复要 ~25 s；每轮推 1 s，
    // 4 片 × ~26 轮 ≈ 104 轮 < 200。
    for (int i = 0; i < 200 && h.delivered.load() < N; ++i) {
        rl_round(h, rl, 1000);
    }
    CHECK_EQ(h.delivered.load(), 1000);
    const auto rs = all_requests(h.stub, N);
    // 1000 = 256 + 256 + 256 + 232（尾片：剩余 232 ≤ 256，不切）
    REQUIRE(rs.size() == size_t{4});
    for (size_t i = 0; i + 1 < rs.size(); ++i) CHECK(rs[i].size() >= 256);
    CHECK_EQ(rs[0], (Range{0, 256}));
    CHECK_EQ(rs[1], (Range{256, 512}));
    CHECK_EQ(rs[2], (Range{512, 768}));
    CHECK_EQ(rs[3], (Range{768, 1000}));
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(unlimited_scheduler_behaves_as_before) {
    // 基线：segment_size_auto_ceil_div（N=3000, conc=3, min=1 → 每片
    // ceil(3000/3)=1000）。R == 0 时请求序列必须与它逐条相同。
    constexpr int64_t N = 3000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    RateLimiter rl(h.clk.clock(), /*start_thread=*/false);
    rl.set_rate(0);
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size = 1;
    cfg.segment_size_hint = 0;
    cfg.limiter = &rl;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1000}));
    pump_probe_response(h.stub);
    CHECK_EQ(h.stub.request_count(), 3);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1000}));
    CHECK_EQ(req_range(h.stub.request_at(1), N), (Range{1000, 2000}));
    CHECK_EQ(req_range(h.stub.request_at(2), N), (Range{2000, 3000}));
    pump_all(h.stub);
    CHECK_EQ(h.stub.request_count(), 3);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.idle_n, 1);
    CHECK_EQ(rl.next_due_ms_for_test(), -1);   // 从未被拒、从未 arm
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(set_rate_class_takes_effect_on_the_next_schedule) {
    constexpr int64_t N = 2000;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    RateLimiter rl(h.clk.clock(), /*start_thread=*/false);
    rl.set_rate(1000);                   // 满桶 1000
    rl.debit(700);                       // 余额 1000 − 700 = 300 ∈ (0, 500]
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 1;
    cfg.rate_class = RateClass::Preload;
    cfg.limiter = &rl;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    CHECK(sched.rate_class() == RateClass::Preload);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    // Preload 需余额 > 容量/2 = 500；300 ≤ 500 ⇒ 被拒，不发请求，也不报 idle。
    CHECK_EQ(h.stub.request_count(), 0);
    CHECK_EQ(h.idle_n, 0);
    // 按 Preload 登记：0 + (500000 − 300000) mB / 1000 + 1 = 201 ms
    CHECK_EQ(rl.next_due_ms_for_test(), 201);

    sched.set_rate_class(RateClass::Playing);
    CHECK(sched.rate_class() == RateClass::Playing);
    // Playing 需余额 > 0；300 > 0 ⇒ 立即准入。假时钟没动，不是唤醒发的。
    CHECK_EQ(h.clk.t, 0);
    CHECK_EQ(h.stub.request_count(), 1);
    // 分片 = min(2000, max(1000, 256)) = 1000
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1000}));
    pump_all(h.stub);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(fail_open_between_rejected_admit_and_arm_does_not_stall) {
    // admit 被拒 → [fail-open：R 被置回 0，
    // 它那次同步 fire_due() 已经跑完，本订阅当时还没 arm 所以没被叫到] →
    // arm。此后 R == 0，唤醒线程根本不存在，再没有任何派发者；若 arm 之后
    // 不复查一次 admit，调度器就永久停在这里。
    //
    // 用例里 fail-open 由钩子模拟：在"被拒之后、arm 之前"把 R 置 0。之后
    // **故意不调 pump_for_test()**——那就是"没有派发者"的世界。
    //
    // persist = false：不让 on_data 经 notify_persisted 再触发 schedule()。
    // 否则首片最后一块数据到达时那轮 schedule()（槽已判为可停）就先被拒、
    // 钩子在那里打，随后任务结束那轮 schedule() 读到 R == 0 顺手把活发了，
    // 窗口被第二个触发点掩盖、测不出缺陷。关掉之后"任务结束"是首片的
    // 最后一个事件，被拒恰好落在它上面。
    constexpr int64_t N = 64 * 1024;   // 65536
    Harness h;
    h.reset_hits(N);
    h.persist = false;
    h.stub.set_default_script(script_ok(N));
    RateLimiter rl(h.clk.clock(), /*start_thread=*/false);
    rl.set_rate(16 * 1024);            // 16384 B/s，满桶 16384 起算
    SchedulerConfig cfg = cfg_small();
    cfg.max_concurrent_tasks = 1;
    cfg.limiter = &rl;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);

    struct Hook {
        RateLimiter* rl = nullptr;
        int calls = 0;
        // 持 Scheduler::mu_ 调用：只对不起线程的独立实例 set_rate(0)，
        // 那条路径只取限速器叶子锁、不回调。
        static void run(void* p) {
            auto* hk = static_cast<Hook*>(p);
            ++hk->calls;
            hk->rl->set_rate(0);
        }
    } hook;
    hook.rl = &rl;
    sched.set_test_hook_before_arm(&Hook::run, &hook);

    sched.start("http://x/v", nullptr, N, HoleSet{});
    // 满桶准入首片：分片 = min(65536, max(16384, 256)) = 16384
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(hook.calls, 0);

    // 首片到齐：余额 16384 − 16384 = 0，Playing 要求 > 0 ⇒ 被拒 ⇒ 钩子把 R
    // 置 0 ⇒ arm。复查读到 R == 0 ⇒ 重跑一轮，同一次 schedule() 里就发出
    // 剩下的 [16384, 65536)（R == 0 分片不设上限，并发 1 ⇒ 一片）。
    pump_all(h.stub);
    CHECK_EQ(hook.calls, 1);
    CHECK_EQ(rl.rate(), 0);
    REQUIRE(h.stub.request_count() == 2);
    CHECK_EQ(req_range(h.stub.request_at(1), N), (Range{16384, 65536}));

    pump_all(h.stub);
    CHECK_EQ(h.delivered.load(), 65536);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.idle_n, 1);
    CHECK_EQ(h.err_n, 0);
    // 钩子只在"被拒"时触发；R == 0 之后再无拒绝。
    CHECK_EQ(hook.calls, 1);
    check_stub(h.stub);
    sched.set_test_hook_before_arm(nullptr, nullptr);
    h.set_scheduler(nullptr);
}

// ── 坏任务检测 ──────────────────────────────────────────────────────
//
// 一律注入**独立** HealthTicker（不起线程、与 Harness 共用同一个假时钟），
// 由用例调 pump_for_test() 在当前线程上同步派发 health_check；限速器也用
// 独立实例（不碰进程单例）。声明顺序 h → tk → rl → sched：析构逆序，
// Scheduler（持两个订阅）先于 ticker / 限速器析构。
//
// 到点序列的算法（手算每条用例的期望值时用）：
//   · schedule() 尾检里"有在途任务且未 armed" ⇒ arm_after(check_interval)，
//     到点 = 当时的假时刻 + check_interval；
//   · health_check 入口先清 armed；本轮没杀人则按"有在途"再 arm 一次，
//     到点 = 本次回调时的假时刻 + check_interval；杀了人则由随后那轮
//     schedule() 的尾检 arm（同一假时刻，结果相同）。
//   · 同步桩的 cancel 在"已 start、不在回调里"时当场回 on_complete(CANCELED)，
//     所以被杀的请求在 health_check 返回前就已经终态、补发也已经发出。
namespace {
using syp::dl::HealthTicker;

SchedulerConfig cfg_health(HealthTicker* tk, RateLimiter* rl) {
    SchedulerConfig c = cfg_small();
    c.health.enabled = true;
    c.health.ticker = tk;
    c.limiter = rl;                  // 独立限速器，不碰进程单例
    c.task.connect_timeout_ms = 10000;
    c.task.read_timeout_ms = 15000;
    c.task.max_retries = 0;          // 被 cancel 的任务不在 DLTask 内部重试
    return c;
}

// 推进假时钟到 t，并派发到点的 ticker 回调（health_check 在当前线程同步跑）。
void tick_to(Harness& h, HealthTicker& tk, int64_t t) {
    h.clk.t = t;
    tk.pump_for_test();
}

// 包一层同步桩：cancel 先扣下、不转发，直到用例 flush()。用来模拟"cancel
// 之后很久才（或根本不）回 on_complete"的后端：被判坏
// 的任务还没到终态，health_check 自己的那轮 schedule() 必须已经补发。
// 同步桩的 cancel 会当场 complete、触发 canceled 分支的重调度，掩盖掉
// "health_check 杀完不 schedule()"这种错误，所以必须用这层包装才测得到。
// passthrough 置真之后不再扣留（Scheduler 析构里的 cancel_all 必须直达，
// 否则 ~DLTask 等终态会永久卡住）。
struct DeferCancelBackend {
    static DeferCancelBackend* current;
    const syp_http_backend* inner = nullptr;
    syp_http_backend table{};
    std::mutex mu;                                   // 护 pending / passthrough
    std::vector<syp_http_request_handle*> pending;
    bool passthrough = false;
    // 每扣下一次 cancel 就在调用线程上（锁外）调一次；用例用来在
    // "health_check 已放锁、正在锁外 cancel"这个窗口里插入别的动作。
    std::function<void()> on_hold;
    // 由用例在 tk.pump_for_test() 前后置位：记录 destroy 是否发生在派发中
    // （= ~DLTask 跑在 ticker 回调路径上）。
    std::atomic<bool> in_pump{false};
    std::atomic<bool> destroyed_in_pump{false};

    explicit DeferCancelBackend(const syp_http_backend* in) : inner(in) {
        table = *in;
        table.cancel  = &on_cancel;
        table.destroy = &on_destroy;
        current = this;
    }
    ~DeferCancelBackend() {
        release_all();
        current = nullptr;
    }
    DeferCancelBackend(const DeferCancelBackend&)            = delete;
    DeferCancelBackend& operator=(const DeferCancelBackend&) = delete;

    const syp_http_backend* backend() const noexcept { return &table; }

    static void on_cancel(syp_http_request_handle* h) {
        DeferCancelBackend* self = current;
        if (self == nullptr) return;
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> g(self->mu);
            if (!self->passthrough) {
                self->pending.push_back(h);
                hook = self->on_hold;
            }
        }
        if (!hook && self->passthrough_now()) {
            self->inner->cancel(h);
            return;
        }
        if (hook) hook();
    }
    static void on_destroy(syp_http_request_handle* h) {
        DeferCancelBackend* self = current;
        if (self == nullptr) return;
        if (self->in_pump.load()) self->destroyed_in_pump.store(true);
        self->inner->destroy(h);
    }
    bool passthrough_now() {
        std::lock_guard<std::mutex> g(mu);
        return passthrough;
    }
    // 放行所有扣下的 cancel，并从此直达。幂等、可跨线程调用。
    void release_all() {
        std::vector<syp_http_request_handle*> v;
        {
            std::lock_guard<std::mutex> g(mu);
            passthrough = true;
            v.swap(pending);
        }
        for (auto* h : v) inner->cancel(h);
    }

    // 声明在 Scheduler **之后**：作用域退出（含 REQUIRE 早退）时先于
    // ~Scheduler 放行，否则 ~Scheduler 的 cancel_all 被扣下、~DLTask 永久等终态。
    struct ReleaseGuard {
        DeferCancelBackend& d;
        explicit ReleaseGuard(DeferCancelBackend& dc) : d(dc) {}
        ~ReleaseGuard() { d.release_all(); }
        ReleaseGuard(const ReleaseGuard&)            = delete;
        ReleaseGuard& operator=(const ReleaseGuard&) = delete;
    };
};
DeferCancelBackend* DeferCancelBackend::current = nullptr;
}  // namespace

TEST_CASE(stall_in_connecting_killed_after_connect_timeout_plus_grace) {
    // 到点序列（check_interval = 1000）：start@0 arm ⇒ 1000；此后每次回调
    // 都有在途 ⇒ 2000, 3000, …, 12000。阈值 = 0 + 10000 + 2000 = 12000，
    // 恰好落在一次检查上：11000 时差 11000 < 12000 不杀，12000 时 ≥ 杀。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(tk.next_due_ms_for_test(), 1000);
    // 一步都不 pump：请求 1 停在 Connecting。
    for (int64_t t = 1000; t <= 11000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(tk.next_due_ms_for_test(), 12000);
    tick_to(h, tk, 11999);           // ticker 到点是 12000，这里不应回调
    CHECK_EQ(sched.stall_kills(), 0);
    tick_to(h, tk, 12000);
    CHECK_EQ(sched.stall_kills(), 1);
    CHECK_EQ(sched.consecutive_errors(), 1);
    CHECK_EQ(h.stub.request_count(), 2);          // 区间当轮重发
    CHECK_EQ(req_range(h.stub.request_at(1), N).start, 0);
    CHECK_EQ(tk.next_due_ms_for_test(), 13000);   // 补发的新任务在途 ⇒ 再 arm
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(stall_threshold_uses_task_default_when_timeout_unset) {
    // connect_timeout_ms = 0 且 read_timeout_ms = 0（都"用后端默认"）：阈值按
    // 10000 算，不能被 0 压成"只剩宽限 2000"而秒杀正常建连。
    // 到点序列同 stall_in_connecting…：1000, …, 12000；11000 不杀、12000 杀。
    // （只有 connect = 0、read > 0 时借读超时，见下一条用例。）
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    cfg.task.connect_timeout_ms = 0;
    cfg.task.read_timeout_ms = 0;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 11000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);
    tick_to(h, tk, 12000);
    CHECK_EQ(sched.stall_kills(), 1);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(connecting_stall_borrows_read_timeout_when_connect_unset) {
    // connect_timeout_ms = 0、read_timeout_ms = 30000。Apple 后端
    // 此时连接阶段借用 read_timeout_ms（apple_http_backend.mm），调度器的
    // Connecting 挂死阈值要与之一致：30000 + 宽限 2000 = 32000，不是 10000 +
    // 2000 = 12000。
    // 到点序列同 stall_in_connecting…：start@0 ⇒ 1000, 2000, …（每次都有在途）。
    //   12000：差 12000 < 32000 ⇒ 不杀（旧实现在这里杀）；
    //   31000：差 31000 < 32000 ⇒ 不杀；32000：差 32000 ≥ 32000 ⇒ 杀。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    cfg.task.connect_timeout_ms = 0;
    cfg.task.read_timeout_ms = 30000;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 12000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);
    for (int64_t t = 13000; t <= 31000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(tk.next_due_ms_for_test(), 32000);
    tick_to(h, tk, 32000);
    CHECK_EQ(sched.stall_kills(), 1);
    CHECK_EQ(sched.consecutive_errors(), 1);
    CHECK_EQ(h.stub.request_count(), 2);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(stall_in_receiving_uses_read_timeout_from_last_progress) {
    // check_interval 设成 500，让阈值时刻落在一次检查上。
    // 到点序列：start@0 arm ⇒ 500；500 时只拨钟、泵数据、不派发；之后每
    // 500ms 派发一次：1000, 1500, …（每次都有在途 ⇒ 下一次 = 当前 + 500）。
    // 最近进展 = 500（首块 1024 字节）。Receiving 阈值 15000 + 2000 = 17000，
    // 从 500 起算 ⇒ 17500 到点：17000 时差 16500 不杀，17500 时差 17000 杀。
    // 反证两条错误实现：
    //   · 用 connect_timeout（12000）⇒ 12500 就杀；
    //   · ref 只用尝试起点 0 ⇒ 17000 就杀。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    auto sc = script_ok(N);
    sc.chunk_size = 1024;
    h.stub.set_default_script(sc);
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    cfg.health.check_interval_ms = 500;
    // 本用例只量挂死阈值，把慢判定挡在外面：否则 3500 那次检查以唯一样本
    // 1024 / 3000 = 341 B/s 播下基线，4000 起窗口内无字节 ⇒ speed == 0 <
    // 基线 / 4 且"剩余时间无穷"⇒ 4000、4500 两次 strike 就被当慢任务替换
    // （Receiving 中 speed == 0 同样走慢判定），轮不到 17500 的挂死。
    cfg.health.slow_strikes = std::numeric_limits<int32_t>::max();
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(tk.next_due_ms_for_test(), 500);
    h.clk.t = 500;
    // 同步桩：Running→Responding（无回调）、on_response、第一块 body。
    h.stub.pump_request(1);
    h.stub.pump_request(1);
    h.stub.pump_request(1);
    CHECK_EQ(h.delivered.load(), 1024);           // last_progress = 500
    for (int64_t t = 1000; t <= 17000; t += 500) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(tk.next_due_ms_for_test(), 17500);
    tick_to(h, tk, 17500);
    CHECK_EQ(sched.stall_kills(), 1);
    CHECK_EQ(h.stub.request_count(), 2);
    // 从 next_offset 续，不重下已收字节（stall_replacement_resumes_from_next_offset）
    CHECK_EQ(req_range(h.stub.request_at(1), N).start, 1024);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(stall_kill_counts_error_and_goes_fatal_with_timeout) {
    // 到点序列（check_interval = 1000）：每 1000ms 一次。永不 pump：
    //   请求 1 起点 0     ⇒ 12000 杀，当场补发请求 2（起点 12000）；
    //   请求 2            ⇒ 24000 杀，补发请求 3（起点 24000）；
    //   请求 3            ⇒ 36000 杀，consecutive_errors 到 3 ⇒ fatal，
    //                        on_error(TIMEOUT, 0)，不再补发、不再 arm。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    cfg.max_consecutive_errors = 3;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 35000; t += 1000) {
        // 同步桩上被 cancel 的请求在 cancel() 里当场 complete，不必另外泵。
        tick_to(h, tk, t);
    }
    CHECK_EQ(sched.stall_kills(), 2);
    CHECK_EQ(sched.consecutive_errors(), 2);
    CHECK_EQ(h.err_n, 0);
    CHECK_EQ(h.stub.request_count(), 3);
    CHECK_EQ(req_range(h.stub.request_at(2), N).start, 0);
    tick_to(h, tk, 36000);
    CHECK_EQ(sched.stall_kills(), 3);
    CHECK_EQ(sched.consecutive_errors(), 3);
    CHECK_EQ(h.err_n, 1);
    CHECK_EQ(h.last_err, SYP_ERR_TIMEOUT);
    CHECK_EQ(h.last_http, 0);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);      // fatal 之后不再 arm
    // fatal 路径（cancel_all_tasks）同样在 ticker 路径上，
    // 它锁外持有的引用也要延后释放。本用例从不在 health 路径之外调公开方法，
    // 所以一次都没 reap。延后释放的引用逐次累计：
    //   12000：受害者 1（to_cancel）+ 同步 cancel 引出的 schedule() 的 to_start = 2；
    //   24000：同上 = 4；
    //   36000：受害者 3 = 5；它被 cancel 时同步回的 on_task_finished 见 fatal_
    //          先调一次 cancel_all_tasks（slots_ 里全部 3 个 Slot）= 8；
    //          health_check 自己的 fatal 路径再调一次 = 11。
    CHECK_EQ(sched.deferred_release_count_for_test(), 11);
    for (int64_t t = 37000; t <= 60000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 3);
    CHECK_EQ(h.err_n, 1);
    CHECK_EQ(h.stub.request_count(), 3);          // fatal 之后不再补发
    CHECK_EQ(sched.active_task_count(), 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(stall_victim_is_replaced_before_its_cancel_completes) {
    // 到点序列同 stall_in_connecting…：1000, …, 12000 杀。后端把 cancel
    // 扣下，被杀的任务仍停在 Connecting（非终态）；它已标 dead，不再占
    // 名额与区间，health_check 那一轮 schedule() 当场补发。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    DeferCancelBackend dc(h.stub.backend());
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    Scheduler sched(dc.backend(), h.clk.clock(), cfg_health(&tk, &rl), h.cbs());
    DeferCancelBackend::ReleaseGuard release_guard(dc);
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 12000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 1);
    CHECK_EQ(dc.pending.size(), static_cast<size_t>(1));   // on_complete 还没回
    CHECK_EQ(h.stub.request_count(), 2);
    CHECK_EQ(req_range(h.stub.request_at(1), N).start, 0);
    CHECK_EQ(sched.active_task_count(), 1);
    CHECK_EQ(sched.consecutive_errors(), 1);
    CHECK_EQ(tk.next_due_ms_for_test(), 13000);
    dc.release_all();                              // 迟到的 on_complete(CANCELED)
    CHECK_EQ(sched.consecutive_errors(), 1);       // 不再计一次
    CHECK_EQ(h.stub.request_count(), 2);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(stall_victim_is_never_destroyed_on_ticker_thread) {
    // health_check 放锁之后、锁外 cancel 期间，另一线程的
    // 公开方法 reap_except 把已标 dead 的受害者从 slots_ 摘掉；此时
    // health_check 的局部 to_cancel 成了最后持有者。若它在函数返回时就地
    // 放掉，~DLTask 会跑在 ticker 线程上，对 cancel 不回调的后端永久等终态、
    // 冻住全进程唯一的 ticker。修复后引用被转进 deferred_release_，等下一次
    // 非 health 路径上的公开方法退出时才拆。
    //
    // 确定性：on_hold 在 ticker 路径上的 cancel() 里被调用，在那里起一条线程
    // 调 request_schedule()（其 reap 摘掉受害者）并 join。修复前的实现会卡在
    // ~DLTask 等被扣下的 cancel——为了让它红而不是挂死，另起一个看门线程：
    // 派发 2 秒还没返回就放行 cancel。修复后派发立即返回，看门线程被提前叫走。
    // 到点序列同 stall_in_connecting…：1000, …, 12000 杀。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    DeferCancelBackend dc(h.stub.backend());
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    Scheduler sched(dc.backend(), h.clk.clock(), cfg_health(&tk, &rl), h.cbs());
    DeferCancelBackend::ReleaseGuard release_guard(dc);
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 11000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);

    std::mutex wmu;
    std::condition_variable wcv;
    bool pump_done = false;
    std::thread watchdog;
    int holds = 0;
    dc.on_hold = [&] {
        if (++holds != 1) return;
        std::thread other([&] { sched.request_schedule(); });
        other.join();
        watchdog = std::thread([&] {
            std::unique_lock<std::mutex> lk(wmu);
            if (!wcv.wait_for(lk, std::chrono::seconds(2), [&] { return pump_done; })) {
                lk.unlock();
                dc.release_all();
            }
        });
    };
    dc.in_pump.store(true);
    tick_to(h, tk, 12000);
    dc.in_pump.store(false);
    {
        std::lock_guard<std::mutex> g(wmu);
        pump_done = true;
    }
    wcv.notify_all();
    if (watchdog.joinable()) watchdog.join();
    dc.on_hold = nullptr;

    CHECK_EQ(holds, 1);
    CHECK_EQ(sched.stall_kills(), 1);
    CHECK(!dc.destroyed_in_pump.load());           // ticker 路径上没有拆 DLTask
    CHECK_EQ(sched.deferred_release_count_for_test(), 1);
    CHECK_EQ(h.stub.request_count(), 2);           // 补发照常
    dc.release_all();                              // 迟到的 on_complete(CANCELED)
    sched.request_schedule();                      // 公开方法退出 ⇒ reap
    CHECK_EQ(sched.deferred_release_count_for_test(), 0);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(reap_is_skipped_on_ticker_path) {
    // health_check 引出的用户回调 / 后端回调若在
    // ticker 线程上重入公开方法，其 reap_except 必须早退——否则它会把
    // deferred_release_ 里（以及 slots_ 里）的死任务就地拆在 ticker 线程上。
    // 做法：12000 杀请求 1（cancel 扣下 ⇒ 延后释放）；24000 杀补发的请求 2，
    // 在它被扣下的 cancel 里（ticker 路径上、同一线程）调 request_schedule()。
    // 修复后那次 reap 早退；错误实现会就地拆请求 1、卡在 ~DLTask 等被扣下的
    // cancel——看门线程 2 秒后放行，让它红而不是挂死。
    // 到点序列：1000, …, 12000 杀；13000, …, 24000 杀（请求 2 起点 12000）。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    DeferCancelBackend dc(h.stub.backend());
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    Scheduler sched(dc.backend(), h.clk.clock(), cfg_health(&tk, &rl), h.cbs());
    DeferCancelBackend::ReleaseGuard release_guard(dc);
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 12000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 1);
    // 延后释放的是**引用**：受害者（health_check 的 to_cancel）+ 补发的新
    // Slot（同一路径上那轮 schedule() 的 to_start）= 2。
    CHECK_EQ(sched.deferred_release_count_for_test(), 2);
    for (int64_t t = 13000; t <= 23000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 1);

    std::mutex wmu;
    std::condition_variable wcv;
    bool pump_done = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lk(wmu);
        if (!wcv.wait_for(lk, std::chrono::seconds(2), [&] { return pump_done; })) {
            lk.unlock();
            dc.release_all();
        }
    });
    int holds = 0;
    dc.on_hold = [&] {
        if (++holds != 1) return;
        sched.request_schedule();                  // ticker 路径上重入公开方法
    };
    dc.in_pump.store(true);
    tick_to(h, tk, 24000);
    dc.in_pump.store(false);
    {
        std::lock_guard<std::mutex> g(wmu);
        pump_done = true;
    }
    wcv.notify_all();
    watchdog.join();
    dc.on_hold = nullptr;

    CHECK_EQ(holds, 1);
    CHECK_EQ(sched.stall_kills(), 2);
    CHECK(!dc.destroyed_in_pump.load());
    CHECK_EQ(sched.deferred_release_count_for_test(), 4);   // 又 +2，前两个没被拆
    dc.release_all();
    sched.request_schedule();                      // 非 health 路径 ⇒ 正常 reap
    CHECK_EQ(sched.deferred_release_count_for_test(), 0);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(deferred_victim_not_reaped_on_its_own_callback_stack) {
    // 受害者 X 被别的线程的 reap 从 slots_ 摘掉、停在
    // deferred_release_ 里之后，它迟到的 on_complete(CANCELED) 仍会进
    // on_task_finished（SlotCbGuard 给 X 记 in_cb）。那条回调栈上若有人调
    // 公开方法（头文件允许在回调里调 stop/pause/notify_persisted），其
    // reap_except 只看 slots_ 的 in_cb 就会把 X 就地拆在它自己的回调栈上 ⇒
    // ~DLTask 见"同线程在回调里"不等、直接析构 ⇒ 返回时 UAF。
    // 回调栈上的"公开方法调用"用 schedule 循环后钩子注入：X 的 on_finished
    // → schedule() → 钩子 → request_schedule() → reap_except。
    // 前半段与 stall_victim_is_never_destroyed_on_ticker_thread 相同（12000 杀、
    // 旁路线程 reap 摘掉 X），不再需要看门线程：修复后与修复前都不会挂住
    // （修复前是就地拆，由 destroyed_in_handler 断言抓到）。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    DeferCancelBackend dc(h.stub.backend());
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    Scheduler sched(dc.backend(), h.clk.clock(), cfg_health(&tk, &rl), h.cbs());
    DeferCancelBackend::ReleaseGuard release_guard(dc);
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 11000; t += 1000) tick_to(h, tk, t);
    int holds = 0;
    dc.on_hold = [&] {
        if (++holds != 1) return;
        std::thread other([&] { sched.request_schedule(); });
        other.join();
    };
    tick_to(h, tk, 12000);
    dc.on_hold = nullptr;
    CHECK_EQ(sched.stall_kills(), 1);
    CHECK_EQ(sched.deferred_release_count_for_test(), 1);   // 只剩 deferred 持有 X

    struct HookCtx {
        Scheduler* sched;
        DeferCancelBackend* dc;
        int calls = 0;
    } hc{&sched, &dc};
    sched.set_test_hook_after_schedule_loop(
        [](void* p) {
            auto* c = static_cast<HookCtx*>(p);
            if (++c->calls != 1) return;
            c->sched->set_test_hook_after_schedule_loop(nullptr, nullptr);
            c->dc->in_pump.store(true);       // 复用：标记"在 X 的回调栈上"
            c->sched->request_schedule();
            c->dc->in_pump.store(false);
        },
        &hc);
    dc.release_all();              // X 的 on_complete(CANCELED) 在本线程同步到达
    CHECK_EQ(hc.calls, 1);
    CHECK(!dc.destroyed_in_pump.load());           // 没在 X 自己的回调栈上拆它
    CHECK_EQ(sched.deferred_release_count_for_test(), 1);
    sched.request_schedule();                      // 回调栈已退出 ⇒ 正常 reap
    CHECK_EQ(sched.deferred_release_count_for_test(), 0);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(canceled_stall_victim_does_not_count_twice) {
    // 挂死已在 health_check 里 ++consecutive_errors_；被杀的任务随后
    // on_task_finished(SYP_ERR_CANCELED) 走 canceled 分支，只重调度不计错。
    // 同步桩上那次 on_complete 在 cancel() 里当场到达，所以 12000 这一次
    // 回调返回时两件事都已发生：计数必须是 1，不是 2。
    // 到点序列同 stall_in_connecting…：1000, 2000, …, 12000。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg_health(&tk, &rl), h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    for (int64_t t = 1000; t <= 12000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 1);
    CHECK(!h.stub.pump_request(1));               // 被杀请求的 on_complete 已到
    CHECK_EQ(sched.consecutive_errors(), 1);
    CHECK_EQ(h.err_n, 0);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(health_victim_failing_before_its_cancel_does_not_count_twice) {
    // 杀的决定在 mu_ 内、cancel 在锁外。两者之间受害者若先以
    // 别的失败码结束（竞态），那次失败与"被杀"是同一件事，不能再计一次。
    // 构造（确定性）：
    //   t=0：请求 1（探测 [0, 2048)）推两步到 on_response(206) ⇒ Supported ⇒
    //        当场建请求 2 [2048, 4096)。两条都没收到字节 ⇒ 都还是 Connecting
    //        （首字节才转 Receiving），ref = 0，阈值 10000 + 2000。请求 2 从不推，
    //        脚本是 timeout。
    //   到点 1000, …, 12000：12000 时两条都满阈值 ⇒ 同一轮杀掉 [1, 2]，
    //        挂死计错 2 次 ⇒ consecutive_errors = 2（上限 5，不 fatal）。
    //   锁外先 cancel 请求 1：被 DeferCancelBackend 扣下，on_hold 里把请求 2
    //        推一步 ⇒ 桩回 on_complete(TIMEOUT)，DLTask 2 还没被 cancel
    //        （user_canceled_ 为假）、max_retries = 0 ⇒ 以 SYP_ERR_TIMEOUT 结束
    //        ⇒ on_task_finished(TIMEOUT)。修复后按 CANCELED 处理 ⇒ 仍是 2；
    //        去掉 killed_by_health ⇒ 3。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    auto to = script_ok(N);
    to.timeout = true;
    h.stub.set_script_for_request(2, to);
    DeferCancelBackend dc(h.stub.backend());
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    cfg.max_concurrent_tasks = 2;
    Scheduler sched(dc.backend(), h.clk.clock(), cfg, h.cbs());
    DeferCancelBackend::ReleaseGuard release_guard(dc);
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    h.stub.pump_request(1);
    h.stub.pump_request(1);
    REQUIRE(h.stub.request_count() == 2);
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 2048}));
    CHECK_EQ(req_range(h.stub.request_at(1), N), (Range{2048, 4096}));
    for (int64_t t = 1000; t <= 11000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);
    int holds = 0;
    bool pumped = false;
    dc.on_hold = [&] {
        if (++holds != 1) return;
        pumped = h.stub.pump_request(2);           // 请求 2 在自己被 cancel 前失败
    };
    tick_to(h, tk, 12000);
    dc.on_hold = nullptr;
    CHECK_EQ(holds, 1);                            // 只有请求 1 的 cancel 到了后端
    CHECK(pumped);                                 // 那一步就是 on_complete(TIMEOUT)
    CHECK_EQ(sched.stall_kills(), 2);
    CHECK_EQ(sched.consecutive_errors(), 2);       // 不是 3
    CHECK_EQ(h.err_n, 0);
    dc.release_all();                              // 请求 1 迟到的 on_complete(CANCELED)
    CHECK_EQ(sched.consecutive_errors(), 2);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(ticker_not_armed_when_idle_and_rearmed_while_live) {
    // 到点序列（check_interval = 1000）：未 start 不 arm（-1）；start@0 ⇒ 1000；
    // 1000 回调时仍在途 ⇒ 2000；下完之后 2000 回调时无在途 ⇒ 不再 arm（-1）；
    // 此后一轮空闲的 schedule() 也不 arm。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg_health(&tk, &rl), h.cbs());
    h.set_scheduler(&sched);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(tk.next_due_ms_for_test(), 1000);
    tick_to(h, tk, 1000);
    CHECK_EQ(tk.next_due_ms_for_test(), 2000);
    pump_all(h.stub);
    CHECK_EQ(h.idle_n, 1);
    CHECK_EQ(sched.active_task_count(), 0);
    CHECK_EQ(tk.next_due_ms_for_test(), 2000);    // 已 arm 的那次还没到点
    tick_to(h, tk, 2000);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    sched.request_schedule();                     // 空闲时的调度不 arm
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(health_disabled_config_never_arms) {
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    cfg.health.enabled = false;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    for (int64_t t = 1000; t <= 60000; t += 1000) tick_to(h, tk, t);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(h.stub.request_count(), 1);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(rate_limited_wait_is_not_a_bad_task) {
    // R = 1024 B/s（满桶 1024 起算），分片 = max(1024, 256) = 1024，并发 1。
    // 每轮（t = 1000, 2000, …, 30000）：
    //   ① tick_to(t)：上一轮 arm 的到点 = t（无在途 ⇒ 不再 arm）；
    //   ② rl.pump_for_test()：限速唤醒 → 余额回满 1024 → 准入一片，新任务
    //      在途 ⇒ schedule 尾检 arm，到点 t + 1000；
    //   ③ pump_all：当场下完，余额 0，下一片被拒，无在途任务。
    // 于是被限速卡住的那 ~1000ms 里根本没有 DLTask，不可能被判坏。
    constexpr int64_t N = 64 * 1024;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    rl.set_rate(1024);
    SchedulerConfig cfg = cfg_health(&tk, &rl);
    cfg.max_concurrent_tasks = 1;
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg, h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(req_range(h.stub.request_at(0), N), (Range{0, 1024}));
    pump_all(h.stub);
    CHECK_EQ(h.delivered.load(), 1024);
    CHECK_EQ(sched.active_task_count(), 0);       // 被拒、无在途
    for (int64_t t = 1000; t <= 30000; t += 1000) {
        tick_to(h, tk, t);
        rl.pump_for_test();
        pump_all(h.stub);
    }
    CHECK_EQ(h.delivered.load(), 31 * 1024);
    CHECK_EQ(h.stub.request_count(), 31);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(sched.slow_kills(), 0);
    CHECK_EQ(sched.consecutive_errors(), 0);
    CHECK_EQ(tk.next_due_ms_for_test(), 31000);   // 30000 那一片在途时 arm 的
    // 限速拒绝后**不 pump 限速器**、拨到 60000：31000 那次回调时无在途 ⇒
    // 不 arm；这 30 秒的等待不计入任何判定。
    tick_to(h, tk, 60000);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(sched.consecutive_errors(), 0);
    // 等了 30 秒之后才发出的那一片，计时从它自己的尝试起点（60000）算，
    // 不继承等待时长：61000 检查时差 1000，远低于阈值。
    rl.pump_for_test();
    CHECK_EQ(h.stub.request_count(), 32);
    CHECK_EQ(tk.next_due_ms_for_test(), 61000);
    tick_to(h, tk, 61000);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(tk.next_due_ms_for_test(), 62000);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, 32 * 1024}));
    CHECK_EQ(h.err_n, 0);
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

TEST_CASE(pause_stops_health_checks) {
    // start@0 arm ⇒ 1000；pause 取消全部任务；1000 那次回调见 paused_ 早退、
    // 不 re-arm（-1）。resume 重新调度 ⇒ 新任务在途 ⇒ 20000 + 1000 = 21000。
    constexpr int64_t N = 4096;
    Harness h;
    h.reset_hits(N);
    h.stub.set_default_script(script_ok(N));
    HealthTicker tk(h.clk.clock(), false);
    RateLimiter rl(h.clk.clock(), false);
    Scheduler sched(h.stub.backend(), h.clk.clock(), cfg_health(&tk, &rl), h.cbs());
    h.set_scheduler(&sched);
    sched.start("http://x/v", nullptr, N, HoleSet{});
    CHECK_EQ(tk.next_due_ms_for_test(), 1000);
    sched.pause();
    tick_to(h, tk, 20000);
    CHECK_EQ(sched.stall_kills(), 0);
    CHECK_EQ(sched.consecutive_errors(), 0);
    CHECK_EQ(tk.next_due_ms_for_test(), -1);
    sched.resume();
    CHECK_EQ(h.stub.request_count(), 2);
    CHECK_EQ(tk.next_due_ms_for_test(), 21000);
    pump_all(h.stub);
    CHECK(h.hits_exactly_once(Range{0, N}));
    check_stub(h.stub);
    h.set_scheduler(nullptr);
}

// ── 慢任务替换 ──────────────────────────────────────────
//
// 共同场景（SlowRig）：
//   N = 3 × 3072000 = 9216000，max_concurrent_tasks = 3，min_segment_size = 1，
//   chunk_size = 1024，speed_window_ms = 3000，connect_estimate_max_ms = 2000，
//   check_interval_ms = 1000。
//   【为什么不是 3 × 300 KiB】100 KiB/s 的快任务 3 秒就下完自己那
//   300 KiB，从 t = 3100 起场上只剩慢任务，"慢任务夹在快兄弟之间"不再成立，
//   基线也不再有快样本。每片放大到 3072000（快任务要 30 秒）后，本节所有
//   用例的时间范围内三条都在途（最长的 60 秒用例后 55 秒全慢，远下不完）。
//   t = 0：start ⇒ 请求 1 是探测（Range Unknown，并发锁 1，分片仍按 3 切）
//   = [0, 3072000)；推它两步到 on_response(206) ⇒ Supported ⇒ 当场建请求 2
//   [3072000, 6144000)、请求 3 [6144000, 9216000)，各推两步到 on_response。
//   三条的 task_start / attempt_started 都是 0。
//
// 驱动（SlowRig::drive）：每 100ms 一步，先按 rate(k, t) 推进请求 k（每推一
//   步 = 1024 字节，DLTask 在该时刻记一个速度样本），再派发 ticker；本步新建
//   的请求（判慢补发）当场推两步到 on_response，与初始三条同样"已建连、等 body"。
//     快 = 每步 10 次 = 10240 B / 100ms；慢 = 逢 t % 500 == 0 推 1 次 = 1024 B / 500ms。
//
// ticker 到点序列：start@0 arm ⇒ 1000；此后每次回调都有在途 ⇒ 每 1000ms 一次
//   （杀了人则由随后那轮 schedule() 尾检 arm，同一时刻 + 1000，结果相同）。
//   检查排在该步推进之后，所以时刻 T 的样本算进 T 这次检查。
//
// speed_bps()（dl_task.h）：left = max(T − 3000, task_start)，span = T − left，
//   sum = 时刻 ≥ left 的样本字节，speed = sum × 1000 / span（整数）。于是：
//     快，T = 3000：样本 100..3000 共 30 个 = 307200 / 3000            = 102400
//     快，T ≥ 4000（窗口内全快）：T−3000..T 共 31 个 = 317440 / 3000   = 105813
//     慢，T = 3000：样本 500..3000 共 6 个 = 6144 / 3000               = 2048
//     慢，T ≥ 4000（窗口内全慢）：T−3000..T 每 500 一个共 7 个 = 7168 / 3000 = 2389
//     补发的慢任务（起点 K）在 K + 3000：样本 K+500..K+3000 共 6 个    = 2048
//   只有尝试满一个窗口（now − attempt_started ≥ 3000）才判慢 / 进基线 ⇒ 1000、
//   2000 两次检查什么都不做，基线从 3000 起算。
//
// 判慢：speed × slow_ratio < baseline（整数，带溢出守卫，见 scheduler.cpp），
//   且按当前速度剩余时间 1000 × remaining / speed > 2000。
// 基线：EWMA，b = (b == 0) ? s : b + (s − b) / 8（整数除法向零截断），按
//   slots_ 顺序（= 请求序号）逐个喂；吃过 strike 的不喂。冷却期按**本轮开始时**
//   判定，本轮替换设的冷却从下一轮起生效（见 scheduler.cpp 注释）。
namespace {

struct SlowRig {
    static constexpr int64_t kSeg = 3072000;
    static constexpr int64_t kN   = 3 * kSeg;

    Harness h;
    std::unique_ptr<DeferCancelBackend> dc;   // 非空 ⇒ 扣下 cancel（见用例 1）
    HealthTicker tk{h.clk.clock(), false};
    RateLimiter  rl{h.clk.clock(), false};
    SchedulerConfig cfg;
    std::unique_ptr<Scheduler> sched;

    explicit SlowRig(bool defer_cancel = false, StubBackend::Script sc = slow_script()) {
        h.reset_hits(kN);
        h.stub.set_default_script(sc);
        if (defer_cancel) dc = std::make_unique<DeferCancelBackend>(h.stub.backend());
        cfg = cfg_health(&tk, &rl);
        cfg.max_concurrent_tasks = 3;
        cfg.min_segment_size = 1;
        cfg.connect_estimate_max_ms = 2000;
        cfg.task.speed_window_ms = 3000;
        cfg.health.check_interval_ms = 1000;
    }
    ~SlowRig() {
        h.set_scheduler(nullptr);
        if (dc) dc->release_all();          // 先放行，~Scheduler 的 cancel_all 才能直达
        sched.reset();
    }
    SlowRig(const SlowRig&)            = delete;
    SlowRig& operator=(const SlowRig&) = delete;

    static StubBackend::Script slow_script() {
        auto sc = script_ok(kN);
        sc.chunk_size = 1024;
        return sc;
    }

    // 建调度器、start，并把三条请求都推到 on_response（见本节头注释）。
    void start(const HoleSet& cached = HoleSet{}) {
        const syp_http_backend* be = dc ? dc->backend() : h.stub.backend();
        sched = std::make_unique<Scheduler>(be, h.clk.clock(), cfg, h.cbs());
        h.set_scheduler(sched.get());
        sched->start("http://x/v", nullptr, kN, cached);
        h.stub.pump_request(1);
        h.stub.pump_request(1);
        for (int k = 2; k <= h.stub.request_count(); ++k) {
            h.stub.pump_request(k);
            h.stub.pump_request(k);
        }
    }

    template <class Rate>
    void drive(int64_t until_ms, Rate rate) {
        for (int64_t t = h.clk.t + 100; t <= until_ms; t += 100) {
            h.clk.t = t;
            const int n = h.stub.request_count();
            for (int k = 1; k <= n; ++k) {
                const int steps = rate(k, t);
                for (int i = 0; i < steps; ++i) h.stub.pump_request(k);
            }
            tk.pump_for_test();
            for (int k = n + 1; k <= h.stub.request_count(); ++k) {
                h.stub.pump_request(k);
                h.stub.pump_request(k);
            }
        }
    }

    Range req(int k1) const { return req_range(h.stub.request_at(k1 - 1), kN); }
};

int fast(int64_t) { return 10; }
int slow(int64_t t) { return t % 500 == 0 ? 1 : 0; }

}  // namespace

TEST_CASE(slow_task_among_fast_siblings_is_replaced) {
    // 请求 1 慢、2 / 3 快。
    //   T=3000：基线 0 ⇒ 不判；喂基线（按请求序号）：请求 1 的 2048 ⇒ 2048；
    //           请求 2：2048 + (102400−2048)/8 = 14592；请求 3：14592 + 87808/8 = 25568。
    //   T=4000：请求 1：2389 × 4 = 9556 < 25568，剩余 ≈3 MB 按 2389 B/s 远超
    //           2000ms ⇒ strike 1，不喂基线；请求 2 / 3 ⇒ 35598 ⇒ 44374。
    //   T=5000：请求 1：9556 < 44374 ⇒ strike 2 ⇒ 判慢，slow_kills = 1。
    //           next_offset = 慢样本 500..5000 共 10 个 = 10240。
    //           补发：remaining = 9216000 − 10240 − 2 × 512000 = 8181760，
    //           seg = ceil(8181760 / 3) = 2727254，在途 2 条 ⇒ 只补一条：
    //           请求 4 = [10240, 10240 + 2727254) = [10240, 2737494)。
    //   T=6000：请求 4 起点 5000，不满窗口，不判。
    // 后端扣下 cancel：请求 4 必须由 health_check 自己那轮 schedule() 发出，
    // 不能靠被杀任务的 on_complete(CANCELED) 走 canceled 分支补发（同步桩会
    // 当场 complete，掩盖"判慢后不 schedule()"的错误）。
    SlowRig rig(true);
    DeferCancelBackend::ReleaseGuard release_guard(*rig.dc);
    rig.start();
    CHECK_EQ(rig.h.stub.request_count(), 3);
    CHECK_EQ(rig.req(1), (Range{0, 3072000}));
    CHECK_EQ(rig.req(2), (Range{3072000, 6144000}));
    CHECK_EQ(rig.req(3), (Range{6144000, 9216000}));
    // 请求 1 在 5000 被杀之后不再推进（它的 cancel 被扣着）。
    auto rate = [](int k, int64_t t) { return k == 1 ? (t <= 5000 ? slow(t) : 0) : fast(t); };
    rig.drive(4000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 0);
    CHECK_EQ(rig.h.stub.request_count(), 3);
    rig.drive(5000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);   // 慢替换不计错误
    CHECK_EQ(rig.dc->pending.size(), static_cast<size_t>(1));   // on_complete 还没回
    CHECK_EQ(rig.h.stub.request_count(), 4);
    CHECK_EQ(rig.req(4), (Range{10240, 2737494}));  // 从 next_offset 续，不重下
    rig.dc->release_all();                          // 迟到的 on_complete(CANCELED)
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    rig.drive(6000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    CHECK(rig.h.synthetic_ok(Range{0, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(slow_needs_consecutive_strikes) {
    // 同用例 1，但请求 1 在 (4000, 5000] 突然变快（每步 10 次），之后又回到慢。
    //   T=4000：strike 1（同用例 1，基线 44374）。
    //   T=5000：窗口 [2000, 5000]：慢样本 2000..4000 共 5 个 = 5120，快样本
    //           4100..5000 共 10 × 10240 = 102400 ⇒ 107520 / 3000 = 35840；
    //           35840 × 4 = 143360 ≥ 基线 ⇒ 不慢 ⇒ strike **清零**（并喂基线）。
    //   T=6000、7000：窗口仍含那 102400 ⇒ 35840，不慢。基线 57957 → 67056 → 73151。
    //   T=8000：窗口 [5000, 8000]：5000 的快样本 10240 + 慢 5500..8000 共 6 个
    //           = 16384 ⇒ 5461；5461 × 4 = 21844 < 73151 ⇒ strike 1（不是 2）。
    //   T=9000：2389 ⇒ strike 2 ⇒ 这才判慢。
    // 反证：阈值写死 1 ⇒ 4000 就杀；非慢时不清零 ⇒ 8000 就杀（1 + 1 = 2）。
    SlowRig rig;
    rig.start();
    auto rate = [](int k, int64_t t) {
        if (k != 1) return fast(t);
        return (t > 4000 && t <= 5000) ? 10 : slow(t);
    };
    rig.drive(4000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 0);
    rig.drive(8000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 0);
    CHECK_EQ(rig.h.stub.request_count(), 3);
    rig.drive(9000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(uniformly_slow_tasks_are_not_replaced) {
    // 三条都 2 KiB/s。T=3000 基线由三条 2048 喂成 2048；此后每次喂 2389，基线
    // 收敛到 2389 附近（20000 时 2382），speed × 4 = 9556 永远 ≥ 基线 ⇒ 不判慢。
    // 慢网络上不杀光所有连接（相对基线，不设绝对阈值）。
    SlowRig rig;
    rig.start();
    rig.drive(20000, [](int, int64_t t) { return slow(t); });
    CHECK_EQ(rig.sched->slow_kills(), 0);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.h.stub.request_count(), 3);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 2382);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    check_stub(rig.h.stub);
}

TEST_CASE(baseline_halves_after_replacement_and_replacements_stop) {
    // 三条先 100 KiB/s 跑到 5000，之后（含补发的）全部 2 KiB/s（骤降约 44×）。
    //   T=3000/4000/5000：三条同速，基线 102400 → 103525 → 104280。
    //   T=6000：窗口 [3000,6000] = 快 3000..5000 共 21 × 10240 + 慢 5500、6000
    //           = 217088 ⇒ 72362，不慢，基线 → 93746。
    //   T=7000：快 4000..5000 共 11 × 10240 + 慢 4 个 = 116736 ⇒ 38912，不慢 → 75648。
    //   T=8000：快 5000 一个 + 慢 6 个 = 16384 ⇒ 5461，× 4 < 75648 ⇒ 三条 strike 1。
    //   T=9000：2389 ⇒ 三条 strike 2；请求 1 先轮到 ⇒ 判慢 #1，基线 /2 = 37824，
    //           冷却到 12000；请求 2、3 本轮不再替换（每轮至多一个），strike 留着、
    //           不喂基线。补发请求 4 = [520192, 3072000)（520192 = 512000 + 8 × 1024）。
    //   T=10000/11000（冷却）：不判、strike 清零，两条 2389 喂基线 ⇒ 29520 ⇒ 23162。
    //   T=12000：请求 2、3：9556 < 23162 ⇒ strike 1；请求 4（起点 9000）2048 ⇒ strike 1。
    //   T=13000：请求 2 strike 2 ⇒ 判慢 #2，基线 /2 = 11581，冷却到 16000。
    //   T=14000/15000（冷却）：喂 2389 ⇒ 9427 ⇒ 7779。
    //   T=16000：基线 7779 ≤ 9556 = 2389 × 4，补发的请求 5（起点 13000）
    //           2048 × 4 = 8192 也 ≥ 7779 ⇒ 谁都不慢（基线不做每轮快照、按 slots_
    //           顺序边判边喂，本轮喂入只会把它往 2389 拉低，不改结论）；基线继续向 2389 收敛
    //           （5959 → 4782 → …），此后再无替换。
    // 共 2 次。反证：不减半 ⇒ 5 次（9000/13000/17000/21000/25000）；不设冷却 ⇒
    // 3 次（9000/10000/11000）；去掉"每轮至多一个" ⇒ 9000 一轮就杀 3 个。
    // 估计 ⌈log₂(旧速/新速 ÷ slow_ratio)⌉ + 1 = ⌈log₂(104280/2389/4)⌉ + 1
    // = ⌈3.45⌉ + 1 = 5 是只靠减半、不靠 EWMA 时的次数（恰是"不减半"变异下的 5 的
    // 反面）；冷却期里未吃 strike 的样本照样进 EWMA，基线跟得更快，实际 2 次。
    // 原估计的"≥ 1 且 ≤ 7"按实际推导收紧为 == 2。
    SlowRig rig;
    rig.start();
    auto rate = [](int, int64_t t) { return t <= 5000 ? fast(t) : slow(t); };
    rig.drive(8000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 0);
    rig.drive(9000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 37824);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    CHECK_EQ(rig.req(4), (Range{520192, 3072000}));
    rig.drive(12000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    rig.drive(13000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 2);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 11581);
    rig.drive(40000, rate);
    const int32_t at40 = rig.sched->slow_kills();
    CHECK_EQ(at40, 2);
    rig.drive(60000, rate);
    CHECK_EQ(rig.sched->slow_kills(), at40);        // 最后 20 秒不再增长
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(slow_replacement_respects_cooldown) {
    // 请求 1、2 慢，请求 3 快（补发的一律慢）。
    //   T=3000：喂基线 2048 → 2048 → 14592。
    //   T=4000：请求 1、2 strike 1（9556 < 14592）；请求 3 喂 ⇒ 25994。
    //   T=5000：请求 1 strike 2 ⇒ 判慢 #1，基线 12997，冷却到 8000；请求 2 也
    //           strike 2，但本轮已替换过 ⇒ 不替换。
    //   T=6000、7000（冷却）：请求 2 不判、strike 清零 ⇒ 不杀。
    //   （7000 与 8000 之间没有检查，所以 7900 = 替换时刻 + 2900 的读数就是
    //    "冷却窗口内"的读数，等价于原估计的 +2999。）
    //   T=8000：冷却结束，请求 2：9556 < 38234 ⇒ strike 1；请求 4（起点 5000）
    //           2048 ⇒ strike 1。38234 是**轮到请求 2 时**的基线：基线不做每轮快照，
    //           health_check 按 slots_ 顺序逐个判、边判边喂；请求 2 排在请求 3 前面，
    //           读到的恰好是上一轮末的值（只有冷却标志是每轮开头取一次）。
    //   T=9000：请求 2 strike 2 ⇒ 判慢 #2。
    // 反证：不设冷却 ⇒ 6000 请求 2（strike 已是 2）当场被杀。
    SlowRig rig;
    rig.start();
    auto rate = [](int k, int64_t t) { return k == 3 ? fast(t) : slow(t); };
    rig.drive(5000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 24599);
    rig.drive(7900, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    rig.drive(8000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    rig.drive(9000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 2);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(nearly_done_slow_task_is_not_replaced) {
    // 启动时已缓存 [12288, 3072000) ⇒ 请求 1（探测）= 第一个洞 [0, 12288)；
    // remaining = 12288 + 6144000 = 6156288，seg = 2052096 ⇒ 请求 2 =
    // [3072000, 5124096)、请求 3 = [5124096, 7176192)（余下的洞等名额）。
    // 请求 1 慢、2 / 3 快。
    //   T=3000：基线 25568（同用例 1）。
    //   T=4000：请求 1：9556 < 25568，但剩余 12288 − 8192 = 4096，按 2389 B/s
    //           要 1000 × 4096 / 2389 = 1714ms ≤ 2000 ⇒ 不值得换 ⇒ 不记 strike。
    //   T=5000：剩余 2048 ⇒ 857ms，不换。
    //   t=6000 那一步收完最后一块：DLTask 区间已满、当场以 OK 结束（不等桩的
    //   on_complete）⇒ 同一步里 schedule() 补上余下的洞。请求 1 在该步里最先被推，
    //   此刻请求 2、3 只到 5900（59 × 10240 = 604160）：remaining = 9216000 −
    //   3059712（已缓存）− 12288 − 2 × 604160 = 4935680，seg = ceil(/3) = 1645227
    //   ⇒ 请求 4 = [7176192, 8821419)。6000 的检查里请求 1 已是终态、请求 4 不满窗口。
    // 反证：去掉"值不值得换" ⇒ 4000 strike 1、5000 判慢。
    SlowRig rig;
    HoleSet cached;
    cached.add(Range{12288, SlowRig::kSeg});
    rig.start(cached);
    CHECK_EQ(rig.h.stub.request_count(), 3);
    CHECK_EQ(rig.req(1), (Range{0, 12288}));
    CHECK_EQ(rig.req(2), (Range{3072000, 5124096}));
    CHECK_EQ(rig.req(3), (Range{5124096, 7176192}));
    rig.drive(6000, [](int k, int64_t t) { return k == 1 ? slow(t) : fast(t); });
    CHECK_EQ(rig.sched->slow_kills(), 0);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    CHECK_EQ(rig.req(4), (Range{7176192, 8821419}));
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, 12288}));
    CHECK(rig.h.hits_exactly_once(Range{SlowRig::kSeg, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(no_slow_replacement_when_range_unsupported) {
    // 服务端不认 Range（200 整包）。启动时已缓存 [0, 1) ⇒ 探测请求 1 起点 1，
    // 回 200 ⇒ DLTask 判 SYP_ERR_RANGE_UNSUPPORTED ⇒ Unsupported，降级为唯一
    // 一条请求 2 = [0, N)（t = 0，起点 0）。它先快到 5000、再慢到约 1/44：
    //   T=3000..7000：102400 / 105813 / 105813 / 72362 / 38912（推导同
    //           baseline_halves…），基线自己喂自己 → 91791。
    //   T=8000：5461 × 4 = 21844 < 基线 91791；T=9000 起 2389 × 4 = 9556 < 基线。
    //   若照常判慢，8000 strike 1、9000 就会被杀（重连要从 0 重下整份文件）。
    // Unsupported ⇒ 不判慢，跑到 25000 都不替换。
    auto sc = SlowRig::slow_script();
    sc.support_range = false;
    sc.http_status = 200;
    SlowRig rig(false, sc);
    HoleSet cached;
    cached.add(Range{0, 1});
    rig.start(cached);
    CHECK_EQ(rig.sched->range_supported(), false);
    CHECK_EQ(rig.h.stub.request_count(), 2);
    CHECK_EQ(rig.req(2), (Range{0, SlowRig::kN}));
    rig.drive(25000, [](int, int64_t t) { return t <= 5000 ? fast(t) : slow(t); });
    CHECK_EQ(rig.sched->slow_kills(), 0);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.h.stub.request_count(), 2);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{1, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(slow_replacement_does_not_count_error) {
    // 同用例 1 的场景（不扣 cancel），max_consecutive_errors = 1：若把慢替换
    // 当成一次失败计数，5000 那次替换就会 fatal、on_error，不计。
    SlowRig rig;
    rig.cfg.max_consecutive_errors = 1;
    rig.start();
    rig.drive(6000, [](int k, int64_t t) { return k == 1 ? slow(t) : fast(t); });
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    CHECK_EQ(rig.h.err_n, 0);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(baseline_excludes_struck_tasks) {
    // 同用例 1 的场景。T=3000 基线 25568；T=4000 请求 1 吃到 strike 1，它的
    // 2389 **不进**基线，只有请求 2、3 的 105813 喂：25568 → 35598 → 44374。
    // 若吃过 strike 的也进：25568 + (2389 − 25568)/8 = 22671 → 33063 → 42156。
    // T=5000（判慢那一轮）：被判慢的样本同样不进；减半 22187 后请求 2、3 喂
    // ⇒ 32640 ⇒ 41786。
    SlowRig rig;
    rig.start();
    auto rate = [](int k, int64_t t) { return k == 1 ? slow(t) : fast(t); };
    rig.drive(2000, rate);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 0);     // 未满窗口，不喂
    rig.drive(3000, rate);
    const int64_t before = rig.sched->baseline_bps_for_test();
    CHECK_EQ(before, 25568);
    rig.drive(4000, rate);
    const int64_t after = rig.sched->baseline_bps_for_test();
    CHECK_EQ(after, 44374);
    CHECK(after >= before);
    rig.drive(5000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 41786);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    check_stub(rig.h.stub);
}

TEST_CASE(one_third_speed_is_not_slow_enough) {
    // 请求 1 每步 3 次（3072 B / 100ms），2 / 3 快。请求 1：T=3000 为
    // 92160 / 3000 = 30720，T ≥ 4000 为 31 × 3072 / 3000 = 31744；基线
    // 47520 → 59672 → 67813 → 73267 → 76921 → …（向 ≈8.3 万收敛）。
    // 约为基线的 1/3 到 1/2：
    //   · slow_ratio = 4：31744 × 4 = 126976 永远 ≥ 基线 ⇒ 不判慢；
    //   · slow_ratio = 2（对照，证明场景确实落在两个比值之间）：T=4000、5000 时
    //     请求 1 最先轮到，63488 ≥ 当时基线 47520 / 59672，不慢；T=6000 请求 1 最先轮到，
    //     31744 × 2 = 63488 < 67813（5000 那轮结束时的基线）⇒ strike 1；
    //     T=7000 strike 2 ⇒ 判慢。
    for (int ratio : {4, 2}) {
        SlowRig rig;
        rig.cfg.health.slow_ratio = ratio;
        rig.start();
        auto rate = [](int k, int64_t t) { return k == 1 ? 3 : fast(t); };
        rig.drive(6000, rate);
        CHECK_EQ(rig.sched->slow_kills(), 0);
        rig.drive(7000, rate);
        CHECK_EQ(rig.sched->slow_kills(), (ratio == 2 ? 1 : 0));
        rig.drive(20000, rate);
        CHECK_EQ(rig.sched->slow_kills(), (ratio == 2 ? 1 : 0));
        pump_all(rig.h.stub);
        CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
        check_stub(rig.h.stub);
    }
}

// 零速慢替换（推翻此前"零速一律计错"的旧规则）：Receiving 中
// 整整一个窗口零字节照样走慢路径**快速替换**，但只有静默已达有效读超时
// （now − ref ≥ read_timeout，ref = max(本次尝试起点, 最近进展)，与挂死同一
// 定义、不含宽限）才按超时 ++consecutive_errors_；满 max_consecutive_errors
// ⇒ fatal + on_error(SYP_ERR_TIMEOUT, 0)。
// 前三条的场景同 SlowRig；请求 1 只在 500、1000 各收一块（next_offset 2048、
// 最近进展 1000）然后不动，2 / 3 快：
//   T=3000：请求 1 窗口 [0,3000] = 2048 / 3000 = 682，基线 0 ⇒ 播种 682；
//           2 / 3 喂 ⇒ 13396 ⇒ 24521。
//   T=4000：窗口 [1000,4000] 只含 1000 那块 ⇒ 341；341 × 4 < 24521 ⇒ strike 1
//           （正速度）；基线 ⇒ 34682 ⇒ 43573。
//   T=5000：窗口 [2000,5000] 零字节 ⇒ speed 0 < 基线 ⇒ strike 2 ⇒ 判慢（零速）。
//           静默 = 5000 − 1000 = 4000。补发请求 4：remaining = 9216000 − 2048 −
//           2 × 512000 = 8189952，seg = 2729984 ⇒ [2048, 2732032)。
TEST_CASE(zero_speed_replacement_within_read_timeout_does_not_count_error) {
    // read_timeout = 15000（cfg_health）：T=5000 静默 4000 < 15000 ⇒ 替换但不计错。
    // max_consecutive_errors = 1：若计错，这一次就 fatal、on_error。
    // 挂死判定也管不到：Receiving 阈值 15000 + 2000，从最近进展 1000 起算要 18000。
    SlowRig rig;
    rig.cfg.max_consecutive_errors = 1;
    rig.start();
    auto rate = [](int k, int64_t t) {
        if (k == 1) return (t == 500 || t == 1000) ? 1 : 0;
        return fast(t);
    };
    rig.drive(4000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 0);
    rig.drive(5000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    CHECK_EQ(rig.h.err_n, 0);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    CHECK_EQ(rig.req(4), (Range{2048, 2732032}));
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(zero_speed_replacement_past_read_timeout_counts_error) {
    // read_timeout = 4000：T=5000 静默 4000 ≥ 4000（恰在边界）⇒ 替换并计错 1。
    // 挂死阈值 4000 + 2000 = 6000：T=4000 静默 3000、T=5000 静默 4000 都不到，
    // 所以这一次确实走的是零速慢替换那条路（stall_kills = 0）。
    // 上限默认 5 ⇒ 不 fatal。
    SlowRig rig;
    rig.cfg.task.read_timeout_ms = 4000;
    rig.start();
    auto rate = [](int k, int64_t t) {
        if (k == 1) return (t == 500 || t == 1000) ? 1 : 0;
        return fast(t);
    };
    rig.drive(4000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    rig.drive(5000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 1);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    CHECK_EQ(rig.req(4), (Range{2048, 2732032}));
    CHECK_EQ(rig.h.err_n, 0);
    pump_all(rig.h.stub);
    CHECK(rig.h.hits_exactly_once(Range{0, SlowRig::kN}));
    CHECK_EQ(rig.h.err_n, 0);
    check_stub(rig.h.stub);
}

TEST_CASE(repeated_zero_speed_goes_fatal_with_timeout) {
    // 同上的请求 1，read_timeout = 3500（挂死阈值 5500），max_consecutive_errors = 2；
    // 补发的请求 4（起点 5000，已建连）只在 5500 收一块。
    // （一个字节都不收的任务 speed_bps() 是 nullopt、不参与判慢，只能等挂死，
    //  所以这里必须给它一块。）
    //   T=4000：请求 1 静默 3000 < 5500，不挂死；strike 1。
    //   T=5000：请求 1 零速判慢、静默 4000 ≥ 3500 ⇒ 计错 1（挂死差 4000 < 5500）；
    //           基线 /2 = 21786，冷却到 8000。
    //   T=6000、7000：冷却；请求 4 不满窗口。期间无任务成功结束，计数不被清零。
    //   T=8000：冷却结束；请求 4 窗口 [5000,8000] 只含 5500 那块 ⇒ 341 ⇒ strike 1。
    //   T=9000：窗口 [6000,9000] 零字节 ⇒ strike 2 ⇒ 判慢，零速，静默 = 9000 −
    //           5500 = 3500 ≥ 3500（恰在边界）⇒ 计错 2 ⇒ fatal：
    //           on_error(SYP_ERR_TIMEOUT, 0)，cancel_all，不再补发、不再 arm。
    //           挂死差 3500 < 5500，不是挂死杀的（stall_kills = 0）。
    SlowRig rig;
    rig.cfg.max_consecutive_errors = 2;
    rig.cfg.task.read_timeout_ms = 3500;
    rig.start();
    auto rate = [](int k, int64_t t) {
        if (k == 1) return (t == 500 || t == 1000) ? 1 : 0;
        if (k == 4) return t == 5500 ? 1 : 0;
        return fast(t);
    };
    rig.drive(5000, rate);
    CHECK_EQ(rig.sched->consecutive_errors(), 1);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    rig.drive(8000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->consecutive_errors(), 1);
    CHECK_EQ(rig.h.err_n, 0);
    rig.drive(9000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 2);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 2);
    CHECK_EQ(rig.h.err_n, 1);
    CHECK_EQ(rig.h.last_err, SYP_ERR_TIMEOUT);
    CHECK_EQ(rig.h.last_http, 0);
    CHECK_EQ(rig.h.stub.request_count(), 4);  // fatal 后不补发
    CHECK_EQ(rig.sched->active_task_count(), 0);
    CHECK_EQ(rig.tk.next_due_ms_for_test(), -1);
    rig.drive(12000, rate);
    CHECK_EQ(rig.h.stub.request_count(), 4);
    CHECK_EQ(rig.h.err_n, 1);
    check_stub(rig.h.stub);
}

TEST_CASE(all_tasks_zero_speed_is_not_fatal_before_read_timeout) {
    // 生产默认形状的回归场景（并发 3、窗口 3000、每秒一检、
    // max_consecutive_errors = 3 = SourceBridge 的 max_retries 默认、
    // read_timeout 15000），三条连接在 T0 = 5000 同时"连着但不走数据"。
    // 旧规则（零速一律计错）：9000 / 13000 / 17000 各换一条、各计一次 ⇒
    // T0 + 12000 = 17000 fatal。新规则下 T0 + 13000 = 18000 仍 err_n == 0。
    //
    // 速度（推导同 baseline_halves…）：T=3000/4000/5000 三条同速，基线
    //   102400 → 103525 → 104280。
    //   T=6000：窗口 [3000,6000] = 快 3000..5000 共 21 × 10240 = 215040 ⇒ 71680，
    //           × 4 ≥ 基线 ⇒ 不慢；喂三次 ⇒ 100205 ⇒ 96640 ⇒ 93520。
    //   T=7000：快 4000..5000 共 11 × 10240 = 112640 ⇒ 37546，不慢；
    //           喂 ⇒ 86524 ⇒ 80402 ⇒ 75045。
    //   T=8000：只剩 5000 那一块 10240 ⇒ 3413，× 4 = 13652 < 75045 ⇒ 三条 strike 1。
    //   T=9000：零字节 ⇒ speed 0 ⇒ strike 2；请求 1 先轮到 ⇒ 判慢 #1，静默
    //           9000 − 5000 = 4000 < 15000 ⇒ **不计错**；基线 /2 = 37522，冷却到
    //           12000；补发请求 4（起点 9000）。
    //   T=10000/11000（冷却）：strike 清零；零速不喂基线。
    //   T=12000：请求 2、3 零速 strike 1；请求 4 已建连但一个字节没收到，DLTask
    //           仍是 Connecting（首字节才转 Receiving），不参与判慢。
    //   T=13000：请求 2 先轮到 ⇒ 判慢 #2，静默 8000 < 15000 ⇒ 不计错；基线 18761，
    //           冷却到 16000；补发请求 5（起点 13000）。
    //   T=16000：请求 3 strike 1（4、5 仍是 Connecting，不判）。
    //   T=17000：请求 3 判慢 #3，静默 12000 < 15000 ⇒ 不计错（旧规则此刻 fatal）；
    //           挂死差 12000 < 17000。基线 9380；补发请求 6（起点 17000）。
    //   T=18000（冷却）：err_n == 0，consecutive_errors == 0。
    // 之后的有界性（"替换任务由 Connecting 挂死兜底"）：补发的请求
    // 4 / 5 / 6 一个字节都没收过，一直是 Connecting，只能等 Connecting 挂死
    // （起点 + connect 10000 + 宽限 2000），每次计错：
    //   21000（请求 4）⇒ 1，补发请求 7；25000（请求 5）⇒ 2，补发请求 8；
    //   29000（请求 6）⇒ 3 ⇒ fatal(SYP_ERR_TIMEOUT)，即 T0 + 24000。
    SlowRig rig;
    rig.cfg.max_consecutive_errors = 3;
    rig.start();
    auto rate = [](int, int64_t t) { return t <= 5000 ? fast(t) : 0; };
    rig.drive(8000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 0);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 75045);
    rig.drive(9000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 1);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    CHECK_EQ(rig.sched->baseline_bps_for_test(), 37522);
    rig.drive(13000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 2);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    rig.drive(17000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 3);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    rig.drive(18000, rate);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    CHECK_EQ(rig.h.err_n, 0);
    // 有界：此后只剩挂死。
    rig.drive(20000, rate);
    CHECK_EQ(rig.sched->slow_kills(), 3);
    CHECK_EQ(rig.sched->stall_kills(), 0);
    CHECK_EQ(rig.sched->consecutive_errors(), 0);
    rig.drive(21000, rate);
    CHECK_EQ(rig.sched->stall_kills(), 1);
    CHECK_EQ(rig.sched->consecutive_errors(), 1);
    rig.drive(25000, rate);
    CHECK_EQ(rig.sched->stall_kills(), 2);
    CHECK_EQ(rig.sched->consecutive_errors(), 2);
    rig.drive(28000, rate);
    CHECK_EQ(rig.h.err_n, 0);
    rig.drive(29000, rate);
    CHECK_EQ(rig.sched->stall_kills(), 3);
    CHECK_EQ(rig.sched->slow_kills(), 3);
    CHECK_EQ(rig.h.err_n, 1);
    CHECK_EQ(rig.h.last_err, SYP_ERR_TIMEOUT);
    CHECK_EQ(rig.sched->active_task_count(), 0);
    check_stub(rig.h.stub);
}

int main() { return tiny_test_main(); }
