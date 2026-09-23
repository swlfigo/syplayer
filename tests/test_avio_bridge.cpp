// test_avio_bridge.cpp — AVIOContext 与 syp_source 之间那条缝。
//
// 映射逻辑做成自由函数是为了能脱离 IO 单独测——这几处正是最容易写错、
// 而且错了以后症状极其难查的地方。
#include "media/avio_bridge.h"
#include "tiny_test.h"
#include "support/stub_backend.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>
#include <syplayer/syp_source.h>
#include <syplayer/syp_types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

using namespace syp::media;
using syp::dl::test::StubBackend;
using syp::dl::test::synthetic_byte;

namespace {

// 最小化的临时目录 + 桩环境，手法照抄 tests/test_source_bridge.cpp 里的
// TempDir/Env：真实走 syp_set_http_backend() + syp_source_open()，零网络。
struct TempDir {
    std::filesystem::path p;
    TempDir() {
        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed);
        p = base / ("syp-avio-" + std::to_string(::getpid()) + "-"
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

struct Env {
    TempDir     td;
    std::string dir;
    StubBackend stub;
    syp_config  cfg{};

    explicit Env(StubBackend::Mode mode = StubBackend::Mode::Async) {
        dir = td.p.string();
        syp_config_init(&cfg);
        cfg.cache_dir             = dir.c_str();
        cfg.max_concurrent_tasks  = 1;
        cfg.min_segment_size      = 32;
        cfg.segment_size_hint     = 32;
        cfg.max_retries           = 1;
        cfg.allow_no_range_fallback = true;
        stub.set_mode(mode);
        syp_set_http_backend(stub.backend());
    }
    ~Env() { syp_set_http_backend(nullptr); }
    Env(const Env&)            = delete;
    Env& operator=(const Env&) = delete;
};

StubBackend::Script script_ok(int64_t n) {
    StubBackend::Script s;
    s.resource_length = n;
    s.support_range   = true;
    s.http_status     = 206;
    s.etag            = "\"e1\"";
    s.last_modified   = "Wed, 01 Jan 2020 00:00:00 GMT";
    return s;
}

// 只等 on_total_length，不做任何 read——这样创建 AvioBridge 时
// AVIOContext 的初始 pos=0 与 syp_source 的真实读位置一致。
struct TotalWaiter {
    std::mutex mu;
    std::condition_variable cv;
    int64_t total = -1;

    static void on_total(void* ctx, int64_t v) {
        auto* self = static_cast<TotalWaiter*>(ctx);
        std::lock_guard<std::mutex> g(self->mu);
        self->total = v;
        self->cv.notify_all();
    }
    bool wait_known(int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [&] { return total >= 0; });
    }
};

}  // namespace

TEST_CASE(status_maps_to_averror) {
    CHECK_EQ(syp_status_to_averror(SYP_ERR_EOF), AVERROR_EOF);
    CHECK_EQ(syp_status_to_averror(SYP_ERR_CANCELED), AVERROR_EXIT);
    CHECK_EQ(syp_status_to_averror(SYP_ERR_TIMEOUT), AVERROR(ETIMEDOUT));
    CHECK_EQ(syp_status_to_averror(SYP_ERR_INVALID_ARG), AVERROR(EINVAL));
    CHECK_EQ(syp_status_to_averror(SYP_ERR_OOM), AVERROR(ENOMEM));
    // BUSY 是瞬态，映射成 EAGAIN（稍后再试），而不是 AVERROR_UNKNOWN。
    CHECK_EQ(syp_status_to_averror(SYP_ERR_BUSY), AVERROR(EAGAIN));
    CHECK_EQ(syp_status_to_averror(SYP_ERR_IO), AVERROR(EIO));
    CHECK_EQ(syp_status_to_averror(SYP_ERR_NETWORK), AVERROR(EIO));
    CHECK_EQ(syp_status_to_averror(SYP_ERR_NOT_IMPLEMENTED), AVERROR(ENOSYS));
    // 未列出的错误码不能映射成 0（0 会被 FFmpeg 当成成功）
    CHECK(syp_status_to_averror(SYP_ERR_CACHE_CORRUPT) < 0);
    // SYP_ERR_CACHE_CORRUPT 上面已经有显式 case，走不到 default 分支；
    // 真正需要守住的是「压根没在枚举里的状态码」也不能被 default 吐成 0。
    // 用一个不属于任何 SYP_ERR_* 常量的值，专门戳中 default 分支。
    CHECK(syp_status_to_averror(-12345) < 0);
}

TEST_CASE(whence_masks_avseek_force) {
    int32_t w = -1;
    CHECK(avio_whence_to_syp(SEEK_SET, &w));
    CHECK_EQ(w, static_cast<int32_t>(SYP_SEEK_SET));
    CHECK(avio_whence_to_syp(SEEK_CUR, &w));
    CHECK_EQ(w, static_cast<int32_t>(SYP_SEEK_CUR));
    CHECK(avio_whence_to_syp(SEEK_END, &w));
    CHECK_EQ(w, static_cast<int32_t>(SYP_SEEK_END));

    // AVSEEK_FORCE 必须先被 mask 掉，否则 SEEK_END|AVSEEK_FORCE 会被认成未知 whence
    CHECK(avio_whence_to_syp(SEEK_END | AVSEEK_FORCE, &w));
    CHECK_EQ(w, static_cast<int32_t>(SYP_SEEK_END));
    CHECK(avio_whence_to_syp(SEEK_SET | AVSEEK_FORCE, &w));
    CHECK_EQ(w, static_cast<int32_t>(SYP_SEEK_SET));

    // AVSEEK_SIZE 不是 whence，由调用方先行处理，这里必须拒绝
    CHECK(!avio_whence_to_syp(AVSEEK_SIZE, &w));
    CHECK(!avio_whence_to_syp(AVSEEK_SIZE | AVSEEK_FORCE, &w));
    CHECK(!avio_whence_to_syp(99, &w));
}

// ---------------------------------------------------------------- 运行时行为
// 上面两个用例只测了纯函数；下面用 StubBackend 搭一个真实的 syp_source*，
// 把 AvioBridge::create()/on_read/on_seek/seekable 真正跑起来。

TEST_CASE(avio_read_matches_source_bytes_with_short_reads) {
    constexpr int64_t N = 300;
    Env e;
    auto sc = script_ok(N);
    // 桩每次只推 7 字节：AVIOContext 内部缓冲区是 64 字节，
    // 这样 on_read 几乎每次都拿不到满 64 字节，强制走短读路径——
    // 如果实现把 buf_size(64) 当成返回值而不是 syp_source_read 的真实
    // 返回值，AVIOContext 会把缓冲区里未写入的垃圾字节也当成有效数据，
    // 拼出来的内容就对不上 synthetic_byte()。
    sc.chunk_size = 7;
    e.stub.set_default_script(sc);

    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, nullptr) == SYP_OK);

    auto bridge = AvioBridge::create(s, /*buffer_size=*/64);
    REQUIRE(bridge != nullptr);

    std::vector<uint8_t> got(static_cast<size_t>(N));
    const int rd = avio_read(bridge->ctx(), got.data(), static_cast<int>(N));
    CHECK_EQ(rd, static_cast<int>(N));
    bool all_match = true;
    for (int64_t i = 0; i < N; ++i) {
        if (got[static_cast<size_t>(i)] != synthetic_byte(i)) { all_match = false; break; }
    }
    CHECK(all_match);
    // diag().reads 数远大于「按 64 字节一口气读完 300 字节所需的 5 次」，
    // 证明确实发生了短读，不是侥幸一次性读满。
    CHECK(bridge->diag().reads > 5);

    bridge.reset();
    syp_source_close(s);
}

TEST_CASE(avio_size_and_seek_reads_from_offset) {
    constexpr int64_t N = 2048;
    Env e;
    e.stub.set_default_script(script_ok(N));

    TotalWaiter tw;
    syp_source_callbacks cb{};
    cb.ctx = &tw;
    cb.on_total_length = &TotalWaiter::on_total;

    syp_source* s = nullptr;
    REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
    // 等总长确定，但不做任何 read——保证创建 bridge 时 AVIOContext 的
    // 初始 pos=0 与 syp_source 的真实读位置一致。
    REQUIRE(tw.wait_known());
    REQUIRE(syp_source_length(s) == N);

    auto bridge = AvioBridge::create(s, /*buffer_size=*/0);
    REQUIRE(bridge != nullptr);

    // avio_size 走的是 AVSEEK_SIZE 分支：不 seek，只报总长。
    //
    // FFmpeg 的 avio_size() 自带兜底：AVSEEK_SIZE 失败时会退化成
    // seek(-1, SEEK_END) 再 +1（aviobuf.c）。这条兜底恰好和我们
    // on_seek 里 SEEK_END 分支的语义吻合，会算出同样正确的答案——
    // 也就是说光测 avio_size() 测不出「AVSEEK_SIZE 分支被整个删掉」
    // 这种问题。直接调 ctx()->seek 送一次 AVSEEK_SIZE，跳过
    // avio_size() 的兜底，才是真正钉住这条分支的方式。
    REQUIRE(bridge->ctx()->seek != nullptr);
    const int64_t direct_size =
        bridge->ctx()->seek(bridge->ctx()->opaque, 0, AVSEEK_SIZE);
    CHECK_EQ(direct_size, N);

    const int64_t sz = avio_size(bridge->ctx());
    CHECK_EQ(sz, N);

    constexpr int64_t target = 1500;
    const int64_t np = avio_seek(bridge->ctx(), target, SEEK_SET);
    CHECK_EQ(np, target);

    uint8_t buf[64];
    const int rd = avio_read(bridge->ctx(), buf, static_cast<int>(sizeof(buf)));
    CHECK_EQ(rd, static_cast<int>(sizeof(buf)));
    bool all_match = true;
    for (int i = 0; i < rd; ++i) {
        if (buf[static_cast<size_t>(i)] != synthetic_byte(target + i)) { all_match = false; break; }
    }
    CHECK(all_match);

    bridge.reset();
    syp_source_close(s);
}

TEST_CASE(ctx_seekable_reflects_known_length) {
    // 分支一：total 已知 → AVIO_SEEKABLE_NORMAL。
    {
        constexpr int64_t N = 4096;
        Env e;
        e.stub.set_default_script(script_ok(N));

        TotalWaiter tw;
        syp_source_callbacks cb{};
        cb.ctx = &tw;
        cb.on_total_length = &TotalWaiter::on_total;

        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, &cb) == SYP_OK);
        REQUIRE(tw.wait_known());
        REQUIRE(syp_source_length(s) == N);

        auto bridge = AvioBridge::create(s, 0);
        REQUIRE(bridge != nullptr);
        CHECK_EQ(bridge->ctx()->seekable, static_cast<int>(AVIO_SEEKABLE_NORMAL));

        bridge.reset();
        syp_source_close(s);
    }

    // 分支二：total 未知 → 0。
    //
    // 没有用「响应缺 Content-Length」去构造——StubBackend 支持这么做
    // （Script::unknown_total_length），但那条路径要求完整走一遍下载到
    // EOF 才能确认 total 真的永久停留在未知（而不是还没到达），牵扯
    // 调度器的窗口/续传逻辑，属于本用例范围之外的复杂度。
    //
    // 用了一个同样真实、且更简单可靠的「未知」态：syp_source_open()
    // 刚返回、还没等到网络第一个响应的那一刻——SourceBridge::open() 本身
    // 不阻塞等待任何网络往返，新建的缓存目录里也没有旧的 total_length，
    // 所以此刻 syp_source_length() 就是 -1。这正是 Demuxer 可能
    // 构造 AvioBridge 的最早时机，同一条不变式（total 未知时必须
    // seekable=0）在这里被同样地覆盖到。用 Sync 桩 + 不调用 pump()，
    // 保证响应绝对不会提前到达，消除时序竞争。
    {
        Env e(StubBackend::Mode::Sync);
        e.stub.set_default_script(script_ok(4096));

        syp_source* s = nullptr;
        REQUIRE(syp_source_open(&s, "http://x/v", nullptr, &e.cfg, nullptr) == SYP_OK);
        REQUIRE(syp_source_length(s) == -1);

        auto bridge = AvioBridge::create(s, 0);
        REQUIRE(bridge != nullptr);
        CHECK_EQ(bridge->ctx()->seekable, 0);

        bridge.reset();
        syp_source_close(s);
    }
}

int main() { return tiny_test_main(); }
