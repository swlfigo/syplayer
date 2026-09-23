// test_preloader.cpp — 三级优先级的额度分配与"暖够就停"。
//
// 【为什么用 LoopbackServer 而不是 StubBackend】要断言的核心是"谁在真的
// 发请求、谁被停掉了"，而 hold_route() 能把请求精确地扣在"服务端已收到
// 请求行、还没写一个字节"那一刻——这一刻既可观测（requests_received_for）
// 又是静止的（没有数据到达 → 驱动线程不会被再次唤醒 → 额度分配结果不会
// 自己变）。StubBackend 是内存桩，给不了这种"卡住但可观测"的中间态。
//
// 【为什么每条断言都不是计时断言】所有等待都走谓词：
//   · wait_settled_for_test()   —— 等驱动线程把这一轮计划执行完并睡下；
//   · wait_terminal_for_test()  —— 等某条目进入 Done/Failed；
//   · wait_received()           —— 等服务端收到第 N 个请求；
//   · wait_state()              —— 等某条目进入指定状态（让路/复工）。
// 四者都带一个超时参数，但超时只用来把"挂死"变成"失败"，不承担任何断言：
// 判定一律是超时返回之后对逻辑状态的 CHECK。
#include "tiny_test.h"
#include "support/loopback_server.h"
#include "support/synthetic_byte.h"
#include "support/watchdog.h"

#include <dl/cache_file.h>
#include <dl/cache_index.h>
#include <dl/cache_store.h>
#include <dl/clock.h>
#include <dl/preloader.h>
#include <dl/rate_limiter.h>
#include <dl/source_bridge.h>
#include <platform/apple/apple_http_backend.h>

#include <syplayer/syp_config.h>
#include <syplayer/syp_preload.h>
#include <syplayer/syp_types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using syp::dl::PreloadConfig;
using syp::dl::PreloadPriority;
using syp::dl::PreloadState;
using syp::dl::PreloadStats;
using syp::dl::Preloader;
using syp::dl::test::LoopbackConfig;
using syp::dl::test::LoopbackServer;
using syp::dl::test::synthetic_byte;

namespace {

constexpr int kSoftMs = 5000;
constexpr int kHardMs = 60000;
constexpr const char* kHint =
    "本用例走真实回环 HTTP 服务器与 Preloader 的驱动线程。卡住通常意味着"
    "驱动线程与 SourceBridge::close() 之间出现了死锁（锁序见 preloader.h "
    "顶部），而不是下载慢。";

struct TempDir {
    std::filesystem::path p;
    TempDir() {
        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed);
        p = std::filesystem::temp_directory_path(ec)
            / ("syp-pre-" + std::to_string(::getpid()) + "-" + std::to_string(n));
        std::filesystem::create_directories(p, ec);
    }
    ~TempDir() {
        std::error_code ec;
        if (!p.empty()) std::filesystem::remove_all(p, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::vector<uint8_t> body(int64_t n) {
    std::vector<uint8_t> v(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) v[static_cast<size_t>(i)] = synthetic_byte(i);
    return v;
}

// 等服务端**收到**某条路径的请求数达到 n。谓词式等待，超时只为把挂死变成失败。
bool wait_received(const LoopbackServer& s, const std::string& path, int64_t n,
                   int timeout_ms = 8000) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (s.requests_received_for(path) >= n) return true;
        std::this_thread::yield();
    }
    return s.requests_received_for(path) >= n;
}

// 等某个条目进入指定状态。同样是谓词式等待：让路/复工这两件事由驱动线程
// 在"看到对等源出现/消失"之后才落到状态上，调用方不能假设它已经发生。
bool wait_state(const Preloader& p, const std::string& url, PreloadState want,
                int timeout_ms = 15000) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (p.state_for_test(url) == static_cast<int32_t>(want)) return true;
        std::this_thread::yield();
    }
    return p.state_for_test(url) == static_cast<int32_t>(want);
}

struct Env {
    TempDir     td;
    std::string dir;
    LoopbackServer server;
    syp_config  dl{};

    explicit Env(LoopbackConfig c = {}) : server(std::move(c)) {
        dir = td.p.string();
        syp_config_init(&dl);
        dl.cache_dir            = dir.c_str();
        dl.max_concurrent_tasks = 2;     // per_max = 2，配额算术好数
        dl.max_cache_bytes      = 0;
        dl.min_free_space_bytes = 0;
        dl.cache_ttl_ms         = 0;
        REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);
    }
    ~Env() { syp_set_http_backend(nullptr); }
    Env(const Env&)            = delete;
    Env& operator=(const Env&) = delete;

    // 【只能用 CHECK，不能用 REQUIRE】REQUIRE 展开成裸 `return;`
    // （tiny_test.h），放在非 void 的辅助函数里编译不过。
    std::unique_ptr<Preloader> make(PreloadConfig pc) {
        syp_status err = SYP_OK;
        auto p = Preloader::create(dl, pc, nullptr,
                                   syp::dl::current_http_backend(),
                                   syp::dl::system_clock(), &err);
        CHECK_EQ(err, SYP_OK);
        CHECK(p != nullptr);
        return p;
    }
};

}  // namespace

// 【C ABI 上的 cache_dir 归一化，preloader 这一半】
// syp_preloader_create() 把调用方给的 cache_dir 原样交给 Preloader，而
// Preloader 拿它去算 CacheStore::make_key（add() 里那一行）并落盘。此前
// 归一化只存在于 src/media 与 SYPBridge.mm，dl 够不到，于是直接用 C ABI 的
// 调用方混用两种拼法就会静默分裂成两份缓存、且**让路逻辑一起失效**
// （open_count 在另一个 key 上恒为 0）。现在收口在 Preloader 的构造函数里。
//
// 断言读的是 cache_dir_for_test()——本对象**真正拿去 make_key** 的那份字符串，
// 不是我们传进去的那个变量。SourceBridge 那一半在
// test_source_bridge.cpp 的 c_abi_cache_dir_spellings_share_one_registry_entry。
TEST_CASE(preloader_normalizes_cache_dir_from_the_c_abi) {
    Env e;
    const std::string want = e.dir;
    const std::string spellings[] = {e.dir + "/", e.dir + "//", e.dir + "/./",
                                     e.dir + "/x/.."};
    for (const std::string& alt : spellings) {
        syp_config dl = e.dl;
        dl.cache_dir = alt.c_str();
        syp_status err = SYP_OK;
        auto p = Preloader::create(dl, PreloadConfig{}, nullptr,
                                   syp::dl::current_http_backend(),
                                   syp::dl::system_clock(), &err);
        REQUIRE(p != nullptr);
        CHECK_EQ(p->cache_dir_for_test(), want);
        // 读回缝（dl 侧）看到的是同一份归一化后的目录：它读的是
        // base_config()，也就是本对象派给每一条 SourceBridge 的那份配置的起点。
        CHECK_EQ(std::string(p->dl_config_for_test().cache_dir), want);
    }
}

// 【容量策略在 preloader 这条链上真的会删东西】
// 上一条是**读回缝**，它断在 base_config() 上；但 config_for() / fetch_text()
// 在 base_config() **之后**还能再改一手，那一手任何缝都看不见（实测：在
// config_for 里把三个字段清零，ctest 33/33 与 xcodebuild 82/0 同时全绿）。
// 这一条是行为用例，钉的是最终后果：预加载开出去的源**必须**带着
// max_cache_bytes，于是它落盘时会把目录里没人引用的旧条目淘汰掉。
//
// 形状照 test_source_bridge.cpp 的
// close_enforces_capacity_evicting_others_never_its_own_key：
//   1) 先用一个不限容量的 preloader 把 victim 整份暖下来再析构 —— 磁盘上留
//      一份**没人引用**的缓存；
//   2) 再用一个 max_cache_bytes 远低于两者合计的 preloader 暖 keeper。
//      keeper 的源 close() 时 enforce_capacity 跑一轮，victim 被删。
//      （enforce_capacity 永远跳过当前还被引用的 key，所以 keeper 自己安全。）
// 这里不需要推时钟：节流判据是 now - last_enforce_ms_ >= 1000，而
// last_enforce_ms_ 初值 0、system_clock 的 now 是个很大的数，第一次就放行。
TEST_CASE(preloader_capacity_actually_evicts_an_unreferenced_entry) {
    syp::test::Watchdog wd("preloader_capacity_actually_evicts_an_unreferenced_entry",
                           kSoftMs, kHardMs, kHint);
    constexpr int64_t kEach = 64 * 1024;
    LoopbackConfig lc;
    lc.routes["/victim.mp4"] = body(kEach);
    lc.routes["/keeper.mp4"] = body(kEach);
    Env e(lc);

    const std::string victim = e.server.url("/victim.mp4");
    const std::string keeper = e.server.url("/keeper.mp4");
    const std::filesystem::path victim_idx =
        e.td.p / (syp::dl::CacheIndex::key_for_url(victim) + ".idx");

    PreloadConfig pc;
    pc.default_preload_bytes = kEach;      // 整份暖下来

    // 1) victim：不限容量，暖完就析构（引用计数归零，磁盘上留着）。
    {
        auto p = e.make(pc);                       // e.dl.max_cache_bytes == 0
        CHECK_EQ(p->add(victim, PreloadPriority::Next, 0), SYP_OK);
        CHECK_EQ(p->wait_terminal_for_test(victim, 15000),
                 static_cast<int32_t>(PreloadState::Done));
        p->wait_settled_for_test();
    }
    REQUIRE(std::filesystem::exists(victim_idx));

    // 2) keeper：上限远低于两者合计，它落盘那一刻 victim 必须被淘汰。
    {
        syp_config dl = e.dl;
        dl.max_cache_bytes = 4096;
        syp_status err = SYP_OK;
        auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                                   syp::dl::system_clock(), &err);
        REQUIRE(p != nullptr);
        CHECK_EQ(p->add(keeper, PreloadPriority::Next, 0), SYP_OK);
        CHECK_EQ(p->wait_terminal_for_test(keeper, 15000),
                 static_cast<int32_t>(PreloadState::Done));
        // 达标的那一轮只改状态，真正的 close()（也就是 enforce_capacity 的
        // 触发点）在下一轮；settled 就是"那一轮已经跑完"这个谓词。
        p->wait_settled_for_test();
    }
    // 【判据取"victim 的 .idx 没了"而不是"目录总字节 <= 上限"】后者在
    // keeper 自己还开着时恒不成立（enforce_capacity 跳过被引用的 key），
    // 会把一条真用例写成一条恒红的用例。
    CHECK(!std::filesystem::exists(victim_idx));
}

// 【容量/TTL 在 dl 这一支的读回缝】
// max_cache_bytes / min_free_space_bytes / cache_ttl_ms 三个字段从 syp_config
// 进来之后，**必须原样出现在 Preloader 派给每一条 SourceBridge 的那份配置里**
// ——它们是缓存"有上限、会过期、不会无限涨"的全部依据，而 config_for() 与
// fetch_text() 只该覆盖并发/超时/窗口这几类字段。
//
// 早先补的读回缝断在 MediaInfoProvider::cfg_ 上，那是**支流**：在
// PreloadStack::create 里 provider 构造之后、Preloader::create 之前把三个字段
// 清零，ctest 33/33 与 xcodebuild 82/0 同时全绿。这条缝（base_config()）补的
// 就是那个洞。三个值刻意互不相同、也不等于默认值，所以可以逐字段单独改动来验证。
TEST_CASE(preloader_carries_capacity_and_ttl_into_its_source_config) {
    Env e;
    syp_config dl = e.dl;
    dl.max_cache_bytes      = 12345678;
    dl.min_free_space_bytes = 2345678;
    dl.cache_ttl_ms         = 1500;
    syp_status err = SYP_OK;
    auto p = Preloader::create(dl, PreloadConfig{}, nullptr,
                               syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);
    const syp_config got = p->dl_config_for_test();
    CHECK_EQ(got.max_cache_bytes, static_cast<int64_t>(12345678));
    CHECK_EQ(got.min_free_space_bytes, static_cast<int64_t>(2345678));
    CHECK_EQ(got.cache_ttl_ms, static_cast<int64_t>(1500));
}

TEST_CASE(byte_target_downloads_then_stops) {
    syp::test::Watchdog wd("byte_target_downloads_then_stops", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/a.mp4"] = body(64 * 1024);
    Env e(lc);
    PreloadConfig pc;
    pc.default_preload_bytes = 16 * 1024;
    auto p = e.make(pc);

    CHECK_EQ(p->add(e.server.url("/a.mp4"), PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->wait_terminal_for_test(e.server.url("/a.mp4"), 15000),
             static_cast<int32_t>(PreloadState::Done));

    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.entries, static_cast<int64_t>(1));
    CHECK_EQ(st.completed, static_cast<int64_t>(1));
    CHECK(st.downloaded_bytes >= 16 * 1024);
    // 只暖了目标那一段，不是整份 64KiB（多下最多 quota-1 字节）。
    CHECK(st.downloaded_bytes < 32 * 1024);

    // 达标即关源。条目进 Done 的那一轮只改状态，真正的 close() 在下一轮——
    // 而进 Done 必然置 dirty_，所以"settled"就等价于"那一轮已经跑完"。
    // 这里等的是一个谓词，不是一段时间。
    p->wait_settled_for_test();
    PreloadStats s2{};
    p->get_stats(&s2);
    CHECK_EQ(s2.active_tasks, static_cast<int64_t>(0));
}

TEST_CASE(playing_beats_next_beats_background) {
    syp::test::Watchdog wd("playing_beats_next_beats_background", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/p.mp4"] = body(1 << 20);
    lc.routes["/n.mp4"] = body(1 << 20);
    lc.routes["/b.mp4"] = body(1 << 20);
    Env e(lc);
    // 三条路径全部扣住：一个字节都不会到达，配额分配结果因此是静止的。
    e.server.hold_route("/p.mp4");
    e.server.hold_route("/n.mp4");
    e.server.hold_route("/b.mp4");

    PreloadConfig pc;
    pc.max_total_tasks       = 4;
    pc.reserved_for_playing  = 2;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);

    const std::string up = e.server.url("/p.mp4");
    const std::string un = e.server.url("/n.mp4");
    const std::string ub = e.server.url("/b.mp4");
    CHECK_EQ(p->add(ub, PreloadPriority::Background, 0), SYP_OK);
    CHECK_EQ(p->add(un, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->add(up, PreloadPriority::Playing, 0), SYP_OK);
    p->wait_settled_for_test();

    // total=4，有 Playing → budget=4；per_max=dl.max_concurrent_tasks=2。
    // 排序：Playing(p) → Next(n) → Background(b)。
    CHECK_EQ(p->quota_for_test(up), 2);
    CHECK_EQ(p->quota_for_test(un), 2);
    CHECK_EQ(p->quota_for_test(ub), 0);           // 额度耗尽，让路
    CHECK_EQ(p->state_for_test(ub), static_cast<int32_t>(PreloadState::Pending));

    CHECK(wait_received(e.server, "/p.mp4", 1));
    CHECK(wait_received(e.server, "/n.mp4", 1));
    CHECK_EQ(e.server.requests_received_for("/b.mp4"), static_cast<int64_t>(0));

    e.server.release_route("/p.mp4");
    e.server.release_route("/n.mp4");
    e.server.release_route("/b.mp4");
}

TEST_CASE(no_playing_entry_reserves_slots_for_the_player) {
    syp::test::Watchdog wd("no_playing_entry_reserves_slots_for_the_player",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/n.mp4"] = body(1 << 20);
    lc.routes["/b.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/n.mp4");
    e.server.hold_route("/b.mp4");

    PreloadConfig pc;
    pc.max_total_tasks      = 4;
    pc.reserved_for_playing = 2;        // 没有 Playing 条目 → budget = 4-2 = 2
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);

    const std::string un = e.server.url("/n.mp4");
    const std::string ub = e.server.url("/b.mp4");
    CHECK_EQ(p->add(un, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->add(ub, PreloadPriority::Background, 0), SYP_OK);
    p->wait_settled_for_test();

    CHECK_EQ(p->quota_for_test(un), 2);
    CHECK_EQ(p->quota_for_test(ub), 0);
    CHECK(wait_received(e.server, "/n.mp4", 1));
    CHECK_EQ(e.server.requests_received_for("/b.mp4"), static_cast<int64_t>(0));

    e.server.release_route("/n.mp4");
    e.server.release_route("/b.mp4");
}

TEST_CASE(raising_priority_stops_the_lower_one) {
    syp::test::Watchdog wd("raising_priority_stops_the_lower_one", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/x.mp4"] = body(1 << 20);
    lc.routes["/y.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/x.mp4");
    e.server.hold_route("/y.mp4");

    PreloadConfig pc;
    pc.max_total_tasks       = 2;
    pc.reserved_for_playing  = 0;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);

    const std::string ux = e.server.url("/x.mp4");
    const std::string uy = e.server.url("/y.mp4");
    CHECK_EQ(p->add(ux, PreloadPriority::Background, 0), SYP_OK);
    CHECK_EQ(p->add(uy, PreloadPriority::Background, 0), SYP_OK);
    p->wait_settled_for_test();
    CHECK_EQ(p->quota_for_test(ux), 2);          // 先到先得，把 2 个额度占满
    CHECK_EQ(p->quota_for_test(uy), 0);
    CHECK(wait_received(e.server, "/x.mp4", 1));

    // y 升到 Playing：x 必须被停（quota 掉到 0、回到 Pending）。
    CHECK_EQ(p->set_priority(uy, PreloadPriority::Playing), SYP_OK);
    p->wait_settled_for_test();
    CHECK_EQ(p->quota_for_test(uy), 2);
    CHECK_EQ(p->quota_for_test(ux), 0);
    CHECK_EQ(p->state_for_test(ux), static_cast<int32_t>(PreloadState::Pending));
    CHECK(wait_received(e.server, "/y.mp4", 1));

    e.server.release_route("/x.mp4");
    e.server.release_route("/y.mp4");
}

TEST_CASE(remove_interrupts_inflight_and_keeps_bytes) {
    syp::test::Watchdog wd("remove_interrupts_inflight_and_keeps_bytes",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/r.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/r.mp4");

    PreloadConfig pc;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);
    const std::string u = e.server.url("/r.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    p->wait_settled_for_test();
    CHECK(wait_received(e.server, "/r.mp4", 1));

    // 扣住的请求被 remove 打断：不挂死、条目消失。
    p->remove(u);
    p->wait_settled_for_test();
    CHECK_EQ(p->state_for_test(u), -1);          // -1 = 没有这个条目
    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.entries, static_cast<int64_t>(0));
    e.server.release_route("/r.mp4");
}

TEST_CASE(destroy_with_inflight_does_not_hang) {
    syp::test::Watchdog wd("destroy_with_inflight_does_not_hang", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/d.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/d.mp4");

    PreloadConfig pc;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);
    CHECK_EQ(p->add(e.server.url("/d.mp4"), PreloadPriority::Next, 0), SYP_OK);
    p->wait_settled_for_test();
    CHECK(wait_received(e.server, "/d.mp4", 1));
    p.reset();                                   // 析构：interrupt + close + join
    CHECK(true);                                 // 走到这里就是没挂
    e.server.release_route("/d.mp4");
}

TEST_CASE(add_is_idempotent_and_raises_priority) {
    syp::test::Watchdog wd("add_is_idempotent_and_raises_priority", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/i.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/i.mp4");
    PreloadConfig pc;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);
    const std::string u = e.server.url("/i.mp4");

    CHECK_EQ(p->add(u, PreloadPriority::Background, 0), SYP_OK);
    CHECK_EQ(p->add(u, PreloadPriority::Playing, 0), SYP_OK);
    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.entries, static_cast<int64_t>(1));   // 不重复建条目
    p->wait_settled_for_test();
    CHECK(p->quota_for_test(u) > 0);
    e.server.release_route("/i.mp4");
}

TEST_CASE(failed_url_becomes_failed_not_stuck) {
    syp::test::Watchdog wd("failed_url_becomes_failed_not_stuck", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/ok.mp4"] = body(1024);            // 路由模式下别的路径一律 404
    Env e(lc);
    PreloadConfig pc;
    auto p = e.make(pc);
    const std::string u = e.server.url("/missing.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->wait_terminal_for_test(u, 15000),
             static_cast<int32_t>(PreloadState::Failed));
    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.failed, static_cast<int64_t>(1));
    // 终态只记一次：驱动线程随后还要再跑一轮把 source 关掉，那一轮不能
    // 把同一条目再算一次失败。
    p->wait_settled_for_test();
    PreloadStats st2{};
    p->get_stats(&st2);
    CHECK_EQ(st2.failed, static_cast<int64_t>(1));
}

// 【头文件给出的唯一出路 `remove() + 再 add()` 按字面照做无效】
//
// 终态（Done/Failed）是粘的，preloader.h 里"当前的出路只有调用方自己
// remove() + 再 add()"是它给出的**唯一**escape。可是 remove() 只置
// `removing = true`，条目要等驱动线程下一次 commit() 才真摘掉；而 add()
// 撞上这条还没摘掉的旧条目时原先只做 `e->removing = false`，**不重置
// state / last_error** ⇒ 把那条 Failed 条目原封不动救活了，还返回 SYP_OK。
// 实测背靠背 remove+add（调用方最自然的写法）**5/5 全中：条目终身
// Failed，一个新请求都没发**；而"先等条目真的消失再 add"公开 C API
// **做不到**（syp_preload_stats 只有 entries 总数，没有 per-URL 信号；
// state_for_test / wait_settled_for_test 都是测试缝）。
//
// 修法是 add() 撞上终态条目时就地复位（state=Pending、last_error=SYP_OK，
// 播放列表条目还要清 expanded）。这条用例跑 5 轮，理由同上：单轮绿有可能
// 是撞上了"条目恰好已经被摘掉"那个窗口。
TEST_CASE(add_after_remove_revives_a_terminal_entry) {
    syp::test::Watchdog wd("add_after_remove_revives_a_terminal_entry",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/ok.mp4"] = body(1024);      // 路由模式：别的路径一律 404
    Env e(lc);
    PreloadConfig pc;
    pc.default_preload_bytes = 32 * 1024;
    auto p = e.make(pc);

    for (int round = 0; round < 5; ++round) {
        const std::string path = "/late" + std::to_string(round) + ".mp4";
        const std::string u = e.server.url(path);
        // 1) 先让它失败（404）。
        CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
        CHECK_EQ(p->wait_terminal_for_test(u, 15000),
                 static_cast<int32_t>(PreloadState::Failed));
        const int64_t before = e.server.requests_received_for(path);

        // 2) 外部条件变了：资源现在有了。
        e.server.set_route(path, body(64 * 1024), "video/mp4");

        // 3) **背靠背** remove + add —— 调用方照着头文件最自然的写法。
        //    这中间刻意不 wait_settled：等条目真的消失正是公开 API 做不到的事。
        p->remove(u);
        CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);

        // 复位之后这条目必须重新下、并且下成。
        CHECK_EQ(p->wait_terminal_for_test(u, 20000),
                 static_cast<int32_t>(PreloadState::Done));
        // 光看状态还不够：Done 也可能是"条目被摘掉后重建"的结果。要的是
        // 真的又发了请求。
        CHECK(e.server.requests_received_for(path) > before);
    }
}

// 【分配到的额度必须**抵达**真正开出去的那条源】
//
// 把 config_for() 里那行 `c.max_concurrent_tasks = quota > 0 ? quota : 1;` 删掉之后，
// `ctest 33/33` + `xcodebuild 82/0` **双绿存活**。结构性原因：本文件里 14 处
// 额度断言**全部**读 quota_for_test()——那是**算术缝**，它只证明"额度算对了"，
// 不证明"算出来的那个数被交给了 SourceBridge"。这条用例读的是
// source_config_for_test()：install 时从 Action::cfg 抄下来的、**真正传给
// SourceBridge::open 的那一份**（不是事后再调一次 config_for 算出来的——
// 那样的缝读的是同一个坏函数，照样绿）。
//
// 构造让"额度"与"dl 配置里的并发数"必然不同，否则错误实现可以靠巧合活下来：
//   dl.max_concurrent_tasks = 4（per_max = clamp(4,1,total=3) = 3）
//   max_total_tasks = 3、reserved = 0 ⇒ 这条目的额度 = 3 ≠ 4。
TEST_CASE(the_allocated_quota_reaches_the_source_that_is_actually_opened) {
    syp::test::Watchdog wd("the_allocated_quota_reaches_the_source_that_is_actually_opened",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/q.mp4"] = body(1 << 20);
    Env e(lc);
    e.dl.max_concurrent_tasks = 4;
    e.server.hold_route("/q.mp4");     // 扣住：条目停在 Running，状态是静止的

    PreloadConfig pc;
    pc.max_total_tasks       = 3;
    pc.reserved_for_playing  = 0;
    pc.default_preload_bytes = 300 * 1024;
    auto p = e.make(pc);

    const std::string u = e.server.url("/q.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    CHECK(wait_state(*p, u, PreloadState::Running));
    CHECK(wait_received(e.server, "/q.mp4", 1));
    CHECK_EQ(p->quota_for_test(u), 3);           // 算术缝（既有的那一类）

    const syp_config c = p->source_config_for_test(u);
    // 【这一条就是那处曾经双绿存活的错误实现的落点】
    CHECK_EQ(c.max_concurrent_tasks, 3);         // 不是 dl 里的 4
    // per_conn = ceil(300KiB / 3)。窗口 = per_conn × 并发 = 300KiB。
    CHECK_EQ(c.min_segment_size, static_cast<int64_t>(102400));
    CHECK_EQ(c.segment_size_hint, static_cast<int64_t>(0));
    CHECK_EQ(c.first_buffer_ms, 0);
    CHECK_EQ(c.target_buffer_ms, 0);
    // 容量三件套原样穿过（config_for 只该改并发/超时/窗口）。
    CHECK_EQ(std::string(c.cache_dir), e.dir);

    e.server.release_route("/q.mp4");
}

namespace {

// 往某个 URL 的缓存里**预先**写进几段互不相邻的区间，制造带洞的起点。
// 用真实字节（与服务端 body 一致），免得留下一份内容不对的缓存。
void seed_cache_with_holes(const std::string& dir, const std::string& url,
                           const std::vector<uint8_t>& src,
                           const std::vector<syp::dl::Range>& rs) {
    syp_config c{};
    syp_config_init(&c);
    c.cache_dir            = dir.c_str();
    c.cache_ttl_ms         = 0;
    c.max_cache_bytes      = 0;
    c.min_free_space_bytes = 0;
    syp_status err = SYP_OK;
    auto h = syp::dl::CacheStore::get().acquire(dir, url, c, &err);
    CHECK(h.valid());
    if (!h.valid()) return;
    {
        std::lock_guard<std::mutex> g(*h.mu);
        for (const syp::dl::Range& r : rs) {
            const auto n = static_cast<size_t>(r.end - r.start);
            CHECK(h.file->write_at(r.start,
                                   std::span<const uint8_t>(
                                       src.data() + static_cast<size_t>(r.start), n))
                      .has_value());
            h.index->add_range(r);
        }
        CHECK(h.index->save().has_value());
    }
    syp::dl::CacheStore::get().release(h.key);
}

}  // namespace

// 【额度的**量级**到底改不改并发？】
//
// 这里给出了一条更值钱的负面结果：它没能在回环用例里造出行为差异，
// 因为 lookahead_bytes_locked() 算出的窗口恰好等于 goal，而**首次调度时
// total_length_ < 0 ⇒ planned_tasks_locked() 返回 1** ⇒ 整个窗口交给第一条
// 任务、窗口里没有洞。实测 quota=1 与 quota=4 都是 peak_concurrent=1、
// requests=1。于是结论是"一个预加载条目在线上只占一条连接"。
//
// 这条用例把那个结论**再往前推一格**：负面结果只对"缓存是空的"这个起点成立。
// 如果窗口里本来就有洞（同一个 cache key 上先跑过一条真正在播的源——它的并发
// 更高、还会 seek，留下带洞的缓存；预加载让路结束后正是从这个起点复工），
// 首次调度那条任务只认领第一个洞，剩下的洞在 206 + 总长到手之后由
// max_tasks_locked() = quota 决定能同时开几条。
//
// 所以两个起点各跑一遍、把数字打出来，让"改不改并发"有据可查，而不是靠推理。
// 断言只取方向性的那一条（带洞起点下 quota 大的那次峰值不低于 quota=1 那次），
// 具体数值只打印——真实峰值受回环速度影响，钉死它就是在量性能。
TEST_CASE(does_quota_magnitude_change_concurrency_on_the_preload_path) {
    syp::test::Watchdog wd("does_quota_magnitude_change_concurrency_on_the_preload_path",
                           20000, 120000, kHint);
    constexpr int64_t kLen  = 1 << 20;
    constexpr int64_t kGoal = 256 * 1024;
    const std::vector<uint8_t> src = body(kLen);

    // quota 与 seed 的两两组合，各自独立的服务器 + 缓存目录。
    const auto run = [&](int32_t quota, bool seeded) -> std::pair<int32_t, int64_t> {
        LoopbackConfig lc;
        lc.routes["/w.mp4"] = src;
        // 放慢 body：不放慢的话回环上一个洞在下一个洞开始之前就下完了，
        // 峰值恒为 1，量到的是"回环太快"，不是"并发上限"。
        lc.body_chunk_bytes    = 4096;
        lc.body_chunk_delay_ms = 15;
        Env e(lc);
        e.dl.max_concurrent_tasks = quota;
        const std::string u = e.server.url("/w.mp4");
        if (seeded) {
            // 窗口 [0,256K) 里留三个洞：[0,64K) [96K,160K) [192K,256K)。
            seed_cache_with_holes(e.dir, u, src,
                                  {syp::dl::Range{64 * 1024, 96 * 1024},
                                   syp::dl::Range{160 * 1024, 192 * 1024}});
        }
        PreloadConfig pc;
        pc.max_total_tasks       = quota;
        pc.reserved_for_playing  = 0;
        pc.default_preload_bytes = kGoal;
        auto p = e.make(pc);
        CHECK(p != nullptr);
        if (p == nullptr) return {-1, -1};
        CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
        CHECK_EQ(p->wait_terminal_for_test(u, 60000),
                 static_cast<int32_t>(PreloadState::Done));
        // 额度**抵达**了那条源：读的是 open_cfg（进终态之后 quota_for_test
        // 已经被 allocate_locked 清成 0，那条缝在这里没有鉴别力）。
        CHECK_EQ(p->source_config_for_test(u).max_concurrent_tasks, quota);
        return {e.server.peak_concurrent_requests(), e.server.total_requests()};
    };

    const auto cold1 = run(1, false);
    const auto cold4 = run(4, false);
    const auto holey1 = run(1, true);
    const auto holey4 = run(4, true);
    std::printf("  [quota-vs-concurrency] cold  quota=1 peak=%d reqs=%lld | "
                "quota=4 peak=%d reqs=%lld\n",
                cold1.first, static_cast<long long>(cold1.second),
                cold4.first, static_cast<long long>(cold4.second));
    std::printf("  [quota-vs-concurrency] holey quota=1 peak=%d reqs=%lld | "
                "quota=4 peak=%d reqs=%lld\n",
                holey1.first, static_cast<long long>(holey1.second),
                holey4.first, static_cast<long long>(holey4.second));

    // 冷起点：无论额度多大，窗口里没有洞 ⇒ 只有一条连接。这是上面那条负面
    // 结果，钉住它，将来有人改窗口公式时会红。
    CHECK_EQ(cold1.first, 1);
    CHECK_EQ(cold4.first, 1);
    // 带洞起点：额度是上界，方向必须是"大额度不比小额度少"。
    CHECK_EQ(holey1.first, 1);
    CHECK(holey4.first >= holey1.first);
}

TEST_CASE(add_rejects_bad_args) {
    Env e;
    PreloadConfig pc;
    auto p = e.make(pc);
    CHECK_EQ(p->add("", PreloadPriority::Next, 0), SYP_ERR_INVALID_ARG);
    CHECK_EQ(p->set_priority("http://nope/", PreloadPriority::Next), SYP_ERR_INVALID_ARG);
    p->remove("http://nope/");     // no-op，不崩
    p->remove_all();
    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.entries, static_cast<int64_t>(0));
}

// 【播放源一开在同一个 cache key 上，预加载必须立刻让路】
//
// 共享索引让**读**路径看得见对等源下好的区间，但每个 SourceBridge 的
// Scheduler 只在 open 那一刻被喂了一次已有区间。于是同一个 key 上同时挂着
// 一条预加载和一条在播的源时，两边会对**同一批字节**各发一次 Range 请求：
// 这正是预加载存在的意义的反面。
//
// 解法选的是"让路"而不是"把对等区间灌进调度器"：预加载条目发现这个 key 上
// 有别人打开着，就把 source 关掉（已下的字节留在缓存里，播放侧 open 时一次
// 性看得见），对方走了再复工。断言分两段：
//   1. 播放源 open 之后，条目必须退回 Pending、活跃任务归零；
//   2. 播放源关掉之后，条目必须复工——服务端会收到预加载发来的**新**请求。
TEST_CASE(preload_yields_the_key_to_a_real_player_and_resumes_after) {
    syp::test::Watchdog wd("preload_yields_the_key_to_a_real_player_and_resumes_after",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/s.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/s.mp4");     // 一个字节都不落地：状态是静止的

    PreloadConfig pc;
    pc.max_total_tasks       = 1;      // quota 恒为 1 → 一条连接一条请求，好数
    pc.reserved_for_playing  = 0;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);

    const std::string u = e.server.url("/s.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    CHECK(wait_state(*p, u, PreloadState::Running));
    CHECK_EQ(p->quota_for_test(u), 1);
    CHECK(wait_received(e.server, "/s.mp4", 1));
    CHECK_EQ(p->peer_yield_for_test(u), 0);        // 还没有对等源

    // 播放侧打开同一个 URL、同一个 cache_dir —— 同一个 cache key。
    syp_config player_cfg = e.dl;
    player_cfg.cache_dir  = e.dir.c_str();
    auto player = syp::dl::SourceBridge::open(u, nullptr, player_cfg, nullptr,
                                              syp::dl::current_http_backend(),
                                              syp::dl::system_clock());
    REQUIRE(player.has_value());

    CHECK(wait_state(*p, u, PreloadState::Pending));
    CHECK_EQ(p->peer_yield_for_test(u), 1);
    CHECK_EQ(p->quota_for_test(u), 0);
    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.active_tasks, static_cast<int64_t>(0));   // source 真的关了
    const int64_t after_yield = e.server.requests_received_for("/s.mp4");

    // 播放源走了：预加载复工，服务端必然再收到一条**新**请求。
    player->reset();
    CHECK(wait_state(*p, u, PreloadState::Running));
    CHECK_EQ(p->peer_yield_for_test(u), 0);
    CHECK(wait_received(e.server, "/s.mp4", after_yield + 1));

    e.server.release_route("/s.mp4");
}

// 【一条 Playing 条目让路时，保底额度必须开始生效】
//
// budget 取决于 has_playing，而 has_playing 取的是"这一轮我们真的会去下的
// 条目"。一条 Playing 条目 peer_busy，意味着这个 key 上真的有一个**本
// preloader 之外**的播放源开着——那正是 reserved_for_playing 存在的场景，
// 所以它退出 live、has_playing 翻 false、保底额度开始生效，是对的方向。
// 反过来（让路中的条目照样算进 has_playing）会在真播放源刚起来的那一刻
// 把整份 total 发给后台预加载，与这里的设计意图正好相反。
//
// 构造让两种实现必然分叉：total=4、reserve=2、per_max=2，三个条目
// p(Playing) / n(Next) / b(Background)。p 让路之后
//   · has_playing 只看 live（正确）：budget = 4-2 = 2 → n=2、b=0；
//   · has_playing 把让路的也算上（错）：budget = 4 → n=2、b=2。
// 判据落在 b 上。为了让"b 的 0"不是从头到尾都成立的废断言，先让 n 变成
// Done 之后把 b 抬起来测一次 —— 见下面第二段。
TEST_CASE(reserve_still_applies_while_a_peer_plays_the_key) {
    syp::test::Watchdog wd("reserve_still_applies_while_a_peer_plays_the_key",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/yp.mp4"] = body(1 << 20);
    lc.routes["/yn.mp4"] = body(1 << 20);
    lc.routes["/yb.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/yp.mp4");
    e.server.hold_route("/yn.mp4");
    e.server.hold_route("/yb.mp4");

    PreloadConfig pc;
    pc.max_total_tasks       = 4;
    pc.reserved_for_playing  = 2;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);

    const std::string up = e.server.url("/yp.mp4");
    const std::string un = e.server.url("/yn.mp4");
    const std::string ub = e.server.url("/yb.mp4");
    CHECK_EQ(p->add(up, PreloadPriority::Playing, 0), SYP_OK);
    CHECK_EQ(p->add(un, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->add(ub, PreloadPriority::Background, 0), SYP_OK);
    CHECK(wait_state(*p, up, PreloadState::Running));
    p->wait_settled_for_test();
    // 有 Playing 条目在跑 → budget = total = 4，被 p、n 各拿 2 吃满。
    CHECK_EQ(p->quota_for_test(up), 2);
    CHECK_EQ(p->quota_for_test(un), 2);
    CHECK_EQ(p->quota_for_test(ub), 0);

    // 播放侧开在 p 的 URL 上：p 让路，保底额度开始生效 → budget = 4-2 = 2。
    syp_config player_cfg = e.dl;
    player_cfg.cache_dir  = e.dir.c_str();
    auto player = syp::dl::SourceBridge::open(up, nullptr, player_cfg, nullptr,
                                              syp::dl::current_http_backend(),
                                              syp::dl::system_clock());
    REQUIRE(player.has_value());

    CHECK(wait_state(*p, up, PreloadState::Pending));
    CHECK_EQ(p->peer_yield_for_test(up), 1);
    CHECK_EQ(p->quota_for_test(up), 0);
    CHECK_EQ(p->quota_for_test(un), 2);
    CHECK_EQ(p->quota_for_test(ub), 0);   // ← 把让路的算进 has_playing 时是 2

    // 【让这条断言不是废话】把 n 也撤掉：让路那条仍然 peer_busy，于是
    // budget 还是 2，b 这时**必须**拿到 2。同一个 b，前后两次分别是 0 和 2，
    // 证明上面那个 0 是保底额度挡出来的，不是"b 永远拿不到额度"。
    p->remove(un);
    p->wait_settled_for_test();
    CHECK_EQ(p->peer_yield_for_test(up), 1);
    CHECK_EQ(p->quota_for_test(ub), 2);

    player->reset();
    e.server.release_route("/yp.mp4");
    e.server.release_route("/yn.mp4");
    e.server.release_route("/yb.mp4");
}

// 【公开方法不得阻塞在一次索引落盘上】
//
// SourceBridge 的两条路径都会在**握着桥的 mu_** 的情况下去取句柄锁：
//   · get_stats()  —— 读 index->ranges().total_bytes()；
//   · persist_chunk() —— 每 256KiB 一次 sync() + save()。
// 而句柄锁在落盘期间是被后端线程握着的。于是只要 Preloader 在自己的 mu_ 下
// 碰桥（无论是 get_stats 还是 interrupt），主线程的一次公开调用就会连着
// add / set_priority / remove / 驱动线程一起等那次 fsync —— 而 preloader.h
// 承诺的恰恰是"公开方法不做任何阻塞 IO"。
//
// 【怎么把那一刻确定性地造出来】测试线程自己握住同一个 cache key 的句柄锁：
//   1. acquire() 拿到句柄，**立刻 release()**。Handle 自带 shared_ptr，对象
//      照样活着、还是同一把锁；而 open_count 回到 1（只剩桥自己），预加载
//      因此不会把我们误判成"播放源"而让路——让路会把 source 从条目上摘走，
//      要验的那条路径就被解除了武装（round 1 的版本正是栽在这里）。
//   2. 锁住 *h.mu。此后驱动线程的下一轮 measure() 会调 get_stats()：它先取
//      桥的 mu_、再去取句柄锁，于是**握着桥的 mu_ 卡在那里**。
//   3. driver_busy_for_test() 一旦变真就不会再变回去（驱动线程出不来），
//      这就是"此刻桥的 mu_ 被别人拿着"的稳定谓词——不需要睡。
//   4. 这时调公开方法。旧实现里 remove() 会在 mu_ 下 interrupt() → 取桥的
//      mu_ → 永远卡住；get_stats() 同理。新实现全部立即返回。
TEST_CASE(public_methods_do_not_block_on_an_index_flush) {
    syp::test::Watchdog wd("public_methods_do_not_block_on_an_index_flush",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/f.mp4"] = body(1 << 20);
    lc.routes["/o.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/f.mp4");
    e.server.hold_route("/o.mp4");

    PreloadConfig pc;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);
    const std::string u = e.server.url("/f.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    CHECK(wait_state(*p, u, PreloadState::Running));   // 必须真有一条源开着

    syp_status err = SYP_OK;
    auto h = syp::dl::CacheStore::get().acquire(e.dir, u, e.dl, &err);
    REQUIRE(h.valid());
    // 引用立刻还回去：只留下 shared_ptr，不留下"有对等源"的假象。
    syp::dl::CacheStore::get().release(h.key);
    {
        // 句柄锁在手 = "这个 key 正在落盘"。
        std::lock_guard<std::mutex> g(*h.mu);

        // 等驱动线程真的卡进桥里（它握着桥的 mu_ 等这把句柄锁）。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline && !p->driver_busy_for_test()) {
            std::this_thread::yield();
        }
        CHECK(p->driver_busy_for_test());
        // 让路没有发生 —— 否则 source 已经被摘走，下面几条就测不到东西了。
        // （acquire 与 release 之间有一个几微秒的窗口，驱动线程恰好在那一刻
        //  探到 open_count=2 的概率可以忽略；真撞上时这条 CHECK 会明说。）
        CHECK_EQ(p->peer_yield_for_test(u), 0);

        PreloadStats st{};
        p->get_stats(&st);                                   // 旧实现挂在这一行
        CHECK_EQ(st.entries, static_cast<int64_t>(1));
        CHECK_EQ(p->set_priority(u, PreloadPriority::Playing), SYP_OK);
        CHECK_EQ(p->add(e.server.url("/o.mp4"), PreloadPriority::Next, 0), SYP_OK);
        p->remove(u);            // 旧实现在 mu_ 下 interrupt()：也挂在这一行
        p->remove_all();
    }
    // 【这里原本还有一次 release(h.key)，注释写着"多还一次是 no-op（key 已摘）"
    //   —— 两句都是假的，实测后删掉】引用早在上面那次
    //   release 就还清了（acquire 之后立刻还），而预加载的桥自己还开着，
    //   所以这一次多还会把 open_count 从 1 压到 0，**把一条还在被使用的注册表
    //   条目直接摘掉**。key 并没有"已摘"，恰恰相反。
    e.server.release_route("/f.mp4");
    e.server.release_route("/o.mp4");
}

// 【走公开 C API 一遍】上面所有用例走的是 C++ 内部类型 Preloader；这一条
// 从 syp_preload.h 声明的 C ABI 进，验证薄壳层（syp_preload_api.cpp）的
// 转发、struct_size 前向兼容、以及空参数不崩这三件事。
TEST_CASE(c_api_roundtrip) {
    syp::test::Watchdog wd("c_api_roundtrip", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/c.mp4"] = body(64 * 1024);
    Env e(lc);

    syp_preload_config pc{};
    syp_preload_config_init(&pc);
    CHECK_EQ(pc.max_total_tasks, 6);
    CHECK_EQ(pc.reserved_for_playing, 3);
    CHECK_EQ(pc.default_preload_bytes, static_cast<int64_t>(1) << 20);
    CHECK_EQ(pc.default_preload_ms, static_cast<int64_t>(3000));
    CHECK_EQ(pc.struct_size, static_cast<uint32_t>(sizeof(syp_preload_config)));
    pc.default_preload_bytes = 16 * 1024;

    syp_preloader* p = syp_preloader_create(&e.dl, &pc, nullptr);
    REQUIRE(p != nullptr);

    const std::string u = e.server.url("/c.mp4");
    CHECK_EQ(syp_preloader_add(p, u.c_str(), SYP_PRELOAD_PRIORITY_NEXT, 0), SYP_OK);
    CHECK_EQ(syp_preloader_set_priority(p, u.c_str(), SYP_PRELOAD_PRIORITY_PLAYING),
             SYP_OK);

    // 没有测试缝可用（公开 API 不暴露它），用 stats 上的 completed 做谓词式等待。
    syp_preload_stats st{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        syp_preloader_get_stats(p, &st);
        if (st.completed >= 1 || st.failed >= 1) break;
        std::this_thread::yield();
    }
    CHECK_EQ(st.completed, static_cast<int64_t>(1));
    CHECK_EQ(st.entries, static_cast<int64_t>(1));

    syp_preloader_remove(p, u.c_str());
    syp_preloader_remove_all(p);
    syp_preloader_destroy(p);

    // 空参数一律不崩。
    CHECK(syp_preloader_create(nullptr, &pc, nullptr) == nullptr);
    CHECK_EQ(syp_preloader_add(nullptr, "x", SYP_PRELOAD_PRIORITY_NEXT, 0),
             SYP_ERR_INVALID_ARG);
    syp_preloader_destroy(nullptr);
    syp_preloader_get_stats(nullptr, &st);
    syp_preload_config_init(nullptr);
}

namespace {

// 一个**确定性的假 provider**：不碰网络，按固定比例把 ms 折成字节。
// 预加载侧要验的是"装了 provider 就走时间路、没装/估不出就退化成字节路"，
// 这是纯粹的分支选择，用真 FFmpeg 去验只会把两件事搅在一起
// （真 provider 的算术在 test_media_info_provider 里单独验）。
struct FakeProvider {
    int64_t bytes_per_ms = 32;          // 1000ms → 32000 字节
    std::string reject_url;             // 命中就返回 NOT_IMPLEMENTED
    std::atomic<int> calls{0};
    std::atomic<int64_t> last_ms{-1};   // 最后一次被问到的毫秒数

    static syp_status est(void* ctx, const char* url, int64_t ms,
                          int64_t* s, int64_t* e) {
        auto* self = static_cast<FakeProvider*>(ctx);
        self->calls.fetch_add(1, std::memory_order_relaxed);
        self->last_ms.store(ms, std::memory_order_relaxed);
        if (url != nullptr && self->reject_url == url) return SYP_ERR_NOT_IMPLEMENTED;
        if (s != nullptr) *s = 0;
        if (e != nullptr) *e = ms * self->bytes_per_ms;
        return SYP_OK;
    }
    syp::dl::MediaInfoProvider table() {
        syp::dl::MediaInfoProvider t;
        t.ctx = this;
        t.estimate_range_for_ms = &FakeProvider::est;
        return t;
    }
    // 同一张表的公开 ABI 形状，给 syp_preloader_create 用。
    syp_media_info_provider c_table() {
        syp_media_info_provider t{};
        t.ctx = this;
        t.estimate_range_for_ms = &FakeProvider::est;
        return t;
    }
};

}  // namespace

TEST_CASE(time_target_uses_provider) {
    syp::test::Watchdog wd("time_target_uses_provider", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/t.mp4"] = body(256 * 1024);
    Env e(lc);
    FakeProvider fp;
    const auto tbl = fp.table();

    PreloadConfig pc;
    pc.default_preload_bytes = 4 * 1024;      // 故意设得很小，好区分两条路
    pc.default_preload_ms    = 1000;          // → 32000 字节
    syp_status err = SYP_OK;
    auto p = Preloader::create(e.dl, pc, &tbl, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(err == SYP_OK);
    REQUIRE(p != nullptr);

    const std::string u = e.server.url("/t.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);   // ms_or_zero=0 → 用默认 ms
    CHECK_EQ(p->wait_terminal_for_test(u, 15000),
             static_cast<int32_t>(PreloadState::Done));
    CHECK(fp.calls.load(std::memory_order_relaxed) >= 1);

    PreloadStats st{};
    p->get_stats(&st);
    // 按时间那条路：32000 字节上下（多下最多 quota-1），远大于 4KiB 的字节默认值。
    CHECK(st.downloaded_bytes >= 32000);
    CHECK(st.downloaded_bytes < 64 * 1024);
    CHECK_EQ(st.provider_miss, static_cast<int64_t>(0));
}

TEST_CASE(provider_miss_falls_back_to_default_bytes) {
    syp::test::Watchdog wd("provider_miss_falls_back_to_default_bytes",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/m.mp4"] = body(256 * 1024);
    Env e(lc);
    FakeProvider fp;
    const std::string u = e.server.url("/m.mp4");
    fp.reject_url = u;
    const auto tbl = fp.table();

    PreloadConfig pc;
    pc.default_preload_bytes = 8 * 1024;
    pc.default_preload_ms    = 1000;
    syp_status err = SYP_OK;
    auto p = Preloader::create(e.dl, pc, &tbl, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);

    CHECK_EQ(p->add(u, PreloadPriority::Next, 2000), SYP_OK);
    CHECK_EQ(p->wait_terminal_for_test(u, 15000),
             static_cast<int32_t>(PreloadState::Done));
    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.provider_miss, static_cast<int64_t>(1));
    CHECK(st.downloaded_bytes >= 8 * 1024);
    CHECK(st.downloaded_bytes < 24 * 1024);   // 用的是 8KiB 那条路，不是 2000ms
}

// struct_size 的前向兼容契约：这条约定写在 syp_preload.h
// 的注释里、实现在 copy_preload_config 里，但在本轮之前**没有任何用例碰过**
// ——syp_config 那边同样是空白（那半边由 test_source_bridge.cpp 的
// config_struct_size_is_forward_compatible 补）。
//
// 判据用 default_preload_ms 而不是字节数：它是结构体的最后一个字段（截断点
// 最好放），而且能被**确定性地**观测到——装一个假 provider，看它被问到的
// 毫秒数是多少，一个整数比较，不依赖任何下载量算术。
TEST_CASE(preload_config_struct_size_is_forward_compatible) {
    syp::test::Watchdog wd("preload_config_struct_size_is_forward_compatible",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/fc.mp4"] = body(64 * 1024);
    Env e(lc);

    // 1) 太小的 struct_size 直接拒绝（连版本号本身都放不下）。
    {
        syp_preload_config bad{};
        syp_preload_config_init(&bad);
        bad.struct_size = 0;
        CHECK(syp_preloader_create(&e.dl, &bad, nullptr) == nullptr);
    }

    // 2) 比我们**小**：尾部的新字段必须保持默认值，不能读调用方没填的内存。
    {
        FakeProvider fp;
        fp.bytes_per_ms = 4;
        const syp_media_info_provider tbl = fp.c_table();
        syp_preload_config pc{};
        syp_preload_config_init(&pc);
        pc.default_preload_ms = 111;     // 落在截断点之后 → 必须被忽略
        pc.struct_size =
            static_cast<uint32_t>(offsetof(syp_preload_config, default_preload_ms));
        syp_preloader* p = syp_preloader_create(&e.dl, &pc, &tbl);
        REQUIRE(p != nullptr);
        const std::string u = e.server.url("/fc.mp4");
        CHECK_EQ(syp_preloader_add(p, u.c_str(), SYP_PRELOAD_PRIORITY_NEXT, 0), SYP_OK);
        syp_preload_stats st{};
        const auto dl1 = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < dl1) {
            syp_preloader_get_stats(p, &st);
            if (st.completed >= 1 || st.failed >= 1) break;
            std::this_thread::yield();
        }
        syp_preloader_destroy(p);
        // 默认值 3000，不是调用方结构体尾巴上那个 111。
        CHECK_EQ(fp.last_ms.load(std::memory_order_relaxed), static_cast<int64_t>(3000));
    }

    // 3) 比我们**大**（未来版本的调用方）：多出来的尾巴一律忽略，已知字段照用。
    {
        struct Bigger {
            syp_preload_config base;
            int64_t            future[3];
        };
        FakeProvider fp;
        fp.bytes_per_ms = 4;
        const syp_media_info_provider tbl = fp.c_table();
        Bigger b{};
        syp_preload_config_init(&b.base);
        b.base.default_preload_ms = 1234;
        for (int64_t& x : b.future) x = -1;
        b.base.struct_size = static_cast<uint32_t>(sizeof(Bigger));
        syp_preloader* p = syp_preloader_create(&e.dl, &b.base, &tbl);
        REQUIRE(p != nullptr);
        const std::string u = e.server.url("/fc.mp4");
        CHECK_EQ(syp_preloader_add(p, u.c_str(), SYP_PRELOAD_PRIORITY_NEXT, 0), SYP_OK);
        syp_preload_stats st{};
        const auto dl2 = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < dl2) {
            syp_preloader_get_stats(p, &st);
            if (st.completed >= 1 || st.failed >= 1) break;
            std::this_thread::yield();
        }
        syp_preloader_destroy(p);
        CHECK_EQ(fp.last_ms.load(std::memory_order_relaxed), static_cast<int64_t>(1234));
    }
}

// ====================================================================
// 限速类别的传递，与单例限速器下的端到端速率
// ====================================================================
//
// 【为什么端到端用例放在本文件，而不是 test_source_bridge】
// test_source_bridge 只链接 syp_dl + StubBackend（内存桩）+ LoopbackServer，
// 那里的 LoopbackServer 只被裸 socket 打过（不经 syp_source），没有能对着
// 回环发 HTTP 的后端。限速要验的是"真实时钟 + 真实到达记账 + 唤醒线程"，
// 桩后端的到达时序是它自己排的，验不出来。本目标链接 syp_platform_apple，
// 有 NSURLSession 后端——与既有的让路用例同一条理由。
namespace {

using syp::dl::RateClass;
using syp::dl::RateLimiter;

// 单例限速器是**进程全局**状态：用例改了速率，结束时（含 REQUIRE 的提前
// return）必须复位成 0，否则同进程后面的用例全都跑在限速下。
struct RateGuard {
    explicit RateGuard(int64_t bps) { RateLimiter::instance().set_rate(bps); }
    ~RateGuard() { RateLimiter::instance().set_rate(0); }
    RateGuard(const RateGuard&)            = delete;
    RateGuard& operator=(const RateGuard&) = delete;
};

// 等条目那条 bridge 的**调度器**进入指定类别。谓词式轮询，超时只把挂死
// 变成失败：类别的下发发生在驱动线程的下一轮 execute() 里，调用方不能假设
// set_priority 返回时已经生效。
bool wait_rate_class(const Preloader& p, const std::string& url, RateClass want,
                     int timeout_ms = 8000) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (p.rate_class_for_test(url) == static_cast<int32_t>(want)) return true;
        std::this_thread::yield();
    }
    return p.rate_class_for_test(url) == static_cast<int32_t>(want);
}

// 在旁路线程上阻塞读一条播放源，直到读满 limit、EOF、出错或被 interrupt。
// 边读边逐字节比对（资源是 synthetic_byte 公式）；只在读线程上写 got/bad，
// 用例 join 之后才读，不用原子；bytes 在读的过程中给主线程采样，用原子。
struct Reader {
    syp::dl::SourceBridge* src = nullptr;
    int64_t               limit = 0;
    std::atomic<int64_t>  bytes{0};
    std::atomic<bool>     done{false};
    int32_t               last = 0;
    bool                  bad = false;
    std::thread           th;

    void start() {
        th = std::thread([this] {
            std::vector<uint8_t> buf(16 * 1024);
            int64_t pos = 0;
            while (pos < limit) {
                const int64_t want64 = std::min<int64_t>(
                    limit - pos, static_cast<int64_t>(buf.size()));
                const int32_t n = src->read(buf.data(), static_cast<int32_t>(want64));
                last = n;
                if (n <= 0) break;
                for (int32_t i = 0; i < n; ++i) {
                    if (buf[static_cast<size_t>(i)] != synthetic_byte(pos + i)) bad = true;
                }
                pos += n;
                bytes.store(pos, std::memory_order_relaxed);
            }
            done.store(true, std::memory_order_release);
        });
    }
    // 等读完或到 deadline；到点还没完就 interrupt 把它放出来（不然 join 挂死）。
    // 返回是否在 deadline 之前自己读完。
    bool finish_within(int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline
               && !done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const bool ok = done.load(std::memory_order_acquire);
        if (!ok) src->interrupt();
        if (th.joinable()) th.join();
        return ok;
    }
    ~Reader() {
        if (th.joinable()) {
            src->interrupt();
            th.join();
        }
    }
};

}  // namespace

// 【条目 bridge 的类别跟着优先级走】Background/Next → Preload，Playing → Playing。
// 读的是条目 SourceBridge 的 rate_class_for_test()，也就是**调度器**里那个
// 准入时真正用的成员——不是 Preloader 自己记的"已下发类别"。所以
// "开条目时恒传 Playing"与"驱动线程不下发类别变化"这两种错误实现都在这里红。
//
// hold_route：请求被扣在服务端，一个字节都不落地 ⇒ 条目一直开着源（Running），
// 状态是静止的，类别是唯一在变的东西。
TEST_CASE(entry_rate_class_follows_its_priority) {
    syp::test::Watchdog wd("entry_rate_class_follows_its_priority", kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/rc.mp4"] = body(1 << 20);
    Env e(lc);
    e.server.hold_route("/rc.mp4");

    PreloadConfig pc;
    pc.default_preload_bytes = 512 * 1024;
    auto p = e.make(pc);
    REQUIRE(p != nullptr);

    const std::string u = e.server.url("/rc.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Background, 0), SYP_OK);
    CHECK(wait_state(*p, u, PreloadState::Running));
    // 开源那一刻交给 Scheduler 的类别（构造时类别，事后不变）：与时序无关地
    // 钉住"开条目时按优先级映射"。只看下面那条当前类别的话，开源时传错会在
    // 下一轮（≤ kPeerPollMs）被类别同步改回来，负载下这种错误实现可以假绿存活。
    CHECK_EQ(p->open_rate_class_for_test(u), static_cast<int32_t>(RateClass::Preload));
    CHECK_EQ(p->rate_class_for_test(u), static_cast<int32_t>(RateClass::Preload));

    CHECK_EQ(p->set_priority(u, PreloadPriority::Playing), SYP_OK);
    CHECK(wait_rate_class(*p, u, RateClass::Playing));
    CHECK_EQ(p->state_for_test(u), static_cast<int32_t>(PreloadState::Running));

    CHECK_EQ(p->set_priority(u, PreloadPriority::Next), SYP_OK);
    CHECK(wait_rate_class(*p, u, RateClass::Preload));

    e.server.release_route("/rc.mp4");
}

// 【紧限速下播放读不会挂死】真实时钟、单例限速器、R = 64 KiB/s；256 KiB 的
// 资源读 3 × R = 192 KiB。
//
// 读的量不是最初计划的 128 KiB——那样"Scheduler 被拒后不 arm"这个错误实现
// **有时会活**。播放源单连接、满桶起算 R、分片上限 R：第 1 片（[0, R) 探测）
// 把桶扣到 ≈0；第 2 片能不能不靠唤醒就发出去，取决于调度那一刻余额是否已被
// 几毫秒的回补推回正数（到达记账、准入时不扣）——实测两种都出现过：读
// 128 KiB 用 7 ms 就完成（第 2 片没等唤醒），而"不 arm"这个错误实现下又见过卡在
// 65536 字节。所以前 2R 字节不保证走到唤醒路径；第 3 片之前余额必然 ≈ -R，
// 只能靠订阅唤醒在回正时重跑调度。读 3R 才把这条路钉死。
//
// 期望耗时：约 1 秒（等 ≈ 1 秒的回补）。5 秒上限**只防挂死**，不是性能断言
// （负载敏感口径：并行 ctest / TSan 下同样留足余量）。
TEST_CASE(playback_read_does_not_block_forever_under_a_tight_limit) {
    syp::test::Watchdog wd("playback_read_does_not_block_forever_under_a_tight_limit",
                           kSoftMs * 2, kHardMs, kHint);
    constexpr int64_t kR     = 64 * 1024;
    constexpr int64_t kTotal = 256 * 1024;
    constexpr int64_t kRead  = 3 * kR;   // 192 KiB，理由见上
    LoopbackConfig lc;
    lc.routes["/tight.mp4"] = body(kTotal);
    Env e(lc);
    RateGuard rg(kR);

    syp_config cfg = e.dl;
    cfg.max_concurrent_tasks = 1;
    auto opened = syp::dl::SourceBridge::open(e.server.url("/tight.mp4"), nullptr, cfg,
                                              nullptr, syp::dl::current_http_backend(),
                                              syp::dl::system_clock());
    REQUIRE(opened.has_value());
    auto src = std::move(*opened);

    const auto t0 = std::chrono::steady_clock::now();
    Reader rd;
    rd.src   = src.get();
    rd.limit = kRead;
    rd.start();
    const bool ok = rd.finish_within(5000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("  [tight] read %lld bytes in %lld ms (ok=%d)\n",
                static_cast<long long>(rd.bytes.load()), static_cast<long long>(ms),
                ok ? 1 : 0);
    CHECK(ok);
    CHECK_EQ(rd.bytes.load(), kRead);
    CHECK(!rd.bad);
}

// 【共享限速下播放跑赢预加载】R = 512 KiB/s；一条播放源（持续 read）与一条
// Background 预加载条目，各自 4 MiB 的资源。
//
// 这是"保留线"的直接后果：Playing 只要余额 > 0 就能准入，Preload 要余额 >
// 半桶。播放持续消费时它的窗口永远有洞，余额一回正就被它拿走、随即扣成负数，
// 几乎到不了半桶——预加载基本拿不到额度。若预加载条目错开成 Playing，两者
// 同阈值平分，比值落到 ≈1，本用例红。
//
// 先让播放源读满 R 字节（= 开局那一整桶）再加预加载条目：确定性地让"播放
// 已经在持续消费、桶已见底"成立。只等"读到第一批字节"是不够的——数据按块
// 到达、按块记账，那一刻桶里往往还剩大半，预加载会在开局一把拿走一整片
// （R 字节，初版实测 play=1.2 MiB / preload=512 KiB，比值只有 2.4，贴着
// 判据）。这不是保留线失效，是开局满桶；本用例要钉的是稳态。判据 ×2 而不是更大，
// 且额外只要求播放至少读了 R 字节：负载容差——并行 ctest /
// TSan 下两者的绝对量都会缩，比值的方向不会翻。期望：播放 ≈ 1–1.5 MiB，
// 预加载 ≈ 0。
TEST_CASE(playback_outpaces_preload_under_a_shared_limit) {
    syp::test::Watchdog wd("playback_outpaces_preload_under_a_shared_limit",
                           kSoftMs * 2, kHardMs, kHint);
    constexpr int64_t kR    = 512 * 1024;
    // 8 MiB：两阶段共 7 秒，播放最多读 ≈ 0.5 + 7×0.5 + 在途 ≈ 5 MiB，不能碰到 EOF。
    constexpr int64_t kSize = 8 * 1024 * 1024;
    LoopbackConfig lc;
    lc.routes["/play.mp4"] = body(kSize);
    lc.routes["/pre.mp4"]  = body(kSize);
    Env e(lc);
    RateGuard rg(kR);

    auto opened = syp::dl::SourceBridge::open(e.server.url("/play.mp4"), nullptr, e.dl,
                                              nullptr, syp::dl::current_http_backend(),
                                              syp::dl::system_clock());
    REQUIRE(opened.has_value());
    auto player = std::move(*opened);
    Reader rd;
    rd.src   = player.get();
    rd.limit = kSize;
    rd.start();
    {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < dl && rd.bytes.load() < kR) {
            std::this_thread::yield();
        }
    }
    REQUIRE(rd.bytes.load() >= kR);

    PreloadConfig pc;
    pc.default_preload_bytes = kSize;
    auto p = e.make(pc);
    REQUIRE(p != nullptr);
    const int64_t play0 = rd.bytes.load();
    CHECK_EQ(p->add(e.server.url("/pre.mp4"), PreloadPriority::Background, 0), SYP_OK);

    std::this_thread::sleep_for(std::chrono::seconds(2));
    const int64_t played = rd.bytes.load() - play0;
    PreloadStats st{};
    p->get_stats(&st);
    const int64_t pre = st.downloaded_bytes;
    std::printf("  [outpace] play=%lld preload=%lld\n", static_cast<long long>(played),
                static_cast<long long>(pre));
    CHECK(played >= kR);
    CHECK(played >= pre * 2);
    // 【比值判据单独用杀不掉"类别映射错"】实测把 rate_class_for 改成恒
    // Playing（两者同阈值）时 play≈1.9 MiB / preload=512 KiB，比值 3.8，
    // ×2 照样绿——播放单凭"读驱动的窗口永远饿"就能多抢。真正区分两种实现
    // 的是：保留线下预加载在稳态里**一整片都拿不到**（实测恒为 0），同阈值
    // 时开局就能拿到一片（= R 字节）。所以再断言预加载不足一片。
    // 负载容差：只有播放读线程被饿住 ≳0.5 秒、桶回到半满以上时预加载才可能
    // 合法地拿到一片；并行 ctest / TSan 下实测仍为 0。
    CHECK(pre < kR);

    // ---- 第二阶段：预加载条目改为 Playing ⇒ 两者持平 ----
    // 等类别真的同步到那条桥的调度器（谓词轮询测试缝），再量一段。
    const std::string pu = e.server.url("/pre.mp4");
    CHECK_EQ(p->set_priority(pu, PreloadPriority::Playing), SYP_OK);
    CHECK(wait_rate_class(*p, pu, RateClass::Playing));
    const int64_t play1 = rd.bytes.load();
    PreloadStats s1{};
    p->get_stats(&s1);
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const int64_t play2 = rd.bytes.load() - play1;
    PreloadStats s2{};
    p->get_stats(&s2);
    const int64_t pre2 = s2.downloaded_bytes - s1.downloaded_bytes;
    std::printf("  [even] play=%lld preload=%lld ratio=%.2f\n",
                static_cast<long long>(play2), static_cast<long long>(pre2),
                pre2 > 0 ? static_cast<double>(play2) / static_cast<double>(pre2) : -1.0);
    // 宽区间 [1/3, 3]：同阈值下谁先抢到准入有随机性——唤醒线程按订阅顺序
    // 逐个同步重跑调度，先跑的那个先把余额扣负；而每次准入拿的是整片（R），
    // 5 秒里一共只有 ≈ 5 次回正（外加在途透支），一两片的归属差就能把比值
    // 推到 2 附近（3 秒窗口初版实测 2.00 = 2 片 : 1 片）。所以窗口取 5 秒、
    // 判据取 [1/3, 3]。
    // 判据只要求"不再是一方几乎独占"：第一阶段预加载恒为 0，若类别没下发
    // （仍是 Preload），pre2 仍 ≈ 0，下界那条必红。
    CHECK(pre2 > 0);
    CHECK(play2 * 3 >= pre2);
    CHECK(play2 <= pre2 * 3);

    rd.finish_within(0);
    CHECK(!rd.bad);
    p.reset();
}

// 【端到端速率准确度】真实时钟、真实回环、单例限速器。
// 这是"限速在真实网络路径上真的限住了"的唯一证据——此前只在假时钟 +
// 桩后端上证明过记账与准入。
//
// 单个播放源（2 连接）持续读 8 MiB 资源，R = 512 KiB/s，量开源后 t ≈ 3 秒内
// 实际**到达**的字节 B（get_stats().downloaded_bytes：on_data 落盘计数）。
//
// 上界 B ≤ R·t + 容量 + 在途上限：
//   · R·t：t 秒的回补；
//   · 容量 = R × 1 秒：0 → 非 0 时满桶起算，开局那一桶是白给的；
//   · 在途上限 = 并发数 × 分片上限 = 2 × R：记账在**到达**时做，准入只看
//     余额 > 0——余额刚回正的那一刻同一轮可以准入 2 片、每片 ≤ R，它们在途
//     时全速到达，把余额扣到 ≈ -2R。这部分是"欠的账"，还没来得及按 R 还。
//   手算：t = 3 s ⇒ 1.5 MiB + 0.5 MiB + 1 MiB = 3 MiB（按实测 t 计算）。
//   去掉 debit 的话余额永不下降，回环全速几毫秒就把 8 MiB 拉完，上界必红。
// 下界 B ≥ 0.5 × R·t（= 0.75 MiB）：防"限得太死 / 唤醒丢失"。取 0.5 而不是
// 更贴近 1：负载容差——并行 ctest / TSan 下读线程与唤醒线程
// 都可能被推迟，实测 B 远在 R·t 之上，0.5 只拦"数量级错了"。
TEST_CASE(throughput_under_a_limit_tracks_the_rate) {
    syp::test::Watchdog wd("throughput_under_a_limit_tracks_the_rate",
                           kSoftMs * 2, kHardMs, kHint);
    constexpr int64_t kR    = 512 * 1024;
    constexpr int64_t kSize = 8 * 1024 * 1024;
    LoopbackConfig lc;
    lc.routes["/rate.mp4"] = body(kSize);
    Env e(lc);
    RateGuard rg(kR);

    syp_config cfg = e.dl;
    cfg.max_concurrent_tasks = 2;     // 在途上限按 2 片算，与上界公式对齐
    const auto t0 = std::chrono::steady_clock::now();
    auto opened = syp::dl::SourceBridge::open(e.server.url("/rate.mp4"), nullptr, cfg,
                                              nullptr, syp::dl::current_http_backend(),
                                              syp::dl::system_clock());
    REQUIRE(opened.has_value());
    auto src = std::move(*opened);
    Reader rd;
    rd.src   = src.get();
    rd.limit = kSize;
    rd.start();

    std::this_thread::sleep_for(std::chrono::seconds(3));
    syp_source_stats ss{};
    src->get_stats(&ss);
    const int64_t b  = ss.downloaded_bytes;
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
    const int64_t rt    = kR * ms / 1000;          // R·t
    const int64_t upper = rt + kR + 2 * kR;        // + 容量 + 在途上限
    const int64_t lower = rt / 2;
    std::printf("  [rate] B=%lld in %lld ms  R*t=%lld  bounds=[%lld, %lld]\n",
                static_cast<long long>(b), static_cast<long long>(ms),
                static_cast<long long>(rt), static_cast<long long>(lower),
                static_cast<long long>(upper));
    CHECK(b <= upper);
    CHECK(b >= lower);

    rd.finish_within(0);
    CHECK(!rd.bad);
}

// 【不支持 Range 的源 + 限速仍能完整读完】（覆盖缺口）
//
// 分片上限让"总长未知时的首个探测请求"变成 [0, R)。服务端（support_range
// = false，LoopbackServer 既有开关）无视 Range 回 200 整文件 ⇒ Scheduler 走
// 既有 no-Range 降级、单连接整文件，且那次 200 的响应体同样记账。断言：
// 字节逐一正确、读到 EOF、长度一致、在有界时间内完成。
//
// 期望耗时：192 KiB @ 64 KiB/s，首响应最多透支 192 KiB ⇒ ≲ 3 秒回补。
// 10 秒上限只防挂死。
TEST_CASE(no_range_source_under_a_limit_still_completes) {
    syp::test::Watchdog wd("no_range_source_under_a_limit_still_completes",
                           kSoftMs * 3, kHardMs, kHint);
    constexpr int64_t kR     = 64 * 1024;
    constexpr int64_t kTotal = 192 * 1024;
    LoopbackConfig lc;
    lc.resource_length = kTotal;
    lc.support_range   = false;
    Env e(lc);
    RateGuard rg(kR);

    syp_config cfg = e.dl;
    cfg.allow_no_range_fallback = true;
    auto opened = syp::dl::SourceBridge::open(e.server.url("/nr.mp4"), nullptr, cfg,
                                              nullptr, syp::dl::current_http_backend(),
                                              syp::dl::system_clock());
    REQUIRE(opened.has_value());
    auto src = std::move(*opened);

    const auto t0 = std::chrono::steady_clock::now();
    Reader rd;
    rd.src   = src.get();
    rd.limit = kTotal + 1;       // 多要 1 字节：必须以 EOF 收尾，而不是读满就停
    rd.start();
    const bool ok = rd.finish_within(10000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("  [no-range] read %lld bytes in %lld ms (last=%d, requests=%lld)\n",
                static_cast<long long>(rd.bytes.load()), static_cast<long long>(ms),
                rd.last, static_cast<long long>(e.server.total_requests()));
    CHECK(ok);
    CHECK_EQ(rd.bytes.load(), kTotal);
    CHECK_EQ(rd.last, static_cast<int32_t>(SYP_ERR_EOF));
    CHECK(!rd.bad);
    CHECK_EQ(src->length(), kTotal);
    // 钉住"探测 → 200 → 降级重发"这条路径真的走过（实测 3 次请求）。将来
    // 首个探测若改回开区间 [0, EOF)，服务端的 200 整文件就是一次合法的完整
    // 响应、只剩 1 次请求——这里会红，而不是悄悄失去 no-Range + 限速的覆盖。
    CHECK(e.server.total_requests() >= 2);
}

int main() { return tiny_test_main(); }

// 【异常穿出驱动线程 = std::terminate = 整个 app 挂掉】
//
// 仓库没有 -fno-exceptions，C ABI 层一律 `new (std::nothrow)`，唯独驱动线程
// 是破例：driver_main() 整条路径原先**一个 try/catch 都没有**，而里面有
// push_back / make_unique<Entry> / fetch_text 最大 8 MiB 的 text.append。
// 任何一次 bad_alloc 在没有 catch 的线程函数里都不是"这条预加载失败"，
// 而是 std::terminate——在内存吃紧的 iOS 设备上就是整个 app 挂掉。
//
// 【能构造出来的那一条是 provider】bad_alloc 不装注入缝造不出来，但驱动线程
// 上**唯一同步调用外来代码**的地方——provider 的 estimate_range_for_ms——是
// 可控的，而且它是真实的风险面：那份 provider 在 src/media，再往上是 ObjC++
// 与 Swift，dl 层管不到它抛不抛。这条用例让它抛，断言三件事：
//   1. 进程活着（修复前这里是 abort，用例连 FAIL 都打不出来）；
//   2. 这条目按"provider 估不出"收尾（provider_miss 记一次，退回按字节），
//      **不是**永远卡在 Estimating；
//   3. 驱动线程没被弄坏——同一个 preloader 随后还能把另一个 URL 下完。
TEST_CASE(a_throwing_provider_does_not_terminate_the_driver_thread) {
    syp::test::Watchdog wd("a_throwing_provider_does_not_terminate_the_driver_thread",
                           kSoftMs, kHardMs, kHint);
    LoopbackConfig lc;
    lc.routes["/boom.mp4"] = body(128 * 1024);
    lc.routes["/after.mp4"] = body(128 * 1024);
    Env e(lc);

    struct Thrower {
        static syp_status est(void*, const char*, int64_t, int64_t*, int64_t*) {
            throw std::bad_alloc{};
        }
    };
    syp::dl::MediaInfoProvider tbl;
    tbl.ctx = nullptr;
    tbl.estimate_range_for_ms = &Thrower::est;

    PreloadConfig pc;
    pc.default_preload_bytes = 16 * 1024;   // 估不出时的退路
    pc.default_preload_ms    = 1000;
    syp_status err = SYP_OK;
    auto p = Preloader::create(e.dl, pc, &tbl, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(err == SYP_OK);
    REQUIRE(p != nullptr);

    const std::string u = e.server.url("/boom.mp4");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 2000), SYP_OK);   // ms>0 ⇒ 要走 provider
    CHECK_EQ(p->wait_terminal_for_test(u, 20000),
             static_cast<int32_t>(PreloadState::Done));
    PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.provider_miss, static_cast<int64_t>(1));   // 按"估不出"收尾
    CHECK(st.downloaded_bytes >= 16 * 1024);
    CHECK(st.downloaded_bytes < 64 * 1024);                // 用的是字节退路

    // 驱动线程还活着、还在干活。
    const std::string u2 = e.server.url("/after.mp4");
    CHECK_EQ(p->add(u2, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->wait_terminal_for_test(u2, 20000),
             static_cast<int32_t>(PreloadState::Done));
}
