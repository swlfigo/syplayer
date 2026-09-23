// test_dl_task.cpp — DLTask 状态机 / 续传 / 重定向 / 取消，全部走同步泵（异步仅 TSan）
#include "tiny_test.h"
#include "support/stub_backend.h"
#include "support/watchdog.h"

#include <dl/clock.h>
#include <dl/dl_task.h>
#include <dl/hole_set.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

using syp::dl::Clock;
using syp::dl::DLTask;
using syp::dl::DLTaskCallbacks;
using syp::dl::DLTaskConfig;
using syp::dl::DLTaskState;
using syp::dl::Range;
using syp::dl::detail::parse_content_range;
using syp::dl::test::StubBackend;
using syp::dl::test::synthetic_byte;

namespace syp::dl {

std::ostream& operator<<(std::ostream& os, DLTaskState s) {
    switch (s) {
    case DLTaskState::Idle:       return os << "Idle";
    case DLTaskState::Connecting: return os << "Connecting";
    case DLTaskState::Receiving:  return os << "Receiving";
    case DLTaskState::Done:       return os << "Done";
    case DLTaskState::Failed:     return os << "Failed";
    case DLTaskState::Canceled:   return os << "Canceled";
    }
    return os << "DLTaskState(" << static_cast<int>(s) << ")";
}

}  // namespace syp::dl

namespace tiny_test {

inline std::string stringify(const std::optional<int64_t>& v) {
    if (!v) return "nullopt";
    return stringify(*v);
}

}  // namespace tiny_test

namespace {

struct FakeClock {
    std::atomic<int64_t> t{0};
    static int64_t now(void* ctx) {
        return static_cast<FakeClock*>(ctx)->t.load(std::memory_order_relaxed);
    }
    Clock clock() { return Clock{&now, this}; }
    void advance(int64_t ms) { t.fetch_add(ms, std::memory_order_relaxed); }
};

struct Collector {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> bytes;
    int64_t first_off   = -1;
    int64_t next_off    = -1;
    bool    contiguous  = true;
    int64_t total       = -1;
    std::string etag;
    std::string last_modified;
    std::atomic<int> finished{0};
    std::atomic<syp_status> st{1};
    std::atomic<int32_t> http{-1};
    DLTask* task        = nullptr;
    int64_t cancel_at   = -1;
    int     data_n      = 0;
    int     total_n     = 0;
    int     val_n       = 0;

    static void on_data(void* ctx, int64_t offset, const uint8_t* data, int32_t len) {
        auto* c = static_cast<Collector*>(ctx);
        DLTask* t = nullptr;
        {
            std::lock_guard<std::mutex> g(c->mu);
            if (c->first_off < 0) c->first_off = offset;
            if (c->next_off >= 0 && offset != c->next_off) c->contiguous = false;
            c->next_off = offset + static_cast<int64_t>(len);
            c->bytes.insert(c->bytes.end(), data, data + len);
            ++c->data_n;
            if (c->cancel_at >= 0
                && static_cast<int64_t>(c->bytes.size()) >= c->cancel_at) {
                t = c->task;
                c->cancel_at = -1;
            }
        }
        if (t != nullptr) t->cancel();
    }

    static void on_total(void* ctx, int64_t total) {
        auto* c = static_cast<Collector*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        c->total = total;
        ++c->total_n;
    }

    static void on_val(void* ctx, const char* etag, const char* lm) {
        auto* c = static_cast<Collector*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        c->etag = etag ? etag : "";
        c->last_modified = lm ? lm : "";
        ++c->val_n;
    }

    static void on_fin(void* ctx, syp_status st, int32_t http) {
        auto* c = static_cast<Collector*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        c->st.store(st, std::memory_order_relaxed);
        c->http.store(http, std::memory_order_relaxed);
        c->finished.fetch_add(1, std::memory_order_release);
        c->cv.notify_all();
    }

    DLTaskCallbacks cbs() {
        return DLTaskCallbacks{this, &on_data, &on_total, &on_val, &on_fin};
    }

    void wait_finished() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return finished.load(std::memory_order_acquire) > 0; });
    }

    int fin() const { return finished.load(std::memory_order_acquire); }
    syp_status status() const { return st.load(std::memory_order_acquire); }
    int32_t http_status() const { return http.load(std::memory_order_acquire); }
};

void check_stub(const StubBackend& s) {
    std::string msg;
    if (!s.contract_ok(&msg)) {
        tiny_test::fail(__FILE__, __LINE__, "stub.contract_ok()", msg);
    }
}

void check_synthetic(const std::vector<uint8_t>& b, int64_t start) {
    for (size_t i = 0; i < b.size(); ++i) {
        const uint8_t want = synthetic_byte(start + static_cast<int64_t>(i));
        if (b[i] != want) {
            tiny_test::fail(__FILE__, __LINE__, "synthetic byte mismatch",
                            "i=" + std::to_string(i));
            return;
        }
    }
}

DLTaskConfig cfg_common() {
    DLTaskConfig c;
    c.connect_timeout_ms = 10000;
    c.read_timeout_ms = 15000;
    c.max_redirects = 8;
    c.max_retries = 3;
    c.speed_window_ms = 3000;
    c.allow_no_range_fallback = true;
    return c;
}

StubBackend::Script script_ok(int64_t n) {
    StubBackend::Script s;
    s.resource_length = n;
    s.support_range = true;
    s.http_status = 206;
    s.etag = "\"abc\"";
    s.last_modified = "Wed, 01 Jan 2020 00:00:00 GMT";
    s.chunk_size = 64;
    return s;
}

}  // namespace

TEST_CASE(full_download_matches_synthetic) {
    StubBackend stub;
    stub.set_default_script(script_ok(256));
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 256});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(256));
    CHECK(c.contiguous);
    CHECK_EQ(c.first_off, int64_t{0});
    check_synthetic(c.bytes, 0);
    CHECK_EQ(task.state(), DLTaskState::Done);
    CHECK_EQ(task.received_bytes(), int64_t{256});
    CHECK_EQ(task.next_offset(), int64_t{256});
    check_stub(stub);
}

TEST_CASE(partial_range_206_parses_total_length) {
    StubBackend stub;
    auto sc = script_ok(1000);
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{200, 350});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(c.total, int64_t{1000});
    CHECK_EQ(c.total_n, 1);
    CHECK_EQ(c.etag, std::string("\"abc\""));
    CHECK_EQ(c.last_modified, std::string("Wed, 01 Jan 2020 00:00:00 GMT"));
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(150));
    CHECK_EQ(c.first_off, int64_t{200});
    CHECK(c.contiguous);
    check_synthetic(c.bytes, 200);
    CHECK_EQ(stub.request_at(0).range_start, int64_t{200});
    CHECK_EQ(stub.request_at(0).range_end, int64_t{349});
    check_stub(stub);
}

TEST_CASE(wanted_end_int64_max_downloads_to_eof) {
    StubBackend stub;
    stub.set_default_script(script_ok(80));
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{10, std::numeric_limits<int64_t>::max()});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(c.total, int64_t{80});
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(70));
    CHECK_EQ(c.first_off, int64_t{10});
    check_synthetic(c.bytes, 10);
    CHECK_EQ(stub.request_at(0).range_end, int64_t{-1});
    check_stub(stub);
}

TEST_CASE(one_byte_chunks_still_contiguous) {
    StubBackend stub;
    auto sc = script_ok(64);
    sc.chunk_size = 1;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 64});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(c.data_n, 64);
    CHECK(c.contiguous);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(64));
    check_synthetic(c.bytes, 0);
    check_stub(stub);
}

TEST_CASE(http_200_start_zero_ok) {
    StubBackend stub;
    auto sc = script_ok(100);
    sc.http_status = 200;
    sc.support_range = false;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(c.total, int64_t{100});
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(100));
    check_synthetic(c.bytes, 0);
    check_stub(stub);
}

TEST_CASE(http_200_start_nonzero_fallback_skips_prefix) {
    StubBackend stub;
    auto sc = script_ok(200);
    sc.http_status = 200;
    sc.support_range = false;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.allow_no_range_fallback = true;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{50, 90});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(40));
    CHECK_EQ(c.first_off, int64_t{50});
    CHECK(c.contiguous);
    check_synthetic(c.bytes, 50);
    check_stub(stub);
}

TEST_CASE(http_200_start_nonzero_no_fallback_is_fatal) {
    StubBackend stub;
    auto sc = script_ok(200);
    sc.http_status = 200;
    sc.support_range = false;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.allow_no_range_fallback = false;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{50, 90});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_RANGE_UNSUPPORTED);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(0));
    CHECK_EQ(stub.request_count(), 1);
    CHECK_EQ(task.state(), DLTaskState::Failed);
    check_stub(stub);
}

// `Full200 && wanted_.start > 0 &&
// !allow_no_range_fallback` 这条分支已经判定致命、已经决定丢弃响应体，
// 却不中止请求句柄——`sink_on_data` 只是把字节丢在地上，body 照样在网线上
// 跑完，白付一整份文件的流量。
//
// 上面那条 `..._is_fatal` 用例证不了这件事：它断的是 `c.bytes`（DLTask
// 交付给上层的字节），丢弃路径下本来就是 0，body 跑没跑完看不出来。
// 这里断的是 `stub.sink_on_data_bytes()`——**后端实际交给 sink 的字节**，
// 与 DLTask 丢不丢无关，正是"线上流量"的桩内等价物。
// 修复前 = 整个 body（100000），修复后 = 0。
TEST_CASE(http_200_no_fallback_aborts_transfer_at_header) {
    StubBackend stub;
    auto sc = script_ok(100000);
    sc.http_status = 200;
    sc.support_range = false;
    sc.chunk_size = 4096;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.allow_no_range_fallback = false;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{50000, 60000});
    stub.pump_all();

    // 缺陷本体：判定丢弃之后一个 body 字节都不该再从网线上过来。
    CHECK_EQ(stub.sink_on_data_bytes(), int64_t{0});

    // 终态与既有行为完全一致——这条改动只掐连接，不改判定。
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_RANGE_UNSUPPORTED);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(0));
    CHECK_EQ(task.state(), DLTaskState::Failed);
    CHECK_EQ(stub.request_count(), 1);
    // 契约自查：cancel 之后后端恰好回调一次 on_complete，句柄不泄漏。
    check_stub(stub);
}

// 同一条中止路径的异步形状：中止是在**后端自己的回调线程**、在 on_response
// 的栈帧里发起的（`backend_->cancel(handle_)`），随后那条 on_complete 也在
// 同一条 worker 线程上到达并同步 destroy 句柄。这正是此前反复咬人的那个
// 形状，所以要有一条
// 异步用例把它钉住；自带看门狗，挂死时给出失败点而不是拖到 ctest TIMEOUT。
TEST_CASE(http_200_no_fallback_abort_is_safe_from_backend_thread) {
    syp::test::Watchdog wd("http_200_no_fallback_abort_is_safe_from_backend_thread",
                           8000, 30000,
                           "on_response 回调栈里调 backend->cancel 之后挂住了");
    constexpr int kRounds = 40;
    for (int round = 0; round < kRounds; ++round) {
        StubBackend stub;
        stub.set_mode(StubBackend::Mode::Async);
        auto sc = script_ok(100000);
        sc.http_status = 200;
        sc.support_range = false;
        sc.chunk_size = 4096;
        stub.set_default_script(sc);
        FakeClock clk;
        Collector c;
        {
            DLTaskConfig cfg = cfg_common();
            cfg.allow_no_range_fallback = false;
            DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
            task.start("http://x/v", nullptr, Range{50000, 60000});
            c.wait_finished();
            CHECK_EQ(c.fin(), 1);
            CHECK_EQ(c.status(), SYP_ERR_RANGE_UNSUPPORTED);
            CHECK_EQ(c.bytes.size(), static_cast<size_t>(0));
        }
        // 中止之后 body 不该再跑完。异步下断"恰好 0"仍然成立：中止发生在
        // on_response 的栈帧里，worker 要等这条回调返回才会进 Body 阶段。
        CHECK_EQ(stub.sink_on_data_bytes(), int64_t{0});
        check_stub(stub);
    }
}

TEST_CASE(redirect_once_succeeds) {
    StubBackend stub;
    auto sc = script_ok(32);
    sc.redirect_urls = {"http://cdn/v"};
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 32});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.redirect_count(), 1);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(32));
    check_stub(stub);
}

TEST_CASE(redirect_exactly_max_succeeds) {
    StubBackend stub;
    auto sc = script_ok(16);
    sc.redirect_urls = {"http://a", "http://b", "http://c"};
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_redirects = 3;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 16});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.redirect_count(), 3);
    check_stub(stub);
}

TEST_CASE(redirect_max_plus_one_fails) {
    StubBackend stub;
    auto sc = script_ok(16);
    sc.redirect_urls = {"http://a", "http://b", "http://c"};
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_redirects = 2;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 16});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_TOO_MANY_REDIRECTS);
    CHECK_EQ(task.redirect_count(), 3);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(0));
    check_stub(stub);
}

TEST_CASE(redirect_to_same_url_still_counts) {
    StubBackend stub;
    auto sc = script_ok(8);
    sc.redirect_urls = {"http://x/v", "http://x/v"};
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_redirects = 2;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 8});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.redirect_count(), 2);
    check_stub(stub);
}

TEST_CASE(midstream_drop_retries_from_next_offset) {
    StubBackend stub;
    auto ok = script_ok(200);
    auto fail = ok;
    fail.break_after_bytes = 50;
    fail.chunk_size = 10;
    stub.set_default_script(ok);
    stub.set_script_for_request(1, fail);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 200});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(stub.request_count(), 2);
    CHECK_EQ(stub.request_at(0).range_start, int64_t{0});
    CHECK_EQ(stub.request_at(1).range_start, int64_t{50});
    CHECK_EQ(stub.request_at(1).range_end, int64_t{199});
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(200));
    CHECK(c.contiguous);
    CHECK_EQ(c.first_off, int64_t{0});
    check_synthetic(c.bytes, 0);
    CHECK_EQ(task.attempt_count(), 2);
    check_stub(stub);
}

TEST_CASE(short_ok_response_retries_and_continues) {
    StubBackend stub;
    auto ok = script_ok(100);
    auto short_ok = ok;
    short_ok.max_body_bytes = 40;
    stub.set_default_script(ok);
    stub.set_script_for_request(1, short_ok);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(stub.request_count(), 2);
    CHECK_EQ(stub.request_at(1).range_start, int64_t{40});
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(100));
    CHECK(c.contiguous);
    check_synthetic(c.bytes, 0);
    check_stub(stub);
}

// wanted.start==0 时「绝对偏移续传」和「已收字节数续传」数值相同。
// 下面两组必须用 start>0，才能抓住重试忘了加 wanted.start 的实现。
TEST_CASE(retry_from_nonzero_start_resumes_at_absolute_offset) {
    StubBackend stub;
    auto ok = script_ok(3000);
    auto fail = ok;
    fail.break_after_bytes = 120;
    fail.chunk_size = 40;
    stub.set_default_script(ok);
    stub.set_script_for_request(1, fail);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{1000, 1500});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(stub.request_count(), 2);
    CHECK_EQ(stub.request_at(0).range_start, int64_t{1000});
    CHECK_EQ(stub.request_at(0).range_end, int64_t{1499});
    CHECK_EQ(stub.request_at(1).range_start, int64_t{1120});
    CHECK_EQ(stub.request_at(1).range_end, int64_t{1499});
    CHECK_EQ(c.first_off, int64_t{1000});
    CHECK(c.contiguous);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(500));
    check_synthetic(c.bytes, 1000);
    check_stub(stub);
}

TEST_CASE(retry_twice_from_nonzero_start_keeps_advancing) {
    StubBackend stub;
    auto ok = script_ok(4000);
    auto fail1 = ok;
    fail1.break_after_bytes = 80;
    fail1.chunk_size = 40;
    auto fail2 = ok;
    fail2.break_after_bytes = 140;
    fail2.chunk_size = 70;
    stub.set_default_script(ok);
    stub.set_script_for_request(1, fail1);
    stub.set_script_for_request(2, fail2);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{2000, 2500});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(stub.request_count(), 3);
    CHECK_EQ(stub.request_at(0).range_start, int64_t{2000});
    CHECK_EQ(stub.request_at(0).range_end, int64_t{2499});
    CHECK_EQ(stub.request_at(1).range_start, int64_t{2080});
    CHECK_EQ(stub.request_at(1).range_end, int64_t{2499});
    CHECK_EQ(stub.request_at(2).range_start, int64_t{2220});
    CHECK_EQ(stub.request_at(2).range_end, int64_t{2499});
    CHECK_EQ(c.first_off, int64_t{2000});
    CHECK(c.contiguous);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(500));
    check_synthetic(c.bytes, 2000);
    CHECK_EQ(task.attempt_count(), 3);
    check_stub(stub);
}

TEST_CASE(http_200_nonzero_start_midstream_drop_retries_without_duplicate) {
    StubBackend stub;
    auto ok = script_ok(2000);
    ok.http_status = 200;
    ok.support_range = false;
    ok.chunk_size = 256;
    auto fail = ok;
    fail.break_after_bytes = 1120;  // 跳过 1000 前缀后交付 120，再断流
    stub.set_default_script(ok);
    stub.set_script_for_request(1, fail);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.allow_no_range_fallback = true;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{1000, 1500});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(stub.request_count(), 2);
    CHECK_EQ(stub.request_at(0).range_start, int64_t{1000});
    CHECK_EQ(stub.request_at(1).range_start, int64_t{1120});
    CHECK_EQ(c.first_off, int64_t{1000});
    CHECK(c.contiguous);
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(500));
    check_synthetic(c.bytes, 1000);
    check_stub(stub);
}

TEST_CASE(retries_exhausted_returns_last_error) {
    StubBackend stub;
    auto fail = script_ok(100);
    fail.break_after_bytes = 0;
    stub.set_default_script(fail);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_retries = 2;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_NETWORK);
    CHECK_EQ(stub.request_count(), 3);  // 1 + max_retries
    CHECK_EQ(task.attempt_count(), 3);
    CHECK_EQ(task.state(), DLTaskState::Failed);
    check_stub(stub);
}

// max_retries 限的是**连续无进展**的尝试，不是总尝试数。一条每次都能往前
// 推进、但总被掐断的连接（弱网断流；Apple 后端在 -1005 时还会丢掉已缓冲未
// 交付的字节，交付量随负载随机）不该把预算耗尽——否则大分片在不稳定链路上
// 下着下着就失败，test_probe_e2e 场景 F 在并发负载下概率性变红正是这个形状。
TEST_CASE(retries_that_make_progress_do_not_consume_the_budget) {
    StubBackend stub;
    auto drop = script_ok(200);
    drop.break_after_bytes = 30;
    drop.chunk_size = 10;
    stub.set_default_script(drop);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_retries = 2;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 200});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(stub.request_count(), 7);   // ceil(200/30)
    CHECK_EQ(task.attempt_count(), 7);   // attempt_count() 仍是总尝试数
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(200));
    CHECK(c.contiguous);
    check_synthetic(c.bytes, 0);
    check_stub(stub);
}

// 进展只重置预算，不是免死金牌：推进过一次之后若连续无进展，照样按
// max_retries 收手。
TEST_CASE(stalled_attempts_after_progress_still_exhaust_the_budget) {
    StubBackend stub;
    auto stall = script_ok(200);
    stall.break_after_bytes = 0;
    auto first = script_ok(200);
    first.break_after_bytes = 50;
    first.chunk_size = 10;
    stub.set_default_script(stall);
    stub.set_script_for_request(1, first);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_retries = 2;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 200});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_NETWORK);
    CHECK_EQ(stub.request_count(), 4);   // 1 次有进展 + 1 + max_retries 次无进展
    CHECK_EQ(c.bytes.size(), static_cast<size_t>(50));
    CHECK_EQ(task.state(), DLTaskState::Failed);
    check_stub(stub);
}

TEST_CASE(fatal_4xx_does_not_retry) {
    StubBackend stub;
    auto sc = script_ok(100);
    sc.http_status = 404;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_retries = 3;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_HTTP_STATUS);
    CHECK_EQ(c.http_status(), 404);
    CHECK_EQ(stub.request_count(), 1);
    check_stub(stub);
}

TEST_CASE(http_500_retries_until_budget) {
    StubBackend stub;
    auto sc = script_ok(100);
    sc.http_status = 500;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_retries = 2;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_HTTP_STATUS);
    CHECK_EQ(c.http_status(), 500);
    CHECK_EQ(stub.request_count(), 3);
    check_stub(stub);
}

TEST_CASE(http_408_and_429_are_retryable) {
    for (int32_t code : {408, 429}) {
        StubBackend stub;
        auto sc = script_ok(50);
        sc.http_status = code;
        stub.set_default_script(sc);
        FakeClock clk;
        Collector c;
        DLTaskConfig cfg = cfg_common();
        cfg.max_retries = 1;
        DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
        task.start("http://x/v", nullptr, Range{0, 50});
        stub.pump_all();
        CHECK_EQ(c.fin(), 1);
        CHECK_EQ(c.status(), SYP_ERR_HTTP_STATUS);
        CHECK_EQ(c.http_status(), code);
        CHECK_EQ(stub.request_count(), 2);
        check_stub(stub);
    }
}

TEST_CASE(http_416_is_content_changed) {
    StubBackend stub;
    auto sc = script_ok(100);
    sc.http_status = 416;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_CONTENT_CHANGED);
    CHECK_EQ(stub.request_count(), 1);
    check_stub(stub);
}

TEST_CASE(error_status_nonempty_body_is_not_delivered) {
    struct Case {
        int32_t    http;
        syp_status expect_st;
        int32_t    expect_http;
        int32_t    max_retries;
    };
    const Case cases[] = {
        {404, SYP_ERR_HTTP_STATUS,     404, 3},
        {500, SYP_ERR_HTTP_STATUS,     500, 0},
        {416, SYP_ERR_CONTENT_CHANGED,   0, 3},
    };
    const std::string html = "<html><body>error page, not media</body></html>";
    for (const auto& tc : cases) {
        StubBackend stub;
        auto sc = script_ok(100);
        sc.http_status = tc.http;
        sc.error_body = html;
        sc.chunk_size = 16;
        stub.set_default_script(sc);
        FakeClock clk;
        Collector c;
        DLTaskConfig cfg = cfg_common();
        cfg.max_retries = tc.max_retries;
        DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
        task.start("http://x/v", nullptr, Range{0, 100});
        stub.pump_all();
        CHECK_EQ(c.fin(), 1);
        CHECK_EQ(c.status(), tc.expect_st);
        CHECK_EQ(c.http_status(), tc.expect_http);
        CHECK_EQ(stub.sink_on_data_bytes(), static_cast<int64_t>(html.size()));
        CHECK_EQ(c.bytes.size(), static_cast<size_t>(0));
        CHECK_EQ(c.data_n, 0);
        CHECK_EQ(task.received_bytes(), int64_t{0});
        check_stub(stub);
    }
}

TEST_CASE(timeout_retries_then_timeout) {
    StubBackend stub;
    auto sc = script_ok(100);
    sc.timeout = true;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_retries = 2;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_TIMEOUT);
    CHECK_EQ(stub.request_count(), 3);
    check_stub(stub);
}

TEST_CASE(cancel_while_connecting) {
    StubBackend stub;
    stub.set_default_script(script_ok(100));
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    task.cancel();
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_CANCELED);
    CHECK_EQ(task.state(), DLTaskState::Canceled);
    check_stub(stub);
}

TEST_CASE(cancel_while_receiving) {
    StubBackend stub;
    auto sc = script_ok(100);
    sc.chunk_size = 1;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 100});
    // 推进到至少有一字节：Running → Responding → Body 若干步
    for (int i = 0; i < 8 && c.bytes.empty(); ++i) stub.pump();
    CHECK(task.received_bytes() > 0 || !c.bytes.empty() || task.state() != DLTaskState::Idle);
    task.cancel();
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_CANCELED);
    check_stub(stub);
}

TEST_CASE(cancel_from_on_data) {
    StubBackend stub;
    auto sc = script_ok(80);
    sc.chunk_size = 8;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    c.task = &task;
    c.cancel_at = 16;
    task.start("http://x/v", nullptr, Range{0, 80});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_CANCELED);
    CHECK(static_cast<int64_t>(c.bytes.size()) >= 16);
    CHECK(static_cast<int64_t>(c.bytes.size()) < 80);
    CHECK(c.contiguous);
    check_stub(stub);
}

TEST_CASE(cancel_after_done_is_noop) {
    StubBackend stub;
    stub.set_default_script(script_ok(20));
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 20});
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    task.cancel();
    stub.pump_all();
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.state(), DLTaskState::Done);
    check_stub(stub);
}

TEST_CASE(dtor_before_finish_cancels_safely) {
    StubBackend stub;
    auto sc = script_ok(1000);
    sc.chunk_size = 1;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    {
        DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
        task.start("http://x/v", nullptr, Range{0, 1000});
        stub.pump();
        stub.pump();
    }
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_CANCELED);
    check_stub(stub);
}

TEST_CASE(content_range_malformed_is_unknown_length) {
    const char* cases[] = {
        "",
        "bytes */*",
        "bytes foo-bar/10",
        "bytes 10-5/20",
        "bytes 0-10/99999999999999999999999",
        "items 0-10/20",
        "bytes 0-10/20 extra",
        "bytes0-10/20",
        "bytes 0-10",
        "bytes 0-/20",
        "bytes -1-10/20",
        "xyz",
    };
    for (const char* s : cases) {
        auto p = parse_content_range(s);
        CHECK_EQ(p.total, int64_t{-1});
    }
    auto ok = parse_content_range("bytes 200-1000/67589");
    CHECK(ok.valid);
    CHECK_EQ(ok.first, int64_t{200});
    CHECK_EQ(ok.last, int64_t{1000});
    CHECK_EQ(ok.total, int64_t{67589});
    auto star = parse_content_range("  BYTES 200-1000/*  ");
    CHECK(star.valid);
    CHECK_EQ(star.total, int64_t{-1});
    CHECK_EQ(star.first, int64_t{200});
}

using MaybeBps = std::optional<int64_t>;

TEST_CASE(speed_zero_span_returns_nullopt) {
    StubBackend stub;
    auto sc = script_ok(3000);
    sc.chunk_size = 3000;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = 3000;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    task.start("http://x/v", nullptr, Range{0, 3000});
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    // now == task_start → span=0，不足最小门槛，估不出来。
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    check_stub(stub);
}

TEST_CASE(speed_startup_does_not_underreport) {
    constexpr int64_t kBytes = 100 * 1024;  // 100 KiB
    constexpr int64_t kSpanMs = 100;
    constexpr int32_t kWindowMs = 3000;
    StubBackend stub;
    auto sc = script_ok(kBytes);
    sc.chunk_size = static_cast<int32_t>(kBytes / 2);  // 两段，中间推进时钟
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = kWindowMs;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, kBytes});
    int guard = 0;
    while (task.received_bytes() == 0 && c.fin() == 0) {
        CHECK(stub.pump());
        if (++guard > 10000) break;
    }
    CHECK_EQ(task.received_bytes(), kBytes / 2);
    clk.advance(kSpanMs);
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.received_bytes(), kBytes);
    // 102400 bytes / 100 ms = 1,024,000 B/s（≈ 1 MiB/s）。
    // 错误公式（÷ 窗口 3000ms）会报 102400*1000/3000 = 34133（≈ 33 KiB/s）。
    CHECK_EQ(task.speed_bps(), MaybeBps{1024000});
    check_stub(stub);
}

TEST_CASE(speed_startup_does_not_overreport) {
    constexpr int64_t kChunk = 51200;
    constexpr int64_t kBytes = kChunk * 2;
    constexpr int32_t kWindowMs = 3000;
    StubBackend stub;
    auto sc = script_ok(kBytes);
    sc.chunk_size = static_cast<int32_t>(kChunk);
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = kWindowMs;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, kBytes});
    // 请求 t=0 发出（task_start=0）；第一块推迟到 t=50，第二块 t=100。
    clk.advance(50);
    int guard = 0;
    while (task.received_bytes() == 0 && c.fin() == 0) {
        CHECK(stub.pump());
        if (++guard > 10000) break;
    }
    CHECK_EQ(task.received_bytes(), kChunk);
    clk.advance(50);
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.received_bytes(), kBytes);
    // left = max(now - 3000, task_start=0) = 0，span = now，sum = 102400。
    // now=100: 102400 * 1000 / 100 = 1,024,000
    // now=200: 102400 * 1000 / 200 = 512,000
    // now=300: 102400 * 1000 / 300 = 341,333
    // 若左端点取最早样本 t=50，now=100 会报 2,048,000（高报 2 倍）。
    CHECK_EQ(task.speed_bps(), MaybeBps{1024000});
    clk.advance(100);
    CHECK_EQ(task.speed_bps(), MaybeBps{512000});
    clk.advance(100);
    CHECK_EQ(task.speed_bps(), MaybeBps{341333});
    check_stub(stub);
}

TEST_CASE(speed_steady_state_matches_window_average) {
    StubBackend stub;
    auto sc = script_ok(3000);
    sc.chunk_size = 3000;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = 3000;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    task.start("http://x/v", nullptr, Range{0, 3000});
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    clk.advance(3000);
    // 经过时间 == 窗口：3000 bytes / 3000 ms = 1000 B/s，与旧稳态语义一致。
    CHECK_EQ(task.speed_bps(), MaybeBps{1000});
    clk.advance(1);
    CHECK_EQ(task.speed_bps(), MaybeBps{0});  // 闭区间：滑出窗口，真停滞
    check_stub(stub);
}

TEST_CASE(speed_window_zero_returns_nullopt) {
    StubBackend stub;
    auto sc = script_ok(3000);
    sc.chunk_size = 3000;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = 0;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    task.start("http://x/v", nullptr, Range{0, 3000});
    clk.advance(100);
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.received_bytes(), int64_t{3000});
    // 窗口配成 0：估不出来，即使已经有数据和足够长的墙钟时间。
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    check_stub(stub);
}

TEST_CASE(speed_retry_loop_keeps_estimate) {
    // 每 50ms 一步；每次尝试 Running → Responding → Body+断流（3 步）。
    // 3 次断流后：now=450，received=153600，attempt=4。
    // left = task_start=0，span=450，sum=153600
    // 153600 * 1000 / 450 = 341333 B/s（≈ 341 KB/s）。
    constexpr int64_t kChunk = 51200;
    constexpr int64_t kStepMs = 50;
    constexpr int kDisconnects = 3;
    constexpr int kSteps = kDisconnects * 3;  // 9
    constexpr int64_t kWantBps = 341333;
    constexpr int64_t kFile = 1024 * 1024;

    StubBackend stub;
    auto sc = script_ok(kFile);
    sc.chunk_size = static_cast<int32_t>(kChunk);
    sc.break_after_bytes = kChunk;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.max_retries = 10;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, kFile});
    for (int step = 1; step <= kSteps; ++step) {
        clk.advance(kStepMs);
        CHECK(stub.pump());
    }
    CHECK_EQ(clk.t.load(std::memory_order_relaxed), int64_t{450});
    CHECK_EQ(task.received_bytes(), kChunk * kDisconnects);
    CHECK_EQ(task.attempt_count(), kDisconnects + 1);
    CHECK_EQ(task.speed_bps(), MaybeBps{kWantBps});
    task.cancel();
    stub.pump_all();
    check_stub(stub);
}

TEST_CASE(speed_short_span_returns_nullopt) {
    constexpr int64_t kChunk = 51200;
    StubBackend stub;
    auto sc = script_ok(kChunk);
    sc.chunk_size = static_cast<int32_t>(kChunk);
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = 3000;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, kChunk});
    clk.advance(1);
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.received_bytes(), kChunk);
    // span=1ms < 50ms 门槛：估不出来，而不是 51,200,000 B/s。
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    check_stub(stub);
}

TEST_CASE(speed_never_received_bytes_is_nullopt) {
    StubBackend stub;
    auto sc = script_ok(1000);
    sc.chunk_size = 1000;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = 3000;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 1000});
    CHECK_EQ(task.state(), DLTaskState::Connecting);
    CHECK_EQ(task.received_bytes(), int64_t{0});
    CHECK_EQ(task.speed_bps(), MaybeBps{});  // span=0，估不出来
    clk.advance(50);
    // 已 start、仍在 Connecting、一个字节都没收到：nullopt，不是 0。
    // 0 会让调度器把刚起步 50ms 的任务当停滞踢掉。
    CHECK_EQ(task.state(), DLTaskState::Connecting);
    CHECK_EQ(task.received_bytes(), int64_t{0});
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    task.cancel();
    stub.pump_all();
    check_stub(stub);
}

TEST_CASE(speed_true_stall_when_data_slides_out_of_window) {
    StubBackend stub;
    auto sc = script_ok(3000);
    sc.chunk_size = 3000;
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = 3000;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, 3000});
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.received_bytes(), int64_t{3000});
    // 数据在 t=0 入窗。推进到 now=window+1，样本滑出，窗口内 0 字节。
    // 曾经收到过数据 + span 已够门槛 → 真停滞，必须是 0 不是 nullopt。
    clk.advance(3001);
    CHECK_EQ(task.speed_bps(), MaybeBps{0});
    CHECK_EQ(task.received_bytes(), int64_t{3000});
    check_stub(stub);
}

TEST_CASE(speed_window_shorter_than_min_span_still_estimates) {
    constexpr int64_t kBytes = 20480;
    constexpr int32_t kWindowMs = 20;
    StubBackend stub;
    auto sc = script_ok(kBytes);
    sc.chunk_size = static_cast<int32_t>(kBytes);
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTaskConfig cfg = cfg_common();
    cfg.speed_window_ms = kWindowMs;
    DLTask task(stub.backend(), clk.clock(), cfg, c.cbs());
    task.start("http://x/v", nullptr, Range{0, kBytes});
    stub.pump_all();
    CHECK_EQ(c.status(), SYP_OK);
    CHECK_EQ(task.received_bytes(), kBytes);
    clk.advance(19);
    // span=19 < min(50, 20)=20：仍不够门槛。
    CHECK_EQ(task.speed_bps(), MaybeBps{});
    clk.advance(1);
    // 若不取 min(kMinSpeedSpanMs, window)，span 被窗口封顶永远 < 50，
    // 这里会一直是 nullopt。20480 * 1000 / 20 = 1,024,000。
    CHECK_EQ(task.speed_bps(), MaybeBps{1024000});
    check_stub(stub);
}

TEST_CASE(async_cancel_unstarted_emits_complete) {
    // 契约：create 之后、start 之前 cancel，后端必须恰好一次
    // on_complete(SYP_ERR_CANCELED, 0)。异步模式下没有 worker，必须由
    // cancel 自己补发，否则 ~DLTask 永久阻塞。
    StubBackend stub;
    stub.set_mode(StubBackend::Mode::Async);
    stub.set_default_script(script_ok(1024));

    struct Box {
        std::mutex mu;
        std::condition_variable cv;
        int n = 0;
        syp_status st = SYP_OK;
        int32_t http = -1;
    } box;

    syp_response_sink sink{};
    sink.ctx = &box;
    sink.on_complete = [](void* ctx, syp_status st, int32_t http) {
        auto* b = static_cast<Box*>(ctx);
        std::lock_guard<std::mutex> g(b->mu);
        ++b->n;
        b->st = st;
        b->http = http;
        b->cv.notify_all();
    };

    syp_http_request req{};
    req.url = "http://x/v";
    req.range_start = 0;
    req.range_end = -1;
    req.connect_timeout_ms = 1000;
    req.read_timeout_ms = 1000;

    const syp_http_backend* be = stub.backend();
    auto* h = be->create(be->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    be->cancel(h);
    {
        std::unique_lock<std::mutex> lk(box.mu);
        CHECK(box.cv.wait_for(lk, std::chrono::seconds(2),
                              [&] { return box.n > 0; }));
        CHECK_EQ(box.n, 1);
        CHECK_EQ(box.st, SYP_ERR_CANCELED);
        CHECK_EQ(box.http, 0);
    }
    be->destroy(h);
    CHECK_EQ(box.n, 1);
    check_stub(stub);
}

TEST_CASE(async_instant_complete_repeated_create_destroy) {
    // 请求瞬间完成：worker 在 spawn_worker 把 thread 对象存进去之前就开始
    // 跑，destroy 会去 join 一个还没写入的 handle。反复 create/destroy 不应
    // terminate，TSan 也不该报 worker 句柄竞态。
    constexpr int kRounds = 80;
    for (int round = 0; round < kRounds; ++round) {
        StubBackend stub;
        stub.set_mode(StubBackend::Mode::Async);
        auto sc = script_ok(8);
        sc.chunk_size = 8;
        stub.set_default_script(sc);
        FakeClock clk;
        Collector c;
        {
            DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
            task.start("http://x/v", nullptr, Range{0, 8});
            c.wait_finished();
            CHECK_EQ(c.fin(), 1);
            CHECK_EQ(c.status(), SYP_OK);
        }
        check_stub(stub);
    }
}

TEST_CASE(async_cancel_race_with_backend_thread) {
    constexpr int kRounds = 30;
    for (int round = 0; round < kRounds; ++round) {
        StubBackend stub;
        stub.set_mode(StubBackend::Mode::Async);
        auto sc = script_ok(64 * 1024);
        sc.chunk_size = 32;
        stub.set_default_script(sc);
        FakeClock clk;
        Collector c;
        DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
        task.start("http://x/v", nullptr, Range{0, 64 * 1024});
        while (task.received_bytes() == 0 && c.fin() == 0) {
            std::this_thread::yield();
        }
        task.cancel();
        c.wait_finished();
        CHECK_EQ(c.fin(), 1);
        CHECK(c.status() == SYP_ERR_CANCELED || c.status() == SYP_OK);
        for (int i = 0; i < 64; ++i) std::this_thread::yield();
        CHECK_EQ(c.fin(), 1);
        check_stub(stub);
    }
}

TEST_CASE(cancel_after_complete_does_not_use_destroyed_handle) {
    // 注入 B3：cancel 取出 handle 放锁之后、backend_->cancel 之前，
    // 同步走完 on_complete → destroy。未修时代 cancel 会拿到已释放句柄。
    struct CancelUafBackend {
        struct Rec {
            uint32_t magic = 0;
            bool     destroyed = false;
            CancelUafBackend* owner = nullptr;
        };
        syp_http_backend table{};
        syp_response_sink sink{};
        Rec rec{};
        int create_n = 0;
        int cancel_n = 0;
        int destroy_n = 0;
        bool cancel_after_destroy = false;
        bool cancel_bad_magic = false;

        enum : uint32_t { kLive = 0x48524E44u, kDead = 0xDEADBEEFu };

        CancelUafBackend() {
            rec.owner = this;
            rec.magic = kLive;
            table.backend_ctx = this;
            table.create = [](void* ctx, const syp_http_request*,
                              const syp_response_sink* sink)
                -> syp_http_request_handle* {
                auto* self = static_cast<CancelUafBackend*>(ctx);
                if (sink == nullptr) return nullptr;
                self->sink = *sink;
                ++self->create_n;
                self->rec.magic = kLive;
                self->rec.destroyed = false;
                return reinterpret_cast<syp_http_request_handle*>(&self->rec);
            };
            table.start = [](syp_http_request_handle*) {};
            table.cancel = [](syp_http_request_handle* h) {
                auto* rec = reinterpret_cast<Rec*>(h);
                if (rec == nullptr || rec->owner == nullptr) {
                    return;
                }
                auto* self = rec->owner;
                ++self->cancel_n;
                if (rec->destroyed || rec->magic != kLive) {
                    self->cancel_after_destroy = true;
                    if (rec->magic != kLive) self->cancel_bad_magic = true;
                }
            };
            table.destroy = [](syp_http_request_handle* h) {
                auto* rec = reinterpret_cast<Rec*>(h);
                if (rec == nullptr || rec->owner == nullptr) return;
                auto* self = rec->owner;
                ++self->destroy_n;
                rec->destroyed = true;
                rec->magic = kDead;
            };
        }
        const syp_http_backend* backend() const noexcept { return &table; }
    } be;

    FakeClock clk;
    Collector c;
    DLTask task(be.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 64});
    CHECK_EQ(be.create_n, 1);

    struct Hook {
        CancelUafBackend* be = nullptr;
        static void run(void* p) {
            auto* hk = static_cast<Hook*>(p);
            if (hk->be->sink.on_complete != nullptr) {
                hk->be->sink.on_complete(hk->be->sink.ctx, SYP_OK, 0);
            }
        }
    } hook;
    hook.be = &be;
    task.set_test_hook_after_cancel_take_handle(&Hook::run, &hook);
    task.cancel();
    c.wait_finished();

    CHECK(!be.cancel_after_destroy);
    CHECK(!be.cancel_bad_magic);
    CHECK_EQ(be.destroy_n, 1);
    CHECK_EQ(c.fin(), 1);
    CHECK_EQ(c.status(), SYP_ERR_CANCELED);
}

TEST_CASE(timestamps_are_minus_one_before_start) {
    StubBackend stub;
    stub.set_default_script(script_ok(64));
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    CHECK_EQ(task.attempt_started_ms(), int64_t{-1});
    CHECK_EQ(task.last_progress_ms(), int64_t{-1});
}

// StubBackend 的 break_after_bytes 是在 Body 阶段"发完这一块数据、再判断
// 是否踩到断点"，踩中时同一次 step()（同一次 pump()）里就同步调用
// on_complete → DLTask::sink_on_complete → issue_request()：数据交付与
// 重试发生在同一个时钟读数上，不会分属两次 pump、
// 两个不同的 now_ms()。这里改用"先证明重试真的把 attempt_started_ms 往
// 前推"，再"证明重试之后的新尝试继续推进 last_progress_ms"来钉住两者互相
// 独立：attempt_started_ms 只在 issue_request 更新，last_progress_ms 只在
// 收到字节时更新，二者不会互相污染。用循环等条件（仿本文件其它用例的
// 写法），不写死 pump() 调用次数——这与 StubBackend::step() 的阶段划分
// （Running→Responding→Body 之间可能有静默过渡）耦合，写死次数会很脆。
TEST_CASE(attempt_started_ms_updates_on_retry) {
    StubBackend stub;
    auto ok = script_ok(300);
    ok.chunk_size = 100;
    auto fail = ok;
    fail.break_after_bytes = 100;  // 发满 100 字节（=chunk_size）后断流
    stub.set_default_script(ok);          // 第 2 次尝试起：正常
    stub.set_script_for_request(1, fail); // 第 1 次尝试：断流
    FakeClock clk;
    clk.advance(1000);
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 300});
    CHECK_EQ(task.attempt_started_ms(), int64_t{1000});
    CHECK_EQ(task.last_progress_ms(), int64_t{-1});

    clk.advance(500);  // now = 1500
    // 推进到第 1 次尝试的 on_response：响应头不算进展。
    int guard = 0;
    while (c.total_n == 0 && c.fin() == 0) {
        REQUIRE(stub.pump());
        if (++guard > 1000) break;
    }
    CHECK_EQ(c.total_n, 1);
    CHECK_EQ(task.last_progress_ms(), int64_t{-1});
    CHECK_EQ(task.attempt_started_ms(), int64_t{1000});

    // 推进到断点：一个 pump 里既交付 100 字节又触发同步重试（见上方注释）。
    guard = 0;
    while (task.attempt_count() < 2 && c.fin() == 0) {
        REQUIRE(stub.pump());
        if (++guard > 1000) break;
    }
    CHECK_EQ(task.attempt_count(), 2);
    // 二者在这一刻**恰好相等**（同一次断流事件既是最后一次进展、也是
    // 重试的触发点），但更新它们的是两处独立代码（record_speed_locked
    // vs. issue_request）——下面继续推进，证明它们此后各走各的。
    CHECK_EQ(task.last_progress_ms(), int64_t{1500});
    CHECK_EQ(task.attempt_started_ms(), int64_t{1500});

    clk.advance(500);  // now = 2000
    // 第 2 次尝试继续收数据：last_progress_ms 该跟着走，
    // attempt_started_ms 不该再变（没有第 3 次尝试）。
    guard = 0;
    while (c.data_n < 2 && c.fin() == 0) {
        REQUIRE(stub.pump());
        if (++guard > 1000) break;
    }
    CHECK(c.data_n >= 2);
    CHECK_EQ(task.last_progress_ms(), int64_t{2000});
    CHECK_EQ(task.attempt_started_ms(), int64_t{1500});
    CHECK_EQ(task.attempt_count(), 2);

    task.cancel();
    stub.pump_all();
    check_stub(stub);
}

TEST_CASE(last_progress_ms_tracks_delivery) {
    StubBackend stub;
    auto sc = script_ok(200);
    sc.chunk_size = 100;  // 两块，各 100 字节
    stub.set_default_script(sc);
    FakeClock clk;
    Collector c;
    DLTask task(stub.backend(), clk.clock(), cfg_common(), c.cbs());
    task.start("http://x/v", nullptr, Range{0, 200});
    CHECK_EQ(task.attempt_started_ms(), int64_t{0});
    CHECK_EQ(task.last_progress_ms(), int64_t{-1});

    clk.advance(300);  // now = 300
    int guard = 0;
    while (c.total_n == 0 && c.fin() == 0) {
        REQUIRE(stub.pump());
        if (++guard > 1000) break;
    }
    CHECK_EQ(c.total_n, 1);
    // 响应头不算进展。
    CHECK_EQ(task.last_progress_ms(), int64_t{-1});

    clk.advance(400);  // now = 700
    REQUIRE(stub.pump());  // Body 阶段的第一步就是交付第一块数据
    CHECK_EQ(c.data_n, 1);
    CHECK_EQ(task.last_progress_ms(), int64_t{700});

    clk.advance(200);  // now = 900
    REQUIRE(stub.pump());  // 第二块（也是最后一块）数据
    CHECK_EQ(c.data_n, 2);
    CHECK_EQ(task.last_progress_ms(), int64_t{900});

    task.cancel();
    stub.pump_all();
    check_stub(stub);
}

int main() { return tiny_test_main(); }
