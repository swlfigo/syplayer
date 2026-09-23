// SYPlayerError.swift — 公开错误类型。业务侧从此看不见 syp_status 整数，
// 也不用自己维护一份 "-99 是什么意思" 的文案表（旧
// demo/shared/PlayerViewController.swift:115,124-132 那份硬编码映射
// 已经删掉）。
//
// 穷举 syp_status 里“调用方需要区分”的那些码；SYP_ERR_EOF / SYP_ERR_BUSY
// 不在其列：前者是正常结束（state.playback == .ended），后者是内部瞬态
// （契约：渲染器保留帧重试）。真碰到它们（不该发生）落进 .unknown，
// 而不是被吞掉。
import Foundation
import SYPlayerKit_Private

/// 播放器对外抛出的错误。数值轴是内核的 `syp_status`，但调用方不需要知道它：
/// 用 `switch` 分支，或者直接拿 `localizedDescription` 显示。
public enum SYPlayerError: Error, Equatable, Sendable {
    /// 参数不合法（区间外的倍速、空 URL……）。
    case invalidArgument
    /// 操作被取消（如 open 期间再次 open）。
    case canceled
    /// 操作超时。
    case timeout
    /// 内存不足。
    case outOfMemory
    /// 本地读写失败。
    case io
    /// 磁盘空间不足。
    case noSpace
    /// 缓存索引损坏。
    case cacheCorrupt
    /// 网络连接失败或中断。
    case network
    /// 载荷是 HTTP 状态码；0 表示“知道是 HTTP 错误但拿不到具体码”。
    /// 桥当前只透传 syp_status，不带 syp_error_info，所以它恒为 0。
    case httpStatus(Int)
    /// 重定向次数过多。
    case tooManyRedirects
    /// 服务器不支持 Range 请求。
    case rangeUnsupported
    /// 源文件已变更，与缓存不一致。
    case contentChanged
    /// 当前平台或所选解码方式不支持这个媒体（例如 Mac Catalyst 上请求硬解）。
    case notImplemented
    /// 未在上面穷举的 syp_status；载荷是原始数值，便于上报排查。
    case unknown(Int)

    /// 对应的 syp_status 取值。httpStatus 的载荷不参与——这个轴上它恒为 -21。
    public var statusCode: Int {
        switch self {
        case .invalidArgument:   return SypStatusCode.invalidArg.rawValue
        case .canceled:          return SypStatusCode.canceled.rawValue
        case .timeout:           return SypStatusCode.timeout.rawValue
        case .outOfMemory:       return SypStatusCode.outOfMemory.rawValue
        // `.IO`（不是 `.io`）：Swift 导入 NS_ENUM 时会把"整个成员名就是一个
        // 首字母缩写"的那一项原样保留大小写（`SypStatusCodeIO` → `.IO`，
        // 同理 `SypStatusCodeOK` → `.OK`），只有缩写后面还跟着单词时才会整体
        // 小写（如 MTLPixelFormatBGRA8Unorm → .bgra8Unorm）。实测确认。
        case .io:                return SypStatusCode.IO.rawValue
        case .noSpace:           return SypStatusCode.noSpace.rawValue
        case .cacheCorrupt:      return SypStatusCode.cacheCorrupt.rawValue
        case .network:           return SypStatusCode.network.rawValue
        case .httpStatus:        return SypStatusCode.httpStatus.rawValue
        case .tooManyRedirects:  return SypStatusCode.tooManyRedirects.rawValue
        case .rangeUnsupported:  return SypStatusCode.rangeUnsupported.rawValue
        case .contentChanged:    return SypStatusCode.contentChanged.rawValue
        case .notImplemented:    return SypStatusCode.notImplemented.rawValue
        case .unknown(let code): return code
        }
    }

    /// 从 syp_status 构造。未穷举的码（含 EOF / BUSY）一律落进 `.unknown`，
    /// 不返回 nil、不崩。
    public init(statusCode: Int) {
        switch statusCode {
        case SypStatusCode.invalidArg.rawValue:       self = .invalidArgument
        case SypStatusCode.canceled.rawValue:         self = .canceled
        case SypStatusCode.timeout.rawValue:          self = .timeout
        case SypStatusCode.outOfMemory.rawValue:      self = .outOfMemory
        case SypStatusCode.IO.rawValue:               self = .io
        case SypStatusCode.noSpace.rawValue:          self = .noSpace
        case SypStatusCode.cacheCorrupt.rawValue:     self = .cacheCorrupt
        case SypStatusCode.network.rawValue:          self = .network
        case SypStatusCode.httpStatus.rawValue:       self = .httpStatus(0)
        case SypStatusCode.tooManyRedirects.rawValue: self = .tooManyRedirects
        case SypStatusCode.rangeUnsupported.rawValue: self = .rangeUnsupported
        case SypStatusCode.contentChanged.rawValue:   self = .contentChanged
        case SypStatusCode.notImplemented.rawValue:   self = .notImplemented
        default:                                      self = .unknown(statusCode)
        }
    }
}

/// 文案走 `NSLocalizedString`，查的是**框架自己的 bundle**。
///
/// 上一版把中文字面量直接 return 出去：接入方想换语言，只能自己 switch 一遍
/// `SYPlayerError` 重写一份文案表——正是这个框架想替业务消掉的事情
/// （旧 demo 那份硬编码 "-99" 映射）。现在每条文案都有稳定的 key，接入方往
/// `SYPlayerKit.strings` 里填一行就能覆盖，**什么都不填时行为与上一版逐字
/// 相同**：`value:` 参数带的就是原来那句中文，表不存在/键找不到时返回它。
///
/// bundle 取法：`SYPlayerKit` 是**静态** framework，类符号最终落在 App 的可
/// 执行文件里，所以 `Bundle(for:)` 解析出来的通常就是 App 主 bundle——这正是
/// 我们要的（本地化资源随 App 走）。用一个私有 token 类而不是 `SYPlayer.self`：
/// 后者是 `@MainActor` 隔离的类型，从任意上下文取它的元类型只是徒增噪声。
private final class SYPlayerKitBundleToken {}

private func SYPlayerLocalized(_ key: String, _ zhHans: String) -> String {
    NSLocalizedString(key, tableName: "SYPlayerKit",
                      bundle: Bundle(for: SYPlayerKitBundleToken.self),
                      value: zhHans, comment: "")
}

extension SYPlayerError: LocalizedError {
    /// 面向用户的文案；缺省是中文。文案集中在框架里，业务不再各写一份。
    public var errorDescription: String? {
        switch self {
        case .invalidArgument:
            return SYPlayerLocalized("syplayer.error.invalidArgument", "参数不合法。")
        case .canceled:
            return SYPlayerLocalized("syplayer.error.canceled", "操作已取消。")
        case .timeout:
            return SYPlayerLocalized("syplayer.error.timeout", "操作超时。")
        case .outOfMemory:
            return SYPlayerLocalized("syplayer.error.outOfMemory", "内存不足。")
        case .io:
            return SYPlayerLocalized("syplayer.error.io", "本地读写失败。")
        case .noSpace:
            return SYPlayerLocalized("syplayer.error.noSpace", "磁盘空间不足。")
        case .cacheCorrupt:
            return SYPlayerLocalized("syplayer.error.cacheCorrupt", "缓存索引损坏，请清理缓存后重试。")
        case .network:
            return SYPlayerLocalized("syplayer.error.network", "网络连接失败或中断。")
        case .httpStatus(let code):
            // 带参数的两条用 String(format:)：本地化后的语序可能与中文不同，
            // 字符串插值会把参数位置钉死在中文的位置上。
            return code > 0
                ? String(format: SYPlayerLocalized("syplayer.error.httpStatus",
                                                   "服务器返回错误状态码 %ld。"), code)
                : SYPlayerLocalized("syplayer.error.httpStatusUnknown", "服务器返回了错误状态码。")
        case .tooManyRedirects:
            return SYPlayerLocalized("syplayer.error.tooManyRedirects", "重定向次数过多。")
        case .rangeUnsupported:
            return SYPlayerLocalized("syplayer.error.rangeUnsupported", "服务器不支持分段请求（Range）。")
        case .contentChanged:
            return SYPlayerLocalized("syplayer.error.contentChanged", "源文件已变更，与缓存不一致。")
        case .notImplemented:
            return SYPlayerLocalized("syplayer.error.notImplemented", "当前平台或所选解码方式不支持这个媒体。")
        case .unknown(let code):
            return String(format: SYPlayerLocalized("syplayer.error.unknown",
                                                    "未知错误（syp_status=%ld）。"), code)
        }
    }
}
