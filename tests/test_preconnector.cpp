// test_preconnector.cpp — Preconnector：源归一化、1 字节 Range、30 秒去重、
// 在途上限 4、回收、不重试、头透传、析构收尾。同步桩 + 假时钟，析构用例另起异步桩。
#include "tiny_test.h"

#include "support/stub_backend.h"

#include <dl/clock.h>
#include <dl/preconnector.h>
#include <dl/source_bridge.h>   // current_http_backend()：C 入口用例保存/还原全局后端用

#include <syplayer/syp_http.h>
#include <syplayer/syp_net.h>
#include <syplayer/syp_types.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using syp::dl::Clock;
using syp::dl::Preconnector;
using syp::dl::test::StubBackend;

namespace {

struct FakeClk {
    int64_t t = 0;
    static int64_t now(void* ctx) { return static_cast<FakeClk*>(ctx)->t; }
    Clock clock() { return Clock{&now, this}; }
};

// backend_fn 的注入口：每次 preconnect 取一次，可以中途换成 nullptr / 非空。
struct BackendSlot {
    const syp_http_backend* be = nullptr;
    static const syp_http_backend* get(void* ctx) {
        return static_cast<BackendSlot*>(ctx)->be;
    }
};

bool contract_ok(const StubBackend& stub) {
    std::string msg;
    const bool ok = stub.contract_ok(&msg);
    if (!ok) std::printf("  contract violation: %s\n", msg.c_str());
    return ok;
}

// 【C 入口用例专用】set_http_backend() 是"就地覆盖"（source_bridge.h:231-235
// 顶注），保存旧后端必须按值拷贝，不能存指针再原样传回去（那是自赋值，什么
// 都不会还原）。写法照抄 test_hls_e2e.cpp 的 HttpBackendGuard。
struct HttpBackendGuard {
    syp_http_backend prev_{};
    bool             had_prev_ = false;
    explicit HttpBackendGuard(const syp_http_backend* next) {
        if (const syp_http_backend* p = syp::dl::current_http_backend()) {
            prev_     = *p;
            had_prev_ = true;
        }
        syp_set_http_backend(next);
    }
    ~HttpBackendGuard() { syp_set_http_backend(had_prev_ ? &prev_ : nullptr); }
    HttpBackendGuard(const HttpBackendGuard&)            = delete;
    HttpBackendGuard& operator=(const HttpBackendGuard&) = delete;
};

}  // namespace

TEST_CASE(origin_key_normalizes) {
    using syp::dl::detail::origin_key;
    CHECK_EQ(origin_key("http://Example.COM/a/b?c=1#f"), std::string("http://example.com:80"));
    CHECK_EQ(origin_key("HTTPS://cdn.x.com:8443/v.mp4"), std::string("https://cdn.x.com:8443"));
    CHECK_EQ(origin_key("https://u:p@cdn.x.com/v"), std::string("https://cdn.x.com:443"));
    CHECK_EQ(origin_key("http://[::1]:9000/x"), std::string("http://[::1]:9000"));
    CHECK_EQ(origin_key("file:///tmp/a.mp4"), std::string());
    CHECK_EQ(origin_key("http:///nohost"), std::string());
    CHECK_EQ(origin_key("http://h:abc/"), std::string());
    CHECK_EQ(origin_key("not a url"), std::string());
    // 补充：无路径、只有查询、IPv6 缺省端口、空串
    CHECK_EQ(origin_key("https://CDN.x.com"), std::string("https://cdn.x.com:443"));
    CHECK_EQ(origin_key("http://h.com?q=1"), std::string("http://h.com:80"));
    CHECK_EQ(origin_key("https://[::1]/x"), std::string("https://[::1]:443"));
    CHECK_EQ(origin_key(""), std::string());
    CHECK_EQ(origin_key("ftp://h.com/a"), std::string());
    // 端口与 IPv6 的补充规则（preconnector.h 的 origin_key 注释）
    CHECK_EQ(origin_key("http://h:/"), std::string("http://h:80"));      // 冒号后空 ⇒ 缺省
    CHECK_EQ(origin_key("http://h:0/"), std::string());                 // 0 非法
    CHECK_EQ(origin_key("http://h:65535/"), std::string("http://h:65535"));
    CHECK_EQ(origin_key("http://h:65536/"), std::string());             // 越界
    CHECK_EQ(origin_key("http://[]/"), std::string());                  // 空 IPv6
    CHECK_EQ(origin_key("http://[::1]x/"), std::string());              // ] 后非冒号
}

TEST_CASE(sends_one_byte_range_request) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        const std::string url = "https://cdn.x.com/v/a.mp4?sig=Ab#frag";
        CHECK(pc.preconnect(url, nullptr));
        REQUIRE(stub.request_count() == 1);
        const auto req = stub.request_at(0);
        CHECK_EQ(req.url, url);
        CHECK_EQ(req.range_start, int64_t{0});
        CHECK_EQ(req.range_end, int64_t{0});
        CHECK(req.headers.empty());
        stub.pump_all();
        CHECK_EQ(pc.inflight_for_test(), 0);
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(same_origin_within_30s_is_deduped) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        clk.t = 0;
        CHECK(pc.preconnect("https://cdn.x.com/a.mp4", nullptr));
        stub.pump_all();
        clk.t = 29999;
        // 同源（路径不同、host 大小写不同、显式写默认端口）⇒ 不发
        CHECK(!pc.preconnect("https://CDN.x.com:443/b.mp4", nullptr));
        CHECK_EQ(stub.request_count(), 1);
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(same_origin_after_30s_sends_again) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        clk.t = 0;
        CHECK(pc.preconnect("https://cdn.x.com/a.mp4", nullptr));
        stub.pump_all();
        clk.t = 30000;
        CHECK(pc.preconnect("https://cdn.x.com/b.mp4", nullptr));
        CHECK_EQ(stub.request_count(), 2);
        stub.pump_all();
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(different_origins_are_independent) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        CHECK(pc.preconnect("http://a.com/x", nullptr));
        CHECK(pc.preconnect("http://b.com/x", nullptr));        // host 不同
        CHECK(pc.preconnect("https://a.com/x", nullptr));       // scheme 不同
        stub.pump_all();
        CHECK(pc.preconnect("http://a.com:8080/x", nullptr));   // 端口不同
        CHECK(!pc.preconnect("http://A.com:80/y", nullptr));    // 与第一个同源
        CHECK_EQ(stub.request_count(), 4);
        stub.pump_all();
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(inflight_cap_is_four) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        // 同步桩不 pump ⇒ 四个都停在途中
        CHECK(pc.preconnect("http://h1.com/", nullptr));
        CHECK(pc.preconnect("http://h2.com/", nullptr));
        CHECK(pc.preconnect("http://h3.com/", nullptr));
        CHECK(pc.preconnect("http://h4.com/", nullptr));
        CHECK_EQ(pc.inflight_for_test(), 4);
        CHECK(!pc.preconnect("http://h5.com/", nullptr));
        CHECK_EQ(stub.request_count(), 4);
        CHECK_EQ(pc.inflight_for_test(), 4);
        // 全部完成后，下一次调用回收并可再发；被拒的 h5 没登记去重，可立刻发
        stub.pump_all();
        CHECK_EQ(pc.inflight_for_test(), 0);
        CHECK(pc.preconnect("http://h5.com/", nullptr));
        CHECK_EQ(pc.inflight_for_test(), 1);
        CHECK_EQ(stub.request_count(), 5);
        stub.pump_all();
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(finished_tasks_are_reaped_on_next_call) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        CHECK(pc.preconnect("http://a.com/", nullptr));
        CHECK(pc.preconnect("http://b.com/", nullptr));
        CHECK_EQ(pc.inflight_for_test(), 2);
        CHECK_EQ(pc.retained_for_test(), 2);
        stub.pump_all();
        // 已结束但尚未回收：没有自有线程，要等下一次调用
        CHECK_EQ(pc.inflight_for_test(), 0);
        CHECK_EQ(pc.retained_for_test(), 2);
        CHECK(pc.preconnect("http://c.com/", nullptr));
        CHECK_EQ(pc.retained_for_test(), 1);
        CHECK_EQ(pc.inflight_for_test(), 1);
        stub.pump_all();
        // 被去重拒绝的调用同样回收
        CHECK(!pc.preconnect("http://c.com/", nullptr));
        CHECK_EQ(pc.retained_for_test(), 0);
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(no_backend_does_nothing) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{nullptr};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        CHECK(!pc.preconnect("https://cdn.x.com/a.mp4", nullptr));
        CHECK_EQ(pc.inflight_for_test(), 0);
        CHECK_EQ(pc.retained_for_test(), 0);
        CHECK_EQ(stub.request_count(), 0);
        // 后端每次调用重新取：注册之后同一源可以立刻发（没因无后端登记去重）
        slot.be = stub.backend();
        CHECK(pc.preconnect("https://cdn.x.com/a.mp4", nullptr));
        CHECK_EQ(stub.request_count(), 1);
        stub.pump_all();
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(non_http_url_does_nothing) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        CHECK(!pc.preconnect("file:///tmp/a.mp4", nullptr));
        CHECK(!pc.preconnect("ftp://h.com/a", nullptr));
        CHECK(!pc.preconnect("not a url", nullptr));
        CHECK(!pc.preconnect("", nullptr));
        CHECK(!pc.preconnect("http:///nohost", nullptr));
        CHECK_EQ(stub.request_count(), 0);
        CHECK_EQ(pc.retained_for_test(), 0);
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(range_unsupported_full_body_is_canceled_after_first_chunk) {
    StubBackend stub;
    StubBackend::Script sc;
    sc.support_range   = false;
    sc.http_status     = 200;
    sc.resource_length = int64_t{1} << 20;
    sc.chunk_size      = 4096;
    stub.set_default_script(sc);
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        CHECK(pc.preconnect("http://norange.com/big.mp4", nullptr));
        stub.pump_all();
        CHECK(stub.sink_on_data_bytes() > 0);
        CHECK(stub.sink_on_data_bytes() <= 4096);
        CHECK(stub.cancel_calls() >= 1);
        CHECK_EQ(pc.inflight_for_test(), 0);
        CHECK_EQ(stub.request_count(), 1);
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(range_206_one_byte_is_not_canceled) {
    // 206：1 字节 body 就是完整响应。不能 cancel——URLSession 可能因此拆掉
    // 连接而不是还回连接池，预连接白做。
    StubBackend stub;
    StubBackend::Script sc;
    sc.support_range   = true;
    sc.http_status     = 206;
    sc.resource_length = int64_t{1} << 20;
    sc.chunk_size      = 4096;
    stub.set_default_script(sc);
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        CHECK(pc.preconnect("http://range.com/big.mp4", nullptr));
        stub.pump_all();
        CHECK_EQ(stub.sink_on_data_bytes(), int64_t{1});
        CHECK_EQ(pc.inflight_for_test(), 0);
        CHECK_EQ(stub.cancel_calls(), 0);
    }
    // 析构路径也不该补发 cancel（任务已终态）
    CHECK_EQ(stub.cancel_calls(), 0);
    CHECK(contract_ok(stub));
}

TEST_CASE(no_retry_on_failure) {
    StubBackend stub;
    StubBackend::Script sc;
    sc.timeout = true;
    stub.set_default_script(sc);
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        CHECK(pc.preconnect("http://slow.com/a", nullptr));
        stub.pump_all();
        CHECK_EQ(stub.request_count(), 1);
        CHECK_EQ(pc.inflight_for_test(), 0);
        // 失败同样计入去重：30 秒内不因失败而重发
        clk.t = 29999;
        CHECK(!pc.preconnect("http://slow.com/b", nullptr));
        CHECK_EQ(stub.request_count(), 1);
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(headers_are_forwarded) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        const char* names[]  = {"Authorization", "X-Trace"};
        const char* values[] = {"Bearer tok", "42"};
        syp_headers h{names, values, 2};
        CHECK(pc.preconnect("https://auth.cdn.com/v.mp4", &h));
        REQUIRE(stub.request_count() == 1);
        const auto req = stub.request_at(0);
        REQUIRE(req.headers.size() == 2);
        CHECK_EQ(req.headers[0].first, std::string("Authorization"));
        CHECK_EQ(req.headers[0].second, std::string("Bearer tok"));
        CHECK_EQ(req.headers[1].first, std::string("X-Trace"));
        CHECK_EQ(req.headers[1].second, std::string("42"));
        stub.pump_all();
    }
    CHECK(contract_ok(stub));
}

TEST_CASE(dtor_cancels_inflight) {
    // 同步桩：请求停在途中（没人 pump），析构必须 cancel 并收回，不挂。
    {
        StubBackend stub;
        FakeClk clk;
        BackendSlot slot{stub.backend()};
        {
            Preconnector pc(&BackendSlot::get, &slot, clk.clock());
            CHECK(pc.preconnect("http://h1.com/", nullptr));
            CHECK(pc.preconnect("http://h2.com/", nullptr));
            CHECK(pc.preconnect("http://h3.com/", nullptr));
            CHECK_EQ(pc.inflight_for_test(), 3);
        }
        CHECK_EQ(stub.request_count(), 3);
        CHECK(contract_ok(stub));
    }
    // 异步桩：后端线程与析构并发。200 全量大资源、1 字节一块，析构时大概率还在途。
    for (int round = 0; round < 20; ++round) {
        StubBackend stub;
        stub.set_mode(StubBackend::Mode::Async);
        StubBackend::Script sc;
        sc.support_range   = false;
        sc.http_status     = 200;
        sc.resource_length = int64_t{1} << 24;
        sc.chunk_size      = 1;
        stub.set_default_script(sc);
        FakeClk clk;
        BackendSlot slot{stub.backend()};
        {
            Preconnector pc(&BackendSlot::get, &slot, clk.clock());
            CHECK(pc.preconnect("http://a1.com/", nullptr));
            CHECK(pc.preconnect("http://a2.com/", nullptr));
            CHECK(pc.preconnect("http://a3.com/", nullptr));
            CHECK(pc.preconnect("http://a4.com/", nullptr));
        }
        CHECK(contract_ok(stub));
    }
}

TEST_CASE(dedupe_map_does_not_grow_without_bound) {
    StubBackend stub;
    FakeClk clk;
    BackendSlot slot{stub.backend()};
    {
        Preconnector pc(&BackendSlot::get, &slot, clk.clock());
        int sent = 0;
        int32_t max_entries = 0;
        for (int i = 0; i < 1000; ++i) {
            clk.t += 30000;
            const std::string url = "http://h" + std::to_string(i) + ".com/v";
            if (pc.preconnect(url, nullptr)) ++sent;
            stub.pump_all();
            const int32_t n = pc.dedupe_entries_for_test();
            if (n > max_entries) max_entries = n;
        }
        CHECK_EQ(sent, 1000);
        CHECK_EQ(max_entries, 1);
    }
    CHECK(contract_ok(stub));
}

// ---- C 入口（syp_preconnect，syp_net.h / syp_net_api.cpp）----
//
// 这三个用例经 syp_preconnect() 打到 Preconnector::instance()（进程单例、
// 真实系统时钟），不是本文件其余用例用的独立实例。单例的 30 秒去重窗口
// 跨用例持续存在，所以每个用例用不同的源，互不干扰。

TEST_CASE(c_entry_null_url_is_noop) {
    StubBackend stub;
    const HttpBackendGuard guard(stub.backend());
    syp_preconnect(nullptr, nullptr);   // 不崩即通过
    stub.pump_all();
    CHECK_EQ(stub.request_count(), 0);
}

TEST_CASE(c_entry_without_backend_is_noop) {
    // guard(nullptr)：清掉本进程当前注册的后端（若有），还原时按 HttpBackendGuard
    // 保存的值原样传回去。
    const HttpBackendGuard guard(nullptr);
    // 没有后端可查请求计数，这里只验证不崩、静默返回。
    syp_preconnect("http://c-entry-no-backend.example.com/a", nullptr);
}

TEST_CASE(c_entry_uses_registered_backend) {
    StubBackend stub;
    const HttpBackendGuard guard(stub.backend());
    syp_preconnect("http://c-entry-uses-backend.example.com/v", nullptr);
    stub.pump_all();
    REQUIRE(stub.request_count() == 1);
    const auto req = stub.request_at(0);
    CHECK_EQ(req.range_start, int64_t{0});
    CHECK_EQ(req.range_end, int64_t{0});
    CHECK(contract_ok(stub));
}

int main() { return tiny_test_main(); }
