// test_source_bridge.cpp — syp_source 闭环：阻塞读 / seek / 续下 / interrupt，内存桩
#include "tiny_test.h"
#include "support/loopback_server.h"
#include "support/stub_backend.h"
#include "support/watchdog.h"

#include <dl/cache_index.h>
#include <dl/cache_store.h>
#include <dl/clock.h>
#include <dl/hole_set.h>
#include <dl/source_bridge.h>

#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>
#include <syplayer/syp_source.h>
#include <syplayer/syp_types.h>

#if defined(__APPLE__)
#include <dl/preconnector.h>
#include <platform/apple/apple_http_backend.h>
#include <syplayer/syp_net.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using syp::dl::Clock;
using syp::dl::HoleSet;
using syp::dl::Range;
using syp::dl::SourceBridge;
using syp::dl::test::StubBackend;
using syp::dl::test::synthetic_byte;

namespace {

// t 是原子量：SourceBridge 的 Scheduler 用默认 health 配置（进程单例
// HealthTicker，真实线程），health_check 会在 ticker 线程上读本时钟，与
// 测试线程的 advance() 并发。relaxed 足够：只要求
// 无数据竞争，不靠它给别的内存建立先后。
struct FakeClock {
    std::atomic<int64_t> t{0};
    static int64_t now(void* ctx) {
        return static_cast<FakeClock*>(ctx)->t.load(std::memory_order_relaxed);
    }
    Clock clock() { return Clock{&now, this}; }
    void advance(int64_t ms) { t.fetch_add(ms, std::memory_order_relaxed); }
};

struct TempDir {
    std::filesystem::path p;
    TempDir() {
        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed);
        p = base / ("syp-src-" + std::to_string(::getpid()) + "-"
                    + std::to_string(n));
        std::filesystem::create_directories(p, ec);
    }
    ~TempDir() {
        std::error_code ec;
        if (!p.empty()) std::filesystem::remove_all(p, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
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

void check_stub(const StubBackend& s) {
    std::string msg;
    if (!s.contract_ok(&msg)) {
        tiny_test::fail(__FILE__, __LINE__, "stub.contract_ok()", msg);
    }
}

StubBackend::Script script_ok(int64_t n) {
    StubBackend::Script s;
    s.resource_length = n;
    s.support_range   = true;
    s.http_status     = 206;
    s.etag            = "\"e1\"";
    s.last_modified   = "Wed, 01 Jan 2020 00:00:00 GMT";
    s.chunk_size      = 32;
    return s;
}

void cfg_small(syp_config* c, const char* dir) {
    syp_config_init(c);
    c->cache_dir            = dir;
    c->max_concurrent_tasks = 1;
    c->min_segment_size     = 64;
    c->segment_size_hint    = 64;
    c->max_retries          = 1;
    c->allow_no_range_fallback = true;
}

struct Cbs {
    mutable std::mutex mu;
    std::condition_variable cv;
    bool buffering = false;
    int  buf_on_n  = 0;
    int  err_n     = 0;
    int  total_n   = 0;
    int  ranges_n  = 0;
    syp_status last_err = SYP_OK;
    int32_t    last_http = 0;
    int64_t    total = -1;
    std::vector<syp_range> ranges;

    static void on_buffering(void* ctx, bool on) {
        auto* c = static_cast<Cbs*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        c->buffering = on;
        if (on) ++c->buf_on_n;
        c->cv.notify_all();
    }
    static void on_speed(void* /*ctx*/, int64_t /*bps*/) {}
    static void on_ranges(void* ctx, const syp_range* rs, int32_t n) {
        auto* c = static_cast<Cbs*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        ++c->ranges_n;
        c->ranges.clear();
        if (rs != nullptr && n > 0) {
            c->ranges.assign(rs, rs + n);
        }
        c->cv.notify_all();
    }
    static void on_total(void* ctx, int64_t total) {
        auto* c = static_cast<Cbs*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        c->total = total;
        ++c->total_n;
        c->cv.notify_all();
    }
    static void on_error(void* ctx, syp_status st, int32_t http) {
        auto* c = static_cast<Cbs*>(ctx);
        std::lock_guard<std::mutex> g(c->mu);
        c->last_err = st;
        c->last_http = http;
        ++c->err_n;
        c->cv.notify_all();
    }

    syp_source_callbacks view() {
        syp_source_callbacks cb{};
        cb.ctx = this;
        cb.on_buffering = &on_buffering;
        cb.on_speed = &on_speed;
        cb.on_cached_ranges = &on_ranges;
        cb.on_total_length = &on_total;
        cb.on_error = &on_error;
        return cb;
    }

    template<typename P>
    bool wait_pred(P p, int timeout_ms = 8000) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), std::move(p));
    }
};

struct Env {
    TempDir     td;
    std::string dir;
    StubBackend stub;
    syp_config  cfg{};
    Cbs         cbs;

    explicit Env(StubBackend::Mode mode = StubBackend::Mode::Async) {
        dir = td.p.string();
        cfg_small(&cfg, dir.c_str());
        stub.set_mode(mode);
        syp_set_http_backend(stub.backend());
    }
    ~Env() { syp_set_http_backend(nullptr); }
    Env(const Env&)            = delete;
    Env& operator=(const Env&) = delete;
};

// 本文件里 Env 默认走 StubBackend::Mode::Async（真实 worker 线程投递
// on_data/on_complete），少数用例另外还自己起 reader/pumper 线程做阻塞读——
// 两类都属于"走真实线程"的用例，都有真实的挂死风险（worker 线程在自己的
// 回调栈上重入 destroy 曾导致整套零输出挂到 ctest TIMEOUT）。照
// test_apple_http_backend.cpp / test_scheduler.cpp 那套配上看门狗：软超时只
// 打诊断（按用例名区分，定位挂在哪条），硬超时兜底 _exit，不做断言——墙钟
// 阈值天生非确定性，判定仍然交给用例本体的确定性 CHECK。
//
// 硬超时取 30000ms（与 test_apple_http_backend.cpp 同一个数）：本文件目标
// ctest TIMEOUT 是 60s，30000 留出的余量与 apple 套件已验证过的取值一致。
constexpr int kAsyncWatchdogSoftMs = 4000;
constexpr int kAsyncWatchdogHardMs = 30000;
constexpr const char* kAsyncWatchdogHint =
    "本用例走真实 HTTP 后端 worker 线程和/或阻塞读用的旁路线程。卡住通常"
    "意味着 syp_source_read/close/interrupt 与后台线程之间出现了死锁，"
    "而不是下载慢。";

std::vector<uint8_t> read_all(syp_source* s, int32_t chunk = 80) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> buf(static_cast<size_t>(chunk));
    for (;;) {
        const int32_t n = syp_source_read(s, buf.data(), chunk);
        if (n == SYP_ERR_EOF) break;
        if (n < 0) {
            tiny_test::fail(__FILE__, __LINE__, "syp_source_read < 0 before eof",
                            "st=" + std::to_string(n));
            break;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    return out;
}

bool bytes_match(const std::vector<uint8_t>& got, int64_t start) {
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i] != synthetic_byte(start + static_cast<int64_t>(i))) return false;
    }
    return true;
}

std::vector<Range> snapshot_ranges(syp_source* s) {
    const int32_t n = syp_source_cached_ranges(s, nullptr, 0);
    std::vector<syp_range> raw(static_cast<size_t>(n > 0 ? n : 0));
    std::vector<Range> out;
    if (n <= 0) return out;
    const int32_t w = syp_source_cached_ranges(s, raw.data(), n);
    out.reserve(static_cast<size_t>(w));
    for (int32_t i = 0; i < w; ++i) {
        out.push_back(Range{raw[static_cast<size_t>(i)].start,
                            raw[static_cast<size_t>(i)].end});
    }
    return out;
}

// 读 [from, to) 并确认内容正确；返回时这段一定已经进了索引（read 只在
// index->ranges().contiguous_from(pos) > 0 时才返回字节）。全程阻塞读，
// 不 sleep：不需要任何墙钟等待就能断言"这段已经落盘且记进了索引"。
void read_exact_range(syp_source* s, int64_t from, int64_t to) {
    REQUIRE(syp_source_seek(s, from, SYP_SEEK_SET) == from);
    std::vector<uint8_t> got;
    std::vector<uint8_t> buf(256);
    while (static_cast<int64_t>(got.size()) < to - from) {
        const int64_t want = to - from - static_cast<int64_t>(got.size());
        const int32_t chunk = want > 256 ? 256 : static_cast<int32_t>(want);
        const int32_t n = syp_source_read(s, buf.data(), chunk);
        REQUIRE(n > 0);
        got.insert(got.end(), buf.begin(), buf.begin() + n);
    }
    CHECK(bytes_match(got, from));
}

bool request_overlaps_cached(const StubBackend& stub, int64_t eof,
                             const std::vector<Range>& cached, int from_req) {
    for (int i = from_req; i < stub.request_count(); ++i) {
        const Range q = req_range(stub.request_at(i), eof);
        for (const Range& c : cached) {
            if (ranges_overlap(q, c)) return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE(sequential_read_matches_synthetic) {
    syp::test::Watchdog wd("sequential_read_matches_synthetic", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 1024;
    Env e;
    e.stub.set_default_script(script_ok(N));
    auto cb = e.cbs.view();
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
    REQUIRE(s != nullptr);

    const auto got = read_all(s);
    CHECK_EQ(got.size(), static_cast<size_t>(N));
    CHECK(bytes_match(got, 0));
    CHECK_EQ(syp_source_length(s), N);
    CHECK_EQ(syp_source_read(s, nullptr, 8), SYP_ERR_INVALID_ARG);

    syp_source_stats st{};
    syp_source_get_stats(s, &st);
    CHECK(st.downloaded_bytes >= N);
    CHECK(st.cached_bytes >= N);

    syp_source_close(s);
    check_stub(e.stub);
}

TEST_CASE(seek_uncached_then_read_correct) {
    syp::test::Watchdog wd("seek_uncached_then_read_correct", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 2048;
    Env e;
    e.stub.set_default_script(script_ok(N));
    auto cb = e.cbs.view();
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);

    uint8_t prefix[100];
    int32_t n = 0;
    int32_t got_n = 0;
    while (got_n < 100) {
        n = syp_source_read(s, prefix + got_n, 100 - got_n);
        REQUIRE(n > 0);
        got_n += n;
    }
    for (int32_t i = 0; i < 100; ++i) {
        CHECK_EQ(prefix[static_cast<size_t>(i)], synthetic_byte(i));
    }

    const int64_t np = syp_source_seek(s, 1500, SYP_SEEK_SET);
    CHECK_EQ(np, int64_t{1500});

    std::vector<uint8_t> tail;
    uint8_t buf[64];
    for (;;) {
        n = syp_source_read(s, buf, 64);
        if (n == SYP_ERR_EOF) break;
        REQUIRE(n > 0);
        tail.insert(tail.end(), buf, buf + n);
    }
    CHECK_EQ(tail.size(), static_cast<size_t>(N - 1500));
    CHECK(bytes_match(tail, 1500));

    const int64_t cur = syp_source_seek(s, 0, SYP_SEEK_CUR);
    CHECK_EQ(cur, N);
    const int64_t end = syp_source_seek(s, -8, SYP_SEEK_END);
    CHECK_EQ(end, N - 8);

    syp_source_close(s);
    check_stub(e.stub);
}

TEST_CASE(seek_does_not_redownload_cached) {
    syp::test::Watchdog wd("seek_does_not_redownload_cached", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 2048;
    Env e(StubBackend::Mode::Async);
    e.stub.set_default_script(script_ok(N));
    auto cb = e.cbs.view();
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);

    uint8_t tmp[128];
    int32_t filled = 0;
    while (filled < 128) {
        const int32_t n = syp_source_read(s, tmp + filled, 128 - filled);
        REQUIRE(n > 0);
        filled += n;
    }
    const auto cached = snapshot_ranges(s);
    REQUIRE(!cached.empty());
    const int nreq = e.stub.request_count();

    const int64_t hole = 1600;
    CHECK_EQ(syp_source_seek(s, hole, SYP_SEEK_SET), hole);
    uint8_t one[16];
    const int32_t n = syp_source_read(s, one, 16);
    REQUIRE(n > 0);
    for (int32_t i = 0; i < n; ++i) {
        CHECK_EQ(one[static_cast<size_t>(i)], synthetic_byte(hole + i));
    }

    CHECK(!request_overlaps_cached(e.stub, N, cached, nreq));
    CHECK(e.stub.request_count() > nreq);

    syp_source_close(s);
    check_stub(e.stub);
}

TEST_CASE(restart_resumes_holes_only) {
    syp::test::Watchdog wd("restart_resumes_holes_only", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 2048;
    TempDir td;
    const std::string dir = td.p.string();
    std::vector<Range> cached;
    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        e.stub.set_default_script(script_ok(N));
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
        uint8_t tmp[96];
        int32_t filled = 0;
        while (filled < 96) {
            const int32_t n = syp_source_read(s, tmp + filled, 96 - filled);
            REQUIRE(n > 0);
            filled += n;
        }
        cached = snapshot_ranges(s);
        REQUIRE(!cached.empty());
        int64_t have = 0;
        for (const Range& r : cached) have += r.size();
        CHECK(have < N);
        syp_source_close(s);
        check_stub(e.stub);
    }

    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        e.stub.set_default_script(script_ok(N));
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);

        const auto got = read_all(s);
        CHECK_EQ(got.size(), static_cast<size_t>(N));
        CHECK(bytes_match(got, 0));
        CHECK(!request_overlaps_cached(e.stub, N, cached, 0));

        syp_source_stats st{};
        syp_source_get_stats(s, &st);
        CHECK(st.cache_hit_bytes >= 96);

        syp_source_close(s);
        check_stub(e.stub);
    }
}

// 【回归用例 —— 这条红过】
//
// 修复前：两个 syp_source 打开同一个 URL，各自 open 一份 CacheIndex。
// A 读 [0,1024)、B 读 [3072,4096)，两边 close() 时 save() 都是
// tmp + rename 整份替换 .idx —— 后 close 的那个把先 close 的区间表整份
// 覆盖掉。磁盘上 8KiB 的字节全在，索引却只认得其中 1KiB，第三次打开
// 会把另一段当洞重新下载。
//
// 【如何验证这条用例真的在测】把 SourceBridge::open() 里的 CacheStore::acquire() 换回
// "每实例自己 CacheIndex::open + CacheFile::open"，本用例必须变红
// （最后那个 CHECK 会看到 C 的索引只剩一段）。验证完记得改回来。
TEST_CASE(two_sources_same_url_share_cached_ranges) {
    syp::test::Watchdog wd("two_sources_same_url_share_cached_ranges",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 8192;
    Env e;
    e.stub.set_default_script(script_ok(N));
    auto cbA = e.cbs.view();

    syp_source* a = nullptr;
    REQUIRE(syp_source_open(&a, "http://x/shared", nullptr, &e.cfg, &cbA) == SYP_OK);
    REQUIRE(a != nullptr);
    read_exact_range(a, 0, 1024);

    // B 后开：必须**立刻**看得见 A 已经下好的区间（共享同一份 CacheIndex），
    // 这是"预加载与播放互相看得见"的最小可验证形态。
    Cbs cbsB;
    auto cbB = cbsB.view();
    syp_source* b = nullptr;
    REQUIRE(syp_source_open(&b, "http://x/shared", nullptr, &e.cfg, &cbB) == SYP_OK);
    REQUIRE(b != nullptr);
    {
        const auto rs = snapshot_ranges(b);
        REQUIRE(!rs.empty());
        CHECK_EQ(rs[0].start, static_cast<int64_t>(0));
        CHECK(rs[0].end >= 1024);
    }

    read_exact_range(b, 3072, 4096);
    // 反向也成立：B 下好的区间 A 立刻看得见。
    {
        const auto rs = snapshot_ranges(a);
        bool covers_b = false;
        for (const auto& r : rs) {
            if (r.start <= 3072 && r.end >= 4096) covers_b = true;
        }
        CHECK(covers_b);
    }

    syp_source_close(b);
    syp_source_close(a);

    // 第三次打开：索引里两段都在 —— 这是当前 bug 的判据。
    Cbs cbsC;
    auto cbC = cbsC.view();
    syp_source* c = nullptr;
    REQUIRE(syp_source_open(&c, "http://x/shared", nullptr, &e.cfg, &cbC) == SYP_OK);
    REQUIRE(c != nullptr);
    const auto rs = snapshot_ranges(c);
    bool has_head = false;
    bool has_tail = false;
    for (const auto& r : rs) {
        if (r.start <= 0 && r.end >= 1024) has_head = true;
        if (r.start <= 3072 && r.end >= 4096) has_tail = true;
    }
    CHECK(has_head);
    CHECK(has_tail);
    syp_source_close(c);
    check_stub(e.stub);
}

// 同 URL 的两个源交替下载时，注册表的引用计数正确归零；全关之后
// 再开一个仍然读得到完整内容（对象真的被释放又重新打开过）。
//
// 【顺带钉死 cache key 的逐字节相同要求】这里的 key 是用例自己按
// cfg.cache_dir 那份字符串算的；open_count(key) 数得出 1/2/0，等于证明
// SourceBridge 内部用的就是**逐字相同**的那个 key。make_key 不做路径
// 规范化（cache_store.h:83-86），所以"dir" 与 "dir/" 会是两个 key、
// 同一批文件两份索引——覆盖 bug 原样复活。见
// test_cache_store.cpp 的 make_key_requires_byte_identical_dir_string。
TEST_CASE(two_sources_same_url_refcount_and_reopen) {
    syp::test::Watchdog wd("two_sources_same_url_refcount_and_reopen",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 2048;
    Env e;
    e.stub.set_default_script(script_ok(N));
    const std::string key = syp::dl::CacheStore::make_key(e.dir, "http://x/rc");

    auto cbA = e.cbs.view();
    syp_source* a = nullptr;
    REQUIRE(syp_source_open(&a, "http://x/rc", nullptr, &e.cfg, &cbA) == SYP_OK);
    CHECK_EQ(syp::dl::CacheStore::get().open_count(key), static_cast<int64_t>(1));

    Cbs cbsB;
    auto cbB = cbsB.view();
    syp_source* b = nullptr;
    REQUIRE(syp_source_open(&b, "http://x/rc", nullptr, &e.cfg, &cbB) == SYP_OK);
    CHECK_EQ(syp::dl::CacheStore::get().open_count(key), static_cast<int64_t>(2));

    read_exact_range(a, 0, 512);
    syp_source_close(a);
    CHECK_EQ(syp::dl::CacheStore::get().open_count(key), static_cast<int64_t>(1));
    read_exact_range(b, 512, 1024);
    syp_source_close(b);
    CHECK_EQ(syp::dl::CacheStore::get().open_count(key), static_cast<int64_t>(0));

    Cbs cbsC;
    auto cbC = cbsC.view();
    syp_source* c = nullptr;
    REQUIRE(syp_source_open(&c, "http://x/rc", nullptr, &e.cfg, &cbC) == SYP_OK);
    const auto got = read_all(c);
    CHECK_EQ(got.size(), static_cast<size_t>(N));
    CHECK(bytes_match(got, 0));
    syp_source_close(c);
    check_stub(e.stub);
}

// 【C ABI 上的 cache_dir 归一化】
// 上一条用的是同一份 e.dir 字符串，所以它证不了这一条：**两种拼法**的同一个
// 目录，经 syp_source_open 之后必须落在同一个注册表条目上。
//
// 在这个归一化落地之前这是一条没有闸的路：归一化只存在于 src/media 与 SYPBridge.mm，
// src/dl 够不到（dl 不许依赖 media），于是直接用 C ABI 的调用方
// 混用 "…/d" 与 "…//d" 会静默分裂成两份缓存——同一组 .idx/.dat 被两个
// CacheIndex 打开、save() 互相整份覆盖（test_cache_store.cpp 的
// trailing_slash_dir_resurrects_the_split_index 把那个后果钉住了），而且
// 两边都不报错。后来把唯一那份实现下沉进 src/dl（syp::dl::normalize_cache_dir，
// 住在 cache_store.h），收口在 SourceBridge 的构造函数里。
//
// 判据取 open_count == 2 而不是"两个 key 相等"：后者只证明我们算 key 时归一化
// 了，前者证明**真的落在同一份 CacheIndex 上**（一份索引被开了两次）。
// 三种拼法各测一遍，因为 Foundation 的 URL.path 实测保留的正是
// "//"、"."、".." 这三类（cache_store.h 里有实测表），而不是尾斜杠。
TEST_CASE(c_abi_cache_dir_spellings_share_one_registry_entry) {
    syp::test::Watchdog wd("c_abi_cache_dir_spellings_share_one_registry_entry",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 1024;
    Env e;
    e.stub.set_default_script(script_ok(N));
    const char* kUrl = "http://x/abi-norm";
    // 归一化之后的那一份 —— e.dir 来自 TempDir，本来就是归一的。
    const std::string key = syp::dl::CacheStore::make_key(e.dir, kUrl);

    const std::string spellings[] = {e.dir + "/", e.dir + "//", e.dir + "/./"};
    for (const std::string& alt : spellings) {
        auto cbA = e.cbs.view();
        syp_source* a = nullptr;
        REQUIRE(syp_source_open(&a, kUrl, nullptr, &e.cfg, &cbA) == SYP_OK);
        CHECK_EQ(syp::dl::CacheStore::get().open_count(key), static_cast<int64_t>(1));

        syp_config c2 = e.cfg;
        c2.cache_dir = alt.c_str();
        Cbs cbsB;
        auto cbB = cbsB.view();
        syp_source* b = nullptr;
        REQUIRE(syp_source_open(&b, kUrl, nullptr, &c2, &cbB) == SYP_OK);
        // 没有归一化的话这里是 1：b 落在 "<dir>/<US><hash>" 那个**另一个** key 上。
        CHECK_EQ(syp::dl::CacheStore::get().open_count(key), static_cast<int64_t>(2));

        syp_source_close(a);
        syp_source_close(b);
        CHECK_EQ(syp::dl::CacheStore::get().open_count(key), static_cast<int64_t>(0));
    }
    check_stub(e.stub);
}

TEST_CASE(etag_change_is_content_changed) {
    syp::test::Watchdog wd("etag_change_is_content_changed", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 1024;
    TempDir td;
    const std::string dir = td.p.string();
    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        e.stub.set_default_script(script_ok(N));
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
        uint8_t tmp[64];
        REQUIRE(syp_source_read(s, tmp, 64) > 0);
        CHECK(e.cbs.wait_pred([&] { return e.cbs.total_n > 0; }));
        syp_source_close(s);
        check_stub(e.stub);
    }

    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        auto sc = script_ok(N);
        sc.etag = "\"e2\"";
        e.stub.set_default_script(sc);
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);

        // 以第二会话打开时磁盘上的真实区间为准（第一会话 close 还可能
        // 再落一点），否则 seek 进的「洞」其实已经有旧缓存，read 会直接
        // 返回正字节数。实现必须在身份确认前把新字节拦在内存里。
        const auto cached_old = snapshot_ranges(s);
        syp::dl::HoleSet hs;
        for (const Range& r : cached_old) hs.add(r);
        const auto hole = hs.first_hole_from(0, N);
        REQUIRE(hole.has_value());
        CHECK_EQ(syp_source_seek(s, hole->start, SYP_SEEK_SET), hole->start);

        std::atomic<int32_t> rd{0};
        std::thread t([&] {
            uint8_t b[64];
            rd.store(syp_source_read(s, b, 64), std::memory_order_relaxed);
        });
        CHECK(e.cbs.wait_pred([&] { return e.cbs.err_n > 0; }));
        CHECK_EQ(e.cbs.last_err, SYP_ERR_CONTENT_CHANGED);
        t.join();
        CHECK_EQ(rd.load(), SYP_ERR_CONTENT_CHANGED);
        CHECK_EQ(syp_source_read(s, nullptr, 0) /* size 0 */, SYP_ERR_INVALID_ARG);
        uint8_t b[8];
        CHECK_EQ(syp_source_read(s, b, 8), SYP_ERR_CONTENT_CHANGED);

        const auto cached_s2 = snapshot_ranges(s);
        int64_t bytes_old = 0, bytes_s2 = 0;
        for (const Range& r : cached_old) bytes_old += r.size();
        for (const Range& r : cached_s2) bytes_s2 += r.size();
        CHECK_EQ(bytes_s2, bytes_old);

        syp_source_close(s);
        check_stub(e.stub);
    }
}

TEST_CASE(content_length_change_is_content_changed) {
    syp::test::Watchdog wd("content_length_change_is_content_changed", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 1024;
    TempDir td;
    const std::string dir = td.p.string();
    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        e.stub.set_default_script(script_ok(N));
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
        uint8_t tmp[64];
        REQUIRE(syp_source_read(s, tmp, 64) > 0);
        CHECK(e.cbs.wait_pred([&] { return e.cbs.total_n > 0; }));
        syp_source_close(s);
        check_stub(e.stub);
    }
    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        auto sc = script_ok(N + 200);
        sc.etag.clear();  // 靠长度判定
        sc.last_modified.clear();
        e.stub.set_default_script(sc);
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
        const auto cached_old = snapshot_ranges(s);
        syp::dl::HoleSet hs;
        for (const Range& r : cached_old) hs.add(r);
        const auto hole = hs.first_hole_from(0, N);
        REQUIRE(hole.has_value());
        CHECK_EQ(syp_source_seek(s, hole->start, SYP_SEEK_SET), hole->start);
        std::thread t([&] {
            uint8_t b[64];
            (void)syp_source_read(s, b, 64);
        });
        CHECK(e.cbs.wait_pred([&] { return e.cbs.err_n > 0; }));
        CHECK_EQ(e.cbs.last_err, SYP_ERR_CONTENT_CHANGED);
        syp_source_interrupt(s);
        t.join();
        syp_source_close(s);
        check_stub(e.stub);
    }
}

TEST_CASE(no_range_fallback_reads_all) {
    syp::test::Watchdog wd("no_range_fallback_reads_all", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 900;
    Env e;
    auto sc = script_ok(N);
    sc.support_range = false;
    sc.http_status = 200;
    sc.chunk_size = 50;
    e.stub.set_default_script(sc);
    e.cfg.max_concurrent_tasks = 3;
    auto cb = e.cbs.view();
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);

    const auto got = read_all(s, 100);
    CHECK_EQ(got.size(), static_cast<size_t>(N));
    CHECK(bytes_match(got, 0));
    CHECK_EQ(e.cbs.err_n, 0);

    syp_source_close(s);
    check_stub(e.stub);
}

TEST_CASE(interrupt_cancels_blocking_read_resume_continues) {
    syp::test::Watchdog wd("interrupt_cancels_blocking_read_resume_continues", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 1024;
    Env e(StubBackend::Mode::Sync);
    e.stub.set_default_script(script_ok(N));
    auto cb = e.cbs.view();
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);

    std::atomic<int32_t> rd{0};
    std::thread reader([&] {
        uint8_t b[32];
        rd.store(syp_source_read(s, b, 32), std::memory_order_relaxed);
    });
    CHECK(e.cbs.wait_pred([&] { return e.cbs.buf_on_n > 0; }));
    syp_source_interrupt(s);
    reader.join();
    CHECK_EQ(rd.load(), SYP_ERR_CANCELED);
    uint8_t b[32];
    CHECK_EQ(syp_source_read(s, b, 32), SYP_ERR_CANCELED);

    syp_source_resume(s);

    std::atomic<bool> stop_pump{false};
    std::thread pumper([&] {
        while (!stop_pump.load(std::memory_order_relaxed)) {
            if (!e.stub.pump()) std::this_thread::yield();
        }
    });
    const int32_t n = syp_source_read(s, b, 32);
    CHECK(n > 0);
    for (int32_t i = 0; i < n; ++i) {
        CHECK_EQ(b[static_cast<size_t>(i)], synthetic_byte(i));
    }
    stop_pump.store(true, std::memory_order_relaxed);
    pumper.join();

    syp_source_close(s);
    check_stub(e.stub);
}

TEST_CASE(close_with_inflight_does_not_hang) {
    syp::test::Watchdog wd("close_with_inflight_does_not_hang", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 64 * 1024;
    Env e;
    auto sc = script_ok(N);
    sc.chunk_size = 16;
    e.stub.set_default_script(sc);
    e.cfg.max_concurrent_tasks = 3;
    e.cfg.min_segment_size = 256;
    e.cfg.segment_size_hint = 1024;
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, nullptr) == SYP_OK);
    syp_source_close(s);
    check_stub(e.stub);
}

TEST_CASE(close_unblocks_waiting_reader) {
    syp::test::Watchdog wd("close_unblocks_waiting_reader", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 1024;
    Env e(StubBackend::Mode::Sync);
    e.stub.set_default_script(script_ok(N));
    auto cb = e.cbs.view();
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);

    std::atomic<int32_t> rd{0};
    std::thread reader([&] {
        uint8_t b[32];
        rd.store(syp_source_read(s, b, 32), std::memory_order_relaxed);
    });
    CHECK(e.cbs.wait_pred([&] { return e.cbs.buf_on_n > 0; }));
    REQUIRE(e.stub.request_count() > 0);
    syp_source_close(s);
    reader.join();
    CHECK_EQ(rd.load(), SYP_ERR_CANCELED);
    check_stub(e.stub);
}

TEST_CASE(notify_persisted_reaches_scheduler) {
    syp::test::Watchdog wd("notify_persisted_reaches_scheduler", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 256;
    TempDir td;
    StubBackend stub;
    stub.set_mode(StubBackend::Mode::Async);
    stub.set_default_script(script_ok(N));
    syp_config cfg{};
    const std::string dir = td.p.string();
    cfg_small(&cfg, dir.c_str());

    auto r = SourceBridge::open("http://x/v", nullptr, cfg, nullptr, stub.backend(),
                                syp::dl::system_clock());
    REQUIRE(r.has_value());
    auto src = std::move(*r);

    std::vector<uint8_t> got;
    uint8_t buf[40];
    for (;;) {
        const int32_t n = src->read(buf, 40);
        if (n == SYP_ERR_EOF) break;
        REQUIRE(n > 0);
        got.insert(got.end(), buf, buf + n);
    }
    CHECK_EQ(got.size(), static_cast<size_t>(N));

    HoleSet sc;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        sc = src->scheduler_cached_for_test();
        if (sc.contains(Range{0, N})) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(sc.total_bytes() >= N);
    CHECK(sc.contains(Range{0, N}));

    src->close();
    check_stub(stub);
}

TEST_CASE(cache_size_remove_clear) {
    syp::test::Watchdog wd("cache_size_remove_clear", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 512;
    Env e;
    e.stub.set_default_script(script_ok(N));
    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, nullptr) == SYP_OK);
    const auto got = read_all(s);
    CHECK_EQ(got.size(), static_cast<size_t>(N));
    syp_source_close(s);

    const int64_t sz = syp_cache_size(e.dir.c_str());
    CHECK(sz > 0);
    CHECK_EQ(syp_cache_remove(e.dir.c_str(), "http://x/v"), SYP_OK);
    CHECK_EQ(syp_cache_size(e.dir.c_str()), int64_t{0});

    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, nullptr) == SYP_OK);
    CHECK_EQ(read_all(s).size(), static_cast<size_t>(N));
    syp_source_close(s);
    CHECK(syp_cache_size(e.dir.c_str()) > 0);
    CHECK_EQ(syp_cache_clear(e.dir.c_str()), SYP_OK);
    CHECK_EQ(syp_cache_size(e.dir.c_str()), int64_t{0});
    check_stub(e.stub);
}

TEST_CASE(open_rejects_bad_args) {
    Env e;
    e.stub.set_default_script(script_ok(64));
    syp_source* s = nullptr;
    CHECK_EQ(syp_source_open(nullptr, "http://x/v", nullptr, &e.cfg, nullptr),
             SYP_ERR_INVALID_ARG);
    CHECK_EQ(syp_source_open(&s, "", nullptr, &e.cfg, nullptr), SYP_ERR_INVALID_ARG);
    CHECK_EQ(syp_source_open(&s, "http://x/v", nullptr, nullptr, nullptr),
             SYP_ERR_INVALID_ARG);
    syp_config c = e.cfg;
    c.cache_dir = "";
    CHECK_EQ(syp_source_open(&s, "http://x/v", nullptr, &c, nullptr),
             SYP_ERR_INVALID_ARG);
    syp_set_http_backend(nullptr);
    CHECK_EQ(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, nullptr),
             SYP_ERR_INVALID_ARG);
    syp_set_http_backend(e.stub.backend());
    syp_source_close(nullptr);
    CHECK_EQ(syp_source_read(nullptr, nullptr, 1), SYP_ERR_INVALID_ARG);
    CHECK_EQ(syp_cache_size(nullptr), SYP_ERR_INVALID_ARG);
    CHECK_EQ(std::string(syp_status_str(SYP_ERR_CONTENT_CHANGED)),
             std::string("content_changed"));
    // SYP_ERR_BUSY 是瞬态"这次没做"，不能落进 "unknown"。
    CHECK_EQ(std::string(syp_status_str(SYP_ERR_BUSY)), std::string("busy"));
    CHECK_EQ(std::string(syp_status_str(12345)), std::string("unknown"));
    CHECK(syp_version() != nullptr);
}

TEST_CASE(resume_without_session_total_still_reads) {
    syp::test::Watchdog wd("resume_without_session_total_still_reads", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    // B2：会话 1 记下 total、validators 为空；会话 2 响应解析不出总长。
    // need_total_ 门禁会把全部字节堵在 pending_，read 永不返回。
    constexpr int64_t N = 512;
    TempDir td;
    const std::string dir = td.p.string();
    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        auto sc = script_ok(N);
        sc.etag.clear();
        sc.last_modified.clear();
        e.stub.set_default_script(sc);
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
        uint8_t tmp[64];
        int32_t filled = 0;
        while (filled < 64) {
            const int32_t n = syp_source_read(s, tmp + filled, 64 - filled);
            REQUIRE(n > 0);
            filled += n;
        }
        CHECK(e.cbs.wait_pred([&] { return e.cbs.total_n > 0; }));
        CHECK_EQ(syp_source_length(s), N);
        syp_source_close(s);
        check_stub(e.stub);
    }

    {
        Env e;
        e.dir = dir;
        cfg_small(&e.cfg, e.dir.c_str());
        auto sc = script_ok(N);
        sc.etag.clear();
        sc.last_modified.clear();
        sc.content_range = StubBackend::HeaderPolicy::Omit;
        sc.unknown_total_length = true;
        e.stub.set_default_script(sc);
        auto cb = e.cbs.view();
        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
        CHECK_EQ(syp_source_length(s), N);

        std::atomic<bool> done{false};
        std::atomic<int32_t> last{0};
        std::vector<uint8_t> got;
        std::mutex got_mu;
        std::thread reader([&] {
            uint8_t buf[80];
            for (;;) {
                const int32_t n = syp_source_read(s, buf, 80);
                if (n == SYP_ERR_EOF) break;
                if (n < 0) {
                    last.store(n, std::memory_order_relaxed);
                    break;
                }
                last.store(n, std::memory_order_relaxed);
                std::lock_guard<std::mutex> g(got_mu);
                got.insert(got.end(), buf, buf + n);
            }
            done.store(true, std::memory_order_release);
        });

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done.load(std::memory_order_acquire)) {
            syp_source_interrupt(s);
            tiny_test::fail(__FILE__, __LINE__,
                            "read hung waiting for identity/total gate");
        }
        reader.join();
        {
            std::lock_guard<std::mutex> g(got_mu);
            CHECK_EQ(got.size(), static_cast<size_t>(N));
            CHECK(bytes_match(got, 0));
        }
        CHECK_EQ(e.cbs.err_n, 0);

        syp_source_close(s);
        check_stub(e.stub);
    }
}

TEST_CASE(pending_respects_max_memory_bytes) {
    syp::test::Watchdog wd("pending_respects_max_memory_bytes", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    // 身份门还开着时 pending_ 必须被 max_memory_bytes 夹住，超了报错。
    // 直接往索引里写 etag、不预填区间，这样 open 就会立刻发请求；
    // 后端只推 on_data、不发 on_response，门一直关着。
    constexpr int64_t N = 4096;
    TempDir td;
    const std::string dir = td.p.string();
    {
        auto idx = syp::dl::CacheIndex::create(td.p, "http://x/v");
        auto v = idx.validate_and_update("\"e1\"", "", N);
        REQUIRE(v.has_value());
        auto sv = idx.save();
        REQUIRE(sv.has_value());
    }

    struct PendingCapBackend {
        struct Rec {
            uint32_t magic = 0x4C495645u;
            PendingCapBackend* owner = nullptr;
        };
        syp_http_backend table{};
        syp_response_sink sink{};
        Rec rec{};
        bool completed = false;

        PendingCapBackend() {
            rec.owner = this;
            table.backend_ctx = this;
            table.create = [](void* ctx, const syp_http_request*,
                              const syp_response_sink* sink)
                -> syp_http_request_handle* {
                auto* self = static_cast<PendingCapBackend*>(ctx);
                if (sink == nullptr) return nullptr;
                self->sink = *sink;
                return reinterpret_cast<syp_http_request_handle*>(&self->rec);
            };
            table.start = [](syp_http_request_handle* h) {
                auto* rec = reinterpret_cast<Rec*>(h);
                auto* self = rec->owner;
                // 故意不发 on_response：身份门保持关闭，字节只进 pending_。
                std::vector<uint8_t> chunk(512, 0xAB);
                if (self->sink.on_data != nullptr) {
                    self->sink.on_data(self->sink.ctx, chunk.data(),
                                       static_cast<int32_t>(chunk.size()));
                }
                if (self->sink.on_complete != nullptr) {
                    self->sink.on_complete(self->sink.ctx, SYP_OK, 0);
                }
                self->completed = true;
            };
            table.cancel = [](syp_http_request_handle* h) {
                auto* rec = reinterpret_cast<Rec*>(h);
                auto* self = rec->owner;
                if (!self->completed && self->sink.on_complete != nullptr) {
                    self->sink.on_complete(self->sink.ctx, SYP_ERR_CANCELED, 0);
                    self->completed = true;
                }
            };
            table.destroy = [](syp_http_request_handle*) {};
        }
        const syp_http_backend* backend() const noexcept { return &table; }
    } be;

    syp_config cfg{};
    cfg_small(&cfg, dir.c_str());
    cfg.max_memory_bytes = 16;
    Cbs cbs;
    auto cb = cbs.view();
    FakeClock clk;
    auto r = SourceBridge::open("http://x/v", nullptr, cfg, &cb, be.backend(),
                                clk.clock());
    REQUIRE(r.has_value());
    auto src = std::move(*r);

    CHECK(cbs.wait_pred([&] { return cbs.err_n > 0; }));
    CHECK_EQ(cbs.last_err, SYP_ERR_OOM);

    uint8_t b[8];
    std::atomic<bool> done{false};
    std::atomic<int32_t> n{0};
    std::thread reader([&] {
        n.store(src->read(b, 8), std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load(std::memory_order_acquire)) {
        src->interrupt();
        tiny_test::fail(__FILE__, __LINE__,
                        "read hung after OOM instead of returning SYP_ERR_OOM");
    }
    reader.join();
    CHECK_EQ(n.load(std::memory_order_relaxed), SYP_ERR_OOM);
    src->close();
}

// 【SourceBridge 这一半的容量策略此前一条用例都没有】max_cache_bytes /
// min_free_space_bytes / cache_ttl_ms 三个字段很早就在 syp_config 里，
// 但曾经**没有任何代码读过它们**。后来把它们接上了，但
// "save_index_locked 置位 → 锁外 maybe_enforce_capacity 消费 → 真的删了
// 文件"这条链任何一环断掉，test_cache_store 都发现不了——那个套件直接调
// enforce_capacity，绕过了整条 SourceBridge 侧的接线。
//
// 【必须显式推进时钟】本文件的 FakeClock 是常量（t 不动），而节流的判据是
// "now - last_enforce_ms_ >= 1000 或 bytes_since_enforce_ >= 8MiB"。t 恒为 0
// 时 0 - 0 < 1000、字节数又远不到 8MiB，于是**节流永远不放行**，淘汰一次
// 都不会发生。不 advance 的话这条用例会变成一条什么都没验的绿灯。
// 推进时钟是纯逻辑操作，不是 sleep。
TEST_CASE(close_enforces_capacity_evicting_others_never_its_own_key) {
    syp::test::Watchdog wd("close_enforces_capacity_evicting_others_never_its_own_key",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 8192;
    TempDir td;
    const std::string dir = td.p.string();
    StubBackend stub;
    stub.set_mode(StubBackend::Mode::Async);
    stub.set_default_script(script_ok(N));

    const std::string victim_url = "http://x/victim";
    const std::string keeper_url = "http://x/keeper";
    const auto path_for = [&](const std::string& url, const char* ext) {
        return td.p / (syp::dl::CacheIndex::key_for_url(url) + ext);
    };

    // 1) 先把 victim 整份下下来再关掉：磁盘上留一份没人引用的缓存。
    {
        FakeClock clk0;
        syp_config cfg{};
        cfg_small(&cfg, dir.c_str());
        cfg.max_cache_bytes      = 0;      // 这一阶段不淘汰
        cfg.min_free_space_bytes = 0;
        auto r = SourceBridge::open(victim_url, nullptr, cfg, nullptr, stub.backend(),
                                    clk0.clock());
        REQUIRE(r.has_value());
        auto src = std::move(*r);
        uint8_t buf[512];
        for (;;) {
            const int32_t n = src->read(buf, 512);
            if (n == SYP_ERR_EOF) break;
            REQUIRE(n > 0);
        }
        src->close();
    }
    REQUIRE(std::filesystem::exists(path_for(victim_url, ".dat")));

    // victim 是 LRU 里最旧的那条（.idx 与 .dat 一起 back-date，排序键是 .idx）。
    {
        const auto when = std::filesystem::file_time_type::clock::now()
                          - std::chrono::milliseconds(60000);
        std::error_code ec;
        std::filesystem::last_write_time(path_for(victim_url, ".idx"), when, ec);
        CHECK(!ec);
        std::filesystem::last_write_time(path_for(victim_url, ".dat"), when, ec);
        CHECK(!ec);
    }

    // 2) keeper：下完、推进时钟放行节流、close()。
    //    max_cache_bytes 取 4096 —— 比 keeper 自己那份（8KiB+）还小，所以
    //    淘汰循环删完 victim 之后**仍然不满足**，会继续走到 keeper。它必须
    //    因为"正被引用"而活下来：close() 里的调用点在 CacheStore::release()
    //    之前，引用计数还是 1。
    FakeClock clk;
    syp_config cfg{};
    cfg_small(&cfg, dir.c_str());
    cfg.max_cache_bytes      = 4096;
    cfg.min_free_space_bytes = 0;
    auto r = SourceBridge::open(keeper_url, nullptr, cfg, nullptr, stub.backend(),
                                clk.clock());
    REQUIRE(r.has_value());
    auto src = std::move(*r);
    uint8_t buf[512];
    for (;;) {
        const int32_t n = src->read(buf, 512);
        if (n == SYP_ERR_EOF) break;
        REQUIRE(n > 0);
    }
    CHECK(std::filesystem::exists(path_for(victim_url, ".dat")));  // 还没轮到
    clk.advance(2000);        // 放行节流（> kEnforceMinIntervalMs = 1000）
    src->close();

    // victim 被淘汰；keeper 一个字节都没少。
    CHECK(!std::filesystem::exists(path_for(victim_url, ".idx")));
    CHECK(!std::filesystem::exists(path_for(victim_url, ".dat")));
    CHECK(std::filesystem::exists(path_for(keeper_url, ".idx")));
    CHECK(std::filesystem::exists(path_for(keeper_url, ".dat")));
    check_stub(stub);
}

TEST_CASE(cpp_open_with_injected_clock) {
    syp::test::Watchdog wd("cpp_open_with_injected_clock", kAsyncWatchdogSoftMs,
                           kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 256;
    TempDir td;
    StubBackend stub;
    stub.set_mode(StubBackend::Mode::Async);
    stub.set_default_script(script_ok(N));
    FakeClock clk;
    syp_config cfg{};
    const std::string dir = td.p.string();
    cfg_small(&cfg, dir.c_str());

    auto r = SourceBridge::open("http://x/v", nullptr, cfg, nullptr, stub.backend(),
                                clk.clock());
    REQUIRE(r.has_value());
    auto src = std::move(*r);
    std::vector<uint8_t> got;
    uint8_t buf[40];
    for (;;) {
        const int32_t n = src->read(buf, 40);
        if (n == SYP_ERR_EOF) break;
        REQUIRE(n > 0);
        got.insert(got.end(), buf, buf + n);
    }
    CHECK_EQ(got.size(), static_cast<size_t>(N));
    CHECK(bytes_match(got, 0));
    src->close();
    check_stub(stub);
}

namespace {

// 【最朴素的 GET】connect、发请求行、读到 EOF、按 "\r\n\r\n"
// 切掉头。不经 syp_source——那会让这条用例依赖被测对象（LoopbackServer 的
// 路由）之外的一整层。url 形如 LoopbackServer::url() 产出的
// "http://127.0.0.1:<port>/path"。
std::string http_get_body(const std::string& full_url) {
    const std::string prefix = "http://127.0.0.1:";
    if (full_url.rfind(prefix, 0) != 0) {
        tiny_test::fail(__FILE__, __LINE__, "http_get_body: unexpected url prefix");
        return {};
    }
    const size_t port_begin = prefix.size();
    const size_t slash = full_url.find('/', port_begin);
    const std::string port_str = full_url.substr(port_begin, slash - port_begin);
    const std::string path =
        (slash == std::string::npos) ? std::string("/") : full_url.substr(slash);
    const int port = std::atoi(port_str.c_str());

    // http_get_body 返回 std::string，不是 void——REQUIRE 失败时的裸
    // "return;" 在这不能用，这里手动 CHECK + 提前 return {} 代替 REQUIRE。
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        tiny_test::fail(__FILE__, __LINE__, "http_get_body: socket() failed");
        return {};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        tiny_test::fail(__FILE__, __LINE__, "http_get_body: connect() failed");
        ::close(fd);
        return {};
    }

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\nHost: 127.0.0.1:" << port
        << "\r\nConnection: close\r\n\r\n";
    const std::string req_str = req.str();
    size_t off = 0;
    while (off < req_str.size()) {
        const ssize_t w = ::send(fd, req_str.data() + off, req_str.size() - off, 0);
        if (w <= 0) {
            tiny_test::fail(__FILE__, __LINE__, "http_get_body: send() failed");
            ::close(fd);
            return {};
        }
        off += static_cast<size_t>(w);
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    const size_t hdr_end = resp.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        tiny_test::fail(__FILE__, __LINE__, "http_get_body: no header/body separator");
        return {};
    }
    return resp.substr(hdr_end + 4);
}

}  // namespace

// 【按路径路由 + 每路径计数】HLS 一个会话要取好几种资源，
// 而后续断言的形状是"某个 URL 一次都没被请求"——必须按路径数。
TEST_CASE(loopback_serves_multiple_routes_and_counts_them) {
    syp::dl::test::LoopbackConfig cfg;
    syp::dl::test::LoopbackServer srv(cfg);
    srv.set_route("/a.txt", std::vector<uint8_t>{'h', 'i'}, "text/plain");
    srv.set_route("/b.txt", std::vector<uint8_t>{'y', 'o', '!'}, "text/plain");

    // 用既有的 stub 之外的最朴素方式验证：直接开 socket 发两次 GET /a.txt、
    // 一次 GET /b.txt，然后数。
    CHECK_EQ(http_get_body(srv.url("/a.txt")), std::string("hi"));
    CHECK_EQ(http_get_body(srv.url("/a.txt")), std::string("hi"));
    CHECK_EQ(http_get_body(srv.url("/b.txt")), std::string("yo!"));

    CHECK_EQ(srv.requests_for("/a.txt"), 2);
    CHECK_EQ(srv.requests_for("/b.txt"), 1);
    CHECK_EQ(srv.requests_for("/never.txt"), 0);

    // 路径要落进记录里，不只是计数
    const auto recs = srv.requests_snapshot();
    CHECK_EQ(static_cast<int64_t>(recs.size()), 3);
    CHECK_EQ(recs[0].path, std::string("/a.txt"));
}

// 【路由必须是精确匹配】路由必须是精确匹配（含 query），不能退化成
// 前缀匹配——`cfg_.routes.find(req_path)` 本来就是精确 key 查找，但没有任何
// 断言守着这一点。这条用例专门锁住它：同时注册 "/a.txt" 和 "/a.txt?x=1"
// （内容不同），如果匹配退化成前缀（例如 rfind(path,0)==0），"/a.txt" 会
// 抢先命中本该匹配 "/a.txt?x=1" 的请求，返回错的内容，计数也会记到错的 key
// 上。别处的用例要断言"未选中的码率档一次都没被请求"、"同一个
// 播放列表 URL 被请求了第二次"，HLS 分片 URL 常带 query（如
// "?tag=14&v=cfc"），一旦这里的匹配逻辑被改松，那些后续断言会假绿——
// 以为验证了，其实没有。不要删这条用例。
TEST_CASE(loopback_route_match_is_exact_not_prefix) {
    syp::dl::test::LoopbackConfig cfg;
    syp::dl::test::LoopbackServer srv(cfg);
    srv.set_route("/a.txt", std::vector<uint8_t>{'A'}, "text/plain");
    srv.set_route("/a.txt?x=1", std::vector<uint8_t>{'B'}, "text/plain");

    // 两次拿到的内容必须各自正确，不能被前缀命中串了。
    CHECK_EQ(http_get_body(srv.url("/a.txt")), std::string("A"));
    CHECK_EQ(http_get_body(srv.url("/a.txt?x=1")), std::string("B"));

    // 计数必须落在各自的 key 上，不能被混计到同一个 key。
    CHECK_EQ(srv.requests_for("/a.txt"), 1);
    CHECK_EQ(srv.requests_for("/a.txt?x=1"), 1);
}

// syp_config.struct_size 的前向兼容契约：头文件里写了
// "新增字段一律追加在末尾"，copy_config 里实现了截断/夹紧，但此前
// **全仓没有任何用例碰过它**——这个缺口是仓库级的，syp_preload_config 那半边
// 由 test_preloader.cpp 的 preload_config_struct_size_is_forward_compatible 补。
//
// 直接调 copy_config 而不是绕 syp_source_open：这里要验的是纯粹的字段搬运，
// 一次函数调用就能逐字段断言，不需要网络、不需要时序。
TEST_CASE(config_struct_size_is_forward_compatible) {
    const syp_config def = syp::dl::default_config();

    // 1) 比我们**小**：截断点之后的字段保持默认，不读调用方没填的内存。
    {
        syp_config c{};
        syp_config_init(&c);
        c.max_concurrent_tasks = 1;                       // 截断点之前 → 照用
        c.first_buffer_ms      = 7;                       // 截断点之后 → 必须忽略
        c.target_buffer_ms     = 9;
        c.allow_no_range_fallback = false;
        c.struct_size = static_cast<uint32_t>(offsetof(syp_config, first_buffer_ms));
        syp_status err = SYP_ERR_IO;
        const syp_config out = syp::dl::copy_config(&c, &err);
        CHECK_EQ(err, SYP_OK);
        CHECK_EQ(out.max_concurrent_tasks, 1);
        CHECK_EQ(out.first_buffer_ms, def.first_buffer_ms);
        CHECK_EQ(out.target_buffer_ms, def.target_buffer_ms);
        CHECK_EQ(out.allow_no_range_fallback, def.allow_no_range_fallback);
        // struct_size 一律被改写成我们自己的版本号。
        CHECK_EQ(out.struct_size, static_cast<uint32_t>(sizeof(syp_config)));
    }

    // 2) 比我们**大**（未来版本的调用方）：多出来的尾巴一律忽略，已知字段照用。
    {
        struct Bigger {
            syp_config base;
            int64_t    future[3];
        };
        Bigger b{};
        syp_config_init(&b.base);
        b.base.max_concurrent_tasks = 5;
        b.base.first_buffer_ms      = 42;
        for (int64_t& x : b.future) x = -1;
        b.base.struct_size = static_cast<uint32_t>(sizeof(Bigger));
        syp_status err = SYP_ERR_IO;
        const syp_config out = syp::dl::copy_config(&b.base, &err);
        CHECK_EQ(err, SYP_OK);
        CHECK_EQ(out.max_concurrent_tasks, 5);
        CHECK_EQ(out.first_buffer_ms, 42);
        CHECK_EQ(out.struct_size, static_cast<uint32_t>(sizeof(syp_config)));
    }

    // 3) 连版本号本身都放不下 → INVALID_ARG，返回的是一份纯默认配置。
    {
        syp_config c{};
        syp_config_init(&c);
        c.max_concurrent_tasks = 1;
        c.struct_size = 1;
        syp_status err = SYP_OK;
        const syp_config out = syp::dl::copy_config(&c, &err);
        CHECK_EQ(err, SYP_ERR_INVALID_ARG);
        CHECK_EQ(out.max_concurrent_tasks, def.max_concurrent_tasks);
    }

    // 4) nullptr 同样是 INVALID_ARG，不崩。
    {
        syp_status err = SYP_OK;
        const syp_config out = syp::dl::copy_config(nullptr, &err);
        CHECK_EQ(err, SYP_ERR_INVALID_ARG);
        CHECK_EQ(out.max_concurrent_tasks, def.max_concurrent_tasks);
    }
}

// 【限速类别从 open() 一路进到 Scheduler】并且可以事后改。
//
// 断言读的是 rate_class_for_test()——它读 Scheduler::rate_class()，也就是
// schedule() 准入时真正用的那个成员，不是 SourceBridge 自己记的那份。于是
// "open 收了 cls 却没填进 SchedulerConfig"这个变异在第二条 CHECK 上必红
// （调度器仍是默认的 Playing）。
TEST_CASE(source_bridge_defaults_to_playing_and_accepts_an_explicit_class) {
    syp::test::Watchdog wd("source_bridge_defaults_to_playing_and_accepts_an_explicit_class",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    constexpr int64_t N = 256;
    TempDir td;
    StubBackend stub;
    stub.set_mode(StubBackend::Mode::Async);
    stub.set_default_script(script_ok(N));
    syp_config cfg{};
    const std::string dir = td.p.string();
    cfg_small(&cfg, dir.c_str());

    {
        auto r = SourceBridge::open("http://x/def", nullptr, cfg, nullptr, stub.backend(),
                                    syp::dl::system_clock());
        REQUIRE(r.has_value());
        CHECK((*r)->rate_class_for_test() == syp::dl::RateClass::Playing);
    }
    {
        auto r = SourceBridge::open("http://x/pre", nullptr, cfg, nullptr, stub.backend(),
                                    syp::dl::system_clock(), syp::dl::RateClass::Preload);
        REQUIRE(r.has_value());
        CHECK((*r)->rate_class_for_test() == syp::dl::RateClass::Preload);
        (*r)->set_rate_class(syp::dl::RateClass::Playing);
        CHECK((*r)->rate_class_for_test() == syp::dl::RateClass::Playing);
        // 构造时类别不随事后的 set_rate_class 变。
        CHECK((*r)->open_rate_class_for_test() == syp::dl::RateClass::Preload);
        (*r)->set_rate_class(syp::dl::RateClass::Preload);
        CHECK((*r)->rate_class_for_test() == syp::dl::RateClass::Preload);
        // close 之后 sched_ 已拆：只记下来，不崩、不挂。
        (*r)->close();
        (*r)->set_rate_class(syp::dl::RateClass::Playing);
        CHECK((*r)->rate_class_for_test() == syp::dl::RateClass::Playing);
    }
}

#if defined(__APPLE__)
// 【端到端】回环服务器 + 真实 Apple 后端 + 真实时钟 + 进程单例
// HealthTicker。同一资源里只有**第一个**供体请求是 slow-loris（每 200ms 吐
// 1 KiB ≈ 5 KiB/s，永远不触发后端 read timeout），其余连接按块节流到
// ≈ 320 KiB/s——节流不是为了慢，是为了让兄弟连接活过一个速度窗口，
// health_check 才采得到健康样本、基线才 > 0（回环不节流时 1 MiB 几毫秒就
// 下完，基线永远是慢连接自己，它永远不"比基线慢"）。
//
// 为什么第一个请求就是那条要被换掉的：SourceBridge 首个请求时总长未知，
// 调度器并发锁 1、按目标窗口整段发一条探测请求 [0, 3 × min_segment_size)
// = [0, 192 KiB)。之后 update_playback 给的 duration 让窗口（总长一知道）
// 覆盖整个文件，其余 ~2.8 MiB 按并发 3 切成 ~1 MiB 的片走快连接。读者顺序
// 读，会卡在慢探测请求上——这正是 slow-loris 真实伤人的形状。
//
// 没有慢替换时：192 KiB ÷ 5 KiB/s ≈ 38s 才能读过探测区间 ⇒ 20s 截止时还没
// 读完、服务端也还没有任何 early_close（被慢供的那条还在供），两条断言一起红。
// 有慢替换时：~1s 采到基线、再两次检查（2 strikes）⇒ 约 3s 替换，读完整份
// 实测几秒。
//
// SourceBridge 不暴露 slow_kills()（其余层不改），所以只能从服务端
// 侧断言：被客户端提前断开的恰是 start == 0 的那条；替换请求从已收字节处
// 续（0 < start < 探测区间终点），不是从 0 重下。
//
// 耗时上界是负载敏感类：失败时先单独重跑再下结论。
TEST_CASE(slow_loris_connection_is_replaced_end_to_end) {
    // 软 35s / 硬 40s（25s 离 20s 截止太近，负载下软超时会先响）。
    // 硬超时是从起点算的绝对值，必须 > 软超时，不能沿用 kAsyncWatchdogHardMs
    // = 30000（Watchdog 会拿 hard − soft 当第二段等待）；40s 仍低于 ctest 的 60s。
    syp::test::Watchdog wd("slow_loris_connection_is_replaced_end_to_end", 35000,
                           40000, kAsyncWatchdogHint);
    constexpr int64_t N = 3 * 1024 * 1024;
    constexpr int64_t kDeadlineMs = 20000;

    syp::dl::test::LoopbackConfig lc;
    lc.resource_length     = N;
    lc.support_range       = true;
    lc.body_chunk_bytes    = 16 * 1024;   // 快连接 ≈ 320 KiB/s
    lc.body_chunk_delay_ms = 50;
    lc.slow_first_requests = 1;           // 只有第 1 个供体请求是 slow-loris
    lc.slow_chunk_bytes    = 1024;
    lc.slow_chunk_delay_ms = 200;         // ≈ 5 KiB/s
    syp::dl::test::LoopbackServer srv(lc);

    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);
    TempDir td;
    const std::string dir = td.p.string();
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir            = dir.c_str();
    cfg.max_concurrent_tasks = 3;
    cfg.min_segment_size     = 64 * 1024;
    cfg.speed_window_ms      = 1000;
    syp_source* s = nullptr;
    const std::string url = srv.url("/v");
    REQUIRE(syp_source_open(&s, url.c_str(), nullptr, &cfg, nullptr) == SYP_OK);

    // 播放器会周期性上报；这里一次就够：duration 1s ⇒ target_buffer 10s 的
    // 窗口 = 10 倍总长，总长一知道窗口就盖住整个文件。
    syp_playback_state ps{};
    ps.play_position_ms = 0;
    ps.duration_ms      = 1000;
    ps.is_playing       = true;
    ps.is_seeking       = false;
    syp_source_update_playback(s, &ps);

    const auto t0 = std::chrono::steady_clock::now();
    std::mutex              mu;
    std::condition_variable cv;
    bool                    done = false;
    int32_t                 last_rc = 0;
    std::vector<uint8_t>    got;
    std::thread reader([&] {
        std::vector<uint8_t> buf(64 * 1024);
        std::vector<uint8_t> out;
        int32_t rc = 0;
        for (;;) {
            rc = syp_source_read(s, buf.data(), static_cast<int32_t>(buf.size()));
            if (rc <= 0) break;
            out.insert(out.end(), buf.begin(), buf.begin() + rc);
        }
        std::lock_guard<std::mutex> g(mu);
        got     = std::move(out);
        last_rc = rc;
        done    = true;
        cv.notify_all();
    });
    bool in_time = false;
    {
        std::unique_lock<std::mutex> lk(mu);
        in_time = cv.wait_for(lk, std::chrono::milliseconds(kDeadlineMs), [&] { return done; });
    }
    if (!in_time) syp_source_interrupt(s);   // 截止未读完：放读者出来，断言照常红
    reader.join();
    const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0).count();

    CHECK(in_time);
    CHECK_EQ(last_rc, SYP_ERR_EOF);
    CHECK(elapsed_ms < kDeadlineMs);
    CHECK_EQ(static_cast<int64_t>(got.size()), N);
    CHECK(bytes_match(got, 0));

    // 被换掉的慢连接要等服务端下一次写失败才落记录（≤ 两个 200ms 节拍）。
    // 在 close 之前看：close 会取消剩余在途任务，同样产生 early_close，
    // 会把"没替换"的变异掩盖掉。
    for (int i = 0; i < 150 && srv.early_close_count() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(srv.total_requests() >= 4);        // 探测 + 快片 + 至少一次替换
    CHECK(srv.early_close_count() >= 1);
    const auto recs = srv.requests_snapshot();
    int64_t probe_end = -1;
    for (const auto& r : recs) {
        if (r.start == 0 && r.early_close) probe_end = r.end;
    }
    CHECK(probe_end > 0);                    // 被客户端断掉的恰是 start == 0 那条
    bool resumed_mid_probe = false;
    for (const auto& r : recs) {
        if (r.start > 0 && r.start < probe_end) resumed_mid_probe = true;
    }
    CHECK(resumed_mid_probe);                // 替换从 next_offset 续，不从 0 重下

    syp_source_close(s);
    syp_set_http_backend(nullptr);
}

// 【预连接端到端】回环服务器 keep-alive + 真实 Apple
// 后端。预连接发的 1 字节 Range 建起一条 TCP 连接、206 正常收尾后还回
// NSURLSession 的连接池；随后 SourceBridge 单连接（max_concurrent_tasks = 1）
// 顺序读完整份资源，全程**不再 accept 新连接**——accepted_connections() == 1
// 就是"预热的连接被播放复用"的直接证据。
//
// 去重：Preconnector 是进程单例，30 秒去重表跨用例存活。每个 LoopbackServer
// 由内核分配新端口，源键 "http://127.0.0.1:<port>" 每次都不同；本文件也只有
// 这一条用例调 syp_preconnect，不会被前面的用例去重掉（下面 REQUIRE 请求数
// == 1 兜底：真被去重了会红在那里，不会假绿）。
//
// 连接复用是 NSURLSession 的池策略，库无法保证；收尾时连续跑 10
// 次的实测分布已经记录在案。
namespace {

constexpr int64_t kPreconnectResourceBytes = 64 * 1024;

syp::dl::test::LoopbackConfig keep_alive_config() {
    syp::dl::test::LoopbackConfig lc;
    lc.resource_length = kPreconnectResourceBytes;
    lc.support_range   = true;
    lc.keep_alive      = true;
    return lc;
}

// 单连接顺序读完整份。返回读到的字节；rc 为最后一次 read 的返回值。
std::vector<uint8_t> read_whole_single_connection(const std::string& url, const std::string& dir,
                                                  int32_t& rc_out) {
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir            = dir.c_str();
    cfg.max_concurrent_tasks = 1;
    syp_source* s = nullptr;
    std::vector<uint8_t> out;
    rc_out = syp_source_open(&s, url.c_str(), nullptr, &cfg, nullptr);
    if (rc_out != SYP_OK) return out;
    std::vector<uint8_t> buf(4096);
    for (;;) {
        rc_out = syp_source_read(s, buf.data(), static_cast<int32_t>(buf.size()));
        if (rc_out <= 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + rc_out);
    }
    syp_source_close(s);
    return out;
}

}  // namespace

TEST_CASE(preconnect_warms_connection_reused_by_playback) {
    syp::test::Watchdog wd("preconnect_warms_connection_reused_by_playback",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    syp::dl::test::LoopbackServer srv(keep_alive_config());
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);
    TempDir td;
    const std::string url = srv.url("/v");

    syp_preconnect(url.c_str(), nullptr);
    // 等两件事都发生：服务端记下预连接那 1 个请求（写完 body 才记），且预连接
    // 任务走到终态（on_finished 已回调 ⇒ 后端已把响应读完，连接回池）。只等
    // 前者的话，播放请求可能赶在连接回池之前发出，被迫新开一条——那是测试
    // 自己造的竞争，不是复用失败。
    auto& pc = syp::dl::Preconnector::instance();
    for (int i = 0; i < 250 && !(srv.total_requests() == 1 && pc.inflight_for_test() == 0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(srv.total_requests() == 1);           // 预连接真的发出且被供完
    CHECK_EQ(pc.inflight_for_test(), 0);
    const auto pre = srv.requests_snapshot();
    REQUIRE(pre.size() == 1u);
    CHECK_EQ(pre[0].start, 0);                     // Range: bytes=0-0 的形状
    CHECK_EQ(pre[0].end, 1);
    CHECK(!pre[0].early_close);                    // 206 不 cancel，完整收尾
    CHECK_EQ(srv.accepted_connections(), 1);

    int32_t rc = 0;
    const std::vector<uint8_t> got = read_whole_single_connection(url, td.p.string(), rc);
    CHECK_EQ(rc, SYP_ERR_EOF);
    CHECK_EQ(static_cast<int64_t>(got.size()), kPreconnectResourceBytes);
    CHECK(bytes_match(got, 0));
    CHECK(srv.total_requests() >= 2);              // 播放确实又发了请求
    CHECK_EQ(srv.accepted_connections(), 1);       // 核心断言：没有新连接

    syp_set_http_backend(nullptr);
}

// 对照：同配置不预连接。读之前 0 条连接（计数只在 accept 时 +1，不是进程级
// 残留、也不含前一条用例的服务器），读完恰 1 条——证明计数口径：播放自己
// 读完这份资源需要且只需要 1 条连接（64 KiB 落在首个探测请求里，实测播放只
// 发 1 个请求）。于是主用例里"预连接 1 条 + 播放 1 条"若没复用必然是 2，
// "== 1"区分得出"预连接那条被复用"与"播放另开了一条"。
TEST_CASE(without_preconnect_playback_opens_its_own_connection) {
    syp::test::Watchdog wd("without_preconnect_playback_opens_its_own_connection",
                           kAsyncWatchdogSoftMs, kAsyncWatchdogHardMs, kAsyncWatchdogHint);
    syp::dl::test::LoopbackServer srv(keep_alive_config());
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);
    TempDir td;
    const std::string url = srv.url("/v");

    CHECK_EQ(srv.accepted_connections(), 0);
    int32_t rc = 0;
    const std::vector<uint8_t> got = read_whole_single_connection(url, td.p.string(), rc);
    CHECK_EQ(rc, SYP_ERR_EOF);
    CHECK_EQ(static_cast<int64_t>(got.size()), kPreconnectResourceBytes);
    CHECK(bytes_match(got, 0));
    CHECK(srv.total_requests() >= 1);
    CHECK_EQ(srv.accepted_connections(), 1);

    syp_set_http_backend(nullptr);
}
#endif  // __APPLE__

int main() { return tiny_test_main(); }
