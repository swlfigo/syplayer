// test_apple_http_backend.cpp — NSURLSession 后端契约：真实回环网络，不连外网
#include "tiny_test.h"
#include "support/loopback_server.h"
#include "support/watchdog.h"

#include <dl/clock.h>
#include <dl/dl_task.h>
#include <dl/hole_set.h>
#include <platform/apple/apple_http_backend.h>
#include <syplayer/syp_http.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using syp::dl::DLTask;
using syp::dl::DLTaskCallbacks;
using syp::dl::DLTaskConfig;
using syp::dl::Range;
using syp::dl::system_clock;
using syp::dl::test::LoopbackConfig;
using syp::dl::test::LoopbackServer;
using syp::dl::test::synthetic_byte;

namespace {

struct Rec {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int> in_cb{0};
    std::atomic<int> max_in_cb{0};
    std::string seq;
    int32_t http_status = 0;
    int64_t content_length = -3;
    int64_t total_length   = -3;
    std::vector<uint8_t> body;
    int complete_n = 0;
    syp_status complete_st = 1;
    int32_t complete_http = -1;
    std::vector<std::string> redirects;
    bool follow_redirect = true;
    int callbacks_after_complete = 0;
    std::vector<std::string> hdr_names;
    std::vector<std::string> hdr_values;

    // 大小写不敏感地取一个响应头；不存在返回空串。
    std::string header(const std::string& name) const {
        for (size_t i = 0; i < hdr_names.size(); ++i) {
            if (hdr_names[i].size() != name.size()) continue;
            bool same = true;
            for (size_t k = 0; k < name.size(); ++k) {
                const char a = static_cast<char>(std::tolower(hdr_names[i][k]));
                const char b = static_cast<char>(std::tolower(name[k]));
                if (a != b) { same = false; break; }
            }
            if (same) return hdr_values[i];
        }
        return {};
    }

    void enter() {
        const int n = in_cb.fetch_add(1, std::memory_order_acq_rel) + 1;
        int m = max_in_cb.load(std::memory_order_relaxed);
        while (n > m
               && !max_in_cb.compare_exchange_weak(m, n, std::memory_order_relaxed)) {
        }
    }
    void leave() { in_cb.fetch_sub(1, std::memory_order_acq_rel); }

    void note_after_complete() {
        if (complete_n > 0) ++callbacks_after_complete;
    }

    static void on_response(void* ctx, int32_t http_status, const syp_headers* headers,
                            int64_t content_length, int64_t total_length) {
        auto* r = static_cast<Rec*>(ctx);
        r->enter();
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->note_after_complete();
            r->seq.push_back('R');
            r->http_status = http_status;
            r->content_length = content_length;
            r->total_length = total_length;
            if (headers != nullptr) {
                for (int32_t i = 0; i < headers->count; ++i) {
                    r->hdr_names.emplace_back(headers->names[i] ? headers->names[i] : "");
                    r->hdr_values.emplace_back(headers->values[i] ? headers->values[i] : "");
                }
            }
        }
        r->leave();
    }

    static void on_data(void* ctx, const uint8_t* data, int32_t len) {
        auto* r = static_cast<Rec*>(ctx);
        r->enter();
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->note_after_complete();
            r->seq.push_back('D');
            if (data != nullptr && len > 0) {
                r->body.insert(r->body.end(), data, data + len);
            }
            r->cv.notify_all();
        }
        r->leave();
    }

    static bool on_redirect(void* ctx, const char* new_url) {
        auto* r = static_cast<Rec*>(ctx);
        r->enter();
        bool follow = true;
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->note_after_complete();
            r->seq.push_back('r');
            r->redirects.emplace_back(new_url ? new_url : "");
            follow = r->follow_redirect;
        }
        r->leave();
        return follow;
    }

    static void on_complete(void* ctx, syp_status st, int32_t http) {
        auto* r = static_cast<Rec*>(ctx);
        r->enter();
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->note_after_complete();
            r->seq.push_back('C');
            ++r->complete_n;
            r->complete_st = st;
            r->complete_http = http;
            r->cv.notify_all();
        }
        r->leave();
    }

    syp_response_sink sink() {
        return syp_response_sink{this, &on_response, &on_data, &on_redirect, &on_complete};
    }

    bool wait_complete(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return complete_n > 0; });
    }

    bool wait_body_at_least(size_t n, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return body.size() >= n || complete_n > 0; });
    }

    void check_serial_and_once() {
        CHECK_EQ(max_in_cb.load(std::memory_order_relaxed) <= 1, true);
        CHECK_EQ(complete_n, 1);
        CHECK_EQ(callbacks_after_complete, 0);
        // on_complete 是最后一个回调；若有响应/数据，顺序是 R...D...C
        if (!seq.empty()) {
            CHECK_EQ(seq.back(), 'C');
            const auto cpos = seq.find('C');
            CHECK(cpos == seq.size() - 1);
            const auto rpos = seq.find('R');
            const auto dpos = seq.find('D');
            if (rpos != std::string::npos && dpos != std::string::npos) {
                CHECK(rpos < dpos);
            }
            if (rpos != std::string::npos) {
                CHECK(rpos < cpos);
            }
        }
    }
};

struct TaskRec {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> bytes;
    int data_n = 0;
    int finished = 0;
    syp_status st = 1;
    int32_t http = -1;

    static void on_data(void* ctx, int64_t /*offset*/, const uint8_t* data, int32_t len) {
        auto* c = static_cast<TaskRec*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        ++c->data_n;
        if (data != nullptr && len > 0) {
            c->bytes.insert(c->bytes.end(), data, data + len);
        }
    }
    static void on_total(void* /*ctx*/, int64_t /*total*/) {}
    static void on_val(void* /*ctx*/, const char* /*etag*/, const char* /*lm*/) {}
    static void on_fin(void* ctx, syp_status st, int32_t http) {
        auto* c = static_cast<TaskRec*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        c->st = st;
        c->http = http;
        ++c->finished;
        c->cv.notify_all();
    }
    DLTaskCallbacks cbs() {
        return DLTaskCallbacks{this, &on_data, &on_total, &on_val, &on_fin};
    }
    bool wait_finished(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return finished > 0; });
    }
};

void fill_req(syp_http_request& req, const std::string& url,
              int64_t start, int64_t end,
              int32_t connect_ms = 5000, int32_t read_ms = 5000) {
    req = {};
    req.url = url.c_str();
    req.range_start = start;
    req.range_end = end;
    req.connect_timeout_ms = connect_ms;
    req.read_timeout_ms = read_ms;
}

void check_synthetic(const std::vector<uint8_t>& b, int64_t start) {
    for (size_t i = 0; i < b.size(); ++i) {
        CHECK_EQ(b[i], synthetic_byte(start + static_cast<int64_t>(i)));
    }
}

const syp_http_backend* backend() {
    const syp_http_backend* b = syp_apple_http_backend();
    if (b == nullptr || b->create == nullptr || b->start == nullptr
        || b->cancel == nullptr || b->destroy == nullptr) {
        std::abort();
    }
    return b;
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

TEST_CASE(full_download_matches_synthetic) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 1024, .support_range = true});
    REQUIRE(srv.port() != 0);
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_OK);
    CHECK_EQ(rec.complete_http, 0);
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{1024});
    check_synthetic(rec.body, 0);
    b->destroy(h);
}

TEST_CASE(range_download_matches_synthetic) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 2000, .support_range = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 100, 199);  // 100 bytes, inclusive
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_OK);
    CHECK_EQ(rec.http_status, 206);
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{100});
    check_synthetic(rec.body, 100);
    b->destroy(h);
}

// NSURLSession 默认自动带 Accept-Encoding: gzip 并透明解压，但上报的长度是压缩后的——
// 下载层据此记账会截断资源（twimg HLS 的 master 播放列表：声明 623、实为 1657 字节，
// FFmpeg 报 Invalid data）。后端必须显式要 identity，服务端才会给原始字节 + 原始 Range。
TEST_CASE(requests_identity_encoding_so_cdn_does_not_gzip) {
    constexpr int64_t kTotal = 5000;
    LoopbackServer srv(LoopbackConfig{.resource_length = kTotal, .support_range = true,
                                      .gzip_when_accepted = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 200, 299);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    CHECK_EQ(rec.complete_st, SYP_OK);
    CHECK_EQ(rec.http_status, 206);
    CHECK_EQ(rec.content_length, int64_t{100});
    CHECK_EQ(rec.total_length, kTotal);
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{100});
    check_synthetic(rec.body, 200);
    b->destroy(h);
}

// 服务端无视 identity 仍发 gzip：交付的是解压后的字节，Content-Length 描述的却是编码表示，
// 两者对不上——长度必须按未知上报，不能拿去记账。
TEST_CASE(gzip_response_reports_unknown_lengths) {
    constexpr int64_t kTotal = 3000;
    LoopbackServer srv(LoopbackConfig{.resource_length = kTotal, .support_range = true,
                                      .gzip_always = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    CHECK_EQ(rec.complete_st, SYP_OK);
    CHECK_EQ(rec.http_status, 206);   // 后端恒带 Range；服务端回编码表示的 Content-Range
    CHECK_EQ(rec.content_length, int64_t{-1});
    CHECK_EQ(rec.total_length, int64_t{-1});
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), kTotal);   // NSURLSession 已解压
    check_synthetic(rec.body, 0);
    b->destroy(h);
}

TEST_CASE(content_range_total_and_content_length) {
    constexpr int64_t kTotal = 5000;
    LoopbackServer srv(LoopbackConfig{.resource_length = kTotal, .support_range = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 200, 299);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_OK);
    CHECK_EQ(rec.http_status, 206);
    CHECK_EQ(rec.content_length, int64_t{100});  // 本次响应体
    CHECK_EQ(rec.total_length, kTotal);          // Content-Range 的 total
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{100});
    check_synthetic(rec.body, 200);
    b->destroy(h);
}

TEST_CASE(status_200_ignores_range_total_length_unknown) {
    constexpr int64_t kTotal = 256;
    LoopbackServer srv(LoopbackConfig{.resource_length = kTotal, .support_range = false});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 10, 19);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_OK);
    CHECK_EQ(rec.http_status, 200);
    CHECK_EQ(rec.content_length, kTotal);
    // 契约：total_length 从 Content-Range 解析；200 没有该头 → -1。
    CHECK_EQ(rec.total_length, int64_t{-1});
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), kTotal);
    check_synthetic(rec.body, 0);
    b->destroy(h);
}

TEST_CASE(redirect_follow_gets_body) {
    LoopbackServer dest(LoopbackConfig{.resource_length = 64, .support_range = true});
    LoopbackServer src(LoopbackConfig{.redirect_to = dest.url("/")});
    const std::string u = src.url("/");
    Rec rec;
    rec.follow_redirect = true;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_OK);
    CHECK(rec.redirects.size() >= 1);
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{64});
    check_synthetic(rec.body, 0);
    b->destroy(h);
}

TEST_CASE(redirect_reject_aborts) {
    LoopbackServer dest(LoopbackConfig{.resource_length = 64, .support_range = true});
    LoopbackServer src(LoopbackConfig{.redirect_to = dest.url("/")});
    const std::string u = src.url("/");
    Rec rec;
    rec.follow_redirect = false;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK_EQ(rec.complete_http, 0);
    CHECK(rec.redirects.size() >= 1);
    CHECK(rec.body.empty());
    sleep_ms(50);
    CHECK_EQ(rec.complete_n, 1);
    b->destroy(h);
}

TEST_CASE(http_4xx_status_passed_to_complete) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 8, .status_code = 404});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_HTTP_STATUS);
    CHECK_EQ(rec.complete_http, 404);
    CHECK_EQ(rec.http_status, 404);
    b->destroy(h);
}

TEST_CASE(http_5xx_status_passed_to_complete) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 8, .status_code = 503});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_HTTP_STATUS);
    CHECK_EQ(rec.complete_http, 503);
    CHECK_EQ(rec.http_status, 503);
    b->destroy(h);
}

TEST_CASE(http_error_status_nonempty_body_delivered_to_sink) {
    const std::string html = "<html><body>error page, not media</body></html>";
    LoopbackServer srv(LoopbackConfig{
        .resource_length = 8,
        .status_code = 404,
        .error_body = html,
    });
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_HTTP_STATUS);
    CHECK_EQ(rec.complete_http, 404);
    CHECK_EQ(std::string(rec.body.begin(), rec.body.end()), html);
    b->destroy(h);
}

TEST_CASE(dl_task_error_status_nonempty_body_not_delivered) {
    struct Case {
        int32_t    status_code;
        syp_status expect_st;
        int32_t    expect_http;
    };
    const Case cases[] = {
        {404, SYP_ERR_HTTP_STATUS,     404},
        {500, SYP_ERR_HTTP_STATUS,     500},
        {416, SYP_ERR_CONTENT_CHANGED,   0},
    };
    const std::string html = "<html><body>error page, not media</body></html>";
    for (const auto& tc : cases) {
        LoopbackServer srv(LoopbackConfig{
            .resource_length = 8,
            .status_code = tc.status_code,
            .error_body = html,
        });
        const std::string u = srv.url("/");
        TaskRec rec;
        DLTaskConfig cfg;
        cfg.connect_timeout_ms = 5000;
        cfg.read_timeout_ms = 5000;
        cfg.max_retries = 0;
        DLTask task(backend(), system_clock(), cfg, rec.cbs());
        task.start(u, nullptr, Range{0, 100});
        CHECK(rec.wait_finished(5000));
        CHECK_EQ(rec.finished, 1);
        CHECK_EQ(rec.st, tc.expect_st);
        CHECK_EQ(rec.http, tc.expect_http);
        CHECK_EQ(rec.bytes.size(), static_cast<size_t>(0));
        CHECK_EQ(rec.data_n, 0);
        CHECK_EQ(task.received_bytes(), int64_t{0});
    }
}

TEST_CASE(mid_stream_close_is_network_error_prefix_correct) {
    constexpr int64_t kTotal = 400;
    constexpr int64_t kCut   = 80;
    LoopbackServer srv(LoopbackConfig{.resource_length = kTotal,
                                      .support_range = true,
                                      .close_after_bytes = kCut});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_NETWORK);
    CHECK_EQ(rec.complete_http, 0);
    // 不能断言 body.size()==kCut。NSURLSession 不保证截断响应前先把缓冲
    // 经 didReceiveData 吐出：headers + 80 字节 + FIN 常落在同一 TCP 段，
    // 看到 Content-Length:400 对不上就直接 didCompleteWithError，一次
    // didReceiveData 都不发。规格是「已交付的前缀正确」，不是「必须交满」。
    const int64_t delivered = static_cast<int64_t>(rec.body.size());
    CHECK(delivered <= kCut);
    check_synthetic(rec.body, 0);
    b->destroy(h);
}

TEST_CASE(cancel_before_start_completes_once) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 128});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->cancel(h);
    CHECK(rec.wait_complete(2000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK_EQ(rec.complete_http, 0);
    CHECK(rec.body.empty());
    sleep_ms(80);
    CHECK_EQ(rec.complete_n, 1);
    CHECK_EQ(rec.callbacks_after_complete, 0);
    b->destroy(h);
}

TEST_CASE(cancel_while_connecting_completes_once) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 128, .hang = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1, /*connect*/ 5000, /*read*/ 5000);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    sleep_ms(20);
    b->cancel(h);
    CHECK(rec.wait_complete(2000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK_EQ(rec.complete_http, 0);
    sleep_ms(80);
    CHECK_EQ(rec.complete_n, 1);
    b->destroy(h);
}

TEST_CASE(cancel_while_receiving_completes_once) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 10000,
                                      .support_range = true,
                                      .pause_after_bytes = 64,
                                      .pause_ms = 5000,
                                      .body_chunk_bytes = 32});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_body_at_least(32, 3000));
    b->cancel(h);
    CHECK(rec.wait_complete(3000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK_EQ(rec.complete_http, 0);
    CHECK(!rec.body.empty());
    check_synthetic(rec.body, 0);
    sleep_ms(80);
    CHECK_EQ(rec.complete_n, 1);
    b->destroy(h);
}

TEST_CASE(cancel_twice_still_one_complete) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 10000, .hang = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    b->cancel(h);
    b->cancel(h);
    CHECK(rec.wait_complete(2000));
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    sleep_ms(80);
    CHECK_EQ(rec.complete_n, 1);
    b->destroy(h);
}

TEST_CASE(destroy_without_start_no_callbacks) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 32});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->destroy(h);
    sleep_ms(100);
    CHECK_EQ(rec.complete_n, 0);
    CHECK(rec.seq.empty());
    CHECK(rec.body.empty());
    CHECK_EQ(rec.max_in_cb.load(std::memory_order_relaxed), 0);
}

TEST_CASE(concurrent_requests_isolated) {
    constexpr int kN = 4;
    constexpr int64_t kLens[kN] = {80, 160, 240, 320};
    constexpr int64_t kStarts[kN] = {0, 10, 20, 30};
    LoopbackServer srv(LoopbackConfig{.resource_length = 1024, .support_range = true});
    const std::string u = srv.url("/");
    const syp_http_backend* b = backend();

    Rec recs[kN];
    syp_response_sink sinks[kN];
    syp_http_request reqs[kN];
    syp_http_request_handle* hs[kN]{};
    std::string url_copy = u;

    for (int i = 0; i < kN; ++i) {
        sinks[i] = recs[i].sink();
        fill_req(reqs[i], url_copy,
                 kStarts[i], kStarts[i] + kLens[i] - 1);
        hs[i] = b->create(b->backend_ctx, &reqs[i], &sinks[i]);
        REQUIRE(hs[i] != nullptr);
    }
    for (int i = 0; i < kN; ++i) b->start(hs[i]);
    for (int i = 0; i < kN; ++i) {
        CHECK(recs[i].wait_complete(5000));
        recs[i].check_serial_and_once();
        CHECK_EQ(recs[i].complete_st, SYP_OK);
        CHECK_EQ(static_cast<int64_t>(recs[i].body.size()), kLens[i]);
        check_synthetic(recs[i].body, kStarts[i]);
        b->destroy(hs[i]);
    }
}

TEST_CASE(timeout_on_hang) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 8, .hang = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1, /*connect*/ 300, /*read*/ 300);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    b->start(h);
    CHECK(rec.wait_complete(2000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    rec.check_serial_and_once();
    CHECK_EQ(rec.complete_st, SYP_ERR_TIMEOUT);
    CHECK_EQ(rec.complete_http, 0);
    CHECK(ms >= 200);   // 不应瞬间失败
    CHECK(ms < 1500);   // 也不该拖到秒级以上
    b->destroy(h);
}

TEST_CASE(loopback_serves_file_body_and_etag) {
    // 路径带上 pid，避免并行跑测试（多 worktree / 多分支）时撞同一个文件名。
    // RAII 兜底删除：任何一条 REQUIRE 提前 return 都不留残留。
    struct TempFileRemover {
        std::string path;
        ~TempFileRemover() { if (!path.empty()) std::remove(path.c_str()); }
    } temp_file{"/tmp/syp_loopback_body_test_" + std::to_string(::getpid()) + ".bin"};
    const std::string& path = temp_file.path;

    // 造一个内容确定的临时文件：第 i 个字节 = i & 0xFF
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        for (int i = 0; i < 10000; ++i) {
            const unsigned char b = static_cast<unsigned char>(i & 0xFF);
            std::fwrite(&b, 1, 1, f);
        }
        std::fclose(f);
    }

    LoopbackConfig cfg;
    cfg.body_file = path;
    cfg.etag = "\"abc123\"";
    LoopbackServer srv(cfg);
    REQUIRE(srv.port() != 0);

    const std::string u = srv.url("/");

    // 全量 GET：长度取自文件，内容与文件一致，响应头带 ETag
    {
        Rec rec;
        auto sink = rec.sink();
        syp_http_request req;
        fill_req(req, u, 0, -1);
        const syp_http_backend* b = backend();
        auto* h = b->create(b->backend_ctx, &req, &sink);
        REQUIRE(h != nullptr);
        b->start(h);
        CHECK(rec.wait_complete(5000));
        CHECK_EQ(rec.complete_st, SYP_OK);
        CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{10000});
        CHECK_EQ(rec.header("ETag"), std::string("\"abc123\""));
        for (size_t i = 0; i < rec.body.size(); ++i) {
            REQUIRE(rec.body[i] == static_cast<uint8_t>(i & 0xFF));
        }
        b->destroy(h);
    }

    // Range GET：区间内容对得上
    {
        Rec rec;
        auto sink = rec.sink();
        syp_http_request req;
        fill_req(req, u, 100, 199);
        const syp_http_backend* b = backend();
        auto* h = b->create(b->backend_ctx, &req, &sink);
        REQUIRE(h != nullptr);
        b->start(h);
        CHECK(rec.wait_complete(5000));
        CHECK_EQ(rec.http_status, 206);
        CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{100});
        for (size_t i = 0; i < rec.body.size(); ++i) {
            REQUIRE(rec.body[i] == static_cast<uint8_t>((100 + i) & 0xFF));
        }
        b->destroy(h);
    }
}

TEST_CASE(loopback_synthetic_path_unchanged_when_no_body_file) {
    // 既有 18 个用例都走合成公式路径，这条守住它没被改坏。
    LoopbackServer srv(LoopbackConfig{.resource_length = 4096, .support_range = true});
    const std::string u = srv.url("/");
    Rec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    b->start(h);
    CHECK(rec.wait_complete(5000));
    CHECK_EQ(static_cast<int64_t>(rec.body.size()), int64_t{4096});
    CHECK_EQ(rec.header("ETag"), std::string{});   // 未配置就不发
    check_synthetic(rec.body, 0);
    b->destroy(h);
}

// ---------------------------------------------------------------- 回调内 destroy
//
// ⚠️ 顺序约束：下面这四条用例必须留在本文件的最后，新用例一律加在它们之前。
// 它们一旦失败（`if (!returned) { dump(); return; }`），会永久遗留阻塞的
// delegate 线程——自等的那条，以及可能堵在同一 Handle 的 emit_mu_ 上的
// didCompleteWithError 那条。这些线程占着全进程共享的那个 NSURLSession 的
// 并发 NSOperationQueue 名额，排在它们之后的用例会被饿死。
// 四条排在最后不是巧合，是这个约束的落实。

// 回归：cancel 落在某个回调的执行窗口内 → 该回调的收尾在同一条 delegate 栈上
// 合成 on_complete(SYP_ERR_CANCELED) → sink 在这个 on_complete 里调 destroy。
//
// 契约允许在 on_complete 内 destroy —— dl 层的 DLTask::sink_on_complete 正是
// 这么做的（拿走句柄后同步 backend_->destroy）。后端为此准备了
// 「同线程回调内 destroy 就跳过等待」的逃生口，判据是 in_callback_。
// 但 emit_data / emit_response / emit_redirect 三处的取消收尾曾在调
// on_complete 之前一行就把 in_callback_ 清成 false，逃生口没有武装：
// destroy() 于是去等自己这条 delegate 帧持有的 inflight_ —— 单线程自死锁，永久挂死。
//
// 交错完全由代码结构钉死，不需要注入钩子、不靠时序：cancel 与 destroy 都由
// sink 自己在后端的回调线程上发出，命中率 100%。
namespace {

// 看门狗已抽到 tests/support/watchdog.h（test_scheduler.cpp 的 dl 层级重入
// 回归用例要用同一套安全网）。这里只留本套件专属的软超时提示文案。
using syp::test::Watchdog;

constexpr const char* kSelfDestroyHint =
    "看下面 dump 的 complete_n 分辨：complete_n=1 ⇒ 卡在 destroy()，它在 "
    "on_complete 的回调栈上等自己那一帧持有的 inflight_（逃生口的两条判据"
    "——本线程是否持有本 handle 的 inflight、in_callback_ 记账——都没武装）；"
    "complete_n=0 ⇒ 更早，卡在回调内 cancel() 合成 on_complete 时同线程重入 emit_mu_。";

// cancel（或拒绝重定向）的落点，对应后端三条取消合成收尾。
enum class CancelAt { kOnResponse, kOnData, kOnRedirect };

struct SelfDestroyRec {
    const syp_http_backend*  b  = nullptr;
    syp_http_request_handle* h  = nullptr;
    CancelAt                 at = CancelAt::kOnData;

    std::mutex              mu;
    std::condition_variable cv;
    bool                    destroy_returned = false;

    std::atomic<int> response_n{0};
    std::atomic<int> data_n{0};
    std::atomic<int> redirect_n{0};
    std::atomic<int> complete_n{0};
    std::atomic<int> cancel_n{0};

    syp_status      complete_st   = SYP_OK;
    int32_t         complete_http = -1;
    std::thread::id cancel_thread{};
    std::thread::id complete_thread{};

    // 在回调线程上取消，只取消一次。
    void cancel_once() {
        if (cancel_n.fetch_add(1, std::memory_order_acq_rel) == 0) {
            cancel_thread = std::this_thread::get_id();
            b->cancel(h);
        }
    }

    static void on_response(void* ctx, int32_t /*http_status*/,
                            const syp_headers* /*headers*/,
                            int64_t /*content_length*/, int64_t /*total_length*/) {
        auto* r = static_cast<SelfDestroyRec*>(ctx);
        r->response_n.fetch_add(1, std::memory_order_acq_rel);
        if (r->at == CancelAt::kOnResponse) r->cancel_once();
    }

    static void on_data(void* ctx, const uint8_t* /*data*/, int32_t /*len*/) {
        auto* r = static_cast<SelfDestroyRec*>(ctx);
        r->data_n.fetch_add(1, std::memory_order_acq_rel);
        if (r->at == CancelAt::kOnData) r->cancel_once();
    }

    static bool on_redirect(void* ctx, const char* /*new_url*/) {
        auto* r = static_cast<SelfDestroyRec*>(ctx);
        r->redirect_n.fetch_add(1, std::memory_order_acq_rel);
        if (r->at != CancelAt::kOnRedirect) return true;
        r->cancel_thread = std::this_thread::get_id();
        return false;   // 拒绝重定向 → emit_redirect 收尾合成 on_complete
    }

    static void on_complete(void* ctx, syp_status st, int32_t http) {
        auto* r = static_cast<SelfDestroyRec*>(ctx);
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->complete_st     = st;
            r->complete_http   = http;
            r->complete_thread = std::this_thread::get_id();
        }
        r->complete_n.fetch_add(1, std::memory_order_acq_rel);
        // 契约允许：在 on_complete 内、由后端自己的回调线程销毁句柄。
        r->b->destroy(r->h);
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->destroy_returned = true;
        }
        r->cv.notify_all();
    }

    syp_response_sink sink() {
        return syp_response_sink{this, &on_response, &on_data, &on_redirect, &on_complete};
    }

    bool wait_destroy_returned(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return destroy_returned; });
    }

    void dump(const char* name) {
        std::fprintf(stdout,
                     "  诊断 %s: destroy() 没有从 on_complete 回调里返回。"
                     "response_n=%d data_n=%d redirect_n=%d complete_n=%d cancel_n=%d\n",
                     name,
                     response_n.load(std::memory_order_relaxed),
                     data_n.load(std::memory_order_relaxed),
                     redirect_n.load(std::memory_order_relaxed),
                     complete_n.load(std::memory_order_relaxed),
                     cancel_n.load(std::memory_order_relaxed));
        std::fflush(stdout);
    }
};

// 软超时只打诊断、不做断言：它是一个墙钟阈值，天生非确定性，
// 一次「正确但慢」的运行（40 路并行叠加回环 server 启动抖动）会把它撞响。
// 本项目最忌讳假红——用例的判据是确定性的 CHECK(returned)，
// 看门狗的职责只是「挂死时让日志可读」，不是计时断言。
// 等待阈值留出足够余量（软 4s 先打诊断，主线程 8s 才判失败）。
constexpr int kSelfDestroySoftMs = 4000;
constexpr int kSelfDestroyWaitMs = 8000;
constexpr int kSelfDestroyHardMs = 30000;

}  // namespace

TEST_CASE(destroy_in_on_complete_after_cancel_in_on_data) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 10000,
                                      .support_range = true,
                                      .pause_after_bytes = 64,
                                      .pause_ms = 5000,
                                      .body_chunk_bytes = 32});
    const std::string u = srv.url("/");
    SelfDestroyRec rec;
    rec.at = CancelAt::kOnData;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    rec.b = b;
    rec.h = h;

    Watchdog wd("destroy_in_on_complete_after_cancel_in_on_data",
                kSelfDestroySoftMs, kSelfDestroyHardMs, kSelfDestroyHint);
    b->start(h);
    const bool returned = rec.wait_destroy_returned(kSelfDestroyWaitMs);
    CHECK(returned);
    if (returned && wd.fired()) {   // 只在真的成功但慢时才提示；真失败不该说“不算失败”
        std::fprintf(stdout, "  注意：本用例慢于软超时才完成（不算失败）。\n");
        std::fflush(stdout);
    }
    if (!returned) {
        rec.dump("destroy_in_on_complete_after_cancel_in_on_data");
        return;   // 句柄已卡死在后端，不再触碰
    }
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK_EQ(rec.complete_http, 0);
    CHECK(rec.data_n.load(std::memory_order_relaxed) >= 1);
    // 真的是重入路径：on_complete 与触发它的 on_data 在同一条后端回调线程上。
    CHECK(rec.complete_thread == rec.cancel_thread);
    sleep_ms(80);
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
}

TEST_CASE(destroy_in_on_complete_after_cancel_in_on_response) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 10000,
                                      .support_range = true,
                                      .pause_after_bytes = 64,
                                      .pause_ms = 5000,
                                      .body_chunk_bytes = 32});
    const std::string u = srv.url("/");
    SelfDestroyRec rec;
    rec.at = CancelAt::kOnResponse;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    rec.b = b;
    rec.h = h;

    Watchdog wd("destroy_in_on_complete_after_cancel_in_on_response",
                kSelfDestroySoftMs, kSelfDestroyHardMs, kSelfDestroyHint);
    b->start(h);
    const bool returned = rec.wait_destroy_returned(kSelfDestroyWaitMs);
    CHECK(returned);
    if (returned && wd.fired()) {   // 只在真的成功但慢时才提示；真失败不该说“不算失败”
        std::fprintf(stdout, "  注意：本用例慢于软超时才完成（不算失败）。\n");
        std::fflush(stdout);
    }
    if (!returned) {
        rec.dump("destroy_in_on_complete_after_cancel_in_on_response");
        return;
    }
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK_EQ(rec.response_n.load(std::memory_order_relaxed), 1);
    CHECK(rec.complete_thread == rec.cancel_thread);
    sleep_ms(80);
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
}

TEST_CASE(destroy_in_on_complete_after_reject_redirect) {
    LoopbackServer dest(LoopbackConfig{.resource_length = 256});
    LoopbackServer src(LoopbackConfig{.redirect_to = dest.url("/")});
    const std::string u = src.url("/");
    SelfDestroyRec rec;
    rec.at = CancelAt::kOnRedirect;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    rec.b = b;
    rec.h = h;

    Watchdog wd("destroy_in_on_complete_after_reject_redirect",
                kSelfDestroySoftMs, kSelfDestroyHardMs, kSelfDestroyHint);
    b->start(h);
    const bool returned = rec.wait_destroy_returned(kSelfDestroyWaitMs);
    CHECK(returned);
    if (returned && wd.fired()) {   // 只在真的成功但慢时才提示；真失败不该说“不算失败”
        std::fprintf(stdout, "  注意：本用例慢于软超时才完成（不算失败）。\n");
        std::fflush(stdout);
    }
    if (!returned) {
        rec.dump("destroy_in_on_complete_after_reject_redirect");
        return;
    }
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK(rec.redirect_n.load(std::memory_order_relaxed) >= 1);
    CHECK_EQ(rec.data_n.load(std::memory_order_relaxed), 0);
    CHECK(rec.complete_thread == rec.cancel_thread);
    sleep_ms(80);
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
}

// ---------------------------------------------------------- 跨线程版本（思路 2）
//
// 上面三条用例的 cancel 全部来自后端自己的回调线程。生产里真正的形状是
// ~DLTask::cancel()（即"流 Z"那种交错）从**另一条线程**落在 emit_data 的
// 执行窗口内。修复对两种形状
// 同样有效——destroy() 的 same_thread 逃生口判据看的是 destroy **调用者**
// 所在的线程/帧，与 cancel 是谁、从哪条线程发起的无关——所以这不是另一
// 个正确性缺口，是矩阵缺口：本用例把交错用闩锁钉死，不靠 sleep 撞。
//
// 交错（两个闩锁排定，无时序赌博）：
//   测试主线程（扮演 ~DLTask::cancel() 的调用者）      delegate 线程 D1
//   ----------------------------------------------      ------------------------
//                                                        emit_data：进 on_data
//                                                        第一次调用：
//                                                          entered.set()
//                                                          等 release
//   entered.wait()
//   b->cancel(h)   // 跨线程！canceled_ 置位
//   release.set()
//                                                        release 到达，on_data 返回
//                                                        finish_with_cancel_emit_locked
//                                                        见 canceled_==true，仍在 D1
//                                                        上合成 on_complete → destroy
namespace {

struct CrossThreadRec {
    const syp_http_backend*  b  = nullptr;
    syp_http_request_handle* h  = nullptr;

    std::mutex              mu;
    std::condition_variable cv;
    bool                    entered          = false;
    bool                    release          = false;
    bool                    destroy_returned = false;

    std::atomic<int> data_n{0};
    std::atomic<int> complete_n{0};

    syp_status      complete_st   = SYP_OK;
    int32_t         complete_http = -1;
    // on_data / on_complete 实际落地的线程（应为同一条 delegate 线程）。
    std::thread::id delegate_thread{};
    // 调 cancel() 的线程——测试主线程，必须与 delegate_thread 不同，
    // 否则这条用例退化成了思路 1，证明不了跨线程那格。
    std::thread::id canceller_thread{};

    static void on_response(void* /*ctx*/, int32_t /*http_status*/,
                            const syp_headers* /*headers*/,
                            int64_t /*content_length*/, int64_t /*total_length*/) {}

    // 只在第一次调用时阻塞：等测试主线程确认已进入窗口、发起跨线程
    // cancel、再放行，交错由闩锁钉死。
    static void on_data(void* ctx, const uint8_t* /*data*/, int32_t /*len*/) {
        auto* r = static_cast<CrossThreadRec*>(ctx);
        if (r->data_n.fetch_add(1, std::memory_order_acq_rel) != 0) return;
        r->delegate_thread = std::this_thread::get_id();
        std::unique_lock<std::mutex> lk(r->mu);
        r->entered = true;
        r->cv.notify_all();
        r->cv.wait(lk, [r] { return r->release; });
    }

    static bool on_redirect(void* /*ctx*/, const char* /*new_url*/) { return true; }

    static void on_complete(void* ctx, syp_status st, int32_t http) {
        auto* r = static_cast<CrossThreadRec*>(ctx);
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->complete_st   = st;
            r->complete_http = http;
        }
        r->complete_n.fetch_add(1, std::memory_order_acq_rel);
        // 契约允许：在 on_complete 内、由后端自己的回调线程销毁句柄。
        // destroy() 的逃生口只看这条线程是不是它自己，与 cancel 是谁在
        // 另一条线程上发起的无关——这正是本用例要证明的那句话。
        r->b->destroy(r->h);
        {
            std::lock_guard<std::mutex> g(r->mu);
            r->destroy_returned = true;
        }
        r->cv.notify_all();
    }

    syp_response_sink sink() {
        return syp_response_sink{this, &on_response, &on_data, &on_redirect, &on_complete};
    }

    bool wait_entered(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [this] { return entered; });
    }

    // 必须从测试主线程调用：捕获调用者线程 id，再跨线程 cancel，最后放行
    // 卡在 on_data 里的 delegate 线程。
    void cancel_from_this_thread_then_release() {
        canceller_thread = std::this_thread::get_id();
        b->cancel(h);
        std::lock_guard<std::mutex> g(mu);
        release = true;
        cv.notify_all();
    }

    bool wait_destroy_returned(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [this] { return destroy_returned; });
    }

    void dump(const char* name) {
        std::fprintf(stdout,
                     "  诊断 %s: 卡住了。data_n=%d complete_n=%d entered=%d release=%d\n",
                     name, data_n.load(std::memory_order_relaxed),
                     complete_n.load(std::memory_order_relaxed),
                     static_cast<int>(entered), static_cast<int>(release));
        std::fflush(stdout);
    }
};

}  // namespace

TEST_CASE(destroy_in_on_complete_after_cross_thread_cancel_in_on_data) {
    LoopbackServer srv(LoopbackConfig{.resource_length = 10000,
                                      .support_range = true,
                                      .pause_after_bytes = 64,
                                      .pause_ms = 5000,
                                      .body_chunk_bytes = 32});
    const std::string u = srv.url("/");
    CrossThreadRec rec;
    auto sink = rec.sink();
    syp_http_request req;
    fill_req(req, u, 0, -1);
    const syp_http_backend* b = backend();
    auto* h = b->create(b->backend_ctx, &req, &sink);
    REQUIRE(h != nullptr);
    rec.b = b;
    rec.h = h;

    Watchdog wd("destroy_in_on_complete_after_cross_thread_cancel_in_on_data",
                kSelfDestroySoftMs, kSelfDestroyHardMs, kSelfDestroyHint);
    b->start(h);

    // 先确认 delegate 线程真的卡在 on_data 的执行窗口里，再从测试主线程
    // （与 delegate 线程不同）跨线程发起 cancel——否则后面的断言证明不了
    // "跨线程"这件事。
    const bool entered = rec.wait_entered(kSelfDestroyWaitMs);
    CHECK(entered);
    if (!entered) {
        rec.dump("destroy_in_on_complete_after_cross_thread_cancel_in_on_data");
        return;   // delegate 线程卡在 wait 上，句柄不再触碰
    }
    rec.cancel_from_this_thread_then_release();

    const bool returned = rec.wait_destroy_returned(kSelfDestroyWaitMs);
    CHECK(returned);
    if (returned && wd.fired()) {   // 只在真的成功但慢时才提示；真失败不该说“不算失败”
        std::fprintf(stdout, "  注意：本用例慢于软超时才完成（不算失败）。\n");
        std::fflush(stdout);
    }
    if (!returned) {
        rec.dump("destroy_in_on_complete_after_cross_thread_cancel_in_on_data");
        return;
    }
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
    CHECK_EQ(rec.complete_st, SYP_ERR_CANCELED);
    CHECK_EQ(rec.complete_http, 0);
    // 真的是跨线程：发起 cancel 的线程与 delegate（on_data/on_complete
    // 落地）线程不是同一条——否则这条用例退化成了思路 1。
    CHECK(rec.canceller_thread != rec.delegate_thread);
    sleep_ms(80);
    CHECK_EQ(rec.complete_n.load(std::memory_order_relaxed), 1);
}

int main() { return tiny_test_main(); }
