// SYPlayerSnapshotMapping.swift — 桥快照 → 公开状态的全部映射逻辑，以及
// 微秒↔秒换算。framework 内部，不公开。
//
// 拆成两步：
//   1. SYPlayerRawSnapshot：ObjC 快照的值类型影子，机械搬运、零判断；
//   2. SYPlayerState.make(from:error:hasEnded:)：有判断的部分（Optional 语义、
//      playback 优先级、枚举翻译）。
// 这么拆是为了可测：SypPlayerSnapshot 的属性对外只读（readwrite 只在
// SYPBridge.mm 的类扩展里），测试构造不出来；值类型可以。
import CoreGraphics
import Foundation
import SYPlayerKit_Private

struct SYPlayerRawSnapshot {
    var hasMedia = false
    var positionUs: Int64 = 0
    var durationUs: Int64 = 0
    var lastPresentedPtsUs: Int64 = Int64.min
    var hasPresentedFrame = false
    var hasVideo = false
    var clockKind: Int = 0
    var clockSwitchReason: Int = 0
    var droppedFrames: Int64 = 0
    var presentFailures: Int64 = 0
    var paused = false
    var speed: Double = 0
    var eof = false
    var hardwareDecoding = false
    var renderBusyFrames: Int64 = 0
    var catchingUp = false
    var buffering = false
    var bufferingReason: Int = 0
    var bufferedMs: Int64 = 0
    var rebufferCount: Int64 = 0
    var startupUs: Int64 = -1
    var audioUnderruns: Int64 = 0
    var videoDisplayWidth: Int32 = 0
    var videoDisplayHeight: Int32 = 0

    init() {}

    init(_ s: SypPlayerSnapshot) {
        hasMedia = s.hasMedia
        positionUs = s.positionUs
        durationUs = s.durationUs
        lastPresentedPtsUs = s.lastPresentedPtsUs
        hasPresentedFrame = s.hasPresentedFrame
        hasVideo = s.hasVideo
        clockKind = s.clockKind.rawValue
        clockSwitchReason = s.clockSwitchReason.rawValue
        droppedFrames = s.droppedFrames
        presentFailures = s.presentFailures
        paused = s.paused
        speed = s.speed
        eof = s.eof
        hardwareDecoding = s.hardwareDecoding
        renderBusyFrames = s.renderBusyFrames
        catchingUp = s.catchingUp
        buffering = s.buffering
        bufferingReason = s.bufferingReason.rawValue
        bufferedMs = s.bufferedMs
        rebufferCount = s.rebufferCount
        startupUs = s.startupUs
        audioUnderruns = s.audioUnderruns
        videoDisplayWidth = s.videoDisplayWidth
        videoDisplayHeight = s.videoDisplayHeight
    }
}

enum SYPlayerTime {
    static func seconds(fromMicroseconds us: Int64) -> TimeInterval {
        Double(us) / 1_000_000.0
    }

    /// 饱和转换。Swift 的 Int64(Double) 对 NaN/±inf/超界是 **trap**（进程直接崩），
    /// 而 seek(to:) 的参数来自业务代码——挡在框架里，不让业务替我们记住这条。
    static func microseconds(fromSeconds seconds: TimeInterval) -> Int64 {
        guard seconds.isFinite else { return seconds.isNaN ? 0 : (seconds > 0 ? .max : .min) }
        let us = (seconds * 1_000_000.0).rounded()
        if us >= Double(Int64.max) { return .max }
        if us <= Double(Int64.min) { return .min }
        return Int64(us)
    }
}

extension SYPlayerState {
    /// 宽高任一为 0 ⇒ nil（无视频轨或尚未打开）。单独成函数只为可单测，
    /// 不内联在 make(from:) 里。
    static func videoSize(width: Int32, height: Int32) -> CGSize? {
        guard width > 0, height > 0 else { return nil }
        return CGSize(width: Int(width), height: Int(height))
    }

    static func make(from raw: SYPlayerRawSnapshot,
                     error: SYPlayerError?,
                     hasEnded: Bool) -> SYPlayerState {
        guard raw.hasMedia else {
            // 没有媒体：除了错误（open 失败也要能显示）之外一律回到 idle 的缺省值。
            // 不在这里替一个不存在的播放器编造 rate/clock/统计。
            if let error = error {
                return SYPlayerState(playback: .failed(error), hasMedia: false, position: 0,
                                     duration: nil, buffered: nil, hasVideo: false,
                                     isHardwareDecoding: false, rate: 1.0, clock: .audio,
                                     clockSwitchReason: .none, statistics: .empty)
            }
            return .idle
        }

        let playback: Playback = {
            if let error = error { return .failed(error) }
            if hasEnded || raw.eof { return .ended }
            if raw.buffering {
                switch raw.bufferingReason {
                case 1:  return .buffering(.startup)
                case 2:  return .buffering(.seek)
                case 3:  return .buffering(.stall)
                // buffering == true 而 reason == None 是桥不该产生的组合
                // （SYPBridge.mm 的 static_assert 钉住了枚举值，None 只在未缓冲时出现）。
                // 防御性归到 .stall：宁可把原因说得保守一点，也不编造“没有原因的缓冲”。
                default: return .buffering(.stall)
                }
            }
            return raw.paused ? .paused : .playing
        }()

        let presented: TimeInterval? = (raw.hasVideo && raw.hasPresentedFrame)
            ? SYPlayerTime.seconds(fromMicroseconds: raw.lastPresentedPtsUs) : nil

        let stats = SYPlayerStatistics(
            droppedFrames: Int(raw.droppedFrames),
            presentFailures: Int(raw.presentFailures),
            renderBusyRetries: Int(raw.renderBusyFrames),
            isCatchingUp: raw.catchingUp,
            rebufferCount: Int(raw.rebufferCount),
            startupDuration: raw.startupUs >= 0
                ? SYPlayerTime.seconds(fromMicroseconds: raw.startupUs) : nil,
            audioUnderruns: Int(raw.audioUnderruns),
            presentedTime: presented)

        return SYPlayerState(
            playback: playback,
            hasMedia: true,
            position: SYPlayerTime.seconds(fromMicroseconds: raw.positionUs),
            duration: raw.durationUs > 0
                ? SYPlayerTime.seconds(fromMicroseconds: raw.durationUs) : nil,
            buffered: raw.bufferedMs >= 0 ? Double(raw.bufferedMs) / 1000.0 : nil,
            hasVideo: raw.hasVideo,
            isHardwareDecoding: raw.hardwareDecoding,
            rate: raw.speed,
            clock: raw.clockKind == 1 ? .system : .audio,
            clockSwitchReason: {
                switch raw.clockSwitchReason {
                case 1:  return .audioFailed
                case 2:  return .audioEnded
                default: return .none
                }
            }(),
            statistics: stats,
            videoSize: videoSize(width: raw.videoDisplayWidth, height: raw.videoDisplayHeight))
    }
}
