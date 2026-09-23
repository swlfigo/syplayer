// SYPlayer.swift — 公开门面。业务侧从此不出现任何 C 类型、指针出参、
// 微秒整数；回调不在泵线程；状态推送由框架自己做。
//
// ── 线程与生命周期，三条不变量（其余一切都是它们的推论）──
//
// 1. **全部公开成员 @MainActor。** CALayer 配置与 UI 消费本来就只能在主线程，
//    与其在文档里写"请在主线程调用"，不如让编译器强制。
//
// 2. **慢操作串到 openQueue 上，快操作直接打给桥。** open 要编译 shader + 探测
//    流信息（几十到几百毫秒，SYPBridge.mm 顶部注释），绝不能上主线程；而
//    play/pause/seek/setSpeed/snapshot 在桥里只是拿一下 mu_ 做几个字段读写，
//    TrackPlayer::step() 契约保证持锁时间恒短，直接在主线程调不会卡。
//    openQueue 是**串行**队列，这一点是正确性而不是性能：SypPlayerBridge 的
//    -openLocalFile:/-openURLString: 在拿 mu_ 之前有一段没有锁保护的工作
//    （close_internal + Pipeline::create_*），两次并发 open 会在 src_/avio_ 上
//    真的数据竞争。close 也排在同一条队列上，就不会和一个正在进行的 open 交错。
//    （duration_us_ 曾经也在这张清单上——它是在锁外写、在 mu_ 下读的，而
//    snapshot() 从主 actor 发起、完全可能与 open 并发。已在桥里修好：现在只在
//    mu_ 下写。这一层不去"绕开"下游的竞争，该修的修在源头。）
//
// 3. **桥的回调先跳主 actor 再发布。** SYPBridge.h 那条"不能在 onEof/onError
//    里释放持有 bridge 的唯一强引用，否则自死锁"的致命约束，
//    在这一层被**结构性**消除：业务拿到 delegate/stream 回调时早已不在泵线程，
//    栈里没有 pump_loop()。框架自己也不在回调里碰生命周期——block 只捕获
//    [weak self]，桥由 SYPlayer 强持有。
//
// close() 幂等、deinit 自动关闭、open 在已打开时由桥自己先 close：与桥的
// 可重开语义一致（SYPBridge.mm 的 close_internal/open_common）。
import Combine
import Foundation
import SYPlayerKit_Private

public enum SYPlayerSource: Equatable, Sendable {
    case file(URL)          // 本地文件
    case url(URL)           // http/https，路径以 .m3u8 结尾时走 HLS
}

public enum SYPlayerDecoding: Equatable, Sendable {
    case hardwarePreferred
    case software
}

/// `@MainActor`：**只是把已经成立的事实写进类型系统**。每一条
/// 回调都在主 actor 上投递，`publish()` / `apply()` / `refresh()` 也确实只从
/// `SYPlayer`（全员主 actor）里调——不标注并不会让它变得更自由，只会让下一个
/// 接入方写出一个非隔离的 conformer 而**没有任何东西拦得住**，直到语言模式提到
/// 6 才变成硬错误。今天标上是源码兼容的：唯一的真实 conformer
/// （demo 的 PlayerViewController）本来就是主 actor 隔离的，测试里的
/// Recorder 走的是"在主声明上遵循 @MainActor 协议即推断隔离"那条规则。
/// 这与"`SWIFT_VERSION` 保持 5.0"不冲突：那管的是语言模式，
/// 不是"不许加安全的标注"。
@MainActor
public protocol SYPlayerDelegate: AnyObject {
    func player(_ player: SYPlayer, didChange state: SYPlayerState)
    func playerDidReachEnd(_ player: SYPlayer)
    func player(_ player: SYPlayer, didFailWith error: SYPlayerError)
}

/// 三个方法都给默认空实现：UIKit 接入方常常只关心其中一个。
@MainActor
public extension SYPlayerDelegate {
    func player(_ player: SYPlayer, didChange state: SYPlayerState) {}
    func playerDidReachEnd(_ player: SYPlayer) {}
    func player(_ player: SYPlayer, didFailWith error: SYPlayerError) {}
}

/// 会话号，**单独成类、什么都不持有**。
///
/// 这不是为了整洁而拆的类，是为了断一条真实的保留环。桥的
/// `onEof`/`onError` 是 `copy` 属性，block 一旦装上去就由桥强持有；如果 block
/// 捕获的是 `SYPlayerResources`（它持有 `bridge`），环就成了
/// `res → bridge → block → res`——而且没有任何地方会把 block 置空，于是
/// **每一个 `SYPlayer` 都会永久泄漏它的桥、桥里的 `PlayerCore`（连同一条
/// dispatch 队列）以及挂上去的 `CAMetalLayer`**（layer 的强引用只在
/// `~PlayerCore` 里释放）。`[weak self]` 是对的，但泄漏的从来不是 `self`。
///
/// 把跨线程需要的那一点点状态（一把锁 + 一个 UInt64）拎成一个不持有任何
/// 东西的对象，block 只捕获它，环就断了：`bridge → block → counter`，
/// counter 指回去的边不存在。
///
/// `@unchecked Sendable`：内部状态全程由 `lock` 保护，没有别的可变状态。
/// 这里的 unchecked 是真正意义上的"人工证明过"，不承载任何调用纪律。
final class SYPlayerSessionCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt64 = 0

    var current: UInt64 {
        lock.lock()
        defer { lock.unlock() }
        return value
    }

    @discardableResult
    func bump() -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        value &+= 1
        return value
    }
}

/// deinit 会碰到的那部分可变状态，装在一个 **nonisolated** 的类里。
///
/// 理由是 Swift 的规则而不是口味：全局 actor 隔离类的 deinit 是非隔离的，
/// 只允许访问不可变（let）存储属性；而 deinit 必须能 invalidate 定时器、
/// finish 掉还挂着的 AsyncStream。把这些装进一个 let 持有的盒子，deinit 只读
/// 那个 let，再把真正的清理派回主线程（Timer 只能在安装它的 runloop 线程上
/// invalidate，而 deinit 不保证跑在主线程）。
final class SYPlayerResources: @unchecked Sendable {
    /// 桥也放在这里：deinit 要关它，open/close 的 block 要在别的队列上用它。
    ///
    /// `@unchecked` 在这里承载的是一条 Swift 看不见、但真实存在的纪律，
    /// **两条，不是一条**：
    ///
    /// 1. **所有 -open*/-close 都排在同一条串行队列（SYPlayer.openQueue）上。**
    ///    SypPlayerBridge 的 -openLocalFile:/-openURLString: 在拿 mu_ 之前有一段
    ///    无锁工作（close_internal + Pipeline::create_*，其中 src_/avio_ 的开关
    ///    始终在锁外），两次并发 open 会在 src_/avio_ 上真的数据竞争。
    /// 2. **-attachVideoLayer: 只在主线程调用**（SYPBridge.h 明写；它在那里配置
    ///    CAMetalLayer 的 device/pixelFormat）。
    ///
    /// 剩下的方法（play/pause/seek/setSpeed/snapshot）桥内部全程持 mu_，任意线程
    /// 安全——`duration_us_` 曾经是这句话的反例（open_common 在锁外写、snapshot 在
    /// 锁内读），已经修掉，不是靠调用方绕开。
    ///
    /// 所以这里**不能**给 SypPlayerBridge 标 NS_SWIFT_SENDABLE：那是在声称
    /// "怎么并发用都安全"，而它只是"按这两条纪律用才安全"。把 unchecked 放在
    /// 遵守纪律的这一侧，纪律就和它的理由写在同一处。
    let bridge = SypPlayerBridge()
    var timer: Timer?
    /// `startPolling()` 被**调用**的次数——是"启动尝试"，不是"成功重启的定时器数"
    /// （startPolling() 每次都会真的换一个新 Timer，但这个计数
    /// 记的是入口，不承诺别的）。只为测试"值没变时不重启定时器"：间隔相同时光看
    /// pollingInterval 分辨不出有没有重启过。
    var pollingStarts = 0
    var continuations: [UUID: AsyncStream<SYPlayerState>.Continuation] = [:]

    /// 会话号。**桥的回调在泵线程上跑，这个字段因此必须加锁**（其余字段都只在
    /// 主 actor 上碰，唯独它要被泵线程读）。
    ///
    /// 它解决的是 `Task { @MainActor }` 那一跳带来的时间差：回调发出时属于会话 N，
    /// 真正执行时可能已经是会话 N+1 了。没有这道闸门会出两种具体的错——
    ///   · close() 期间到达的 onError：把 state 冻在 .failed，而轮询已经停了，
    ///     再也没有人把它推回 .idle；
    ///   · open() 挂在 await 上时到达的 onError：lastError 在 await 之前刚被清空，
    ///     这一次赋值落在清空之后，新会话一起播就带着上一份的错。
    /// 所以回调 block 在**泵线程上**先把当时的会话号抓下来，跳到主 actor 之后
    /// 与当前值比对，不一致就整条丢弃。
    ///
    /// 计数器本体在 `SYPlayerSessionCounter` 里，不在这个类里：
    /// 回调 block 需要的只是它，捕获整个 `SYPlayerResources` 会把桥圈进一条
    /// 永不断开的保留环。这里只是**转持**一份引用，供主 actor 侧使用。
    let sessionCounter = SYPlayerSessionCounter()

    var currentSession: UInt64 { sessionCounter.current }

    @discardableResult
    func bumpSession() -> UInt64 { sessionCounter.bump() }
}

@MainActor
public final class SYPlayer: ObservableObject, SYPlayerVideoHost {

    // MARK: - 公开状态

    @Published public private(set) var state: SYPlayerState = .idle

    /// 每次访问返回一条新的流；订阅方结束（取消 for-await 或流被释放）时自动注销。
    /// 缓冲策略是 .bufferingNewest(1)：状态快照只有最新那份有意义，消费慢的一方
    /// 不该积压出一串过期状态。
    public var stateStream: AsyncStream<SYPlayerState> {
        var handle: AsyncStream<SYPlayerState>.Continuation!
        let stream = AsyncStream<SYPlayerState>(bufferingPolicy: .bufferingNewest(1)) {
            handle = $0
        }
        register(handle)
        return stream
    }

    public weak var delegate: SYPlayerDelegate?

    /// 本平台是否支持硬解 H.264 8-bit。Mac Catalyst 上恒 false（FFmpeg 在
    /// Catalyst 上禁用了 videotoolbox hwaccel）。不需要具体流，UI 可以用它提前
    /// 禁用硬解选项，而不是等 open 抛 .notImplemented。
    public static var isHardwareDecodeAvailable: Bool {
        SypPlayerBridge.hardwareDecodeAvailable()
    }

    /// 状态轮询间隔，默认 0.1（10Hz）。下限 1/60——比这更密只会空转：
    /// 画面走的是真上屏（GPU 到 GPU），这个定时器只负责"人眼能读"的数字。
    ///
    /// 钳位后**继续**走重启轮询那一步，不 return。
    /// Swift 的规则是"在 didSet 里给自己赋值不会再次触发 didSet"，所以计划里
    /// 那句"第二次 clamped == 自己，不递归"成立、但紧跟的 return 会让被钳位的
    /// 那次设置**只钳位、不重启定时器**——用户设成 0.001 会得到一个仍然按旧间隔
    /// 跑的定时器。这里赋值之后直接往下走，递归同样不会发生（原因是语言规则，
    /// 不是那个 return）。
    public var stateUpdateInterval: TimeInterval = 0.1 {
        didSet {
            let clamped = max(1.0 / 60.0, stateUpdateInterval)
            if clamped != stateUpdateInterval {
                stateUpdateInterval = clamped   // 不会再次触发 didSet（Swift 语言规则）
            }
            // 值没变就别重启定时器：赋同一个值是很常见的
            // （SwiftUI 的 onChange、配置面板每次都整份写回），重启一次就把下一拍
            // 往后推满一个周期，看起来像状态更新"卡了一下"。
            guard stateUpdateInterval != oldValue else { return }
            if resources.timer != nil { startPolling() }
        }
    }

    /// 线性音量，`0...1`，默认 1.0。越界与非有限值按 0/1 夹取：NaN → 0，
    /// +inf → 1，-inf 与负数 → 0，大于 1 → 1。
    /// 与 `isMuted` 相互独立：静音不会擦掉这个值。
    ///
    /// **跨 `open(_:)`/`close()` 保留**，open 之前设也有效：值存在桥上，每次打开
    /// 新素材都会重新下发。
    ///
    /// 夹取口径与 `SYPlayerCacheConfiguration` 同源（非有限值不得穿到桥下面去），
    /// 但**各写一份、不互相引用**。桥（`-setVolume:`）**不**
    /// 再夹，原样存、原样转发——否则这一道的变异全部等价、没有用例守着（理由见
    /// `SYPBridge.h`）；TrackPlayer 为它自己的消费者按同一口径再夹一次。
    public var volume: Double {
        get { bridge.volume }
        set {
            let v = newValue
            if v.isNaN { bridge.volume = 0.0 }
            else if v < 0 { bridge.volume = 0.0 }   // 含 -.infinity
            else if v > 1 { bridge.volume = 1.0 }   // 含 +.infinity
            else { bridge.volume = v }
        }
    }

    /// 静音，默认 false。**不是暂停**——时钟照常走、画面照常出，只是听不见。
    /// 跨 `open(_:)`/`close()` 保留，同 `volume`。
    public var isMuted: Bool {
        get { bridge.isMuted }
        set { bridge.isMuted = newValue }
    }

    // MARK: - 私有状态

    private let resources = SYPlayerResources()
    private var bridge: SypPlayerBridge { resources.bridge }
    private let openQueue = DispatchQueue(label: "com.syplayer.kit.open", qos: .userInitiated)

    /// open/close 的代号。每次 open 或 close 递增；一次 open 的 async 结果回到
    /// 主 actor 时代号已变，说明期间又 open 或 close 过，这次结果整体作废并抛
    /// .canceled（"open 期间再次 open"这一情形）。
    private var generation: UInt64 = 0
    private var lastError: SYPlayerError?

    /// 闸门关着时到达的、属于当前会话的错误，等 open() 成功后补投。
    /// 生命周期与 `lastError` 完全一致：凡是清 lastError 的地方都要清它，
    /// 否则一次 close 之后残留的暂存会在下一次 open 成功时诈尸。
    private var pendingError: SYPlayerError?

    private var hasEnded = false
    private var endReported = false

    /// 主 actor 侧的回调闸门：只有"当前确实打开着一份素材"时泵回调才允许改状态。
    ///
    /// 它与 `SYPlayerResources.session` 是**两道互补的闸门**，不是重复：
    ///   · 这一道是**同步**的（open/close 一进来就关、open 成功才开），挡住
    ///     "close() 之后才执行"和"open 还没成功就执行"的回调；
    ///   · 会话号是**跨线程**的（在 openQueue 上、桥关完之后才换），挡住
    ///     "重新 open 成功之后，上一份素材的回调混进新会话"。
    /// 少任何一道都有一条能走通的真实路径——只有会话号、且换号的时机
    /// 还在旧泵死之前，就会漏过。
    private var isSessionLive = false

    /// internal 而非 private：回归用例要断言"走一圈窗口进出之后
    /// 这一对仍然互相认得"，而这正是本属性的值（见 SYPlayerLayerTests 里的
    /// SYPlayerAttachLifecycleTests）。与 SYPlayerLayer.syncDrawableSize() 同理。
    weak var attachedView: SYPlayerView?

    /// 本实例用的缓存配置（此前是写死在桥里的 tmp 目录）。
    /// 与 `SYPlayerPreloader` 用同一个 `directory` 时，预加载暖好的字节这里
    /// 直接命中——两边的目录经同一个归一化函数处理，写法差异（尾斜杠）不会
    /// 变成两份缓存。
    ///
    /// **只对之后的 `open(_:)` 生效**，且只影响 `.url` 源（`.file` 不走缓存）。
    public let cacheConfiguration: SYPlayerCacheConfiguration

    /// 检验缝（`internal`，不是公开 API）：桥**下一次 `-openURLString:` 真正
    /// 会交给 dl 层**的那份缓存目录，已归一化。
    ///
    /// 存在的理由与 `PreloadStack::provider_cache_dir_for_test()` 一样：
    /// "预加载与播放落在同一份缓存里"这条要可断言，而断言必须落在两边
    /// **实际拿去 `CacheStore::make_key` 的那个字符串**上——断 `cacheConfiguration`
    /// 相等是不鉴别的（两个 `URL` 相等不代表桥里拼出来的 C 字符串相等）。
    var cacheDirectoryInUseForTest: String { resources.bridge.cacheDirectoryInUse() }

    /// 检验缝（`internal`）：桥**下一次 `-openURLString:` 真正会交给
    /// dl 层**的那四个数（目录 + 容量 + 最小可用空间 + TTL 毫秒）。
    ///
    /// 与 `cacheDirectoryInUseForTest` 同理：断 `cacheConfiguration` 相等是不
    /// 鉴别的——目录那一半有这条缝守着，容量/TTL 那一半此前一路裸奔到
    /// `syp_config`，把 `prepare_cache_dir` 那三行改成硬 0（缓存无上限、永不
    /// 过期）74 条用例一条都不红。
    var cacheSettingsInUseForTest: SypCacheSettings { resources.bridge.cacheSettingsInUse() }

    /// 检验缝（`internal`）：最近一次真的 `-openURLString:` 交给 dl 层的那份
    /// 缓存配置。`nil` = 还没 open 过 URL。上面那条是"再算一遍"，这一条是
    /// "真的发生了什么"。
    var lastOpenCacheConfigForTest: SypCacheSettings? { resources.bridge.lastOpenCacheConfig }

    /// 检验缝（`internal`）：**当前 TrackPlayer（消费者）手里**
    /// 的音量 / 静音（0/1）/ gravity（`SypVideoGravity` 的原始值）。没打开时都是 -1。
    ///
    /// 与 `volume`/`isMuted` 的 getter 不是一回事：那两个读的是桥上的持久副本，
    /// 桥哪怕从没把值下发给新建的 TrackPlayer，它们照样返回调用方设的数——"跨
    /// open 保留"只断言它们就是假绿。这三条读的是交给消费者的那个值
    /// （`SYPBridge.h` 里同名方法的注释）。
    var volumeInUseForTest: Double { resources.bridge.volumeInUseForTest() }
    var mutedInUseForTest: Int32 { resources.bridge.mutedInUseForTest() }
    var videoGravityInUseForTest: Int32 { resources.bridge.videoGravityInUseForTest() }

    /// 检验缝（`internal`）：当前音频 sink 在 open 那一刻的增益起点
    /// （float 精度）。没打开时 -1。上面 `volumeInUseForTest` 看不出"create 之后
    /// 才下发"与"open 之前就落下"的区别，这条看得出（`SYPBridge.h` 同名方法的注释）。
    var sinkGainAtOpenForTest: Double { resources.bridge.sinkGainAtOpenForTest() }

    /// 检验缝（`internal`）：经桥的装饰层、用 MetalRenderer **当前**的
    /// 几何与 gravity 把最近一次呈现的帧画进 `width×height` 离屏纹理，回读归一化坐标
    /// `(x, y)`（左上为原点）处的像素 `(b, g, r)`。无 player / 尚无已呈现帧时为 nil。
    /// 守的是装饰层 → MetalRenderer 那一跳（`SYPBridge.h` 同名方法的注释）。
    func debugBlitPixelForTest(width: Int32, height: Int32,
                               x: Double, y: Double) -> (b: UInt8, g: UInt8, r: UInt8)? {
        var packed: UInt32 = 0
        guard resources.bridge.debugBlitPixel(forTest: width, height: height,
                                              atX: x, y: y, bgra: &packed) else { return nil }
        return (UInt8(truncatingIfNeeded: packed),
                UInt8(truncatingIfNeeded: packed >> 8),
                UInt8(truncatingIfNeeded: packed >> 16))
    }

    /// 检验缝（`internal`）：经桥的装饰层下发显示几何。生产上几何只由
    /// TrackPlayer::create() 下发；这条只为让 SAR 1:1、不旋转的样本也能区分
    /// "装饰层把几何转发给 MetalRenderer 了没有"。无 player 返回 false。
    @discardableResult
    func debugSetSourceGeometryForTest(sarNum: Int32, sarDen: Int32, rotationDeg: Int32) -> Bool {
        resources.bridge.debugSetSourceGeometry(forTest: sarNum, sarDen: sarDen,
                                                rotationDeg: rotationDeg)
    }

    // MARK: - 构造

    public convenience init() {
        self.init(cache: SYPlayerCacheConfiguration())
    }

    /// 指定缓存目录与容量上限。目录在这里就会被建好。
    ///
    /// ⚠️ **建不出来这件事不会告诉你**。本初始化器不
    /// `throws`，桥的 `-setCacheSettings:` 返回 `void`，底层
    /// `createDirectoryAtPath:` 的 `error:` 传的是 `nil`：实测把词法父目录
    /// `chmod 0500` 之后目录建不出来，而调用方拿到的信号是**零**，真实后果是
    /// 第一次 `open` 报 `.io`。提前建的收益是"少一次失败往返"和"不把文件系统
    /// 操作压进 `open` 的 IO 路径"，不是可诊断性。要自己确认，`open` 之前
    /// `FileManager.default.fileExists(atPath: cache.directory.path)` 查一下。
    public init(cache: SYPlayerCacheConfiguration) {
        self.cacheConfiguration = cache
        // 桥必须**在装回调之前**就拿到配置：-openURLString: 在建 syp_source
        // 时读它，而 open 排在 openQueue 上、可能早于任何后续设置。
        resources.bridge.setCacheSettings(cache.bridged())

        // ↓ 以下是原 init() 的全部内容，一个字不改。
        // [weak self]：桥由 self 强持有，block 反过来强引用 self 就是循环。
        // Task { @MainActor } 是这一层最关键的一跳——泵线程到此为止。
        //
        // `counter.current` 在**泵线程上、跳转之前**读：这才是"这次回调属于
        // 哪个会话"的正确取样点。跳到主 actor 之后再读就只能读到当前会话，
        // 闸门等于不存在（见 SYPlayerResources.sessionCounter 的注释）。
        //
        // block 捕获的**只能是 counter**，绝不能是 `resources`：
        // 后者持有 bridge，而 bridge 以 copy 属性持有这两个 block，捕获它就造出
        // `res → bridge → block → res` 这条没有任何地方会断开的环，
        // 每个 SYPlayer 都会永久泄漏桥、PlayerCore 与挂上去的 CAMetalLayer。
        // 详见 SYPlayerSessionCounter 的注释。
        let counter = resources.sessionCounter
        bridge.onEof = { [weak self] in
            let session = counter.current
            Task { @MainActor in self?.handleEof(session: session) }
        }
        bridge.onError = { [weak self] status in
            let session = counter.current
            let error = SYPlayerError(statusCode: Int(status))
            Task { @MainActor in self?.handleError(error, session: session) }
        }
    }

    deinit {
        // 只读 let 存储属性（resources / openQueue），不碰任何隔离可变状态。
        let res = resources
        DispatchQueue.main.async {
            res.timer?.invalidate()
            res.timer = nil
            for (_, c) in res.continuations { c.finish() }
            res.continuations.removeAll()
        }
        // 强捕获 resources（进而是桥）：保证它活到 close 跑完。openQueue 不是
        // 泵队列，-close 里那次 dispatch_sync(pump_queue) 不会嵌套到自己身上。
        openQueue.async {
            res.bridge.close()
            // 【双保险】把两个 block 从桥上摘掉。断环靠的是上面
            // "block 只捕获 counter"那一条；这里是防御性的第二道——将来谁再往
            // block 里捕获一个持有桥的东西，至少泄漏不会跨越 deinit。
            //
            // 顺序必须在 close() **之后**：onEof/onError 是 nonatomic 属性，泵
            // 线程会读它们；close() 内部那次 dispatch_sync(pump_queue) 返回即泵
            // 线程确定已停，这时写才没有数据竞争。
            res.bridge.onEof = nil
            res.bridge.onError = nil
        }
    }

    // MARK: - 打开与关闭

    /// 打开一份素材。成功后播放器落在 `.paused`，业务需显式调用 `play()`。
    ///
    /// **一次失败的 open 会通知三遍，这是有意的，不是重复投递**：
    ///   1. 本方法 `throw` 出 `SYPlayerError`（`try await` 的调用方就地拿到）；
    ///   2. `delegate` 的 `player(_:didFailWith:)` 回调一次；
    ///   3. `state.playback` 发布成 `.failed(错误)`，`@Published` 与每一条
    ///      `stateStream` 都会看到。
    /// 三条路径服务的是三种消费方式（一份快照、三种投递），
    /// 同一个 `SYPlayer` 上同时用 `try/catch` 和 delegate 的调用方会收到两次
    /// 同一个错误——按需要只处理其中一条，不要把它当成两次失败。
    public func open(_ source: SYPlayerSource,
                     decoding: SYPlayerDecoding = .hardwarePreferred) async throws {
        generation &+= 1
        let gen = generation
        // 主 actor 侧的闸门，**同步**关上（见 isSessionLive 的注释）。
        isSessionLive = false
        stopPolling()
        lastError = nil
        pendingError = nil
        hasEnded = false
        endReported = false
        publish(.idle)

        let useHardware = (decoding == .hardwarePreferred)
        let res = resources
        let status: Int = await withCheckedContinuation { continuation in
            self.openQueue.async {
                // 显式 close + 换会话号，都在 -open* **之前**、同一个 block 里。
                //
                // 桥的 -open* 自己也会 close_internal()，所以这次 close 是幂等的
                // 多余动作——多余的是调用，不是它带来的**顺序保证**：close 返回
                // 时那次 dispatch_sync(pump_queue) 已经回来了，旧泵线程确定死透，
                // 于是"旧泵还能发出回调"这件事在 bumpSession() 之前就结束了。
                // 换句话说，新会话号是一道**屏障**：号一变，旧素材的回调就再也
                // 不可能采到它。反过来（先换号再 close）那段几十
                // 毫秒的窗口里旧泵发出的回调采到的是**新**号，一路畅通无阻。
                res.bridge.close()
                res.bumpSession()

                var err: Int32 = 0
                let ok: Bool
                switch source {
                case .file(let url):
                    ok = res.bridge.openLocalFile(url.path, hardwareDecode: useHardware,
                                                  errorOut: &err)
                case .url(let url):
                    ok = res.bridge.openURLString(url.absoluteString, hardwareDecode: useHardware,
                                                  errorOut: &err)
                }
                // 【产品决定】open 成功即代表"已经在播放"，
                // 这在 Swift 门面这一层是反直觉的：AVPlayer 的惯例是打开不等于
                // 播放，调用方要显式调 play()；暂停快速首帧本来就设计成
                // "暂停时也能看到第一帧、听不到声音"。**只在这一层**补一次
                // pause()，不去动 SYPBridge/TrackPlayer——核心 `paused_` 默认
                // false 是 29 个 ctest 用例与 ObjC demo 都依赖的既有契约。
                //
                // 这里调用得尽量早（还在 openQueue 上、还没跳回主 actor），是
                // 本层能做到的最早时点，但**关不严**：-openLocalFile:/
                // -openURLString: 内部的 -startPump() 是把 pump_loop() 派发到
                // 另一条串行队列（dispatch_async），真正跑起来、执行第一次
                // step() 需要一次线程切换；如果那次切换抢在下面这行之前拿到
                // mu_，快照上就已经带着一帧画面或一段音频了。经查 pump_loop()
                // 的 step() 分支在 paused_ 为 false 时会真的写音频、真的推进
                // 时钟（见 track_player.cpp 的非暂停分支），所以这不是纸面上
                // 的竞争。结构上要关严就得让核心"以暂停态启动"，那是在改
                // TrackPlayer/桥的默认语义，本轮明确不做。
                if ok { res.bridge.pause() }
                continuation.resume(returning: ok ? 0 : Int(err))
            }
        }

        guard gen == generation else { throw SYPlayerError.canceled }

        if status != 0 {
            let error = SYPlayerError(statusCode: status)
            lastError = error
            publish(SYPlayerState.make(from: SYPlayerRawSnapshot(), error: error, hasEnded: false))
            delegate?.player(self, didFailWith: error)
            throw error
        }

        // 闸门重新打开必须在 startPolling 之前：新素材的泵已经在跑了。
        isSessionLive = true
        // 补投在闸门关着时到达的那条错误。
        // 走 apply() 而不是就地展开，保证补投与正常投递永远是同一条路径。
        if let pending = pendingError {
            pendingError = nil
            apply(pending)
        }
        startPolling()
        refresh()
    }

    /// 幂等：重复调用、未打开时调用都安全。关闭排在 openQueue 上，与一次正在
    /// 进行的 open 严格前后相接；主线程不等它（桥内部会 request_abort 打断阻塞读，
    /// 但那仍可能是几十毫秒级，没理由让 UI 线程为它停住）。
    public func close() {
        generation &+= 1
        // **这一行是 close 这条路径上真正管用的那道闸门**，而且必须是同步的。
        //
        // 只靠会话号挡不住 close：号在 openQueue 上、桥关完之后才换，而 close()
        // 自己在主 actor 上一路跑完不挂起。那几十毫秒里旧泵发出的 onError 采到
        // 的是旧号、跳回主 actor 时号还没变，照样过关——然后把 state 冻在
        // .failed，而轮询已经停了，再没有人把它推回 .idle。
        // isSessionLive 在这里同步置 false，就把"close() 返回之后才执行的回调"
        // 整类排除掉了：它们要么在 close() 之前已经跑完（合法，那时会话还活着），
        // 要么看见 false。会话号解决的是另一半——重新 open 成功、闸门再次打开
        // 之后，上一份素材的回调不能混进新会话。两道闸门各挡一半，都不多余。
        isSessionLive = false
        stopPolling()
        lastError = nil
        pendingError = nil
        hasEnded = false
        endReported = false
        let res = resources
        // 换号排在 close 之后：close 返回即旧泵已死（见 open 里那段长注释）。
        openQueue.async {
            res.bridge.close()
            res.bumpSession()
        }
        publish(.idle)
    }

    // MARK: - 传输控制（未打开时全是 no-op）

    public func play() {
        // 和 `seek()` 一样先清错误闩。
        //
        // 桥那一侧把错误当作**可恢复**的（SYPBridge.h 的 onError：报错后泵线程
        // 不停，出现非 Error 结果即解除、再次出错会再触发），所以"出错之后再按
        // 一次播放"是调用方最自然的重试动作；不清的话 `lastError` 会让
        // `refresh()` 永远算出 `.failed`，重试看起来毫无反应。
        //
        // 【已知缺口】这只让**显式重试**能走通，不能让 `.failed` 自己退出：
        // 快照里没有错误字段，`refresh()` 只能读本层的闩，于是桥自行恢复时
        // Swift 这一层仍停在 `.failed`（位置却在走）。正确的修法是给快照加一个
        // 错误字段、让 refresh() 从桥的活闩派生 failed，属于后续工作。
        lastError = nil
        pendingError = nil
        bridge.play()
        refresh()
    }

    public func pause() {
        bridge.pause()
        refresh()
    }

    public func seek(to time: TimeInterval) {
        // seek 之后重新武装 EOF 与错误（每次 open/seek 后重新武装）。
        //
        // 这三行清的是**本层**的闩。桥那一侧原先并没有兑现同样的承诺——
        // SYPBridge.h 写着"直到下一次 open/seek"，但 -seekToUs: 从不清
        // eof_notified_，而 -snapshot 原样上报它，于是 refresh() 会立刻把
        // hasEnded 重新置回来，.ended 永远退不掉。已在桥里修好；
        // 这里保留清闩是因为两侧各管各的：本层的 endReported 决定
        // playerDidReachEnd 只报一次，桥那边的标志决定快照里的 eof。

        hasEnded = false
        endReported = false
        lastError = nil
        pendingError = nil
        bridge.seek(toUs: SYPlayerTime.microseconds(fromSeconds: max(0, time)))
        refresh()
    }

    /// [0.5, 2.0]，越界抛 .invalidArgument（未打开时区间照样校验）。
    public func setRate(_ rate: Double) throws {
        guard rate >= 0.5, rate <= 2.0 else { throw SYPlayerError.invalidArgument }
        _ = bridge.setSpeed(rate)
        refresh()
    }

    // MARK: - 视频输出

    public func attach(to view: SYPlayerView) {
        // 先把**别的**播放器从这个视图上摘干净。
        //
        // 下面那行只处理"我自己换了视图"，管不到"这个视图原本属于别人"。
        // SwiftUI 换播放器（`SYPlayerViewRepresentable(player: a)` → `…(player: b)`）走的
        // 正是后一种：视图被复用，a 从来没被通知过。不摘的话两个桥会同时往同一个
        // CAMetalLayer 上 present，而且 a 的桥会一直把这个 layer retain 到它 close
        // 为止——表现为画面撕裂/闪烁加一份不该有的持有。
        if let previous = view.attachedPlayer, previous !== self {
            previous.detachVideo()
        }
        if let old = attachedView, old !== view { old.attachedPlayer = nil }
        attachedView = view
        view.attachedPlayer = self
        view.playerLayer.syncDrawableSize()
        bridge.attachVideoLayer(view.playerLayer)
        // gravity 跟着视图走：挂上的这一刻把视图当前的值推给桥。
        // 视图在 attach 之前改过的 gravity（那时 attachedPlayer 为 nil，didSet 无处
        // 可去）靠这一行生效；换视图时新视图的值覆盖旧视图的。
        bridge.videoGravity = Self.bridged(view.videoGravity)
    }

    public func detachVideo() {
        attachedView?.attachedPlayer = nil
        attachedView = nil
        bridge.attachVideoLayer(nil)
    }

    // SYPlayerVideoHost：视图移出/移回窗口时解挂/重挂，但**不清 attachedView**
    // ——否则切一次 tab 就再也回不来（见 SYPlayerView.didMoveToWindow 的注释）。
    func videoViewDidLeaveWindow(_ view: SYPlayerView) {
        guard attachedView === view else { return }
        bridge.attachVideoLayer(nil)
    }

    func videoViewDidEnterWindow(_ view: SYPlayerView) {
        guard attachedView === view else { return }
        bridge.attachVideoLayer(view.playerLayer)
    }

    // SYPlayerVideoHost：只认当前 attach 的视图。一个已经被换掉
    // 的旧视图（或者别的什么视图）改 gravity，不许影响当前画面。
    //
    // 【已知局限】这条"只认当前视图"的规则建立在"一个播放器同一时刻只画一个视图"
    // 之上（attach(to:) 本来就是这个语义）；将来若要一个播放器同时画多个视图，
    // gravity 要跟着每个输出各存一份，这里要重想。
    func videoGravityDidChange(_ gravity: SYPlayerVideoGravity, from view: SYPlayerView) {
        guard attachedView === view else { return }
        bridge.videoGravity = Self.bridged(gravity)
    }

    /// 公开枚举 → 桥枚举。switch 全枚举、不写 default：加 case 时这里编译报错。
    private static func bridged(_ gravity: SYPlayerVideoGravity) -> SypVideoGravity {
        switch gravity {
        case .aspectFit:  return .aspectFit
        case .aspectFill: return .aspectFill
        case .resize:     return .resize
        }
    }

    // MARK: - 轮询与投递

    private func startPolling() {
        resources.pollingStarts += 1
        stopPolling()
        // 用 Timer(timeInterval:…) + RunLoop.main.add(forMode: .common) 而不是
        // scheduledTimer：后者只进 .default 模式，用户一按住 slider 拖动
        // （tracking 模式）状态就停更了。
        let timer = Timer(timeInterval: stateUpdateInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        RunLoop.main.add(timer, forMode: .common)
        resources.timer = timer
    }

    private func stopPolling() {
        resources.timer?.invalidate()
        resources.timer = nil
    }

    // MARK: - 测试缝（internal，不进公开 API）
    //
    // 为什么需要它们：`stateUpdateInterval` 的钳位分支曾经有一个"只钳值、不重启
    // 定时器"的 bug，而**只看这个属性本身是看不出来的**——钳位后的读数两种实现
    // 完全一样，差别全在那个看不见的 Timer 上。要让用例真的能失败，就得让它看见
    // 定时器。这是最小的一对缝：一个读、一个开。

    /// 当前定时器实际在用的间隔；没有在轮询时为 nil。
    var pollingInterval: TimeInterval? { resources.timer?.timeInterval }

    /// `startPolling()` 被调用过多少次（启动尝试次数，见 pollingStarts 的注释）。
    var pollingStartCount: Int { resources.pollingStarts }

    /// 当前会话号。用例要拿它构造"上一份素材的回调"。
    var currentSessionForTesting: UInt64 { resources.currentSession }

    /// 资源盒与桥本身，**只给弱引用断言用**。
    ///
    /// deinit 用例要证明的不是"SYPlayer 被回收了"——那一条修复前就成立，泄漏的
    /// 从来不是 `self`——而是"桥和资源盒也一起走了"。没有这两道缝，用例只能
    /// 断言在唯一一个**不**泄漏的对象上。返回 `AnyObject` 是刻意的：用例只需要
    /// 能持一个弱引用，不需要（也不该）碰它们的任何成员。
    var resourcesForTesting: AnyObject { resources }
    var bridgeForTesting: AnyObject { resources.bridge }

    /// 闸门关着时暂存的错误是否还在。用例靠它断言"确实走了暂存那条路"，
    /// 而不是碰巧走了闸门已开的那条、把用例测成了空跑。
    var hasPendingErrorForTesting: Bool { pendingError != nil }

    /// 模拟桥在某个会话号上报一次错误。**走的就是真实 onError 的那条路**
    /// （handleError），两道闸门一个不少——这是覆盖双闸门机制的唯一入口，
    /// 因为 `bridge.onError` 由 init 独占设置、`resources` 是私有的，
    /// 用例没有别的办法触发它。
    func simulateBridgeError(_ error: SYPlayerError, session: UInt64) {
        handleError(error, session: session)
    }

    /// 不打开媒体也把轮询开起来，用于验证 startPolling/stopPolling 与钳位的联动。
    func startPollingForTesting() { startPolling() }

    private func refresh() {
        // 闸门关着就什么都不做。
        //
        // 没有这一行会出一个永久性的错状态：`close()` **同步**发布 `.idle`，
        // 但真正拆桥排在 openQueue 上异步进行；那几十毫秒里桥的快照仍然报
        // `hasMedia == YES`、`paused == NO`。于是 `close()` 之后紧跟一次
        // `play()`（或 pause/seek/setRate，四个都无条件调 refresh()）就会把
        // 状态从 `.idle` 拉回 `.playing`、`hasMedia == true`——而轮询已经停了，
        // 再也没有人把它推回去，播放器永远显示着一份不存在的媒体。
        //
        // 对其余调用方都是安全的：`open()` 的失败路径直接 publish `.failed`
        // 不走 refresh()；`apply(_:)` 只在闸门已开之后才跑（handleError 自己
        // 守了一道，open() 的补投排在 `isSessionLive = true` 之后）；定时器
        // 只在会话活着时安装。
        guard isSessionLive else { return }
        let raw = SYPlayerRawSnapshot(bridge.snapshot())
        if raw.eof { hasEnded = true }
        publish(SYPlayerState.make(from: raw, error: lastError, hasEnded: hasEnded))
        if hasEnded && !endReported {
            endReported = true
            delegate?.playerDidReachEnd(self)
        }
    }

    private func publish(_ newState: SYPlayerState) {
        // 相等就不投递。轮询是 10Hz 的定时器，暂停/未打开时每一拍算出来的都是
        // 同一份快照；不挡住的话 objectWillChange、每一条 AsyncStream、delegate
        // 都要空跑一遍——SwiftUI 那边就是每秒 10 次无谓的 body 重算。
        // SYPlayerState 是全字段 Equatable 的值类型，这个比较是廉价且完整的。
        guard newState != state else { return }
        state = newState
        for (_, continuation) in resources.continuations { continuation.yield(newState) }
        delegate?.player(self, didChange: newState)
    }

    private func register(_ continuation: AsyncStream<SYPlayerState>.Continuation) {
        let id = UUID()
        resources.continuations[id] = continuation
        continuation.yield(state)
        let res = resources
        continuation.onTermination = { _ in
            // onTermination 在任意线程被调用，回主 actor 再改字典。
            Task { @MainActor in res.continuations.removeValue(forKey: id) }
        }
    }

    private func handleEof(session: UInt64) {
        guard isSessionLive, session == resources.currentSession else { return }
        // **不在这里置 hasEnded**：这条回调可能是 seek 之前
        // 发出、seek 之后才执行的，直接点亮就会把 seek 刚清掉的 .ended 又贴回去。
        // 交给 refresh() 里那句 `if raw.eof { hasEnded = true }`——它读的是桥的
        // 当前快照，而 seek 已经在桥里把 eof 闩解除了，答案天然是对的。
        refresh()   // 单次投递由 refresh 里的 endReported 闩管
    }

    private func handleError(_ error: SYPlayerError, session: UInt64) {
        // 第一道：会话号。号不对 = 上一份素材的错误，**丢弃**（这一条绝不能改成
        // 暂存，否则新素材会带着旧素材的错误起播）。
        guard session == resources.currentSession else { return }

        // 第二道：主 actor 闸门。号对上了、闸门却还关着，只有一种情况——
        // 这条错误属于**正在打开的这一份**（号是在 openQueue 上、桥 open 之前换的，
        // 新泵一启动就采到新号），而 open() 还没从 await 回来。
        //
        // 这里**必须暂存，不能丢**。丢了就是永久丢：
        //   · SypPlayerSnapshot 根本没有错误字段，轮询捡不回来；
        //   · 泵的 error_reported 闩每段只触发一次、要出现非 Error 结果才解除，
        //     线程模式下第二次回调永远不会来。
        // 于是一次真实的起播失败会表现成"显示 .playing/.buffering、位置不动、
        // 既没有 .failed 也没有 didFailWith"，直到用户自己 seek 一下。
        guard isSessionLive else {
            pendingError = error
            return
        }

        apply(error)
    }

    /// 应用一条属于当前会话的错误。handleError 与 open() 的暂存补投走同一条路径，
    /// 以免"补投"和"正常投递"在语义上慢慢分叉。
    private func apply(_ error: SYPlayerError) {
        // 桥已经做了"同一段连续 Error 只回调一次"的闩，
        // 这里不再去重，原样转发。
        lastError = error
        refresh()
        delegate?.player(self, didFailWith: error)
    }
}
