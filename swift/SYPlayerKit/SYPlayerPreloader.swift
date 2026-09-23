// SYPlayerPreloader.swift — 公开门面：缓存配置 + 预加载。
//
// 与 SYPlayer 同一套纪律（SYPlayer.swift 顶部三条不变量）：全部公开成员
// @MainActor，业务侧看不到任何 C 类型。这里比 SYPlayer 简单得多——预加载
// 没有回调、没有状态推送，全部方法都是"登记一条意图"，底层驱动线程自己
// 去跑，所以不需要 openQueue、不需要会话号，**也不需要给桥装任何 block**
// （那条 `res → bridge → block → res` 的保留环在这里根本没有
// 成立的条件：桥上没有一个属性会回指过来）。
//
// **除析构外所有方法都不阻塞**：syp_preloader_* / syp::dl::Preloader 只在
// 内部锁下改一张表再唤醒驱动线程（syp_preload.h 明写），真正的网络 IO 一次
// 都不发生在调用线程上。这是它可以标 @MainActor 而不必 async 的全部理由。
//
// **析构是例外，而且例外得很厉害**：~Preloader 要 join 驱动线程，而驱动线程
// 可能正卡在 provider 的 estimate_range_for_ms 里。两类慢服务端都量过：
// "只接受连接、永不响应"实测 **6,015–6,021ms**（三个单次读超时 × 重试），
// "1 KiB/s 慢速滴流"此前**无界**（跑满 150 秒未结束，理论 ~34 分钟），
// 后来给它补了一条 15,000ms 的墙钟看门狗，实测 15,047–15,065ms
// 返回。见 media_info_provider.h 的 kProbeWallClockMs。
// deinit 跑在释放最后一份强引用的那个线程上，对一个 @MainActor 类来说通常
// 就是主线程——所以 deinit **不在原地析构桥**，见下面 deinit 的注释。
import Foundation
import SYPlayerKit_Private

/// HTTP 缓存的目录与容量。`SYPlayer` 与 `SYPlayerPreloader` 各自持有一份；
/// **只要 `directory` 相同，两者就共享同一份缓存索引**，于是"预加载暖好的
/// 字节播放时直接命中"是结构事实，不靠调用方自己对齐。
///
/// 目录会在两边被同一个**词法**归一化函数处理（`syp::dl::normalize_cache_dir`：
/// 折叠 `//`、消掉 `.`、按字面弹掉 `..`，再剥尾部 `/`），所以写法上的差异
/// （`…/dir` vs `…/dir/` vs `…//dir` vs `…/./dir` vs `…/x/../dir`）不会变成
/// 两份缓存。
///
/// **两条例外，写在这里免得下一个人以为"目录相同"是个直觉判断**：
/// - **不做 Unicode 归一化**。实际上不需要：两边都经 `directory.path`，而
///   Foundation 的 `URL.path` 恒输出 NFD（实测 NFC 的 `café` 进去出来是
///   `e` + U+0301），带重音的目录名逐字节相同。
/// - **不解析符号链接**。`/tmp/x` 与 `/private/tmp/x` 在 macOS 上是同一个
///   目录、却是两个 key；`..` 也是按字面弹的。要跨符号链接合流得调用方自己
///   先 `resolvingSymlinksInPath()`。
///   **后果不止"多一份缓存"**：当一个跨目录的符号链接出现在 `..` 前面时，
///   我们**真正使用的目录与调用方自己拼出来的不是同一个**（实测：`$T/x` 是
///   指向 `$T/y` 的符号链接，`$T/x/link/../c` 我们解成 `$T/x/c`，而 POSIX
///   解析给的是 `$T/y/c`），而且如果那个词法父目录只读，目录会**静默建不
///   出来**（建目录的失败不上报，见 `SYPlayer.init(cache:)`）。顺带一提，
///   Foundation 自己的 `standardizedFileURL` 与 `resolvingSymlinksInPath`
///   在这种输入上给的也都是词法答案，我们与平台的约定一致。
public struct SYPlayerCacheConfiguration: Sendable, Equatable {
    /// 缓存根目录。默认 `Caches/syplayer-http-cache`。
    ///
    /// 【为什么不是 tmp】此前写死在 `NSTemporaryDirectory()` 下。
    /// tmp 由系统在空间紧张时无预警回收，而预加载
    /// 花了带宽暖出来的字节恰恰最不该被那样回收。Caches 同样可被系统回收，
    /// 但优先级低得多，也是 App 自己的清理逻辑扫得到的位置。
    ///
    /// ⚠️ **必须是 file URL**，而且这一条**没有任何校验**。
    /// 底层只取 `directory.path`，而 `URL`
    /// 对非 file URL 也照样给得出一个 `path`，两种实测结果都很糟：
    ///   · `URL(string: "https://example.com/cache")!.path` == `"/cache"`
    ///     —— 文件系统**根目录**下的 cache；
    ///   · `URL(string: "~/Library/Caches/syp")!.path` == `"~/Library/Caches/syp"`
    ///     —— 字面量 `~`，会在**当前工作目录**下建一个名叫 `~` 的目录。
    /// **这里刻意不加 `precondition`**：`directory` 是个公开的可变存储属性，
    /// 校验只能放在 `init` 或 `bridged()` 里，而在一个公开初始化器上崩掉
    /// 调用方的 App，代价比"缓存落到一个奇怪目录"（沙盒里通常就是建不出来、
    /// 退化成没有缓存）大得多。
    /// **但 `bridged()` 里有一条 `assert`**：Debug 下断住、
    /// Release 下完全 no-op，所以写错的人在开发期就会看见，线上一个人都不崩。
    /// 请用 `URL(fileURLWithPath:)` 或 `FileManager.urls(for:in:)` 构造。
    public var directory: URL
    /// 磁盘缓存上限，0 = 不限。超出时按最久未使用淘汰，**正在被打开的资源
    /// 不会被淘汰**。默认 512 MiB。
    public var maxBytes: Int64
    /// 卷可用空间低于此值时触发淘汰，0 = 不看可用空间。默认 256 MiB。
    public var minFreeSpaceBytes: Int64
    /// 缓存条目存活时间，`nil` = 不按时间过期。默认 7 天。
    ///
    /// **非有限与超大值都被夹住，不会崩**：`.infinity` /
    /// `-.infinity` / `.nan` 一律等同于 `nil`（不按时间过期）；有限但
    /// ×1000 之后超出 `Int64` 的值（`1e18`、`.greatestFiniteMagnitude`）
    /// 夹到 `Int64.max` 毫秒。此前这里是一次无保护的 `Int64(Double)`，
    /// `timeToLive: .infinity` 会 `Fatal error` 打崩调用方的 App，Release
    /// 下也一样。
    public var timeToLive: TimeInterval?

    public init(directory: URL = SYPlayerCacheConfiguration.defaultDirectory,
                maxBytes: Int64 = 512 * 1024 * 1024,
                minFreeSpaceBytes: Int64 = 256 * 1024 * 1024,
                timeToLive: TimeInterval? = 7 * 24 * 60 * 60) {
        self.directory = directory
        self.maxBytes = maxBytes
        self.minFreeSpaceBytes = minFreeSpaceBytes
        self.timeToLive = timeToLive
    }

    public static var defaultDirectory: URL {
        URL(fileURLWithPath: SypCacheSettings.defaultDirectory(), isDirectory: true)
    }

    /// `internal` 而不是 `public`：`SypCacheSettings` 是私有模块
    /// （`SYPlayerKit_Private`）里的类型，把它露在公开 API 上等于把桥的类型
    /// 钉进公开 ABI——`SYPBridge.h` 顶部那条"C/ObjC 桥类型不许穿透到公开
    /// Swift API"的纪律正是冲着这件事。也不能是 `fileprivate`：`SYPlayer.swift`
    /// 的 `init(cache:)` 要用它。
    func bridged() -> SypCacheSettings {
        let s = SypCacheSettings()
        // 【非 file URL 的第三档处置】
        // 上一轮在"加 precondition（崩 App）"与"什么都不做"之间二选一，选了
        // 后者。中间还有一档没考虑到，而它正是 Swift 为这种情况准备的惯用法：
        // `assertionFailure` 在 **Debug 下断住**（写这行代码的人当场看见），
        // 在 **Release 下是完全的 no-op**（不崩任何线上用户）。
        //
        // 选它而不是"退回 defaultDirectory"：静默换一个目录会让调用方拿到一个
        // "看起来成功了、但缓存落在别处"的结果，比落到 `/cache` 更难查。行为
        // 因此一个字节都没变，变的只是 Debug 下有没有人告诉你。
        assert(directory.isFileURL,
               "SYPlayerCacheConfiguration.directory 必须是 file URL。"
               + "非 file URL 的 .path 会给出一个看似合理却完全错误的路径"
               + "（\"https://example.com/cache\" → \"/cache\"，文件系统根目录下）。"
               + "请用 URL(fileURLWithPath:) 或 FileManager.urls(for:in:) 构造。")
        s.directory = directory.path
        s.maxBytes = maxBytes
        s.minFreeSpaceBytes = minFreeSpaceBytes
        // nil → 0（不按时间过期），与 syp_config.cache_ttl_ms 的 0 语义一致。
        //
        // 原先是 `Int64(max(0, $0 * 1000).rounded())`，**连
        // isFinite 都没有**。实测 `SYPlayerCacheConfiguration(timeToLive: .infinity)`
        // ⇒ `Fatal error: Double value cannot be converted to Int64 because it
        // is either infinite or NaN`，把进程打崩（xctest EXIT=65），而且
        // `Int64(Double)` 的越界检查在 **Release 下同样在**（它是 fatalError，
        // 不是 Debug-only 的 precondition）——也就是说线上用户照崩。
        // `.infinity` 恰恰是"永不过期"最自然的写法：文档指定的拼法是 `nil`，
        // 但没有任何东西拒绝 `.infinity`，而它经 `SYPlayer.init(cache:)` 也
        // 到得了。改成夹住（clampedMilliseconds），不再盲转。
        s.ttlMs = SYPlayerCacheConfiguration.clampedMilliseconds(timeToLive)
        return s
    }

    /// 秒 → 毫秒，**夹住而不是盲转**。与
    /// `SYPlayerPreloader.milliseconds(for:)` 是同一套规则，刻意各写一份
    /// 三行而不是互相引用：两者是两个独立公开入口上的输入净化，把它们绑成
    /// 一条依赖只会让"改一边忘一边"变成"改一边悄悄改了另一边的契约"。
    /// 规则本身由用例钉住（`SYPlayerDoubleClampTests`）。
    ///
    ///   · `nil` / 非有限（`.infinity` / `-.infinity` / `.nan`）⇒ 0
    ///     ＝ `syp_config.cache_ttl_ms` 的"不按时间过期"，正是 `.infinity`
    ///     想表达的那件事；
    ///   · ≤ 0（含负数、`-0.0`）⇒ 0，与原行为一致；
    ///   · 有限但 ×1000 之后超出 `Int64` ⇒ 夹到 `Int64.max`（≈2.9 亿年，
    ///     行为上与"不过期"等价），**不 trap**。
    static func clampedMilliseconds(_ seconds: TimeInterval?) -> Int64 {
        guard let s = seconds, s.isFinite else { return 0 }
        let ms = (s * 1000).rounded()
        guard ms > 0 else { return 0 }
        // `Double(Int64.max)` 舍入成 2^63（比 Int64.max 大 1），所以判据必须
        // 是严格小于：任何严格小于 2^63 的 Double 都能无损转成 Int64。
        guard ms < Double(Int64.max) else { return Int64.max }
        return Int64(ms)
    }
}

/// 预加载的一次统计快照。**六个字段全映射** `syp_preload_stats`。
public struct SYPlayerPreloadStatistics: Sendable, Equatable {
    /// 当前条目数（HLS 会把播放列表展开成多条分片条目，这里数的是展开后的总数）。
    public var entries: Int
    /// 正在下载的任务数。
    public var activeTasks: Int
    /// 本对象的**条目源**累计从网络下载的字节数。
    ///
    /// ⚠️ **这个数会低报真实的网络用量**，不要拿它当流量统计：按时间预加载时
    /// 底层要先只读 header 探一次容器，那条源不由预加载的条目持有、因此不计入
    /// 本字段。那些字节确实落进了同一份缓存、播放时会命中，但它们走过网络。
    ///
    /// ⚠️ **低报的量没有"每 URL 2 MiB"这个上界**。上界
    /// 的正确口径是"**每一次探测**最多 `kProbeMaxBytes`(2 MiB)"，而**失败的
    /// 探测不被记忆**，同一个 URL 会被反复探测。实测 moov 在尾部的素材、同一
    /// provider、同一 URL 连探三次：`probe#1` 放弃时已拉 2,215,936 B、
    /// `probe#2` 成功再拉 49,669 B、`probe#3` 零字节 —— **前两次合计
    /// 2,265,605 B，超过一个上界**。所以这里只能说"低报的量与探测次数成正比、
    /// 每次不超过 2 MiB"，不能说"每个 URL 不超过 2 MiB"。
    public var downloadedBytes: Int64
    /// 累计已暖够的条目数（不随 `remove` 减少）。
    public var completed: Int
    /// 累计失败的条目数。
    public var failed: Int
    /// 累计"想按时间暖、但这个 URL 估不出时长"的次数——它涨就说明那些条目
    /// 退化成了按字节暖（默认 1 MiB），常见原因是 moov 在文件尾、探测够到的
    /// 字节超过上界而放弃，或者服务端根本没响应。
    ///
    /// ⚠️ **不要把它当成"这个 URL 支不支持按时间预加载"的判据 —— 它依赖缓存
    /// 冷热**（上一版这里写的是"唯一可观测信号"，那句话
    /// 把一个与缓存状态有关的量说成了 URL 的属性）。moov 在尾部的容器**不是
    /// 永久退化**：第一次探测放弃时下下来的那 ~2 MiB 前缀留在缓存里，第二次
    /// 探测从这份暖缓存起步、只需再新下几十 KB 就**成功**（实测 49,669 B）。
    /// 于是同一个 URL 冷态让 `providerMiss` 涨一次、热态就不涨了。
    /// 它仍然是"整体上有多少条目退化成按字节"的有效观测口，只是不可归因、
    /// 也不该拿来推断单个 URL 的能力。
    public var providerMiss: Int

    public init(entries: Int = 0, activeTasks: Int = 0, downloadedBytes: Int64 = 0,
                completed: Int = 0, failed: Int = 0, providerMiss: Int = 0) {
        self.entries = entries
        self.activeTasks = activeTasks
        self.downloadedBytes = downloadedBytes
        self.completed = completed
        self.failed = failed
        self.providerMiss = providerMiss
    }
}

/// 桥的持有盒，**存在的唯一理由是让 `deinit` 能把桥交给别的队列去析构**。
///
/// `@unchecked Sendable` 在这里是真的证明过的，不像 `SYPlayerResources` 那样
/// 承载调用纪律：`SypPreloaderBridge` 的每个方法都只是转发给
/// `syp::dl::Preloader`，后者的每个公开方法一进来就取自己那把 `mu_`，没有
/// 任何"必须在某个线程上调"的亲和性（对照 `SypPlayerBridge`：它的
/// `-attachVideoLayer:` 只能在主线程调、`-open*` 必须串行化）。`-dealloc`
/// 在后台队列上跑因此是安全的。
final class SYPlayerPreloaderBox: @unchecked Sendable {
    let bridge: SypPreloaderBridge
    init(_ bridge: SypPreloaderBridge) { self.bridge = bridge }
}

@MainActor
public final class SYPlayerPreloader {
    /// 只影响**并发额度的分配**，不改变每个条目要暖多少。
    public enum Priority: Sendable, Equatable {
        case background   // 更远的候选
        case next         // 下一个要播的
        case playing      // 正在播的那条（若也交给本对象管）

        fileprivate var bridged: SypPreloadPriority {
            switch self {
            case .background: return .background
            case .next:       return .next
            case .playing:    return .playing
            }
        }
    }

    public let cacheConfiguration: SYPlayerCacheConfiguration

    /// `internal`：泄漏用例要对桥本身做 `weak` + `XCTAssertNil`（此前
    /// 的 deinit 用例恰好断言在唯一没泄漏的那个对象上）。
    let box: SYPlayerPreloaderBox
    private var bridge: SypPreloaderBridge { box.bridge }

    /// - Parameters:
    ///   - maxTotalTasks: 本对象能同时占用的下载连接总数上限。
    ///   - reservedForPlaying: **没有 `.playing` 条目时**留给"真正在播的那条流"
    ///     的额度——预加载此时最多只用 `maxTotalTasks - reservedForPlaying` 个。
    ///     两者相等时预加载在没有 `.playing` 条目的情况下完全不工作（全部让路），
    ///     这是合法配置，不是 bug。
    public init(cache: SYPlayerCacheConfiguration = .init(),
                maxTotalTasks: Int = 6,
                reservedForPlaying: Int = 3) {
        self.cacheConfiguration = cache
        self.box = SYPlayerPreloaderBox(
            SypPreloaderBridge(cache: cache.bridged(),
                               maxTotalTasks: Int32(clamping: maxTotalTasks),
                               reservedForPlaying: Int32(clamping: reservedForPlaying),
                               defaultPreloadBytes: 1 << 20,
                               defaultPreloadMs: 3000))
    }

    deinit {
        // **不在这里析构桥**，把最后一份强引用交给后台队列。
        //
        // 为什么：`~PreloadStack` 要 join 预加载的驱动线程，而那条线程可能正卡在
        // provider 的"只读 header 探测"里。那次探测有字节上界也有自己的超时，
        // 但超时管的是单次读、后面还会再试一次，所以墙钟是它的一个小倍数——
        // 一个只接受连接、永不响应的服务端上实测 **6,015–6,021ms**（从 120,071ms
        // 压到这个数，见 media_info_provider.h）。deinit 跑在
        // 释放最后一份强引用的那个线程上，对一个 @MainActor 类来说正常写法下
        // 就是主线程，于是"一次析构 = 主线程卡 6 秒"。
        //
        // 【这个 6 秒此前不是上界】"1 KiB/s 慢速滴流"的
        // 服务端三个超时一个都踩不到，实测跑满 150 秒未结束、理论 ~34 分钟。
        // 现在由 kProbeWallClockMs（15,000ms）的墙钟看门狗兜住，所以下面那条
        // "后台队列"的最坏驻留时间从 ~34 分钟降到 ~15 秒。**交后台队列这条
        // 措施仍然必要**（15 秒的主线程卡顿同样不可接受），但它不再是唯一
        // 防线。
        //
        // 代价（写在这里，不要让下一个人以为是免费的）：
        //   · 析构变成**异步**的——本对象 deinit 返回时驱动线程还活着，还可能
        //     继续往缓存目录里写字节。用例要断言"目录已经安静了"必须自己等
        //     （SYPlayerPreloaderTests 的 deinit 用例就是这么写的）；
        //   · 进程退出时这条后台析构可能来不及跑完，已下的字节留在缓存里——
        //     这本来就是 remove/destroy 的既有语义（syp_preload.h），不是新增
        //     的不确定性；
        //   · 换来的是主线程恒定 < 1ms（实测）。
        //
        // 只读 `let` 存储属性（box），不碰任何隔离可变状态——@MainActor 类的
        // deinit 是非隔离的，这是它唯一允许做的事（与 SYPlayer.deinit 同形）。
        let held = box
        DispatchQueue.global(qos: .utility).async {
            // withExtendedLifetime 而不是 `_ = held`：后者会被优化成"捕获了但
            // 立刻不用"，强引用的存活区间就不再由这个 block 说了算。
            withExtendedLifetime(held) {}
        }
    }

    /// 登记一条预加载。**立即返回**，不发起任何同步网络 IO。
    ///
    /// - `seconds` 为 `nil` 时暖默认时长（3 秒）；给了值就按该时长换算成字节
    ///   区间（换算靠只读 header 的容器探测，估不出时退化为按字节暖 1 MiB，
    ///   并让 `statistics.providerMiss` 加一）。
    /// - 同一个 URL 重复 `add` 不会重复建条目，只会把优先级抬到两者中较高的
    ///   那个——**这种情况返回 `true`**，"已存在"不是失败。
    /// - `.file` 源直接忽略并返回 `false`：本地文件没有"预加载"可言。
    /// - URL 路径以 `.m3u8` / `.m3u` 结尾时走 HLS：暖 master 与 media 播放列表，
    ///   再暖 `#EXT-X-MAP` 与前若干个分片。遇到加密（非 `METHOD=NONE` 的
    ///   `#EXT-X-KEY`）整条放弃，**一次密钥请求都不会发**。
    /// ## 返回值只说"登记成功了"，**不说"这个 URL 下得下来"**
    ///
    /// 返回 `true` 的含义**仅仅是**"条目已经进表、驱动线程会去试"。同步能查的
    /// 只有三件事，返回 `false` 的也只有这三种情形：
    /// 1. `.file` 源（本地文件没有"预加载"可言）；
    /// 2. URL 的 `absoluteString` 为空串；
    /// 3. 底层没建起来（缓存目录为空串、HTTP 后端没注册上）。
    ///
    /// 其余一切都**返回 `true` 然后异步失败**：拼错的主机名、`ftp://` 之类没有
    /// 后端的 scheme、`URL(string: "notaurl")!` 这种非空但不成 URL 的串、404、
    /// 连不上、以及加密的 HLS 播放列表。唯一的观测口是
    /// `statistics.failed`（它会涨），**不会有任何回调或抛出**。
    ///
    /// ⚠️ **`statistics.failed` 不可归因**：它是一个单调累积的计数器，没有
    /// 按 URL 的分解（底层 `preloader.cpp` 只在条目进 Failed 时 `++failed`）。
    /// 两条坏 URL 只会让它从 0 涨到 2，你无法知道是哪一条、也无法知道是不是
    /// 某一条失败了两次。所以它可以用来做"整体健康度"的观测，**不能**用来
    /// 写"这一条失败了就重试这一条"的逻辑——要那种语义得先给底层加按条目的
    /// 终态查询。
    ///
    /// 为什么不把这些也做成 `false`：那要求门面自己判断"dl 层到底能下什么"，
    /// 也就是把 scheme 白名单与 HLS 判据在 Swift 侧**再转写一份**。这套逻辑
    /// 已经两次被"同一道闸的第二份转写悄悄漂移"咬到（
    /// `playlist_has_endlist` 的两份实现对 `\v`/`\f` 和 NUL 处理已经不一致）。
    /// 而且判准了也不值：`add` 的语义本来就是"登记一条意图"，真正的失败（DNS、
    /// 404、断网）本质上就是异步的，已经有 `statistics.failed` 这个指定通道。
    /// 用 `throws` 或错误枚举同理——它们会给调用方增加一条它无论如何也挡不住
    /// 异步失败的分支。
    @discardableResult
    public func add(_ source: SYPlayerSource,
                    priority: Priority = .next,
                    seconds: TimeInterval? = nil) -> Bool {
        guard case .url(let u) = source else { return false }
        return bridge.add(u.absoluteString, priority: priority.bridged,
                          milliseconds: SYPlayerPreloader.milliseconds(for: seconds))
    }

    /// 秒 → 毫秒。**单独成函数只为了它可被直接断言**：`add()` 把这个值交给桥
    /// 之后就再也观察不到了（`syp::dl::Preloader` 没有暴露 `want_ms` 的测试缝，
    /// 而本任务不改 `src/dl`），所以"只验 add 不崩"那种用例对这段换算是没有
    /// 鉴别力的——换算写错了它照样绿。
    ///
    /// 约定：`nil` → 0（= 由底层用 `default_preload_ms`，本类给的是 3000）；
    /// 负数同样 → 0，而不是一个负的毫秒数（底层把 `<= 0` 当"没给"，两者语义
    /// 一致，但在这里夹一次，不把垃圾往下传）。
    ///
    /// 【补上界】原先只有 `isFinite` 这一条 guard，**没有
    /// 上界**：实测 `add(seconds: 1e18)`（1e18 × 1000 = 1e21 ms，远超
    /// `Int64.max` ≈ 9.22e18）⇒ `Fatal error: Double value cannot be
    /// converted to Int64 because it is greater than Int64.max`，把 xctest
    /// 进程打崩（EXIT=65）。这个检查在 **Release 下同样在**，线上照崩。
    /// 1e18 秒是个合法的有限 `Double`，公开 API 没有任何地方拒绝它。
    /// 现在有限但溢出的值夹到 `Int64.max`（底层再按真实时长夹一次），
    /// 非有限的值与"没给值"同义 ⇒ 0。
    static func milliseconds(for seconds: TimeInterval?) -> Int64 {
        guard let s = seconds else { return 0 }
        guard s.isFinite else { return 0 }
        let ms = (s * 1000).rounded()
        guard ms > 0 else { return 0 }
        // 判据必须是严格小于：`Double(Int64.max)` 舍入成 2^63（比 Int64.max
        // 大 1），严格小于 2^63 的 Double 才保证能无损转成 Int64。
        guard ms < Double(Int64.max) else { return Int64.max }
        return Int64(ms)
    }

    /// 改一条已登记条目的优先级。URL 不在表里时是 no-op（不是错误）。
    public func setPriority(_ priority: Priority, for source: SYPlayerSource) {
        guard case .url(let u) = source else { return }
        bridge.setPriority(priority.bridged, forURLString: u.absoluteString)
    }

    /// 打断在途下载并摘掉条目。**已下的字节留在缓存里**，下次再 `add` 或者
    /// 直接播放时从缺口续传。HLS 条目会连同它展开出来的分片条目一起摘掉。
    ///
    /// 立即返回；真正的打断与关闭在驱动线程上完成，所以紧接着读
    /// `statistics.entries` 可能还没归位。
    public func remove(_ source: SYPlayerSource) {
        guard case .url(let u) = source else { return }
        bridge.remove(u.absoluteString)
    }

    /// 摘掉全部条目，语义同 `remove` 的逐条版本（同样是异步生效）。
    public func removeAll() {
        bridge.removeAll()
    }

    public var statistics: SYPlayerPreloadStatistics {
        let s = bridge.statistics()
        return SYPlayerPreloadStatistics(entries: Int(s.entries),
                                         activeTasks: Int(s.activeTasks),
                                         downloadedBytes: s.downloadedBytes,
                                         completed: Int(s.completed),
                                         failed: Int(s.failed),
                                         providerMiss: Int(s.providerMiss))
    }
}
