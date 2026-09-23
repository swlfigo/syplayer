// SYPlayerPreloaderTests.swift — 缓存配置与预加载门面。
//
// 【为什么大部分用例不需要网络】add() 只是把条目登记进内部表并唤醒驱动线程，
// **同步返回**，所以 statistics.entries 在 add() 返回之后立刻可读。真正的
// 下载会因为拿不到网络而失败，但那不影响本文件要验的东西：映射、转发、
// 目录可配。下载行为本身由 C++ 侧的 ctest（preloader / preloader_hls）覆盖。
//
// 【有几条用例确实要 socket，而且必须要】
//   · deinit 不卡主线程：要的恰恰是"驱动线程正卡在 provider 探测里"
//     这个最坏情况，所以起一个**只监听、永不 accept 也永不响应**的 socket
//     （内核会替我们完成三次握手，连接建得上、数据永远不来）。这条用例量的
//     是墙钟，不是推理。
//   · 共享缓存（行为版）：要的是"预加载源真的把那个
//     cache key 打开着"，所以也得挂在同一个 socket 上不返回。
// 其余用例一律指向一个**刚刚关掉的**本地端口（连接立刻被拒），既不打外网，
// 也不会因为 DNS 或超时而变慢——包括唯一一条真的驱动 `-openURLString:` 的
// `testRealOpenURLStringCarriesTheCacheConfiguration`（实测 11ms）。
import XCTest
@testable import SYPlayerKit
import SYPlayerKit_Private

// MARK: - 测试用的两种本地端口

/// 只 bind + listen、从不 accept 也从不写任何字节的 TCP 端口。
///
/// 为什么不需要 accept：内核在 backlog 里就把三次握手做完了，客户端的
/// connect() 会成功、请求也发得出去，然后永远等不到响应——这正是
/// media_info_provider.h 里"只接受连接、永不响应的服务端"那个场景。
private final class HangingPort {
    private var fd: Int32 = -1
    let port: UInt16

    init() {
        // 全程用局部的 s，最后才赋给成员：闭包里碰 self.fd 会触发
        // "'self' captured by a closure before all members were initialized"。
        let s = socket(AF_INET, SOCK_STREAM, 0)
        var yes: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = 0                       // 让内核挑一个空闲端口
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        withUnsafePointer(to: &addr) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                _ = bind(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        _ = listen(s, 16)
        var out = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        withUnsafeMutablePointer(to: &out) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                _ = getsockname(s, $0, &len)
            }
        }
        fd = s
        port = UInt16(bigEndian: out.sin_port)
    }

    func shutdownNow() {
        if fd >= 0 { Darwin.close(fd); fd = -1 }
    }
    deinit { shutdownNow() }

    func url(_ name: String) -> URL { URL(string: "http://127.0.0.1:\(port)/\(name)")! }
}

/// 刚刚被关掉的本地端口——连上去立刻 ECONNREFUSED，不会有任何等待。
private func closedLocalPort() -> UInt16 {
    let p = HangingPort()
    let n = p.port
    p.shutdownNow()
    return n
}

private func tempCacheDir(_ tag: String) -> URL {
    FileManager.default.temporaryDirectory
        .appendingPathComponent("syp-\(tag)-\(UUID().uuidString)")
}

// MARK: - 缓存配置

@MainActor
final class SYPlayerCacheConfigurationTests: XCTestCase {
    func testDefaultsMatchTheSpec() {
        let c = SYPlayerCacheConfiguration()
        XCTAssertEqual(c.maxBytes, 512 * 1024 * 1024)
        XCTAssertEqual(c.minFreeSpaceBytes, 256 * 1024 * 1024)
        XCTAssertEqual(c.timeToLive, 7 * 24 * 60 * 60)
        XCTAssertEqual(c.directory.lastPathComponent, "syplayer-http-cache")
    }

    func testDefaultDirectoryIsUnderCachesNotTemporary() {
        let dir = SYPlayerCacheConfiguration.defaultDirectory
        // 此前写死在 NSTemporaryDirectory()；后来挪到
        // Caches——tmp 由系统在空间紧张时无预警回收，预加载花力气暖的字节
        // 不该落在那里。
        XCTAssertTrue(dir.path.contains("Caches"), dir.path)
    }

    func testCustomDirectoryIsCreatedOnUse() {
        let dir = tempCacheDir("kit-cache")
        let preloader = SYPlayerPreloader(cache: SYPlayerCacheConfiguration(directory: dir))
        XCTAssertEqual(preloader.cacheConfiguration.directory, dir)
        XCTAssertTrue(FileManager.default.fileExists(atPath: dir.path))
        try? FileManager.default.removeItem(at: dir)
    }

    /// `timeToLive` 是 `TimeInterval?`（秒），桥那侧是 `int64_t ttlMs`。
    /// nil → 0 是"永不过期"的约定值（syp_config.cache_ttl_ms 的 0 语义）。
    func testTimeToLiveMapsToMillisecondsAndNilMeansNeverExpire() {
        XCTAssertEqual(SYPlayerCacheConfiguration(timeToLive: 1.5).bridged().ttlMs, 1500)
        XCTAssertEqual(SYPlayerCacheConfiguration(timeToLive: nil).bridged().ttlMs, 0)
        XCTAssertEqual(SYPlayerCacheConfiguration(maxBytes: 7, minFreeSpaceBytes: 9)
                        .bridged().maxBytes, 7)
        XCTAssertEqual(SYPlayerCacheConfiguration(maxBytes: 7, minFreeSpaceBytes: 9)
                        .bridged().minFreeSpaceBytes, 9)
    }
}

// MARK: - 预加载门面

@MainActor
final class SYPlayerPreloaderTests: XCTestCase {
    private var deadPort: UInt16 = 0

    override func setUp() {
        super.setUp()
        deadPort = closedLocalPort()
    }

    private func makePreloader() -> SYPlayerPreloader {
        SYPlayerPreloader(cache: SYPlayerCacheConfiguration(directory: tempCacheDir("pre")))
    }

    private func url(_ path: String) -> SYPlayerSource {
        .url(URL(string: "http://127.0.0.1:\(deadPort)/\(path)")!)
    }

    func testAddAndRemoveTrackEntryCount() {
        let p = makePreloader()
        XCTAssertEqual(p.statistics.entries, 0)
        XCTAssertTrue(p.add(url("a.mp4")))
        XCTAssertEqual(p.statistics.entries, 1)
        XCTAssertTrue(p.add(url("b.mp4"), priority: .background))
        XCTAssertEqual(p.statistics.entries, 2)
        p.remove(url("a.mp4"))
        // remove 是异步生效的（真正的关闭在驱动线程上），所以这里不断言它
        // 立刻变成 1——只断言 removeAll 之后不会**涨**。
        p.removeAll()
        XCTAssertLessThanOrEqual(p.statistics.entries, 2)
    }

    func testAddIsIdempotentForTheSameURL() {
        let p = makePreloader()
        XCTAssertTrue(p.add(url("same.mp4"), priority: .background))
        // 同一个 URL 再 add 一次只抬优先级，**不是失败**（syp_preload.h）。
        XCTAssertTrue(p.add(url("same.mp4"), priority: .playing))
        XCTAssertEqual(p.statistics.entries, 1)
    }

    func testLocalFileSourceIsIgnored() {
        let p = makePreloader()
        let f = SYPlayerSource.file(URL(fileURLWithPath: "/tmp/nope.mp4"))
        XCTAssertFalse(p.add(f))          // 本地文件不需要预加载
        XCTAssertEqual(p.statistics.entries, 0)
        p.remove(f)                        // 也不该崩
        p.setPriority(.playing, for: f)
    }

    func testSetPriorityDoesNotChangeEntryCount() {
        let p = makePreloader()
        XCTAssertTrue(p.add(url("p.mp4"), priority: .next))
        p.setPriority(.playing, for: url("p.mp4"))
        XCTAssertEqual(p.statistics.entries, 1)
        // 没 add 过的 URL 是 no-op，不建条目、不崩。
        p.setPriority(.playing, for: url("never-added.mp4"))
        XCTAssertEqual(p.statistics.entries, 1)
    }

    /// 【偏离计划正文】计划里这条只验"不崩、条目仍是一条"——那对换算本身
    /// 没有鉴别力（写成 `$0 * 100` 照样绿）。`want_ms` 交给桥之后就观察不到了
    /// （`syp::dl::Preloader` 没有暴露它的测试缝，本任务不改 `src/dl`），所以
    /// 把换算提成了 `SYPlayerPreloader.milliseconds(for:)` 直接断言。真实的
    /// 时间→字节换算仍由 C++ 侧覆盖（tests/test_preloader.cpp 的
    /// time_target_uses_provider）。
    func testSecondsMapsToMilliseconds() {
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: 2.5), 2500)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: 0.0004), 0)   // 四舍五入
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: nil), 0)      // = 用默认 3000
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: -5), 0)       // 不把负数传下去
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: .infinity), 0)

        let p = makePreloader()
        XCTAssertTrue(p.add(url("t.mp4"), priority: .next, seconds: 2.5))
        XCTAssertEqual(p.statistics.entries, 1)
    }

    /// `providerMiss` 是"按时间预加载有没有真的生效"的唯一可观测信号。
    /// 这条用例证明它真的接上了底层那个计数器
    /// ——端口是关着的，探测必然失败，于是它必然涨。
    func testProviderMissCountsFailedTimeEstimates() {
        let p = makePreloader()
        XCTAssertEqual(p.statistics.providerMiss, 0)
        XCTAssertTrue(p.add(url("miss.mp4"), priority: .next, seconds: 2))
        let deadline = Date().addingTimeInterval(20)
        while p.statistics.providerMiss == 0 && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertGreaterThan(p.statistics.providerMiss, 0,
                             "providerMiss 没涨——要么没映射，要么探测根本没跑")
    }

    /// 桥上没装任何 block，但"没有保留环"这句话要有用例守着
    /// （教训：当时的 deinit 用例恰好断言在唯一没泄漏的对象上）。
    /// 所以这里 weak 的是**桥本身**，不是门面。
    func testPreloaderReleasesItsBridge() {
        weak var weakBridge: SypPreloaderBridge?
        do {
            let p = SYPlayerPreloader(
                cache: SYPlayerCacheConfiguration(directory: tempCacheDir("leak")))
            weakBridge = p.box.bridge
            XCTAssertNotNil(weakBridge)
        }
        // deinit 把最后一份强引用交给了后台队列（见 SYPlayerPreloader.deinit），
        // 所以释放是异步的，必须等——但等的是毫秒级：这个 preloader 没有条目，
        // 驱动线程一唤醒就能退出。
        let deadline = Date().addingTimeInterval(5)
        while weakBridge != nil && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertNil(weakBridge, "SypPreloaderBridge 泄漏了")
    }

    /// **量**出来的，不是推出来的。
    ///
    /// 场景是最坏情况：驱动线程正卡在 provider 的只读 header 探测里，而对面
    /// 是一个只接受连接、永不响应的服务端——同一台机器上实测一次这样的探测
    /// 要 6,015–6,021ms 才返回，
    /// 而 ~PreloadStack 必须等它返回才能 join。
    /// 若 deinit 在原地析构桥，这里量到的就是那 6 秒。
    /// 换成"1 KiB/s 慢速滴流"的服务端，这个数此前是**无界**
    /// 的（跑满 150 秒未结束，理论 ~34 分钟）；现在由 kProbeWallClockMs
    /// 兜在 ~15 秒。本用例仍然用 hang 那一类，因为它更短、更确定。
    func testDeinitDoesNotBlockTheCallingThread() {
        let hang = HangingPort()
        defer { hang.shutdownNow() }
        let dir = tempCacheDir("deinit")

        var p: SYPlayerPreloader? = SYPlayerPreloader(
            cache: SYPlayerCacheConfiguration(directory: dir),
            maxTotalTasks: 2, reservedForPlaying: 0)
        XCTAssertTrue(p!.add(.url(hang.url("hang.mp4")), priority: .next, seconds: 3))

        // 给驱动线程时间走到 Estimate 那一步并卡在里面。0.8s 远小于探测本身的
        // 6s 量级，所以这时它一定还没返回。
        RunLoop.current.run(until: Date().addingTimeInterval(0.8))

        let t0 = Date()
        p = nil
        let elapsedMs = Date().timeIntervalSince(t0) * 1000
        print("【实测】SYPlayerPreloader.deinit 墙钟 = \(elapsedMs) ms")
        XCTAssertLessThan(elapsedMs, 200,
                          "deinit 阻塞了调用线程 \(elapsedMs) ms（应当把析构交给后台队列）")
    }
}

// MARK: - SYPlayer(cache:) 与"同一份缓存"

@MainActor
final class SYPlayerCacheInitTests: XCTestCase {
    func testPlayerAcceptsACacheConfiguration() {
        let dir = tempCacheDir("player-cache")
        let cfg = SYPlayerCacheConfiguration(directory: dir, maxBytes: 1 << 20,
                                             minFreeSpaceBytes: 1 << 20,
                                             timeToLive: nil)
        let player = SYPlayer(cache: cfg)
        XCTAssertEqual(player.cacheConfiguration, cfg)
        // 目录在构造时就建好，不等到第一次 open 才失败。
        XCTAssertTrue(FileManager.default.fileExists(atPath: dir.path))
        // 桥真正会用的也是它（而不是仍然写死的 tmp）。
        XCTAssertEqual(player.cacheDirectoryInUseForTest, dir.path)
        player.close()
        try? FileManager.default.removeItem(at: dir)
    }

    func testDefaultInitKeepsDefaultConfiguration() {
        let player = SYPlayer()
        XCTAssertEqual(player.cacheConfiguration, SYPlayerCacheConfiguration())
        XCTAssertFalse(player.cacheDirectoryInUseForTest.contains("/T/"),
                       "默认目录仍然落在 tmp 下：\(player.cacheDirectoryInUseForTest)")
        player.close()
    }

    /// 预加载侧写带尾斜杠的目录、播放侧写不带的，
    /// **两边交给 CacheStore::make_key 的字符串必须逐字节相同**。
    ///
    /// 为什么尾斜杠这一侧只能从桥进：Swift 的 `URL.path` 恒会剥掉尾斜杠
    /// （`URL(fileURLWithPath: "/a/b/").path == "/a/b"`，本机实测），所以公开
    /// API 这一层根本产生不出带尾斜杠的目录。但桥那一层产生得出来——
    /// `NSTemporaryDirectory()` 就是带尾斜杠的，`SypCacheSettings.directory`
    /// 是一个裸 `NSString`。归一化必须守在桥里，这条用例就守在桥的入口上。
    func testPreloaderWithTrailingSlashSharesTheSameCacheDirString() throws {
        let dir = tempCacheDir("share")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        let settings = SypCacheSettings()
        settings.directory = dir.path + "/"        // ← 故意带尾斜杠
        let pre = SypPreloaderBridge(cache: settings, maxTotalTasks: 2,
                                     reservedForPlaying: 0,
                                     defaultPreloadBytes: 1 << 16,
                                     defaultPreloadMs: 0)
        let player = SYPlayer(cache: SYPlayerCacheConfiguration(directory: dir))
        defer { player.close() }

        let preDir = try XCTUnwrap(pre.cacheDirectoryInUse())
        XCTAssertEqual(preDir, player.cacheDirectoryInUseForTest)
        XCTAssertEqual(preDir, dir.path)
    }

    /// 上一条守的是尾斜杠，而尾斜杠
    /// **公开 API 根本产不出**（`URL(fileURLWithPath:"/a/b/").path == "/a/b"`）。
    /// 真正产得出、而且是**最自然写法**产出的，是内部的 `//`：
    /// `NSTemporaryDirectory()` 自身以 `/` 结尾，于是
    /// `NSTemporaryDirectory() + "/" + name` 就是 `…/T//name`。这种
    /// 拼法下 `peer_yield = 0`——让路逻辑静默失效，而
    /// `SYPlayerPreloader.swift` 顶上那句"只要 directory 相同就共享同一份缓存
    /// 索引"的公开承诺对它不成立、且不报任何错。
    func testInteriorDoubleSlashLandsOnTheSameCacheDirString() throws {
        let name = "syp-dslash-\(UUID().uuidString)"
        // 前提先钉住：这条用例的全部意义建立在"这样拼真的会多一个斜杠"上。
        let natural = NSTemporaryDirectory() + "/" + name
        XCTAssertTrue(natural.contains("//"), "NSTemporaryDirectory() 不再带尾斜杠了？\(natural)")
        let clean = URL(fileURLWithPath: NSTemporaryDirectory() + name)
        XCTAssertFalse(clean.path.contains("//"))
        defer { try? FileManager.default.removeItem(at: clean) }

        let settings = SypCacheSettings()
        settings.directory = natural               // ← 内部双斜杠
        let pre = SypPreloaderBridge(cache: settings, maxTotalTasks: 2,
                                     reservedForPlaying: 0,
                                     defaultPreloadBytes: 1 << 16,
                                     defaultPreloadMs: 0)
        let player = SYPlayer(cache: SYPlayerCacheConfiguration(directory: clean))
        defer { player.close() }

        let preDir = try XCTUnwrap(pre.cacheDirectoryInUse())
        XCTAssertEqual(preDir, player.cacheDirectoryInUseForTest)
        XCTAssertEqual(preDir, clean.path)
    }

    /// `.` 与 `..` 这两种拼法 **`URL.path` 也原样保留**（本机实测
    /// `URL(fileURLWithPath:"/tmp/./d").path == "/tmp/./d"`、
    /// `URL(fileURLWithPath:"/tmp/a/../b").path == "/tmp/a/../b"`），所以它们能
    /// 从**公开 API** 直接进来，不像尾斜杠那样只能从桥进。这条用例整条都在
    /// 公开 API 上，一个桥类型都不碰。
    func testDotAndDotDotSpellingsLandOnTheSameCacheDirString() {
        let base = NSTemporaryDirectory() + "syp-dots-\(UUID().uuidString)"
        let clean = URL(fileURLWithPath: base)
        defer { try? FileManager.default.removeItem(at: clean) }

        let dotted = URL(fileURLWithPath: base + "/./")
        let dotdot = URL(fileURLWithPath: base + "/sub/..")
        // 前提：Foundation 确实没有替我们归一化。任一条不成立这条用例就失去意义。
        // 实测 `URL(fileURLWithPath: "…/x/./").path` == "…/x/."（尾斜杠被剥、
        // "." 留着），`"…/x/sub/.."` 原样保留。
        XCTAssertTrue(dotted.path.hasSuffix("/."), dotted.path)
        XCTAssertTrue(dotdot.path.hasSuffix("/.."), dotdot.path)

        let a = SYPlayer(cache: SYPlayerCacheConfiguration(directory: clean))
        let b = SYPlayer(cache: SYPlayerCacheConfiguration(directory: dotted))
        let c = SYPlayer(cache: SYPlayerCacheConfiguration(directory: dotdot))
        defer { a.close(); b.close(); c.close() }

        XCTAssertEqual(b.cacheDirectoryInUseForTest, a.cacheDirectoryInUseForTest)
        XCTAssertEqual(c.cacheDirectoryInUseForTest, a.cacheDirectoryInUseForTest)
        XCTAssertEqual(a.cacheDirectoryInUseForTest, clean.path)
    }

    /// 【行为版】上面三条断的是字符串，这条断的是**后果**：用内部双
    /// 斜杠拼法开着的那个缓存条目，播放侧（干净拼法）能不能看见。看不见就意味
    /// 着 `Preloader` 的"给同一 key 上的播放源让路"整个失效——正是
    /// 这个 `peer_yield = 0`。形状与
    /// `testPlayerAndPreloaderLandOnTheSameCacheKey` 相同，只把尾斜杠换成 `//`。
    func testInteriorDoubleSlashLandsOnTheSameCacheKey() {
        let hang = HangingPort()
        defer { hang.shutdownNow() }
        let name = "syp-dskey-\(UUID().uuidString)"
        let clean = URL(fileURLWithPath: NSTemporaryDirectory() + name)
        try? FileManager.default.createDirectory(at: clean, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: clean) }
        let mediaURL = hang.url("shared-dslash.mp4")

        let withDoubleSlash = SypCacheSettings()
        withDoubleSlash.directory = NSTemporaryDirectory() + "/" + name
        let holder = SypPreloaderBridge(cache: withDoubleSlash, maxTotalTasks: 2,
                                        reservedForPlaying: 0,
                                        defaultPreloadBytes: 1 << 16,
                                        defaultPreloadMs: 0)
        XCTAssertEqual(holder.cacheOpenCount(forURLString: mediaURL.absoluteString), 0)
        XCTAssertTrue(holder.add(mediaURL.absoluteString, priority: .next, milliseconds: 0))

        let player = SYPlayer(cache: SYPlayerCacheConfiguration(directory: clean))
        defer { player.close() }
        let asPlayerSeesIt = SypCacheSettings()
        asPlayerSeesIt.directory = player.cacheDirectoryInUseForTest
        let probe = SypPreloaderBridge(cache: asPlayerSeesIt, maxTotalTasks: 1,
                                       reservedForPlaying: 1,
                                       defaultPreloadBytes: 1 << 16,
                                       defaultPreloadMs: 0)

        var count: Int64 = 0
        let deadline = Date().addingTimeInterval(10)
        while count <= 0 && Date() < deadline {
            count = probe.cacheOpenCount(forURLString: mediaURL.absoluteString)
            if count > 0 { break }
            RunLoop.current.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertGreaterThan(count, 0,
                             "内部双斜杠的目录拼法没有归一化——两边的 cache key 不是"
                             + "同一个，预加载暖的字节播放侧看不见，且不报任何错")
        holder.removeAll()
    }

    /// 行为版，这条才是真正有鉴别力的：让播放真的把一个 URL 的
    /// 缓存条目打开着，然后从**预加载那一侧**去问"这个 key 上有几个持有者"。
    ///
    /// 【为什么不从文件系统数 .idx/.dat】数不出来。`CacheIndex::key_for_url`
    /// 只哈希 URL，不含目录，所以 "…/dir/" 与 "…/dir" 落到磁盘上**本来就是
    /// 同一组文件**——两份缓存这件事根本不发生在文件系统上，它发生在
    /// `CacheStore` 的进程内注册表里
    ///
    /// 【澄清】`std::filesystem::path` 的 `/` 运算符只吃掉**一个尾斜杠**，
    /// "…/dir/" 拼出来是 `/tmp/dir//<hash>.idx`（两个斜杠原样留着）。
    /// 磁盘上仍然只有一组文件，靠的是 **POSIX 路径解析把 `//` 当 `/`**，不是
    /// `fs::path` 折叠。
    /// （`make_key` 拼的是 `cache_dir + '\0' + hash`，**不归一化目录**）。
    /// 所以"目录写法不同 ⇒ 两份缓存"的可观测后果是：两个 CacheIndex 对象同时
    /// 打开同一组文件互相盖写，以及 Preloader 的"给同一 key 上的播放源让路"
    /// 整个失效。open_count 就是后者用的那个数，所以断在它上面。
    func testPlayerAndPreloaderLandOnTheSameCacheKey() {
        let hang = HangingPort()
        defer { hang.shutdownNow() }
        let dir = tempCacheDir("samekey")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let mediaURL = hang.url("shared.mp4")

        // 甲：预加载侧，目录**带尾斜杠**。defaultPreloadMs 传 0 是故意的——
        // 这样条目不需要先问 provider（不走那条 6 秒的探测路径），驱动线程
        // 下一轮就直接把源开出来，而对面那个端口只握手不回数据，于是这条源
        // 会一直开着，open_count 稳定可观测。
        let withSlash = SypCacheSettings()
        withSlash.directory = dir.path + "/"
        let holder = SypPreloaderBridge(cache: withSlash, maxTotalTasks: 2,
                                        reservedForPlaying: 0,
                                        defaultPreloadBytes: 1 << 16,
                                        defaultPreloadMs: 0)
        XCTAssertEqual(holder.cacheOpenCount(forURLString: mediaURL.absoluteString), 0)
        XCTAssertTrue(holder.add(mediaURL.absoluteString, priority: .next, milliseconds: 0))

        // 乙：播放侧，公开 API，目录不带尾斜杠。**不真的去 open** —— 对着一个
        // 永不响应的端口 open 会按 syp_config 的默认读超时×重试挂住（本机实测
        // 整条用例要 120 秒，而 close() 排在同一条串行队列上、打断不了它）。
        // 要验的也不是 open 本身，而是"播放侧算出来的那个 cache_dir 与预加载侧
        // 的是不是同一个 key"，所以拿播放侧**真正会用**的那个字符串去问。
        let player = SYPlayer(cache: SYPlayerCacheConfiguration(directory: dir))
        defer { player.close() }
        let asPlayerSeesIt = SypCacheSettings()
        asPlayerSeesIt.directory = player.cacheDirectoryInUseForTest
        let probe = SypPreloaderBridge(cache: asPlayerSeesIt, maxTotalTasks: 1,
                                       reservedForPlaying: 1,   // 额度 0：它自己什么都不下
                                       defaultPreloadBytes: 1 << 16,
                                       defaultPreloadMs: 0)

        var count: Int64 = 0
        let deadline = Date().addingTimeInterval(10)
        while count <= 0 && Date() < deadline {
            count = probe.cacheOpenCount(forURLString: mediaURL.absoluteString)
            if count > 0 { break }
            RunLoop.current.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertGreaterThan(count, 0,
                             "用播放侧的 cache_dir 去问，看不见预加载侧开着的那个条目——"
                             + "两边的 cache key 不是同一个，缓存被拆成了两份")
        holder.removeAll()
    }
}

// MARK: - 容量 / TTL 真的到 C 层了没有
//
// 【为什么单开一节】"目录/容量/TTL 从写死变成可配"这句承诺，
// 目录那一半有 `cacheDirectoryInUse` 这条缝、有变异验证；容量/TTL 那一半在
// ObjC++→C 边界上断了：把 `SYPBridge.mm` 的 `prepare_cache_dir` 里
//     cfg->max_cache_bytes = 0; cfg->min_free_space_bytes = 0; cfg->cache_ttl_ms = 0;
// 三行改成硬 0（缓存不再有上限、不再过期、无限涨），`Executed 74 tests, with 0
// failures`——一条都不红。按"没做变异验证的用例视为没写"这条纪律，
// 那一半等于没写。下面三条补的就是它。
//
// 三个字段**刻意取三个互不相同、也不等于默认值的数**，这样一个一个单独变异
// 都能被单独钉住（把某一个字段改成 0 或抄另一个字段的值，只有对应的断言红）。
@MainActor
final class SYPlayerCacheCapacityWiringTests: XCTestCase {
    private static let maxBytes: Int64 = 12_345_678
    private static let minFree: Int64  = 2_345_678
    private static let ttlSeconds      = 1.5
    private static let ttlMs: Int64    = 1500

    private func config(_ dir: URL) -> SYPlayerCacheConfiguration {
        SYPlayerCacheConfiguration(directory: dir,
                                   maxBytes: Self.maxBytes,
                                   minFreeSpaceBytes: Self.minFree,
                                   timeToLive: Self.ttlSeconds)
    }

    private func assertCarried(_ s: SypCacheSettings, dir: URL,
                               file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(s.directory, dir.path, "cache_dir", file: file, line: line)
        XCTAssertEqual(s.maxBytes, Self.maxBytes, "max_cache_bytes", file: file, line: line)
        XCTAssertEqual(s.minFreeSpaceBytes, Self.minFree,
                       "min_free_space_bytes", file: file, line: line)
        XCTAssertEqual(s.ttlMs, Self.ttlMs, "cache_ttl_ms", file: file, line: line)
    }

    /// 预加载侧：读的是 **provider 自己手里那份 `syp_config`**（`PreloadStack::
    /// create` 交给它、它在构造时整份拷下来的那一个），不是 `initWithCache:`
    /// 收到的 `SypCacheSettings` 的回声。也就是说这条断言穿过了
    /// Swift → `SypCacheSettings` → `prepare_cache_dir` → `syp_config` →
    /// `PreloadStack::create` → `MediaInfoProvider::cfg_` 整条链。
    /// 第二条断言断的是 **`Preloader` 那一支**。
    /// 上面那条只穿到 `MediaInfoProvider::cfg_`，而那是**支流**：探测只开一次
    /// 源读个 header，真正承担全部预加载下载、给每一条 `SourceBridge` 派配置的
    /// 是 `syp::dl::Preloader`。在 `media_info_provider.cpp` 里 provider
    /// 构造之后、`Preloader::create` 之前把三个容量字段清零，
    /// **`ctest 33/33` 与 `xcodebuild 82 tests, 0 failures` 同时全绿**——后果
    /// 正是要拦的那一条：缓存不再有上限、不再过期、无限涨，且不报错。
    /// 两条断言一起，那条变异才必须变红。
    func testPreloaderCarriesCapacityAndTTLIntoTheCLayer() throws {
        let dir = tempCacheDir("cap-pre")
        defer { try? FileManager.default.removeItem(at: dir) }
        let p = SYPlayerPreloader(cache: config(dir))
        let eff = try XCTUnwrap(p.box.bridge.cacheSettingsInUse(),
                                "底层没建起来——目录为空或 HTTP 后端没注册上")
        assertCarried(eff, dir: dir)

        let dl = try XCTUnwrap(p.box.bridge.preloaderCacheSettingsInUse(),
                               "底层没建起来——目录为空或 HTTP 后端没注册上")
        assertCarried(dl, dir: dir)
    }

    /// 播放侧（"下一次 open 会用什么"）：`-cacheSettingsInUse` 走的是与
    /// `-openURLString:` 同一组函数（`effective_cache_dir` + `fill_cache_config`）。
    /// 它不驱动真的 open，所以是 ~0 秒；真 open 那条在下面。
    func testPlayerCarriesCapacityAndTTLIntoItsNextOpenConfig() {
        let dir = tempCacheDir("cap-player")
        defer { try? FileManager.default.removeItem(at: dir) }
        let player = SYPlayer(cache: config(dir))
        defer { player.close() }
        assertCarried(player.cacheSettingsInUseForTest, dir: dir)
    }

    /// 【用例从不驱动真正的 `-openURLString:`】上面那条与
    /// `-openURLString:` 共用 `prepare_cache_dir`，所以今天分叉不了；但"今天
    /// 共用"不是用例的性质，是实现的性质。这条**真的调 `-openURLString:`**，
    /// 断言落在它自己构造、真的交给 `syp_source_open` / `Pipeline::create_hls`
    /// 的那份 `syp_config` 上（`-lastOpenCacheConfig`，在任何网络 IO 之前拍下）。
    ///
    /// 【为什么便宜 —— 量过】目标是一个**刚刚关掉**的本地端口：`connect()` 立刻
    /// ECONNREFUSED，不走 DNS、不等超时。**本机实测整条 11.3 ms**
    /// （`ok=false, status=-10`）。以前实测的 120 秒是对着一个"只握手、
    /// 永不响应"的端口量的（默认读超时 × 重试，且 `close()` 排在同一条串行
    /// 队列上打断不了），那是另一种端口。门槛设在 2,000 ms（180× 余量）：
    /// 哪天这条路径变慢了它会红，而不是默默把整个套件拖住。
    func testRealOpenURLStringCarriesTheCacheConfiguration() throws {
        let dir = tempCacheDir("cap-open")
        defer { try? FileManager.default.removeItem(at: dir) }
        let player = SYPlayer(cache: config(dir))
        defer { player.close() }
        let bridge = try XCTUnwrap(player.bridgeForTesting as? SypPlayerBridge)
        XCTAssertNil(bridge.lastOpenCacheConfig, "还没 open 就有快照？")

        let dead = closedLocalPort()
        var status: Int32 = 0
        let t0 = Date()
        let ok = bridge.openURLString("http://127.0.0.1:\(dead)/nope.mp4",
                                      hardwareDecode: false, errorOut: &status)
        let elapsedMs = Date().timeIntervalSince(t0) * 1000
        print("【实测】-openURLString: 对着已关闭端口 = \(elapsedMs) ms, ok=\(ok), status=\(status)")
        XCTAssertFalse(ok, "对着已关闭的端口竟然打开成功了？")
        XCTAssertLessThan(elapsedMs, 2_000,
                          "-openURLString: 对 ECONNREFUSED 花了 \(elapsedMs) ms（实测应 ~11ms）")

        let eff = try XCTUnwrap(bridge.lastOpenCacheConfig,
                                "-openURLString: 没有拍下它真正用的那份配置")
        assertCarried(eff, dir: dir)
    }

    /// `-cacheDirectoryInUse` / `-cacheSettingsInUse` 是 **getter**，
    /// 读一下**不应该在开发机上建出目录**。此前播放侧的那个走的是
    /// `prepare_cache_dir`，于是光跑一条 `SYPlayer()` 的用例就在
    /// `~/Library/Caches/` 下留了个 `syplayer-http-cache`；而预加载侧的同名方法
    /// 无副作用——两侧不对称。现在统一成纯读，建目录只发生在
    /// `-setCacheSettings:`（建不出来要在配置那一刻就暴露）与真的 open。
    ///
    /// 做法：先让 init 把目录建出来，删掉，再读两个 getter，断言它没回来。
    func testCacheGettersDoNotCreateTheDirectory() {
        let dir = tempCacheDir("pure-getter")
        let player = SYPlayer(cache: config(dir))
        defer { player.close() }
        XCTAssertTrue(FileManager.default.fileExists(atPath: dir.path),
                      "init 应该建目录（D-3）")
        try? FileManager.default.removeItem(at: dir)
        XCTAssertFalse(FileManager.default.fileExists(atPath: dir.path))

        XCTAssertEqual(player.cacheDirectoryInUseForTest, dir.path)
        assertCarried(player.cacheSettingsInUseForTest, dir: dir)

        XCTAssertFalse(FileManager.default.fileExists(atPath: dir.path),
                       "getter 把目录建回来了——它不是个 getter")
    }

    /// `add(_:priority:seconds:)` 对"非空但下不下来"的 URL
    /// **返回 `true`**，失败只在 `statistics.failed` 里露出来。这不是缺陷，是
    /// 契约（理由见 `add` 的文档注释：判它要把 dl 层的 scheme/HLS 判据在 Swift
    /// 侧再转写一份，而本里程碑已被"同一道闸的第二份转写"咬过两次）。
    /// 契约要有用例守着，否则下一个人"顺手加个校验"没人会发现文档失效了。
    func testAddReturnsTrueForANonEmptyButUnfetchableURL() {
        let p = SYPlayerPreloader(cache: SYPlayerCacheConfiguration(directory: tempCacheDir("bogus")))
        // 非空 absoluteString，但没有任何后端能下它。
        XCTAssertTrue(p.add(.url(URL(string: "ftp://example.invalid/x.mp4")!)))
        XCTAssertTrue(p.add(.url(URL(string: "notaurl")!)))
        XCTAssertEqual(p.statistics.entries, 2, "两条都该进表——登记成功与下得下来是两件事")

        // 文档说"唯一的观测口是 statistics.failed"，
        // 而上一轮只钉了 entries==2，那句话本身没有任何用例守着——把它删了、
        // 或者哪天 failed 不再涨了，没人会发现。这里把它钉住。
        //
        // 两条都下不下来（`ftp://` 没有后端；`notaurl` 连 scheme 都没有），
        // 所以 failed 最终必须到 2。失败是**异步**的，所以等一个谓词而不是睡。
        // 门槛 5,000 ms：两条都不涉及网络往返（一条无后端、一条连主机都解析
        // 不出），实测远在 100 ms 内收敛；设这么宽是为了"哪天变慢它红而不是
        // 默默拖住套件"。
        let deadline = Date().addingTimeInterval(5.0)
        while p.statistics.failed < 2 && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        XCTAssertEqual(p.statistics.failed, 2,
                       "两条下不下来的 URL 应该都进 failed（文档承诺的唯一观测口）")
        // 注意这条断言**证不了归因**：failed 是单调累积的总数，没有按 URL 的
        // 分解（preloader.cpp 只在条目进 Failed 时 ++failed）。"涨到 2" 与
        // "某一条失败了两次" 在这个通道上不可分辨——文档里写明了这一点。
    }
}

// MARK: - 有限/非有限 Double 不能把调用方的 App 打崩
//
// 【这一节补的是什么】两个**公开**入口原先在合法的 Double 上直接 trap，
// Release 下同样崩（Swift 的 `Int64(Double)` 越界是 `fatalError`，不是
// Debug-only 的 precondition）：
//   · `SYPlayerCacheConfiguration.bridged()` 的 `Int64(max(0, $0*1000).rounded())`
//     **连 isFinite 都没有** ⇒ `SYPlayerCacheConfiguration(timeToLive: .infinity)`
//     实测 `Fatal error: Double value cannot be converted to Int64 because it is
//     either infinite or NaN`，xctest 进程 EXIT=65。
//   · `SYPlayerPreloader.milliseconds(for:)` 有 isFinite、**没有上界**
//     ⇒ `add(seconds: 1e18)` 实测 `Fatal error: Double value cannot be
//     converted to Int64 because it is greater than Int64.max`，同样 EXIT=65。
// 而 `.infinity` 恰恰是"永不过期"最自然的写法（文档说的是 `nil`，但没有
// 任何东西拒绝 `.infinity`），且经 `SYPlayer.init(cache:)` 也到得了。
//
// 【夹住而不是盲转 —— 语义在这里钉死】
//   · 非有限（`.infinity` / `-.infinity` / `.nan`）⇒ 与"没给值"同义 ⇒ 0。
//     TTL 的 0 = 不按时间过期（正是 `.infinity` 想表达的那件事）；
//     `add(seconds:)` 的 0 = 由底层用 default_preload_ms。
//   · 有限但乘 1000 之后超出 Int64 ⇒ 夹到 `Int64.max`，不 trap。
//   · ≤ 0（含负数、-0.0）⇒ 0，与原行为一致。
//
// 去掉上界那一条 guard ⇒ 1e18 / greatestFiniteMagnitude 两条
// 立刻把进程打崩（EXIT=65）；去掉 isFinite 那一条 guard ⇒ `.infinity` /
// `.nan` 两条同样打崩。两个 guard 各自 load-bearing。
@MainActor
final class SYPlayerDoubleClampTests: XCTestCase {

    // MARK: milliseconds(for:) —— add(seconds:) 那一条路

    func testMillisecondsClampsNonFiniteInput() {
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: .infinity), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: -.infinity), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: .nan), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: .signalingNaN), 0)
    }

    func testMillisecondsClampsFiniteOverflowInsteadOfTrapping() {
        // 1e18 秒 × 1000 = 1e21 ms，远超 Int64.max(≈9.22e18)。原先在这里 trap。
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: 1e18), Int64.max)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: .greatestFiniteMagnitude),
                       Int64.max)
        // 边界：恰好 Int64.max 毫秒附近不能因为 ×1000 / rounded() 往上跨一格就 trap。
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: Double(Int64.max) / 1000),
                       Int64.max)
    }

    func testMillisecondsKeepsOrdinaryValuesExact() {
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: 0), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: -0.0), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: -5), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: -1e18), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: nil), 0)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: 2.5), 2500)
        XCTAssertEqual(SYPlayerPreloader.milliseconds(for: 0.0004), 0)
    }

    /// 走**真的公开 API**，不是只测那个静态函数：`add(seconds:)` 是调用方
    /// 手上那条路，它才是原先崩的地方。
    func testAddWithExtremeSecondsDoesNotCrash() {
        let dir = tempCacheDir("clamp-add")
        defer { try? FileManager.default.removeItem(at: dir) }
        let p = SYPlayerPreloader(cache: SYPlayerCacheConfiguration(directory: dir))
        XCTAssertTrue(p.add(.url(URL(string: "http://127.0.0.1:9/a.mp4")!), seconds: 1e18))
        XCTAssertTrue(p.add(.url(URL(string: "http://127.0.0.1:9/b.mp4")!), seconds: .infinity))
        XCTAssertTrue(p.add(.url(URL(string: "http://127.0.0.1:9/c.mp4")!), seconds: .nan))
        XCTAssertTrue(p.add(.url(URL(string: "http://127.0.0.1:9/d.mp4")!),
                            seconds: .greatestFiniteMagnitude))
        XCTAssertEqual(p.statistics.entries, 4)
    }

    // MARK: SYPlayerCacheConfiguration.timeToLive —— bridged() 那一条路

    private func ttlMs(_ ttl: TimeInterval?) -> Int64 {
        SYPlayerCacheConfiguration(directory: tempCacheDir("clamp-ttl"),
                                   timeToLive: ttl).bridged().ttlMs
    }

    func testCacheConfigurationClampsNonFiniteTimeToLive() {
        // `.infinity` = "永不过期"，与文档指定的 `nil` 落在同一个值上（0）。
        XCTAssertEqual(ttlMs(.infinity), 0)
        XCTAssertEqual(ttlMs(-.infinity), 0)
        XCTAssertEqual(ttlMs(.nan), 0)
        XCTAssertEqual(ttlMs(nil), 0)
    }

    func testCacheConfigurationClampsFiniteOverflowTimeToLive() {
        XCTAssertEqual(ttlMs(1e18), Int64.max)
        XCTAssertEqual(ttlMs(.greatestFiniteMagnitude), Int64.max)
    }

    func testCacheConfigurationKeepsOrdinaryTimeToLiveExact() {
        XCTAssertEqual(ttlMs(0), 0)
        XCTAssertEqual(ttlMs(-0.0), 0)
        XCTAssertEqual(ttlMs(-5), 0)
        XCTAssertEqual(ttlMs(1.5), 1500)
        XCTAssertEqual(ttlMs(7 * 24 * 60 * 60), 604_800_000)
    }

    /// `.infinity` 经 `SYPlayer.init(cache:)` 也到得了——这条路同样不许崩。
    func testPlayerInitWithInfiniteTimeToLiveDoesNotCrash() {
        let dir = tempCacheDir("clamp-player")
        defer { try? FileManager.default.removeItem(at: dir) }
        let player = SYPlayer(cache: SYPlayerCacheConfiguration(directory: dir,
                                                               timeToLive: .infinity))
        defer { player.close() }
        XCTAssertEqual(player.cacheSettingsInUseForTest.ttlMs, 0)
    }
}
