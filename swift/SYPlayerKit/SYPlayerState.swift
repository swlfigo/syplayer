// SYPlayerState.swift — 公开状态。**一次性带全字段的值类型**：三种消费方式
// （@Published / AsyncStream / delegate）底下是同一份快照，不做三套语义。
//
// hasMedia 这个字段是从 SYPBridge.h 的同名契约原样搬上来的，不是冗余：
// playback == .idle 只说明“没在播”，而 rate/clock/统计这些字段在“根本没打开”
// 时全都没有意义。零值在语义上恰好都看起来合法（0.00x、Audio 时钟、未暂停），
// 调用方需要的是**可分辨性**，显示成什么由它自己决定。
import CoreGraphics
import Foundation

/// 缓冲的原因。只在 `playback == .buffering(_)` 时存在，所以没有 `.none`
/// （“没有原因的缓冲”不是一个需要表达的状态）。
public enum SYPlayerBufferingReason: Equatable, Sendable {
    /// 起播缓冲。
    case startup
    /// seek 之后的缓冲。
    case seek
    /// 播放中卡顿。
    case stall
}

/// 播放质量统计。所有计数都是“本次 open 以来的累计值”，`close()` 后复位。
public struct SYPlayerStatistics: Equatable, Sendable {
    /// 追帧时主动丢弃的视频帧数。
    public let droppedFrames: Int
    /// 呈现失败的次数（拿不到 drawable 等）。
    public let presentFailures: Int
    /// 渲染器在途上限已满时的重试次数（BUSY 的帧不丢、留着下一轮
    /// 重试，同一帧重试几次计几次）——**不是丢帧数**，真丢掉的计入 droppedFrames。
    public let renderBusyRetries: Int
    /// 是否正处于追帧（丢帧赶进度）状态。
    public let isCatchingUp: Bool
    /// 卡顿（重新缓冲）次数。
    public let rebufferCount: Int
    /// 起播耗时；nil = 尚未完成一次起播（桥给 -1）。
    public let startupDuration: TimeInterval?
    /// 音频欠载段数。
    public let audioUnderruns: Int
    /// 最近一次成功呈现帧的 pts；nil = 无视频轨或尚无已呈现帧。
    public let presentedTime: TimeInterval?

    /// 未打开媒体时的空统计。
    public static let empty = SYPlayerStatistics(
        droppedFrames: 0, presentFailures: 0, renderBusyRetries: 0, isCatchingUp: false,
        rebufferCount: 0, startupDuration: nil, audioUnderruns: 0, presentedTime: nil)

    /// 逐字段构造器，**公开**。
    ///
    /// 全字段 `public let` 的结构体，Swift 只会合成一个 `internal` 的逐字段
    /// 构造器——框架外面拿不到，SwiftUI 预览和接入方自己的用例就没法造一份
    /// 假统计出来。每个参数都带缺省值（等于 `.empty` 的那一份），于是
    /// `SYPlayerStatistics(droppedFrames: 3)` 就够写一个 fixture。
    public init(droppedFrames: Int = 0,
                presentFailures: Int = 0,
                renderBusyRetries: Int = 0,
                isCatchingUp: Bool = false,
                rebufferCount: Int = 0,
                startupDuration: TimeInterval? = nil,
                audioUnderruns: Int = 0,
                presentedTime: TimeInterval? = nil) {
        self.droppedFrames = droppedFrames
        self.presentFailures = presentFailures
        self.renderBusyRetries = renderBusyRetries
        self.isCatchingUp = isCatchingUp
        self.rebufferCount = rebufferCount
        self.startupDuration = startupDuration
        self.audioUnderruns = audioUnderruns
        self.presentedTime = presentedTime
    }
}

/// 播放器的一次完整状态快照。值类型，可以放心跨线程传递与比较。
public struct SYPlayerState: Equatable, Sendable {
    /// 播放阶段。优先级从高到低：失败 > 结束 > 缓冲 > 暂停 > 播放。
    public enum Playback: Equatable, Sendable {
        /// 尚未打开媒体（或已 close）。
        case idle
        /// 正在缓冲，附带原因。
        case buffering(SYPlayerBufferingReason)
        /// 正在播放。
        case playing
        /// 已暂停。
        case paused
        /// 已播放到结尾。
        case ended
        /// 已失败，附带错误。
        case failed(SYPlayerError)
    }
    /// 当前主时钟来源。
    public enum Clock: Equatable, Sendable { case audio, system }
    /// 主时钟从音频切到系统时钟的原因。
    public enum ClockSwitchReason: Equatable, Sendable {
        /// 仍是音频钟（或从来没有音频钟）。
        case none
        /// 音频 sink 失败，不可逆。
        case audioFailed
        /// 音频已播完，seek 后可逆。
        case audioEnded
    }

    public let playback: Playback
    /// 是否已经打开了媒体。false 时除 playback 外的字段都没有意义（见文件顶部）。
    public let hasMedia: Bool
    /// 当前播放位置（秒）。未打开时为 0。
    public let position: TimeInterval
    public let duration: TimeInterval?          // nil = 未知/直播
    public let buffered: TimeInterval?          // nil = 无上限（在播轨都已读完）
    /// 本次 open 是否绑定了视频轨。
    public let hasVideo: Bool
    /// 本次 open 是否走的硬解。
    public let isHardwareDecoding: Bool
    /// 倍速；未打开时为缺省的 1.0。
    public let rate: Double
    public let clock: Clock
    public let clockSwitchReason: ClockSwitchReason
    public let statistics: SYPlayerStatistics
    /// 画面的显示尺寸（像素）：已按 SAR 拉伸、按旋转交换宽高——即"画面该按
    /// 什么宽高比摆"，业务据此给视图定宽高比。nil = 没有视频轨（纯音频、或只有
    /// 封面图）或尚未打开。
    public let videoSize: CGSize?

    /// 逐字段构造器，**公开**，理由同 `SYPlayerStatistics.init`：
    /// 合成出来的那个是 internal，接入方造不出预览用的假状态。缺省值取
    /// `.idle` 的那一份，所以 `SYPlayerState(playback: .playing, hasMedia: true,
    /// position: 12, duration: 300)` 就是一条可用的 fixture。
    public init(playback: Playback = .idle,
                hasMedia: Bool = false,
                position: TimeInterval = 0,
                duration: TimeInterval? = nil,
                buffered: TimeInterval? = nil,
                hasVideo: Bool = false,
                isHardwareDecoding: Bool = false,
                rate: Double = 1.0,
                clock: Clock = .audio,
                clockSwitchReason: ClockSwitchReason = .none,
                statistics: SYPlayerStatistics = .empty,
                videoSize: CGSize? = nil) {
        self.playback = playback
        self.hasMedia = hasMedia
        self.position = position
        self.duration = duration
        self.buffered = buffered
        self.hasVideo = hasVideo
        self.isHardwareDecoding = isHardwareDecoding
        self.rate = rate
        self.clock = clock
        self.clockSwitchReason = clockSwitchReason
        self.statistics = statistics
        self.videoSize = videoSize
    }

    /// 未打开媒体时的缺省状态。
    public static let idle = SYPlayerState(
        playback: .idle, hasMedia: false, position: 0, duration: nil, buffered: nil,
        hasVideo: false, isHardwareDecoding: false, rate: 1.0, clock: .audio,
        clockSwitchReason: .none, statistics: .empty, videoSize: nil)

    /// 是否正在播放（既没暂停也没缓冲、没结束、没失败）。
    public var isPlaying: Bool { playback == .playing }
    /// 是否正在缓冲（不关心原因）。
    public var isBuffering: Bool { if case .buffering = playback { return true }; return false }
    /// 是否已播放到结尾。
    public var isEnded: Bool { playback == .ended }
    /// 当前错误；没有失败时为 nil。
    public var error: SYPlayerError? { if case .failed(let e) = playback { return e }; return nil }

    /// 最近呈现帧 pts − 当前播放位置。这是 demo 壳存在的主要理由：
    /// AudioUnitSink 的设备延迟常数没有自动化覆盖，取错时这个数会稳定地偏在某个
    /// 非零值上而不是在 0 附近抖动。框架只算、不平滑——做了滤波就把该看见的偏差
    /// 抹掉了。无视频轨或尚无已呈现帧时为 nil。
    public var drift: TimeInterval? {
        guard hasVideo, let presented = statistics.presentedTime else { return nil }
        return presented - position
    }
}
