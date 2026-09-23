// SYPlayerLayerTests.swift — bounds 变化后 drawableSize 同步，
// 含 0×0 保持上一次有效值。
//
// 这两条都是**离屏可验证**的（不需要真的上屏、不需要 GPU 出画面），正是
// "Swift 层可测的部分"。它们分别对应两个真实事故面：
//   · 不同步 drawableSize → nextDrawable 恒 nil → present() 报 BUSY，画面永远出不来
//     （PlayerView.swift 旧注释里那段）；
//   · 同步成 0×0 → 同样恒 nil，而且这一次连"有没有设过"都看不出来
//     （视图刚建好、还没布局，或被放进隐藏容器）。
import XCTest
@testable import SYPlayerKit

final class SYPlayerLayerTests: XCTestCase {
    func testDrawableSizeFollowsBoundsTimesContentsScale() {
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
        view.playerLayer.contentsScale = 2.0
        view.layoutIfNeeded()
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 640, height: 360))
    }

    func testDrawableSizeUpdatesWhenBoundsChange() {
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
        view.playerLayer.contentsScale = 1.0
        view.layoutIfNeeded()
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 320, height: 180))

        view.frame = CGRect(x: 0, y: 0, width: 400, height: 300)
        view.layoutIfNeeded()
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 400, height: 300))
    }

    /// 0×0 必须保持上一次有效值：置 0 会让 nextDrawable 恒 nil，而且之后即使
    /// 尺寸回来了，中间那段时间的帧已经白丢了。
    func testZeroBoundsKeepsLastValidDrawableSize() {
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
        view.playerLayer.contentsScale = 1.0
        view.layoutIfNeeded()
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 320, height: 180))

        view.frame = .zero
        view.layoutIfNeeded()
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 320, height: 180),
                       "0×0 不该下发给 drawableSize")
    }

    /// 从未有过有效尺寸时也不能崩，而且 drawableSize 必须**恰好**停在 .zero。
    ///
    /// 断言 `>= 0` 是不够的：那条断言对"syncDrawableSize 把
    /// bounds×scale 算成 0×0 之后照样下发"和"守卫生效、一个字都没写"这两种
    /// 行为同样成立，而它们在真机上差别很大——恒 0 的 drawableSize 会让
    /// nextDrawable 永远返回 nil。这里钉死"守卫生效、值仍是构造时的 0×0"。
    func testZeroBoundsFromTheStartIsHarmless() {
        let view = SYPlayerView(frame: .zero)
        view.layoutIfNeeded()
        XCTAssertEqual(view.playerLayer.drawableSize, .zero,
                       "从未有过有效尺寸时 drawableSize 应保持构造时的 0×0")
    }

    func testLayerClassAndConfiguration() {
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 10, height: 10))
        XCTAssertTrue(view.layer is SYPlayerLayer)
        // device 为 nil 时 CAMetalLayer 一帧都出不来，而且不会报错、只是安静地黑屏，
        // 所以这条必须断言。
        XCTAssertNotNil(view.playerLayer.device)
        XCTAssertEqual(view.playerLayer.pixelFormat, .bgra8Unorm)
        XCTAssertTrue(view.playerLayer.framebufferOnly)
        XCTAssertEqual(view.videoGravity, .aspectFit)
    }
}

/// 只记事的宿主，用来单独验证 SYPlayerView 那一侧的通知，不牵扯播放器。
@MainActor
private final class RecordingVideoHost: SYPlayerVideoHost {
    var enterCount = 0
    var leaveCount = 0
    var detachCount = 0
    func videoViewDidEnterWindow(_ view: SYPlayerView) { enterCount += 1 }
    func videoViewDidLeaveWindow(_ view: SYPlayerView) { leaveCount += 1 }
    // 这组用例只关心窗口进出与解挂，gravity 通知不记。
    func videoGravityDidChange(_ gravity: SYPlayerVideoGravity, from view: SYPlayerView) {}
    func detachVideo() { detachCount += 1 }
}

/// 回归：**解挂必须是可逆的**。
///
/// 这组用例存在的理由是一个具体的事故形状：UITabBarController 切页会把非当前
/// 页的视图移出窗口，如果 videoViewDidLeaveWindow 顺手把 attachedView /
/// attachedPlayer 清掉，切回来时就没有人再把 layer 挂回渲染器——表现为"切一次
/// tab 之后永久黑屏，声音还在"。当时 SYPlayer 还不存在，这条路径只能靠
/// 编译通过来保证；现在 SYPlayer 实现了这个协议，把它钉住。
///
/// **这里不建 UIWindow。** 无 host 的 Mac
/// Catalyst 测试 bundle 里 `UIWindow(frame:)` 直接抛
/// NSInternalInconsistencyException（"NSApplication has not been created yet"），
/// 因为 UIKit 根本没被引导起来——实测四条用例全挂在构造窗口那一行，不是偶发
/// 而是必然。所以这组用例驱动的是 didMoveToWindow **会调用的那两个方法本身**：
/// 要保护的不变量（"离开窗口不清挂接"）整个位于 SYPlayer 这一侧，它是可测的；
/// 而"didMoveToWindow 分派到这两个方法"那一跳只有三行、无分支，留给
/// demo 人工验收。宁可少测一跳，也不要一个只能靠环境
/// 碰运气的用例。
@MainActor
final class SYPlayerAttachLifecycleTests: XCTestCase {

    private func makeView() -> SYPlayerView {
        SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
    }

    /// 离开窗口 → 回到窗口走一整圈之后，播放器与视图仍然相互认得。
    func testPlayerIsReattachedAfterLeavingAndReenteringWindow() {
        let player = SYPlayer()
        let view = makeView()

        player.attach(to: view)
        XCTAssertTrue(view.attachedPlayer === player)
        XCTAssertTrue(player.attachedView === view)

        player.videoViewDidLeaveWindow(view)
        // 关键断言：离开窗口只是解挂 layer，**不许**把挂接关系清掉。
        XCTAssertTrue(view.attachedPlayer === player, "离开窗口不该清掉视图侧的挂接")
        XCTAssertTrue(player.attachedView === view, "离开窗口不该清掉播放器侧的挂接")

        player.videoViewDidEnterWindow(view)
        XCTAssertTrue(view.attachedPlayer === player)
        XCTAssertTrue(player.attachedView === view, "回到窗口后必须仍然是同一对")

        // 来回多次也不该退化（切 tab 不止一次）。
        for _ in 0..<3 {
            player.videoViewDidLeaveWindow(view)
            player.videoViewDidEnterWindow(view)
        }
        XCTAssertTrue(player.attachedView === view)
        XCTAssertTrue(view.attachedPlayer === player)
    }

    /// 视图移出窗口时确实会通知宿主。didMoveToWindow 的这条分支（window == nil）
    /// 不需要 UIWindow 就能走到，所以它在这里是可测的；另一条分支见类注释。
    func testLeavingWindowNotifiesHost() {
        let view = makeView()
        let host = RecordingVideoHost()
        view.attachedPlayer = host

        view.didMoveToWindow()   // window 仍是 nil，走"离开"分支

        XCTAssertEqual(host.leaveCount, 1)
        XCTAssertEqual(host.enterCount, 0)
    }

    /// 已经不是当前挂接对象的视图发来的窗口事件必须被忽略——否则一个刚被换下的
    /// 旧视图进一次窗口，就会把渲染器的输出 layer 抢回它自己那份。
    func testWindowEventsFromAStaleViewAreIgnored() {
        let player = SYPlayer()
        let current = makeView()
        let stale = makeView()

        player.attach(to: stale)
        player.attach(to: current)

        player.videoViewDidLeaveWindow(stale)
        XCTAssertTrue(player.attachedView === current, "旧视图的离开事件不该影响当前挂接")
        player.videoViewDidEnterWindow(stale)
        XCTAssertTrue(player.attachedView === current, "旧视图的进入事件不该抢回挂接")
    }

    /// 挂到第二个视图时，第一个视图要被摘干净——否则它还会往播放器发窗口事件。
    func testAttachingToAnotherViewReleasesThePreviousOne() {
        let player = SYPlayer()
        let first = makeView()
        let second = makeView()

        player.attach(to: first)
        player.attach(to: second)

        XCTAssertNil(first.attachedPlayer, "旧视图必须被摘掉")
        XCTAssertTrue(second.attachedPlayer === player)
        XCTAssertTrue(player.attachedView === second)
    }

    /// 同一个视图重复 attach 是幂等的（SwiftUI 的 updateUIView 可能重复走到）。
    func testAttachingTheSameViewTwiceIsIdempotent() {
        let player = SYPlayer()
        let view = makeView()

        player.attach(to: view)
        player.attach(to: view)

        XCTAssertTrue(view.attachedPlayer === player, "重复 attach 不该把自己摘掉")
        XCTAssertTrue(player.attachedView === view)
    }

    /// detachVideo() 是显式解挂，与窗口进出不同：两侧都清空，而且幂等。
    func testDetachVideoClearsBothSidesAndIsIdempotent() {
        let player = SYPlayer()
        let view = makeView()

        player.attach(to: view)
        player.detachVideo()
        XCTAssertNil(view.attachedPlayer)
        XCTAssertNil(player.attachedView)

        player.detachVideo()   // 重复调用不该崩
        XCTAssertNil(player.attachedView)
    }

    /// 解挂之后视图再发窗口事件，不该再惊动播放器（宿主引用已经断了）。
    func testWindowEventsAfterDetachDoNotReachThePlayer() {
        let player = SYPlayer()
        let view = makeView()

        player.attach(to: view)
        player.detachVideo()
        view.didMoveToWindow()

        XCTAssertNil(player.attachedView)
        XCTAssertNil(view.attachedPlayer)
    }

    /// attach 会顺手同步一次 drawableSize：SwiftUI 的 makeUIView 里视图还没布局过，
    /// 不在 attach 时补这一下，第一帧就会因为 0×0 拿不到 drawable。
    func testAttachSyncsDrawableSize() {
        let player = SYPlayer()
        let view = makeView()
        view.playerLayer.contentsScale = 1.0

        player.attach(to: view)

        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 320, height: 180))
    }
}

/// SYPlayer 里不需要打开媒体就能验的那几条公开契约。真正的 open → play → .ended
/// 生命周期用例属于另一组（要 fixture 与音频设备），这里只钉住"没打开时也必须
/// 成立"的部分。
@MainActor
final class SYPlayerClosedStateTests: XCTestCase {

    func testFreshPlayerIsIdle() {
        let player = SYPlayer()
        XCTAssertEqual(player.state.playback, .idle)
        XCTAssertFalse(player.state.hasMedia)
        XCTAssertNil(player.state.duration)
    }

    /// 未打开时 play/pause/seek 全是 no-op，且不会崩。
    func testTransportControlsAreNoOpsWhenClosed() {
        let player = SYPlayer()
        player.play()
        player.pause()
        player.seek(to: 5)
        player.seek(to: -1)          // 负数被 max(0,·) 挡住
        player.seek(to: .infinity)   // 饱和转换，不 trap
        XCTAssertEqual(player.state.playback, .idle)
    }

    /// 区间校验不依赖是否已打开。
    func testSetRateValidatesRangeEvenWhenClosed() {
        let player = SYPlayer()
        XCTAssertThrowsError(try player.setRate(0.49)) { error in
            XCTAssertEqual(error as? SYPlayerError, .invalidArgument)
        }
        XCTAssertThrowsError(try player.setRate(2.01))
        XCTAssertNoThrow(try player.setRate(0.5))
        XCTAssertNoThrow(try player.setRate(2.0))
        XCTAssertNoThrow(try player.setRate(1.5))
    }

    /// close() 幂等：没打开时调、连着调都安全。
    func testCloseIsIdempotent() {
        let player = SYPlayer()
        player.close()
        player.close()
        XCTAssertEqual(player.state.playback, .idle)
        XCTAssertFalse(player.state.hasMedia)
    }

    /// 轮询间隔钳到 1/60。**只验值是不够的**——见下一条。
    func testStateUpdateIntervalIsClamped() {
        let player = SYPlayer()
        XCTAssertEqual(player.stateUpdateInterval, 0.1, accuracy: 1e-9)
        player.stateUpdateInterval = 0.001
        XCTAssertEqual(player.stateUpdateInterval, 1.0 / 60.0, accuracy: 1e-9)
        player.stateUpdateInterval = 0.5
        XCTAssertEqual(player.stateUpdateInterval, 0.5, accuracy: 1e-9)
    }

    /// 钳位那一次**也必须重启定时器**。
    ///
    /// 这条用例要盯住的是：上面那条只读 stateUpdateInterval，而
    /// "钳完就 return、不重启"的错误实现给出的读数**一模一样**（都是 1/60），
    /// 差别整个藏在那个看不见的 Timer 上。所以这里穿过 pollingInterval 这道
    /// internal 缝去看真实的定时器间隔——把 `return` 加回实现里，本条会失败，
    /// 上面那条仍然会过。
    func testClampedIntervalRestartsThePollingTimer() {
        let player = SYPlayer()
        XCTAssertNil(player.pollingInterval, "没开始轮询时不该有定时器")

        player.startPollingForTesting()
        XCTAssertEqual(player.pollingInterval ?? -1, 0.1, accuracy: 1e-9)

        player.stateUpdateInterval = 0.001          // 走钳位分支
        XCTAssertEqual(player.stateUpdateInterval, 1.0 / 60.0, accuracy: 1e-9)
        XCTAssertEqual(player.pollingInterval ?? -1, 1.0 / 60.0, accuracy: 1e-9,
                       "钳位那一次也必须按新间隔重启定时器")

        player.stateUpdateInterval = 0.25           // 不钳位分支，同样要重启
        XCTAssertEqual(player.pollingInterval ?? -1, 0.25, accuracy: 1e-9)

        player.close()
        XCTAssertNil(player.pollingInterval, "close() 要停掉轮询")
    }

    /// 没在轮询时改间隔不该凭空把定时器开起来。
    func testChangingIntervalWhileNotPollingDoesNotStartTimer() {
        let player = SYPlayer()
        player.stateUpdateInterval = 0.2
        XCTAssertNil(player.pollingInterval)
    }

    /// 赋同一个值不该重启定时器。间隔相同时光看
    /// pollingInterval 分辨不出有没有重启过，所以这里看启动次数。
    func testAssigningTheSameIntervalDoesNotRestartTheTimer() {
        let player = SYPlayer()
        player.startPollingForTesting()
        let startsAfterFirst = player.pollingStartCount

        player.stateUpdateInterval = 0.1        // 与当前值相同
        XCTAssertEqual(player.pollingStartCount, startsAfterFirst, "值没变不该重启定时器")

        player.stateUpdateInterval = 0.2        // 真的变了
        XCTAssertEqual(player.pollingStartCount, startsAfterFirst + 1)

        // 钳位后与原值相同的情况：1/60 已经是下限，再设更小的值钳回同一个数。
        player.stateUpdateInterval = 1.0 / 60.0
        let startsAtFloor = player.pollingStartCount
        player.stateUpdateInterval = 0.001      // 钳回 1/60，与当前值相同
        XCTAssertEqual(player.pollingStartCount, startsAtFloor, "钳位后与原值相同也不该重启")

        player.close()
    }

    /// 新订阅者立刻拿到当前状态（register 里的那次 yield）。
    func testStateStreamDeliversCurrentStateImmediately() async {
        let player = SYPlayer()
        var iterator = player.stateStream.makeAsyncIterator()
        let first = await iterator.next()
        XCTAssertEqual(first?.playback, .idle)
    }

    /// delegate 拿到的每一次回调都在主线程。
    ///
    /// 用"打开一个不存在的文件"来制造状态变化：它不需要素材、不需要音频设备、
    /// 立刻失败（源打开那一步就返回），却真的走了一遍 open → publish(.failed) →
    /// delegate 的完整路径，包括跨 openQueue 回到主 actor 的那一跳。
    func testDelegateIsCalledOnMainThread() async {
        final class Recorder: SYPlayerDelegate {
            var changes = 0
            var failures = 0
            var sawOffMainThread = false
            func player(_ player: SYPlayer, didChange state: SYPlayerState) {
                if !Thread.isMainThread { sawOffMainThread = true }
                changes += 1
            }
            func player(_ player: SYPlayer, didFailWith error: SYPlayerError) {
                if !Thread.isMainThread { sawOffMainThread = true }
                failures += 1
            }
        }
        let player = SYPlayer()
        let recorder = Recorder()
        player.delegate = recorder

        let missing = URL(fileURLWithPath: "/tmp/syplayer-does-not-exist-\(UUID().uuidString).mp4")
        do {
            try await player.open(.file(missing), decoding: .software)
            XCTFail("打开不存在的文件不该成功")
        } catch {
            XCTAssertNotNil(error as? SYPlayerError)
        }

        XCTAssertGreaterThan(recorder.changes, 0)
        XCTAssertEqual(recorder.failures, 1)
        XCTAssertFalse(recorder.sawOffMainThread)
        XCTAssertEqual(player.state.hasMedia, false)
        XCTAssertNotNil(player.state.error)
    }

    /// 相等状态不重复投递：10Hz 的定时器在没打开媒体时每一拍
    /// 算出来的都是同一份 .idle，不挡住就是每秒 10 次空的 objectWillChange。
    func testIdenticalStateIsNotRepublished() {
        final class Recorder: SYPlayerDelegate {
            var changes = 0
            func player(_ player: SYPlayer, didChange state: SYPlayerState) { changes += 1 }
        }
        let player = SYPlayer()
        let recorder = Recorder()
        player.delegate = recorder

        // 全都停在 .idle：close() 与 pause() 各自都会走到 publish(.idle)。
        player.close()
        player.close()
        player.pause()
        player.play()

        XCTAssertEqual(recorder.changes, 0, "状态没变就不该投递")
    }

    /// 硬解可用性只是一个查询，不该因为没打开媒体就崩。Catalyst 上恒 false
    /// （FFmpeg 在 Catalyst 上禁用了 videotoolbox hwaccel），但这里不写死平台
    /// 结论——断言平台就是在测运行环境。
    func testHardwareDecodeAvailabilityIsQueryable() {
        _ = SYPlayer.isHardwareDecodeAvailable
    }
}

/// SwiftUI 表示层的挂接同步。
///
/// 直接调 `updateUIView` 是不行的：`UIViewRepresentableContext` 没有公开构造器，
/// 测试造不出 `Context`。所以 updateUIView 的全部逻辑被拎进了 internal 的
/// `syncAttachment(to:)`，这里测的就是那一份。
@MainActor
final class SYPlayerViewRepresentableTests: XCTestCase {

    func testSwappingThePlayerReattachesTheView() {
        let first = SYPlayer()
        let second = SYPlayer()
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))

        SYPlayerViewRepresentable(player: first).syncAttachment(to: view)
        XCTAssertTrue(view.attachedPlayer === first)
        XCTAssertTrue(first.attachedView === view)

        // SwiftUI 复用同一个 UIView、只重建表示层：makeUIView 不会再跑，
        // 全靠 updateUIView 把画面接到新播放器上。
        SYPlayerViewRepresentable(player: second).syncAttachment(to: view)
        XCTAssertTrue(view.attachedPlayer === second, "换播放器后画面必须跟着换，否则黑屏 + 串音")
        // **这才是关键**：旧播放器必须被摘干净。
        // 只改 view.attachedPlayer 是不够的——first 那一侧还记着这个视图，它的桥
        // 也还 retain 着同一个 CAMetalLayer，两个渲染器会往同一个 layer 上present。
        XCTAssertNil(first.attachedView, "旧播放器必须被摘掉，否则两个桥抢同一个 layer")
    }

    /// 直接用 attach(to:) 抢一个别的播放器正占着的视图，结果必须一样。
    func testAttachingToAViewOwnedByAnotherPlayerDetachesThatPlayer() {
        let first = SYPlayer()
        let second = SYPlayer()
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))

        first.attach(to: view)
        second.attach(to: view)

        XCTAssertNil(first.attachedView)
        XCTAssertTrue(second.attachedView === view)
        XCTAssertTrue(view.attachedPlayer === second)
    }

    /// 没换播放器时 update 是空操作（SwiftUI 每帧都可能调一次）。
    func testUpdateWithTheSamePlayerKeepsTheAttachment() {
        let player = SYPlayer()
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
        let representable = SYPlayerViewRepresentable(player: player)

        representable.syncAttachment(to: view)
        representable.syncAttachment(to: view)
        representable.syncAttachment(to: view)

        XCTAssertTrue(view.attachedPlayer === player)
    }
}

/// 回归：**播到结尾之后 seek 回去必须能重新播**。
///
/// 这一条需要真素材（测试 bundle 里的 sample.mp4，12 秒 640×360 h264 + aac），
/// 要跑起来需要音频设备、也需要 XCTest 里主 runloop 被泵到。
///
/// 为什么 seek 到 11.5 秒再 play：要验的是"能不能走到 .ended 并且能从 .ended
/// 回来"，不是"能不能连续播 12 秒"。这样两趟加起来也就两三秒。
@MainActor
final class SYPlayerEofRearmTests: XCTestCase {

    private final class EndRecorder: SYPlayerDelegate {
        var endCount = 0
        var sawOffMainThread = false
        func playerDidReachEnd(_ player: SYPlayer) {
            if !Thread.isMainThread { sawOffMainThread = true }
            endCount += 1
        }
    }

    private var sampleURL: URL? {
        Bundle(for: SYPlayerEofRearmTests.self).url(forResource: "sample", withExtension: "mp4")
    }

    /// 轮询而不是 expectation：SYPlayer 的状态本来就是轮询出来的（10Hz），
    /// 再套一层 expectation 只会多一层时序噪声。
    private func wait(timeout: TimeInterval, until predicate: () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return predicate()
    }

    func testSeekAfterEndLeavesEndedAndReportsEndOncePerPass() async throws {
        guard let sampleURL = sampleURL else {
            XCTFail("测试 bundle 里没有 sample.mp4——检查生成器的 resources_build_phase")
            return
        }
        let player = SYPlayer()
        let recorder = EndRecorder()
        player.delegate = recorder

        try await player.open(.file(sampleURL), decoding: .software)
        XCTAssertTrue(player.state.hasMedia)

        // 第一趟：走到结尾。
        player.seek(to: 11.5)
        player.play()
        let ended = await wait(timeout: 30) { player.state.isEnded }
        XCTAssertTrue(ended, "30 秒内没走到 .ended，当前 \(player.state.playback)")
        XCTAssertEqual(recorder.endCount, 1)

        // 第二趟：seek 回去。**修复前这里会失败**——桥的 eof_notified_ 从不在
        // seek 时清，快照里 eof 恒 YES，refresh() 立刻把 .ended 又贴回来。
        player.seek(to: 11.5)
        XCTAssertFalse(player.state.isEnded, "seek 之后必须退出 .ended（EOF 要重新武装）")

        player.play()
        let endedAgain = await wait(timeout: 30) { player.state.isEnded }
        XCTAssertTrue(endedAgain, "第二趟 30 秒内没走到 .ended，当前 \(player.state.playback)")
        XCTAssertEqual(recorder.endCount, 2, "每趟恰好报一次 playerDidReachEnd")
        XCTAssertFalse(recorder.sawOffMainThread)

        player.close()
    }
}

/// 双闸门（`isSessionLive` + 会话号）与错误暂存的覆盖。
///
/// 这套机制是整个任务里最绕的一处，而它原本一条用例都没有：`bridge.onError` 由
/// `init` 独占设置、`resources` 是私有的，用例根本没法触发它。所以加了一个
/// internal 缝 `simulateBridgeError(_:session:)`——它**直接调用真实的
/// handleError**，两道闸门一个不少，不是另写一份仿制品。
///
/// 三条用例分别对应两道闸门各自挡的那一半，以及"关着的闸门不许丢错误"。
@MainActor
final class SYPlayerSessionGateTests: XCTestCase {

    private final class FailureRecorder: SYPlayerDelegate {
        var failures: [SYPlayerError] = []
        func player(_ player: SYPlayer, didFailWith error: SYPlayerError) { failures.append(error) }
    }

    private var sampleURL: URL? {
        Bundle(for: SYPlayerSessionGateTests.self).url(forResource: "sample", withExtension: "mp4")
    }

    /// 闸门那一半：close() 之后到达的错误不许改状态。
    ///
    /// 这里**不能**只靠会话号——close() 把换号排在 openQueue 上、桥关完之后，
    /// 而本用例紧接着 close() 同步触发回调，那时号还没换、比对是相等的。
    /// 挡住它的只可能是同步置 false 的 isSessionLive。
    func testErrorArrivingAfterCloseLeavesStateIdle() async throws {
        guard let sampleURL = sampleURL else { XCTFail("缺 sample.mp4"); return }
        let player = SYPlayer()
        let recorder = FailureRecorder()
        player.delegate = recorder

        try await player.open(.file(sampleURL), decoding: .software)
        let sessionWhileOpen = player.currentSessionForTesting

        player.close()
        // 同步触发：此刻 openQueue 上的 close 还没跑完，会话号必然还没换。
        player.simulateBridgeError(.io, session: sessionWhileOpen)

        XCTAssertEqual(player.state.playback, .idle, "close() 之后的错误不该把状态冻在 .failed")
        XCTAssertNil(player.state.error)
        XCTAssertTrue(recorder.failures.isEmpty)
    }

    /// 会话号那一半：重新 open 成功之后，上一份素材的错误不许混进来。
    ///
    /// 这里 isSessionLive 已经是 true（新素材打开着），挡住它的只可能是会话号。
    func testErrorFromAPreviousSessionIsIgnoredAfterReopen() async throws {
        guard let sampleURL = sampleURL else { XCTFail("缺 sample.mp4"); return }
        let player = SYPlayer()
        let recorder = FailureRecorder()
        player.delegate = recorder

        try await player.open(.file(sampleURL), decoding: .software)
        let oldSession = player.currentSessionForTesting

        try await player.open(.file(sampleURL), decoding: .software)
        XCTAssertNotEqual(player.currentSessionForTesting, oldSession, "重新 open 必须换会话号")

        player.simulateBridgeError(.network, session: oldSession)

        XCTAssertNil(player.state.error, "上一份素材的错误不该污染新会话")
        XCTAssertTrue(recorder.failures.isEmpty)
        XCTAssertTrue(player.state.hasMedia)
        player.close()
    }

    /// 暂存那一条：闸门关着时到达的**本会话**错误必须在 open() 完成后浮出来。
    ///
    /// 复现真实窗口的办法：另起一个 Task 去 open，主 actor 这边自旋等会话号翻页
    /// （翻页发生在 openQueue 上、桥 open **之前**，此时闸门必然还关着），
    /// 然后**不经任何挂起**同步触发回调。闸门要想在这中间打开，必须让整个
    /// 桥 open（上百毫秒）挤进两次 Task.yield 之间，实际不可能；而且用例用
    /// hasPendingErrorForTesting 断言"确实走了暂存那条路"，万一没走到会直接红，
    /// 不会静悄悄地变成一条空跑的用例。
    func testErrorArrivingWhileGateClosedIsDeliveredAfterOpenCompletes() async throws {
        guard let sampleURL = sampleURL else { XCTFail("缺 sample.mp4"); return }
        let player = SYPlayer()
        let recorder = FailureRecorder()
        player.delegate = recorder

        let sessionBefore = player.currentSessionForTesting
        async let opening: Void = player.open(.file(sampleURL), decoding: .software)

        while player.currentSessionForTesting == sessionBefore {
            await Task.yield()
        }
        player.simulateBridgeError(.io, session: player.currentSessionForTesting)
        XCTAssertTrue(player.hasPendingErrorForTesting,
                      "闸门此刻应当还关着，这条错误应当被暂存（否则本用例没测到该测的路径）")

        try await opening

        XCTAssertFalse(player.hasPendingErrorForTesting, "open() 成功后必须把暂存补投掉")
        XCTAssertEqual(player.state.error, .io, "起播期的错误不能被永久丢掉")
        XCTAssertEqual(recorder.failures, [.io], "didFailWith 必须恰好投递一次")

        player.close()
    }

    /// 暂存不许跨会话诈尸：close() 之后残留的暂存，在下一次 open 成功时必须已被清掉。
    func testPendingErrorDoesNotSurviveIntoTheNextOpen() async throws {
        guard let sampleURL = sampleURL else { XCTFail("缺 sample.mp4"); return }
        let player = SYPlayer()
        let recorder = FailureRecorder()
        player.delegate = recorder

        // 未打开时闸门就是关着的，且号能对上——错误会被暂存。
        player.simulateBridgeError(.network, session: player.currentSessionForTesting)
        XCTAssertTrue(player.hasPendingErrorForTesting)

        try await player.open(.file(sampleURL), decoding: .software)

        XCTAssertNil(player.state.error, "上一轮残留的暂存不该在新的 open 成功时诈尸")
        XCTAssertTrue(recorder.failures.isEmpty)
        player.close()
    }
}
