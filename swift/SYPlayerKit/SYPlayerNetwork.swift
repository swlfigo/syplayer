// SYPlayerNetwork.swift — 进程级网络设置。
import Foundation
import SYPlayerKit_Private

/// 进程级网络设置。所有播放器与预加载器共享。
///
/// **不是 `@MainActor`**：与 SYPlayer/SYPlayerPreloader 不同，这里没有任何
/// CALayer/UI 状态——底下的 `syp_rate_limit_set/get`（含 SypNetworkBridge 转发）
/// 本身线程安全、可从任意线程调用（见 syp_net.h）。标 `@MainActor` 反而会让
/// XCTestCase 非隔离的 `tearDown()` 里那行复位编译不过。
public enum SYPlayerNetwork {
    /// 进程级下行限速（字节/秒）。`nil` = 不限（默认）；≤ 0 视同 `nil`。
    /// 播放优先：预加载只在还有富余额度时才会**开始新的下载**；已经开始的预加载
    /// 会先把额度用超，超出的部分之后慢慢还上——所以一波预加载刚发出去之后，
    /// 播放可能要短暂等一下（通常不超过几秒）。只控平均速率；服务端不支持 Range
    /// 时的整文件下载不受限；**播放过程中**抓取的 HLS 播放列表（含直播刷新）
    /// 不受限，但**预加载**抓取的播放列表与预加载读取视频信息的那一小段受限
    /// （按播放优先级走正常的额度）。
    /// 过大的值会被夹到内部上限（2^40 字节/秒）。
    ///
    /// ⚠️ **通常是一次原子写、立即返回**，可以从任意线程调用；但极少数情况下
    /// （内部的后台调度在这一刻起不来，限速会自动放开以避免永久卡住）这次
    /// 赋值会**在调用它的这个线程上同步触发一轮下载调度**，可能直接发起新的
    /// 网络请求。因此不要在持有自己的锁时调用它，也不要在播放器的事件回调
    /// （例如状态变化、错误回调）内部调用它——那种上下文本身可能已经在别的
    /// 锁里。
    public static var maximumDownloadRate: Int? {
        get {
            let v = SypNetworkBridge.rateLimitBytesPerSecond()
            return v > 0 ? Int(v) : nil
        }
        set {
            let v = newValue ?? 0
            SypNetworkBridge.setRateLimitBytesPerSecond(v > 0 ? Int64(v) : 0)
        }
    }

    /// 测试缝：直接读 C 层（消费者），不经上面的 getter 的换算。
    static var rateLimitInCLayerForTest: Int64 { SypNetworkBridge.rateLimitBytesPerSecond() }

    /// 预连接：尽力而为地提前与 `url` 所在服务器建好连接（DNS/TCP/TLS），让随后的
    /// 播放或预加载少等一次建连。立即返回；同一服务器 30 秒内重复调用只生效一次；
    /// 不下载内容、不占缓存、不受 `maximumDownloadRate` 限制。适合"用户很可能马上
    /// 点开、但还不值得预加载"的条目（例如信息流里即将滑入屏幕的视频）。
    public static func preconnect(_ url: URL, headers: [String: String] = [:]) {
        // 非 http(s) 直接 return：C 层（Preconnector::origin_key）本来就会判掉，
        // 这里提前判只是省一次桥调用，不是补一道 C 层没有的防线（去掉这一判断、
        // 只留 C 层挡，testPreconnectNonHTTPIsNoop 仍然绿）。
        guard let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https" else {
            return
        }
        let names = Array(headers.keys)
        let values = names.map { headers[$0]! }
        SypNetworkBridge.preconnectURL(url.absoluteString, headerNames: names, headerValues: values)
    }

    /// 测试缝：直接读 C 层在途预连接数（消费者），不经业务侧任何状态。
    static var preconnectInflightForTest: Int32 { SypNetworkBridge.preconnectInflightForTest() }

    /// 测试缝：`preconnect(_:headers:)` 是否已经（惰性）注册好 HTTP 后端。
    /// 已知局限：同进程内先跑过的用例可能已经注册过，这条
    /// 钉不住"本次调用自己触发了注册"这件事，只能钉"调用之后一定已注册"。
    static var preconnectBackendRegisteredForTest: Bool {
        SypNetworkBridge.preconnectBackendRegisteredForTest()
    }
}
