// SYPlayerStateMappingTests.swift — 微秒↔秒换算与
// 桥快照 → SYPlayerState 的映射（含 buffering reason、clock、clock switch
// reason、statistics 全字段）。
//
// 输入用的是 SYPlayerRawSnapshot（值类型）而不是 SypPlayerSnapshot：后者的
// 属性对外只读（readwrite 只在 SYPBridge.mm 的类扩展里），测试构造不出来。
// 把“ObjC 快照 → 值类型”与“值类型 → 公开状态”拆成两步，前一步是机械搬运、
// 后一步才是有判断的映射逻辑，单测覆盖后者。
import XCTest
@testable import SYPlayerKit

final class SYPlayerStateMappingTests: XCTestCase {
    private func playing() -> SYPlayerRawSnapshot {
        var raw = SYPlayerRawSnapshot()
        raw.hasMedia = true
        raw.speed = 1.0
        return raw
    }

    // MARK: - 时间换算

    func testMicrosecondsToSeconds() {
        XCTAssertEqual(SYPlayerTime.seconds(fromMicroseconds: 1_500_000), 1.5, accuracy: 1e-9)
        XCTAssertEqual(SYPlayerTime.seconds(fromMicroseconds: 0), 0, accuracy: 1e-9)
        XCTAssertEqual(SYPlayerTime.seconds(fromMicroseconds: -250_000), -0.25, accuracy: 1e-9)
    }

    /// seek 的入口换算必须对 NaN/±inf/超界安全：Swift 的 Int64(Double) 在这些
    /// 输入上是 trap（直接崩），而 seek(to:) 的参数来自业务，挡在框架里。
    func testSecondsToMicrosecondsIsSaturatingAndNaNSafe() {
        XCTAssertEqual(SYPlayerTime.microseconds(fromSeconds: 1.5), 1_500_000)
        XCTAssertEqual(SYPlayerTime.microseconds(fromSeconds: .nan), 0)
        XCTAssertEqual(SYPlayerTime.microseconds(fromSeconds: .infinity), Int64.max)
        XCTAssertEqual(SYPlayerTime.microseconds(fromSeconds: -.infinity), Int64.min)
        XCTAssertEqual(SYPlayerTime.microseconds(fromSeconds: 1e30), Int64.max)
    }

    // MARK: - 未打开

    func testNoMediaMapsToIdleWithNoFabricatedFields() {
        let s = SYPlayerState.make(from: SYPlayerRawSnapshot(), error: nil, hasEnded: false)
        XCTAssertEqual(s.playback, .idle)
        XCTAssertFalse(s.hasMedia)
        XCTAssertEqual(s.position, 0)
        XCTAssertNil(s.duration)
        XCTAssertNil(s.buffered)
        XCTAssertNil(s.drift)
        XCTAssertEqual(s.rate, 1.0, "没有媒体时不许显示 0.00x —— 缺省倍速是 1.0")
        XCTAssertEqual(s.clockSwitchReason, .none)
        XCTAssertEqual(s.statistics, .empty)
    }

    // MARK: - 时长 / 缓冲的 Optional 语义

    func testUnknownDurationBecomesNil() {
        var raw = playing()
        raw.durationUs = 0                       // 桥的契约：0 表示未知
        XCTAssertNil(SYPlayerState.make(from: raw, error: nil, hasEnded: false).duration)
        raw.durationUs = 12_000_000
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).duration!,
                       12.0, accuracy: 1e-9)
    }

    func testNegativeBufferedMeansUnbounded() {
        var raw = playing()
        raw.bufferedMs = -1                      // 桥的契约：-1 = 无上限（都已缓冲）
        XCTAssertNil(SYPlayerState.make(from: raw, error: nil, hasEnded: false).buffered)
        raw.bufferedMs = 2500
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).buffered!,
                       2.5, accuracy: 1e-9)
    }

    // MARK: - playback 的优先级

    func testPlaybackPrecedenceFailedOverEndedOverBufferingOverPaused() {
        var raw = playing()
        raw.paused = true
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).playback, .paused)

        raw.buffering = true
        raw.bufferingReason = 3                  // SypBufferingReasonStall
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).playback,
                       .buffering(.stall))

        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: true).playback, .ended)

        XCTAssertEqual(SYPlayerState.make(from: raw, error: .network, hasEnded: true).playback,
                       .failed(.network))
    }

    func testPlayingWhenNotPausedNotBufferingNotEnded() {
        XCTAssertEqual(SYPlayerState.make(from: playing(), error: nil, hasEnded: false).playback,
                       .playing)
        XCTAssertTrue(SYPlayerState.make(from: playing(), error: nil, hasEnded: false).isPlaying)
    }

    func testBufferingReasonMapping() {
        var raw = playing()
        raw.buffering = true
        for (rawReason, expected): (Int, SYPlayerBufferingReason) in
            [(1, .startup), (2, .seek), (3, .stall)] {
            raw.bufferingReason = rawReason
            XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).playback,
                           .buffering(expected))
        }
        // buffering == true 而 reason == None 是桥不该产生的组合；防御性归到 .stall，
        // 不编造一个“没有原因的缓冲”，也不崩。
        raw.bufferingReason = 0
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).playback,
                       .buffering(.stall))
    }

    // MARK: - 时钟

    func testClockAndSwitchReasonMapping() {
        var raw = playing()
        raw.clockKind = 0
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).clock, .audio)
        raw.clockKind = 1
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).clock, .system)
        for (rawReason, expected): (Int, SYPlayerState.ClockSwitchReason) in
            [(0, .none), (1, .audioFailed), (2, .audioEnded)] {
            raw.clockSwitchReason = rawReason
            XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).clockSwitchReason,
                           expected)
        }
    }

    // MARK: - statistics 全字段

    func testStatisticsCarriesEveryField() {
        var raw = playing()
        raw.droppedFrames = 3
        raw.presentFailures = 4
        raw.renderBusyFrames = 5
        raw.catchingUp = true
        raw.rebufferCount = 6
        raw.startupUs = 450_000
        raw.audioUnderruns = 7
        raw.hasVideo = true
        raw.hasPresentedFrame = true
        raw.lastPresentedPtsUs = 2_030_000
        raw.positionUs = 2_000_000

        let s = SYPlayerState.make(from: raw, error: nil, hasEnded: false)
        XCTAssertEqual(s.statistics.droppedFrames, 3)
        XCTAssertEqual(s.statistics.presentFailures, 4)
        XCTAssertEqual(s.statistics.renderBusyRetries, 5)
        XCTAssertTrue(s.statistics.isCatchingUp)
        XCTAssertEqual(s.statistics.rebufferCount, 6)
        XCTAssertEqual(s.statistics.startupDuration!, 0.45, accuracy: 1e-9)
        XCTAssertEqual(s.statistics.audioUnderruns, 7)
        XCTAssertEqual(s.statistics.presentedTime!, 2.03, accuracy: 1e-9)
        XCTAssertEqual(s.drift!, 0.03, accuracy: 1e-9)
    }

    /// startupUs 的未知值是 -1（不是 0，0 会被读成“瞬间起播”）；
    /// 尚无已呈现帧时 presentedTime 与 drift 都必须是 nil，不是某个大负数。
    func testUnknownStartupAndNoPresentedFrame() {
        var raw = playing()
        raw.startupUs = -1
        raw.hasVideo = true
        raw.hasPresentedFrame = false
        raw.lastPresentedPtsUs = Int64.min
        let s = SYPlayerState.make(from: raw, error: nil, hasEnded: false)
        XCTAssertNil(s.statistics.startupDuration)
        XCTAssertNil(s.statistics.presentedTime)
        XCTAssertNil(s.drift)
    }

    /// 纯音频：即使桥给了 lastPresentedPtsUs，也不该算漂移（没有视频轨可对齐）。
    func testAudioOnlyHasNoDrift() {
        var raw = playing()
        raw.hasVideo = false
        raw.hasPresentedFrame = true
        raw.lastPresentedPtsUs = 1_000_000
        XCTAssertNil(SYPlayerState.make(from: raw, error: nil, hasEnded: false).drift)
    }

    // MARK: - 视频显示尺寸

    func testVideoSizeIsNilWhenEitherDimensionIsZero() {
        XCTAssertNil(SYPlayerState.videoSize(width: 0, height: 1080))
        XCTAssertNil(SYPlayerState.videoSize(width: 1920, height: 0))
        XCTAssertEqual(SYPlayerState.videoSize(width: 1080, height: 1920),
                       CGSize(width: 1080, height: 1920))
    }

    /// 上一条只测了纯函数；这一条钉住 make(from:) 真的把快照里的两个字段接进了
    /// videoSize——否则纯函数全对、公开状态里却恒为 nil，照样全绿。
    func testMakeCarriesVideoDisplaySizeFromSnapshot() {
        var raw = playing()
        raw.hasVideo = true
        raw.videoDisplayWidth = 1080
        raw.videoDisplayHeight = 1920
        XCTAssertEqual(SYPlayerState.make(from: raw, error: nil, hasEnded: false).videoSize,
                       CGSize(width: 1080, height: 1920))
        raw.hasMedia = false
        XCTAssertNil(SYPlayerState.make(from: raw, error: nil, hasEnded: false).videoSize,
                     "没有媒体时不该编造尺寸")
    }
}
