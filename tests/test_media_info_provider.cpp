// test_media_info_provider.cpp — media 层把"毫秒 → 字节区间"注入 dl 层。
//
// 判据不是"估得准"，而是**"不会少于请求的那段时长真正需要的字节数"**
// （CBR 估算对 VBR 偏差可能很大，本轮只保证多下那一侧）。
// 所以断言是：估出来的字节数 ≥ 按真实 (总字节 / 总时长) 线性折算的值，
// 且 ≤ 文件总长。
//
// 素材走 fixture 里的 faststart.mp4，经 LoopbackServer 的 body_file 供体
// （真实 HTTP + 真实 Range），不打外网。
#include "tiny_test.h"
#include "support/loopback_server.h"
#include "support/watchdog.h"
#include "scenarios.h"

#include "media/demuxer.h"
#include "media/media_info_provider.h"

#include <dl/cache_store.h>

#include <syplayer/syp_config.h>
#include <syplayer/syp_preload.h>
#include <syplayer/syp_source.h>
#include <syplayer/syp_types.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using syp::dl::test::LoopbackConfig;
using syp::dl::test::LoopbackServer;
using syp::media::MediaInfoProvider;

namespace {

constexpr int kSoftMs = 20000;
constexpr int kHardMs = 120000;

std::string fixture_dir() {
    const char* p = std::getenv("SYP_FIXTURE_DIR");
    return p != nullptr ? std::string(p) : std::string();
}

}  // namespace

TEST_CASE(estimate_never_undershoots_and_caps_at_total) {
    syp::test::Watchdog wd("estimate_never_undershoots_and_caps_at_total",
                           kSoftMs, kHardMs, "provider 会真的开一次 HTTP 连接读头。");
    const std::string fd = fixture_dir();
    REQUIRE(!fd.empty());
    const std::string mp4 = fd + "/faststart.mp4";
    REQUIRE(std::filesystem::exists(mp4));
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackConfig lc;
    lc.body_file = mp4;
    LoopbackServer server(lc);

    // 真值：用本地文件直接问 FFmpeg 要总时长，再按线性比例折算。
    syp_status derr = SYP_OK;
    auto d = syp::media::Demuxer::open_file(mp4, &derr);
    REQUIRE(d != nullptr);
    const int64_t dur_us = d->duration_us();
    REQUIRE(dur_us > 0);
    d.reset();
    const int64_t total = static_cast<int64_t>(std::filesystem::file_size(mp4));

    syp::probe::TempCacheDir tmp("mip");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);

    const int64_t want_ms = 1000;
    int64_t s = -1;
    int64_t e = -1;
    CHECK_EQ(mip.estimate_range_for_ms(server.url("/v.mp4").c_str(), want_ms, &s, &e),
             SYP_OK);
    CHECK_EQ(s, static_cast<int64_t>(0));
    const int64_t linear = total * (want_ms * 1000) / dur_us;
    CHECK(e >= linear);          // 宁可多下
    CHECK(e <= total);           // 不超过资源本身
    // 【为什么还要这一条】上面那条 `e >= linear` 对 (a) 分支是**恒真**的：
    // 估算用的就是 total/duration 这个线性值，去掉 5/4 的安全系数它照样成立。
    // 安全系数是"宁可多下"的全部实质内容，必须有一条断言真的
    // 盯着它，否则删掉它这套用例一条都不会红。
    const int64_t safety =
        linear * syp::media::kEstimateSafetyNum / syp::media::kEstimateSafetyDen;
    CHECK(e >= safety);
    // 容器头的常数项也确实加上去了：估算必须**严格大于**只乘安全系数的值
    // （本素材的探测前缀是几百 KiB，远不是 0）。
    CHECK(e > safety);
    // 但它仍然被探测上界钉住——常数项不可能大于 kProbeMaxBytes。
    CHECK(e <= safety + syp::media::kProbeMaxBytes);
    // 诊断行：这条用例只断言"不少于线性值"，但多出多少是这一版估算的精度
    // 本身，值得在日志里留个数（换素材/换算法时一眼能看出漂移）。
    std::printf("  [est] dur_us=%lld total=%lld linear=%lld est=%lld ratio=%.3f\n",
                static_cast<long long>(dur_us), static_cast<long long>(total),
                static_cast<long long>(linear), static_cast<long long>(e),
                linear > 0 ? static_cast<double>(e) / static_cast<double>(linear) : 0.0);

    // 第二次命中内存缓存：值一致，且不再发新请求。
    const int64_t before = server.total_requests();
    int64_t s2 = -1;
    int64_t e2 = -1;
    CHECK_EQ(mip.estimate_range_for_ms(server.url("/v.mp4").c_str(), want_ms, &s2, &e2),
             SYP_OK);
    CHECK_EQ(e2, e);
    CHECK_EQ(server.total_requests(), before);
}

// "只读 header"对 **moov 在文件尾** 的 MP4 不成立：
// FFmpeg 会 seek 到尾部去取 moov，而 syp_source 把 [0, total) 当一个窗口填，
// 于是"读个头"变成"下整个文件"（实测 moovend.mp4 22832904 B，即整份）。
// 那次下载是同步发生在 Preloader 的驱动线程上的，谁也打断不了它。
//
// 判据是**服务端实际写出的字节数**，不是估算值——要证明的正是"没有真的去
// 下整个文件"，而估算值对此一个字都说明不了。
TEST_CASE(probe_is_bounded_on_moov_at_end_and_falls_back) {
    syp::test::Watchdog wd("probe_is_bounded_on_moov_at_end_and_falls_back",
                           kSoftMs, kHardMs, "moov 在尾部的素材会诱发整片下载。");
    const std::string fd = fixture_dir();
    REQUIRE(!fd.empty());
    const std::string mp4 = fd + "/moovend.mp4";
    REQUIRE(std::filesystem::exists(mp4));
    REQUIRE(syp::probe::ensure_apple_backend());
    const int64_t total = static_cast<int64_t>(std::filesystem::file_size(mp4));
    REQUIRE(total > 8 * syp::media::kProbeMaxBytes);   // 上界必须真的咬得住

    LoopbackConfig lc;
    lc.body_file = mp4;
    LoopbackServer server(lc);

    syp::probe::TempCacheDir tmp("mip-moovend");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);

    int64_t s = -1;
    int64_t e = -1;
    // 索引在文件末尾的容器，前缀根本暖不出首帧——退化为按字节才是对的答案。
    CHECK_EQ(mip.estimate_range_for_ms(server.url("/e.mp4").c_str(), 1000, &s, &e),
             SYP_ERR_NOT_IMPLEMENTED);

    const int64_t sent = server.total_bytes_sent();
    std::printf("  [bound] total=%lld sent=%lld cap=%lld\n",
                static_cast<long long>(total), static_cast<long long>(sent),
                static_cast<long long>(syp::media::kProbeMaxBytes));
    // 上界是"已缓存字节"，而打断有一个在途窗口的延迟，所以给 4 倍余量——
    // 判据要挡住的是"下了整个文件"（22.8MB），不是精确到字节的上界。
    CHECK(sent < 4 * syp::media::kProbeMaxBytes);
    CHECK(sent < total / 2);
}

// 探测的字节上界数的必须是**这次
// 探测新够到的字节**，不是缓存里本来就有的字节。反过来的写法（数 covered
// 总量）让一份热缓存悄悄关掉按时间预加载——而热缓存正是预加载自己造出来的：
// 一次按时间的预加载就能缓存超过上界的字节，于是同一个 URL 下一次探测直接
// 放弃。实测：3.1MB 预热 → 零新网络字节就返回 NOT_IMPLEMENTED；1MB
// 预热 → 同一个 URL、同样的 ms，估算值 2,229,233 对冷态的 1,025,009。
//
// 判据两条，缺一不可：热缓存下**仍然估得出**（不是 NOT_IMPLEMENTED），
// 且估出来的值与冷态**逐字节相同**（估算不能随缓存温度漂移——第二条才是
// 挡住"header_bytes 取自已缓存区间"那一半的那条）。
TEST_CASE(probe_cap_and_estimate_ignore_bytes_cached_before_the_probe) {
    syp::test::Watchdog wd("probe_cap_and_estimate_ignore_bytes_cached_before_the_probe",
                           kSoftMs, kHardMs, "先灌热缓存再探测，两次都会开连接。");
    const std::string fd = fixture_dir();
    REQUIRE(!fd.empty());
    const std::string mp4 = fd + "/faststart.mp4";
    REQUIRE(std::filesystem::exists(mp4));
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackConfig lc;
    lc.body_file = mp4;
    LoopbackServer server(lc);
    const std::string url = server.url("/v.mp4");

    // 冷态基准。
    int64_t e_cold = -1;
    {
        syp::probe::TempCacheDir tmp("mip-cold");
        syp_config cfg{};
        syp_config_init(&cfg);
        cfg.cache_dir = tmp.path.c_str();
        MediaInfoProvider mip(cfg);
        int64_t s = -1;
        CHECK_EQ(mip.estimate_range_for_ms(url.c_str(), 1000, &s, &e_cold), SYP_OK);
    }
    REQUIRE(e_cold > 0);

    // 热态：先把远超上界的一段灌进同一份缓存，再探测同一个 URL。
    syp::probe::TempCacheDir tmp("mip-warm");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    const int64_t warm_bytes = 3 * syp::media::kProbeMaxBytes / 2 + 1024 * 1024;
    {
        syp_source* src = nullptr;
        REQUIRE(syp_source_open(&src, url.c_str(), nullptr, &cfg, nullptr) == SYP_OK);
        REQUIRE(src != nullptr);
        std::vector<uint8_t> buf(256 * 1024);
        int64_t got = 0;
        while (got < warm_bytes) {
            const int32_t n = syp_source_read(src, buf.data(),
                                              static_cast<int32_t>(buf.size()));
            if (n <= 0) break;
            got += n;
        }
        syp_source_close(src);
        CHECK(got >= warm_bytes);   // 预热真的发生了，否则这条用例是空的
    }

    MediaInfoProvider mip(cfg);
    int64_t s = -1;
    int64_t e_warm = -1;
    // 缓存里有 3MB 不等于"这次探测下了 3MB"：上界不该在这里咬。
    CHECK_EQ(mip.estimate_range_for_ms(url.c_str(), 1000, &s, &e_warm), SYP_OK);
    std::printf("  [warm] cold=%lld warm=%lld delta=%lld\n",
                static_cast<long long>(e_cold), static_cast<long long>(e_warm),
                static_cast<long long>(e_warm - e_cold));
    // 同一个 URL、同样的 ms：估算值不许**随缓存温度漂移**。
    //
    // 判据是"差不到一个 avio 缓冲"而不是逐字节相等：常数项取的是 FFmpeg 经
    // AvioBridge 读到的最大偏移，而每次 read 拿回多少字节本身与数据到没到齐
    // 有关（冷缓存下会有短读），于是最后一次读的落点在两次之间可以差不到
    // 一个缓冲（64KiB）。要挡住的是旧写法那种量级的漂移：header_bytes 取自
    // **已缓存区间**时，1MB 预热就让同一个 URL 从 1,025,009 变成 2,229,233，
    // 差了 1.2MB。
    constexpr int64_t kAvioBuffer = 64 * 1024;
    CHECK(e_warm - e_cold <= kAvioBuffer && e_cold - e_warm <= kAvioBuffer);
}

// 上界是**字节**，不是时间。一个
// 只接受连接、永不响应的服务端用默认配置（15s 读超时 × 重试）把驱动线程
// 按住了实测 120,071ms ——而 syp_preload.h 对 provider 的头一条硬约束就是
// "必须尽快返回"，那条承诺连本仓库自己的 provider 都没守住。
// 判据是墙钟，但方向是单边的：只要求它**远小于**默认配置下那两分钟，
// 不对具体耗时下断言。
TEST_CASE(probe_gives_up_quickly_on_a_stalled_server) {
    syp::test::Watchdog wd("probe_gives_up_quickly_on_a_stalled_server",
                           60000, 180000, "服务端收下请求后永不响应。");
    REQUIRE(syp::probe::ensure_apple_backend());
    LoopbackConfig lc;
    lc.hang = true;                 // 收下请求行之后永不写响应
    LoopbackServer server(lc);

    syp::probe::TempCacheDir tmp("mip-stall");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    int64_t s = -1;
    int64_t e = -1;
    CHECK_EQ(mip.estimate_range_for_ms(server.url("/stall.mp4").c_str(), 1000, &s, &e),
             SYP_ERR_NOT_IMPLEMENTED);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("  [stall] estimate returned after %lld ms\n", static_cast<long long>(ms));
    // 【门槛是量出来的，别随手放宽】原来写的是 30,000ms，而
    // kProbeMaxRetries 改回 0（退化值）实测是 15,022ms —— 那条判据对本仓库
    // 唯一在意的回归**一个字都没拦**。本机 13 次实测（ctest 串行 + -j4）：
    // 6,004 / 6,005 / 6,007×2 / 6,008 / 6,009×2 / 6,010×3 / 6,011×4，
    // 跨度 7ms。门槛取 10,000ms：比实测上限宽 1.66 倍（扛得住被压的机器），
    // 又比 15,022ms 紧 5s（常数改回 0 必定变红）。
    CHECK(ms < 10000);
}

// 可达但不是媒体的 URL：能开、能拿到总长，但 FFmpeg 解析不出任何流。
// 这条路（open 成功 / demuxer 失败）此前没有用例走过。
TEST_CASE(estimate_reports_not_implemented_for_non_media_url) {
    syp::test::Watchdog wd("estimate_reports_not_implemented_for_non_media_url",
                           kSoftMs, kHardMs, "provider 会真的开一次 HTTP 连接读头。");
    REQUIRE(syp::probe::ensure_apple_backend());
    LoopbackConfig lc;
    lc.routes["/a.txt"] = std::vector<uint8_t>(4096, 'x');
    LoopbackServer server(lc);

    syp::probe::TempCacheDir tmp("mip-nonmedia");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);

    int64_t s = -1;
    int64_t e = -1;
    CHECK_EQ(mip.estimate_range_for_ms(server.url("/a.txt").c_str(), 1000, &s, &e),
             SYP_ERR_NOT_IMPLEMENTED);
    CHECK(server.total_requests() >= 1);   // 确实开过连接，不是被前置判据挡掉的
}

// 换算本身的纯算术。脱离素材测的理由与 demuxer.h 的 normalize_duration 相同：
// 真正会走到码率分支的场景（直播源、chunked、时长未知）做不成 fixture，
// 而这段换算与素材无关。
TEST_CASE(estimate_bytes_for_covers_every_branch) {
    using syp::media::estimate_bytes_for;
    const int64_t kMin = syp::media::kMinEstimateBytes;

    // (a) 时长 + 总长：线性 × 5/4，再加头部常数。
    //     10MB / 100s，要 10s → 1MB，×5/4 = 1.25MB，+8KiB 头部。
    CHECK_EQ(estimate_bytes_for(100'000'000, 10'000'000, 0, 8192, 10'000),
             static_cast<int64_t>(1'250'000 + 8192));
    // 头部是**加数**不是 max：去掉它结果正好小 8192。
    CHECK_EQ(estimate_bytes_for(100'000'000, 10'000'000, 0, 0, 10'000),
             static_cast<int64_t>(1'250'000));

    // (b) 时长未知（直播 / chunked）→ 退到码率：800kbps = 100000 B/s，
    //     10s → 1MB，×5/4 = 1.25MB。总长未知时不夹上界。
    CHECK_EQ(estimate_bytes_for(0, -1, 800'000, 0, 10'000),
             static_cast<int64_t>(1'250'000));
    // 总长未知但时长已知，同样只能走码率分支。
    CHECK_EQ(estimate_bytes_for(100'000'000, -1, 800'000, 0, 10'000),
             static_cast<int64_t>(1'250'000));

    // (c) 两者都没有 → 估不出。
    CHECK_EQ(estimate_bytes_for(0, 10'000'000, 0, 0, 10'000), static_cast<int64_t>(0));

    // 下限：算出来只有几百字节也至少暖 kMinEstimateBytes。
    CHECK_EQ(estimate_bytes_for(100'000'000, 10'000'000, 0, 0, 1),
             kMin);
    // 上限：夹到资源总长，头部加数也不能把它顶出去。
    CHECK_EQ(estimate_bytes_for(100'000'000, 10'000'000, 0, 5'000'000, 90'000),
             static_cast<int64_t>(10'000'000));

    // 非法 ms / 溢出的 ms 一律 0（调用方退化为按字节）。
    CHECK_EQ(estimate_bytes_for(100'000'000, 10'000'000, 0, 0, 0),
             static_cast<int64_t>(0));
    CHECK_EQ(estimate_bytes_for(100'000'000, 10'000'000, 0, 0, -1),
             static_cast<int64_t>(0));
    CHECK_EQ(estimate_bytes_for(100'000'000, 10'000'000, 0, 0,
                                std::numeric_limits<int64_t>::max()),
             static_cast<int64_t>(0));

    // 对 ms 单调不减——"加常数"而不是"取 max"保住的就是这条。
    int64_t prev = 0;
    for (int64_t ms = 100; ms <= 20'000; ms += 100) {
        const int64_t v = estimate_bytes_for(100'000'000, 10'000'000, 0, 8192, ms);
        CHECK(v >= prev);
        prev = v;
    }
}

TEST_CASE(estimate_reports_not_implemented_for_unopenable_url) {
    syp::test::Watchdog wd("estimate_reports_not_implemented_for_unopenable_url",
                           kSoftMs, kHardMs, "provider 会真的开一次 HTTP 连接读头。");
    REQUIRE(syp::probe::ensure_apple_backend());
    LoopbackConfig lc;
    lc.routes["/ok.bin"] = std::vector<uint8_t>(16, 0);   // 别的路径一律 404
    LoopbackServer server(lc);

    syp::probe::TempCacheDir tmp("mip-miss");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);

    int64_t s = -1;
    int64_t e = -1;
    CHECK_EQ(mip.estimate_range_for_ms(server.url("/nope.mp4").c_str(), 1000, &s, &e),
             SYP_ERR_NOT_IMPLEMENTED);
}

TEST_CASE(table_forwards_to_instance) {
    syp::probe::TempCacheDir tmp("mip-table");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);
    const syp_media_info_provider t = mip.table();
    REQUIRE(t.estimate_range_for_ms != nullptr);
    CHECK(t.ctx == static_cast<void*>(&mip));
    int64_t s = -1;
    int64_t e = -1;
    // 空 URL 一定走不通，但必须是"返回 NOT_IMPLEMENTED"而不是崩。
    CHECK_EQ(t.estimate_range_for_ms(t.ctx, "", 1000, &s, &e), SYP_ERR_NOT_IMPLEMENTED);
}

// "provider 与 preloader 必须用同一个
// cache_dir"此前是一句无人检查的约定：两个对象各自构造，没有任何断言把它们
// 绑在一起，而目录一字之差（Apple 的 NSTemporaryDirectory() 带尾斜杠、
// URL.path 往返之后不带）就是两份缓存——探测下来的头部字节播放时命中不了，
// 头部常数项白加，"探测不算白下"的论证整个不成立。
//
// 判据不是"我们传了同一个变量进去"，而是**两个对象各自真正在用的那个字符串
// 逐字节相同**，并且同一个 URL 在两边算出同一个 CacheStore key——后者才是
// "同一份缓存"的实质。故意传一个带尾斜杠的目录：归一化只发生一次、在
// PreloadStack 里，两边拿到的都是归一化之后那一份。
TEST_CASE(preload_stack_sources_one_cache_dir_for_both_objects) {
    REQUIRE(syp::probe::ensure_apple_backend());
    syp::probe::TempCacheDir tmp("mip-stack");
    const std::string with_slash = tmp.path + "/";
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = with_slash.c_str();

    syp_status err = SYP_OK;
    auto stack = syp::media::PreloadStack::create(cfg, syp::dl::PreloadConfig{},
                                                  nullptr, &err);
    REQUIRE(stack != nullptr);
    CHECK_EQ(err, SYP_OK);
    REQUIRE(stack->preloader() != nullptr);

    CHECK_EQ(stack->cache_dir(), tmp.path);            // 尾斜杠被归一化掉了
    CHECK_EQ(stack->provider_cache_dir_for_test(), stack->cache_dir());
    CHECK_EQ(stack->preloader_cache_dir_for_test(), stack->cache_dir());
    // 实质判据：同一个 URL 在两边是同一个缓存条目。
    const std::string url = "http://127.0.0.1:9/one.mp4";
    CHECK_EQ(syp::dl::CacheStore::make_key(stack->provider_cache_dir_for_test(), url),
             syp::dl::CacheStore::make_key(stack->preloader_cache_dir_for_test(), url));

    // cache_dir 为空是明确的错误，不是"悄悄用个默认目录"。
    syp_config bad{};
    syp_config_init(&bad);
    bad.cache_dir = "";
    syp_status berr = SYP_OK;
    CHECK(syp::media::PreloadStack::create(bad, syp::dl::PreloadConfig{}, nullptr,
                                           &berr) == nullptr);
    CHECK_EQ(berr, SYP_ERR_INVALID_ARG);
}

// 【容量/TTL 三件套的四条读回缝，一条用例全钉住】
//
// 三个字段（max_cache_bytes / min_free_space_bytes / cache_ttl_ms）从调用方
// 进来之后要一路原样抵达每一条真正开出去的 SourceBridge，中途任何一处
// "拍照之后再改一手"都让缓存变成无上限、不过期、无限涨，而外部**没有任何
// 可观测症状**——这正是它历史上被漏掉四次的原因。
//
// 【为什么这条用例要写在 C++ 侧而不是只靠 Swift】
// `PreloadStack::preloader_config_for_test()` 全仓唯一的消费者是
// `SYPBridge.mm:807`，C++ 侧零消费者。于是这条缝**只有 xcodebuild
// 杀得掉**：一个变异体（在 PreloadStack::create 里 provider 构造之后、
// Preloader::create 之前清零三字段）之下 `ctest` 全绿。没有 Xcode 的环境上
// 那个洞会重新变成单绿。下面这三条 CHECK_EQ 就是把它补成双绿。
//
// 四条缝各自断在哪一跳（顺序即数据流）：
//   provider_config_for_test()  → PreloadStack 交给 provider 的那一份
//   probe_config_for_test()     → probe() 真正交给 syp_source_open 的那一份
//   preloader_config_for_test() → PreloadStack 交给 Preloader 的那一份
//   playlist_config_for_test()  → fetch_text 真正交给 SourceBridge 的那一份
// 三个值刻意互不相同、也都不等于 syp_config_init 的默认值，所以逐字段
// 单独变异都区分得出来。
TEST_CASE(preload_stack_carries_capacity_and_ttl_down_every_branch) {
    REQUIRE(syp::probe::ensure_apple_backend());
    syp::probe::TempCacheDir tmp("mip-cap");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir            = tmp.path.c_str();
    cfg.max_cache_bytes      = 12345678;
    cfg.min_free_space_bytes = 2345678;
    cfg.cache_ttl_ms         = 1500;

    syp_status err = SYP_OK;
    auto stack = syp::media::PreloadStack::create(cfg, syp::dl::PreloadConfig{},
                                                  nullptr, &err);
    REQUIRE(stack != nullptr);
    REQUIRE(stack->preloader() != nullptr);

    const syp_config prov = stack->provider_config_for_test();
    CHECK_EQ(prov.max_cache_bytes, static_cast<int64_t>(12345678));
    CHECK_EQ(prov.min_free_space_bytes, static_cast<int64_t>(2345678));
    CHECK_EQ(prov.cache_ttl_ms, static_cast<int64_t>(1500));

    const syp_config probe = stack->provider_probe_config_for_test();
    CHECK_EQ(probe.max_cache_bytes, static_cast<int64_t>(12345678));
    CHECK_EQ(probe.min_free_space_bytes, static_cast<int64_t>(2345678));
    CHECK_EQ(probe.cache_ttl_ms, static_cast<int64_t>(1500));

    const syp_config pre = stack->preloader_config_for_test();
    CHECK_EQ(pre.max_cache_bytes, static_cast<int64_t>(12345678));
    CHECK_EQ(pre.min_free_space_bytes, static_cast<int64_t>(2345678));
    CHECK_EQ(pre.cache_ttl_ms, static_cast<int64_t>(1500));

    const syp_config pl = stack->preloader()->playlist_config_for_test();
    CHECK_EQ(pl.max_cache_bytes, static_cast<int64_t>(12345678));
    CHECK_EQ(pl.min_free_space_bytes, static_cast<int64_t>(2345678));
    CHECK_EQ(pl.cache_ttl_ms, static_cast<int64_t>(1500));
}

// 归一化被提成了一个独立函数（normalize_cache_dir），因为接入层
// （SYPBridge.mm 的播放侧）也要走同一份——那一侧够不到 PreloadStack 的函数体，
// 而"在 Objective-C++ 里再抄一份"就是埋下两份会各自漂移的实现。上面那条用例
// 只覆盖了"带一个尾斜杠"这一种输入，这里把边界补齐。
//
// 这个函数现在住在 syp::dl（src/dl/cache_store.h），
// 好让 C ABI 的 syp_source_open / syp_preloader_create 也能收口。表留在本文件
// 而不是搬去 test_cache_store.cpp：它守的是"预加载与播放落在同一个 cache_dir"
// 这条前置条件，上下文（PreloadStack、两侧读回缝）全在这里。
//
// 【原先只剥尾斜杠，方向守反了】
// 实测 Foundation：URL(fileURLWithPath:"/tmp/d/").path 是 "/tmp/d"（尾斜杠
// 公开 API 产不出），而 "/tmp//d"、"/tmp/./d"、"/tmp/a/../b" 原样保留——公开
// API 能产出的恰恰是后三类。而 NSTemporaryDirectory() 自身以 '/' 结尾，所以
// 最自然的 NSTemporaryDirectory() + "/" + name 就是 "…/T//name"。于是补
// lexically_normal()。尾斜杠那一路仍要守（桥的 SypCacheSettings.directory 是
// 裸 NSString，绕过 URL 就能带进来）。
TEST_CASE(normalize_cache_dir_is_lexically_normal_without_trailing_slash) {
    using syp::dl::normalize_cache_dir;
    // —— 原有的尾斜杠一路（回归）——
    CHECK_EQ(normalize_cache_dir("/a/b"), std::string("/a/b"));
    CHECK_EQ(normalize_cache_dir("/a/b/"), std::string("/a/b"));
    CHECK_EQ(normalize_cache_dir("/a/b///"), std::string("/a/b"));

    // —— 新补：公开 API 真的产得出的三类 ——
    CHECK_EQ(normalize_cache_dir("/a//b"), std::string("/a/b"));       // 内部 "//"
    CHECK_EQ(normalize_cache_dir("/a/./b"), std::string("/a/b"));      // "."
    CHECK_EQ(normalize_cache_dir("/a/b/../c"), std::string("/a/c"));   // ".."
    // 这两条就是真实触发形状：NSTemporaryDirectory() 带尾斜杠 +
    // 手工拼 "/"。两种写法必须合流到同一个串，否则 peer_yield 读到 0。
    CHECK_EQ(normalize_cache_dir("/var/T//cache"), std::string("/var/T/cache"));
    CHECK_EQ(normalize_cache_dir("/var/T/cache"), std::string("/var/T/cache"));

    // —— 顺序的钉子：lexically_normal **自己会造出尾斜杠** ——
    // 实测 lexically_normal("a/./")=="a/"、("/a/.")=="/a/"、("/a/b/..")=="/a/"。
    // 所以"先剥尾斜杠、再 lexically_normal"那一版会停在 "a/"，与 "a" 仍是两个
    // key。这三条是唯一能把顺序写反区分出来的输入。
    CHECK_EQ(normalize_cache_dir("a/./"), std::string("a"));
    CHECK_EQ(normalize_cache_dir("/a/."), std::string("/a"));
    CHECK_EQ(normalize_cache_dir("/a/b/.."), std::string("/a"));

    // —— 相对路径：不替调用方补 cwd，只做词法归一 ——
    CHECK_EQ(normalize_cache_dir("a//b"), std::string("a/b"));
    CHECK_EQ(normalize_cache_dir("a/./b"), std::string("a/b"));
    CHECK_EQ(normalize_cache_dir("a/b/../c"), std::string("a/c"));
    CHECK_EQ(normalize_cache_dir("a/"), std::string("a"));
    CHECK_EQ(normalize_cache_dir("a//"), std::string("a"));
    CHECK_EQ(normalize_cache_dir("rel/path"), std::string("rel/path"));
    // "." / ".." 本身是合法的相对目录，不能被归一成空串（空串在
    // PreloadStack::create 里是 SYP_ERR_INVALID_ARG，两者语义天差地别）。
    CHECK_EQ(normalize_cache_dir("."), std::string("."));
    CHECK_EQ(normalize_cache_dir(".."), std::string(".."));
    CHECK_EQ(normalize_cache_dir("../a"), std::string("../a"));

    // —— 根目录：剥空了就不是同一个目录了 ——
    CHECK_EQ(normalize_cache_dir("/"), std::string("/"));
    CHECK_EQ(normalize_cache_dir("///"), std::string("/"));
    // ".." 在根目录上不弹栈（与 POSIX 一致：".." of "/" is "/"）。
    CHECK_EQ(normalize_cache_dir("/.."), std::string("/"));
    CHECK_EQ(normalize_cache_dir("/a/b/../../.."), std::string("/"));

    // —— 前导 "//"：POSIX 说实现定义，我们按 "/" 处理（Darwin 无独立语义，
    // libc++ 的 lexically_normal 也这么折）。钉住这个决定，换平台时它会红。
    CHECK_EQ(normalize_cache_dir("//"), std::string("/"));
    CHECK_EQ(normalize_cache_dir("//a/b"), std::string("/a/b"));

    // —— 空串原样返回（"没填目录"由调用方当错误处理，不在这里编默认值）——
    CHECK_EQ(normalize_cache_dir(""), std::string(""));

    // —— 纯词法：目录不存在也照常工作、不碰盘（缓存目录第一次用时正是不存在的）——
    CHECK_EQ(normalize_cache_dir("/does/not/exist/yet//x/./"),
             std::string("/does/not/exist/yet/x"));
}

// 【幂等】预加载那条链上归一化被调用**两次**（接入层 prepare_cache_dir 一次、
// PreloadStack::create 又一次），播放那条链上只调一次。两条链要落在同一个串上
// 就必须 normalize(normalize(x)) == normalize(x)。整张表跑第二遍钉住它——
// 上一条用例里任何一个输入将来被改成"不幂等"的实现，这里会红。
TEST_CASE(normalize_cache_dir_is_idempotent) {
    using syp::dl::normalize_cache_dir;
    static const char* kInputs[] = {
        "/a/b", "/a/b/", "/a/b///", "/a//b", "/a/./b", "/a/b/../c",
        "/var/T//cache", "a/./", "/a/.", "/a/b/..", "a//b", "a/./b", "a/b/../c",
        "a/", "a//", "rel/path", ".", "..", "../a", "/", "///", "/..",
        "/a/b/../../..", "//", "//a/b", "", "/does/not/exist/yet//x/./",
    };
    for (const char* in : kInputs) {
        const std::string once  = normalize_cache_dir(in);
        const std::string twice = normalize_cache_dir(once);
        CHECK_EQ(twice, once);
    }
}

// 两条被写进三处公开文档的说法都是假的，这条用例把**真实
// 行为**钉住，免得文档订正之后又有人照着旧说法把行为"改回去"。
//
//   假说法 1：「每个 URL 最多一次探测上界（2 MiB）」。
//     真相：失败的探测**不进 cache_**（probe() 返回 nullopt，
//     estimate_range_for_ms 只在成功时 emplace），而字节上界的基线是**每次
//     探测重新量的**。于是同一个 URL 连探三次，前两次各自拿走一个上界的额度。
//   假说法 2：「moov 在尾部的容器**永久**退化为按字节」。
//     真相：只退化一次。第一次探测虽然放弃了，但它下下来的那 ~2 MiB 前缀
//     **留在缓存里**；第二次探测从这份暖缓存起步，只需再新下很少的字节就
//     够 FFmpeg 解析出流信息 ⇒ **成功**。
//     推论（也写进文档了）：`providerMiss` 这个"按时间预加载有没有生效的
//     唯一信号"**依赖缓存冷热**——同一个 URL 冷态涨、热态不涨。
//
// 【为什么不顺手把失败也记忆掉】不记忆是**更好的行为**：这个 URL 最终会
// 成功，记忆失败等于把"第一次没成"变成"这个 URL 从此永远按字节"。代价只是
// 上界的会计口径与文档写的不一样——所以本轮修的是文档，不是行为。
TEST_CASE(a_failed_probe_is_not_memoized_so_one_url_can_exceed_the_probe_bound) {
    syp::test::Watchdog wd("a_failed_probe_is_not_memoized_so_one_url_can_exceed_the_probe_bound",
                           kSoftMs, kHardMs, "同一个 URL 连探三次，前两次都会开连接。");
    const std::string fd = fixture_dir();
    REQUIRE(!fd.empty());
    const std::string mp4 = fd + "/moovend.mp4";
    REQUIRE(std::filesystem::exists(mp4));
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackConfig lc;
    lc.body_file = mp4;
    LoopbackServer server(lc);

    syp::probe::TempCacheDir tmp("mip-nomemo");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);

    const char* kUrl = "/moovend.mp4";
    int64_t s = -1;
    int64_t e = -1;

    const syp_status st1 = mip.estimate_range_for_ms(server.url(kUrl).c_str(), 1000, &s, &e);
    const int64_t sent1 = server.total_bytes_sent();
    const syp_status st2 = mip.estimate_range_for_ms(server.url(kUrl).c_str(), 1000, &s, &e);
    const int64_t sent2 = server.total_bytes_sent() - sent1;
    const syp_status st3 = mip.estimate_range_for_ms(server.url(kUrl).c_str(), 1000, &s, &e);
    const int64_t sent3 = server.total_bytes_sent() - sent1 - sent2;

    std::printf("  [nomemo] probe#1 st=%d bytes=%lld | probe#2 st=%d bytes=%lld"
                " | probe#3 st=%d bytes=%lld | cap=%lld\n",
                static_cast<int>(st1), static_cast<long long>(sent1),
                static_cast<int>(st2), static_cast<long long>(sent2),
                static_cast<int>(st3), static_cast<long long>(sent3),
                static_cast<long long>(syp::media::kProbeMaxBytes));

    // 第一次放弃（moov 在尾部，够不到）。
    CHECK_EQ(st1, SYP_ERR_NOT_IMPLEMENTED);
    // 第二次**成功**——"永久退化"那句话就是在这一行上假的。
    CHECK_EQ(st2, SYP_OK);
    // 第三次是记忆命中：一个字节都不发。
    CHECK_EQ(st3, SYP_OK);
    CHECK_EQ(sent3, static_cast<int64_t>(0));
    // 前两次各自开销一份额度，合计**超过**一个上界——"每个 URL 最多 2 MiB"
    // 就是在这一行上假的。
    CHECK(sent1 + sent2 > syp::media::kProbeMaxBytes);
    // 但仍然远小于整片（22.8MB）：上界本身没有失效，失效的是"每 URL"这个口径。
    CHECK(sent1 + sent2 < 4 * syp::media::kProbeMaxBytes);
}

// 慢速滴流的服务端：每秒 1 KiB，**永远踩不到 5s 的单次读
// 超时**，于是三个 kProbe*TimeoutMs 一个都管不着它；而按这个速率够到
// kProbeMaxBytes（2 MiB）需要 ~2,048 秒（~34 分钟），字节上界同样管不着。
// 实测这条路径 **跑满 150 秒仍未结束**。这期间驱动线程被占死，
// ~PreloadStack 无法 join，SYPlayerPreloader.deinit 的后台块会让一条线程 +
// 一条连接在 App 认为 preloader 已经没了之后继续写缓存目录最多 ~34 分钟。
//
// 上界现在由 kProbeWallClockMs（15,000ms）这条墙钟看门狗给出：超时与超字节
// 走同一条开火路径（打断 + 放弃），对调用方的后果一样是
// SYP_ERR_NOT_IMPLEMENTED ⇒ 退化为按字节预加载。
//
// 门槛取 25,000ms：比 15,000 宽 1.66 倍（与上面那条 stall 用例同一个余量
// 口径，扛得住被压的机器），又远紧于"没有看门狗"时的 ~2,048 秒——把看门狗
// 拿掉必定变红，而不是变慢。
TEST_CASE(probe_gives_up_on_a_trickling_server_within_the_wall_clock_bound) {
    syp::test::Watchdog wd("probe_gives_up_on_a_trickling_server_within_the_wall_clock_bound",
                           60000, 180000,
                           "服务端按 1 KiB/s 滴流，每次读都在读超时之前回来一点点。");
    REQUIRE(syp::probe::ensure_apple_backend());
    const std::string fd = fixture_dir();
    REQUIRE(!fd.empty());
    // moov 在文件尾：FFmpeg 会去够文件尾部，探测因此真的要读很多字节，
    // 滴流才有意义（faststart 只读 512KiB，同样够不完，但用 moovend 更贴近
    // 实测的那个场景）。
    const std::string mp4 = fd + "/moovend.mp4";
    REQUIRE(std::filesystem::exists(mp4));

    LoopbackConfig lc;
    lc.body_file             = mp4;
    lc.body_chunk_bytes      = 1024;   // 1 KiB
    lc.body_chunk_delay_ms   = 1000;   // 每块隔 1 秒 ⇒ 1 KiB/s
    LoopbackServer server(lc);

    syp::probe::TempCacheDir tmp("mip-trickle");
    syp_config cfg{};
    syp_config_init(&cfg);
    cfg.cache_dir = tmp.path.c_str();
    MediaInfoProvider mip(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    int64_t s = -1;
    int64_t e = -1;
    const syp_status st =
        mip.estimate_range_for_ms(server.url("/trickle.mp4").c_str(), 1000, &s, &e);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("  [trickle] estimate returned st=%d after %lld ms\n",
                static_cast<int>(st), static_cast<long long>(ms));
    // 判据是**时间**，不是状态码：这条路径上 st 必然是 NOT_IMPLEMENTED
    // （一份只读了几 KiB 的 moov-at-end MP4 解析不出流），但那件事已经由
    // probe_is_bounded_on_moov_at_end_and_falls_back 钉住了，这里要钉的是
    // "它多久之内放弃"。
    CHECK_EQ(st, SYP_ERR_NOT_IMPLEMENTED);
    CHECK(ms < 25000);
    // 下界同样要钉：看门狗被改成"立刻放弃"（比如 kProbeWallClockMs 写成 0）
    // 会把所有正常的慢探测一并打死，而上面那条 `< 25000` 对它一个字都没拦。
    CHECK(ms > 10000);
}

int main() { return tiny_test_main(); }
