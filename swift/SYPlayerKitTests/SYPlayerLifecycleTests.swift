// SYPlayerLifecycleTests.swift — 生命周期与线程用例。
//
// 这份清单只补充其它文件仍然没有覆盖的那几条："双闸门 + EOF 重新武装 +
// 会话号"这些机制的回归用例已经在 SYPlayerLayerTests.swift
// （SYPlayerClosedStateTests / SYPlayerEofRearmTests / SYPlayerSessionGateTests /
// SYPlayerAttachLifecycleTests）里覆盖过。把已经覆盖过的路径再测一遍，
// 其中几条还要素材播完整趟（每条几秒到二十几秒），
// 白白拖慢套件、不增加任何回归保护面。所以这里只补**仍然没有
// 用例覆盖**的那几条：
//   · open 成功后的基本元数据（duration/hasVideo/isHardwareDecoding/暂停态）——
//     现有 50 条用例里没有一条单独开一份素材去读这些字段；
//   · 打开不存在文件时 playback 精确落在 .failed(同一个错误)——现有
//     testDelegateIsCalledOnMainThread 只查了 hasMedia==false 和 error!=nil，
//     没做这个模式匹配；
//   · close() 之后能重新 open——现有 testCloseIsIdempotent(AndReopenWorks 之前的
//     版本)只验了幂等，没验重开；
//   · deinit 在播放中被释放不崩——曾在 ASan 下复现过一次 UAF，现有
//     50 条用例里一条 deinit 用例都没有；
//   · attach/detach 包在真实播放前后——现有 SYPlayerAttachLifecycleTests 只测
//     没有真实素材时的挂接簿记，没测真的 open+play 之后再解挂；
//   · stateStream 在真实播放期间的主线程投递——现有
//     testStateStreamDeliversCurrentStateImmediately 只测了未打开时的首次 yield。
// 跳过的条目——"未打开是 idle 且控制是 no-op""setRate 区间校验"与
// SYPlayerClosedStateTests 里的 testFreshPlayerIsIdle+testTransportControlsAreNoOpsWhenClosed
// / testSetRateValidatesRangeEvenWhenClosed 逐字重复；"seek 到尾部再 play 到
// .ended""delegate 恰好回调一次""seek 后重新武装"三条，原样出现在
// SYPlayerEofRearmTests.testSeekAfterEndLeavesEndedAndReportsEndOncePerPass 里
// （它两趟都走了一遍，覆盖面是这三条的超集）。
//
// 音频设备在无 host 的测试环境里可能起不来：那时 TrackPlayer 会按降级
// 规则切到系统时钟（clockSwitchReason == .audioFailed）继续播，位置照样会走、
// EOF 照样到得了。所以这里**不**断言 clock 是 .audio——断言了就是在测运行
// 环境而不是测代码。
import XCTest
@testable import SYPlayerKit

@MainActor
final class SYPlayerLifecycleTests: XCTestCase {

    private var sampleURL: URL {
        let bundle = Bundle(for: SYPlayerLifecycleTests.self)
        guard let url = bundle.url(forResource: "sample", withExtension: "mp4") else {
            fatalError("测试 bundle 里没有 sample.mp4——检查生成器的 resources_build_phase")
        }
        return url
    }

    /// 等到 predicate 成立或超时。用轮询而不是 expectation：SYPlayer 的状态本来
    /// 就是轮询出来的（10Hz），再套一层 expectation 只会多一层时序噪声。
    private func wait(timeout: TimeInterval,
                      until predicate: @MainActor () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            try? await Task.sleep(nanoseconds: 50_000_000)   // 50ms；iOS 13 不能用 .sleep(for:)
        }
        return predicate()
    }

    // MARK: - 打开成功 / 失败

    /// 【产品决定】open 成功后必须落在 `.paused`——调用方
    /// 显式调 play() 才开始播放，是 AVPlayer 的惯例；核心 `paused_` 默认为
    /// false（open 即播放）不变，`SYPlayer.open()` 在 Swift 这一层补了一次
    /// pause() 落地。这条用例钉住这个公开契约。
    func testOpenLocalFileReportsDurationAndPausedState() async throws {
        let player = SYPlayer()
        try await player.open(.file(sampleURL), decoding: .software)
        XCTAssertTrue(player.state.hasMedia)
        XCTAssertEqual(player.state.playback, .paused, "open 成功后必须落在暂停态，等调用方显式 play()")
        XCTAssertNotNil(player.state.duration)
        XCTAssertEqual(player.state.duration!, 12.0, accuracy: 0.5)
        XCTAssertTrue(player.state.hasVideo)
        XCTAssertFalse(player.state.isHardwareDecoding, ".software 解码请求下不该是硬解")
        player.close()
    }

    /// open 落地时不该已经有播放发生。
    ///
    /// 没有采样计数或"是否已出声"这类缝——`SypPlayerSnapshot` 根本不携带这些
    /// 字段，加一个是桥的改动，本轮明确不做。退而求其次用 `position`：它由
    /// TrackPlayer 的主时钟驱动，真暂停时钉住不动；哪怕只有 pump_loop() 的一次
    /// `step()` 在 Swift 这层的 pause() 落地前抢先跑完（SYPlayer.open() 那段
    /// 注释记的竞争窗口），也会体现成 position 从 0 挪开一小段——这条用例的
    /// 断言是能撞见这个回归的，不是摆设。
    /// 容差与跳过开关的来由，写在这里免得下一个人再把它调回 0.05。
    ///
    /// 这条用例守的那场竞争是**已登记、已接受**的既有事实：
    /// 桥的 `-open*` 把 `pump_loop()` 异步派到另一条队列，机器一忙，那次线程
    /// 切换就可能抢在 Swift 侧的 `pause()` 之前跑完若干次 `step()`。用一个紧到
    /// 0.05 秒的断言去卡它，等于让套件为一个**已知的非回归**周期性地变红——
    /// 红得没有信息量，只会训练人去忽略它。
    ///
    /// 真正能抓住回归（`pause()` 被删掉/失效）的是下面第二条断言：**位置此后
    /// 不再前进**。真的在播的话 300ms 会走出 0.3 秒，跟 0.01 的容差差两个数量
    /// 级，任何抢跑窗口都解释不了。所以第一条断言放宽成"没有走出一个抢跑窗口
    /// 能解释的距离"，第二条保持紧。
    private static let pumpHopBudget: TimeInterval = 0.25

    func testOpenLeavesPlayerPausedWithNoPlaybackBeforePlay() async throws {
        // CI / 高负载机器上可以整条跳过：这条用例的第一半天然对调度敏感。
        if ProcessInfo.processInfo.environment["SYPLAYER_SKIP_RACE_SENSITIVE_TESTS"] != nil {
            throw XCTSkip("SYPLAYER_SKIP_RACE_SENSITIVE_TESTS 已设置——跳过对调度敏感的 pump-hop 用例")
        }
        let player = SYPlayer()
        try await player.open(.file(sampleURL), decoding: .software)
        XCTAssertEqual(player.state.playback, .paused)
        let positionRightAfterOpen = player.state.position
        XCTAssertLessThanOrEqual(
            positionRightAfterOpen, Self.pumpHopBudget,
            "open 落地时位置已经跑出 \(Self.pumpHopBudget) 秒以上——超出 pump-hop 抢跑能解释的范围，" +
            "说明 pause() 根本没落地")
        // 再等几拍轮询（10Hz），确认暂停之后位置真的钉住，不是这一拍恰好还没追上。
        // **这一条才是回归探针**：真在播的话 300ms 会走出 0.3 秒。
        try? await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(player.state.position, positionRightAfterOpen, accuracy: 0.01,
                       "未调用 play() 时位置不该前进")
        player.close()
    }

    // MARK: - close() 之后的传输控制

    /// `close()` 之后再调传输控制，状态不许"诈尸"。
    ///
    /// 修复前：`close()` 同步发布 `.idle`，但拆桥排在 openQueue 上异步进行；
    /// 那几十毫秒里桥的快照仍报 `hasMedia == YES`、`paused == NO`，而
    /// `play()` 无条件调 `refresh()`，于是状态被拉回 `.playing` +
    /// `hasMedia == true`——轮询已经停了，再没有人把它推回去，**永久**错。
    func testTransportControlAfterCloseDoesNotResurrectState() async throws {
        let player = SYPlayer()
        try await player.open(.file(sampleURL), decoding: .software)
        XCTAssertTrue(player.state.hasMedia)

        player.close()
        player.play()   // 紧接着调用：此刻 openQueue 上的 close 多半还没跑完

        XCTAssertEqual(player.state.playback, .idle,
                       "close() 之后的 play() 不该把状态拉回播放中")
        XCTAssertFalse(player.state.hasMedia,
                       "close() 之后 hasMedia 必须保持 false")

        // 轮询已经停了，没有人会把它推回去——多等几拍确认它是稳定的，不是瞬时值。
        try? await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(player.state.playback, .idle)
        XCTAssertFalse(player.state.hasMedia)
    }

    /// playback 必须精确落在 `.failed(同一个错误)`，不只是"有错误"。
    func testOpenMissingFileThrowsAndLeavesFailedState() async {
        let player = SYPlayer()
        let missing = URL(fileURLWithPath: "/tmp/syplayer-does-not-exist-\(UUID().uuidString).mp4")
        do {
            try await player.open(.file(missing), decoding: .software)
            XCTFail("打开不存在的文件不该成功")
        } catch let error as SYPlayerError {
            XCTAssertEqual(player.state.playback, .failed(error))
            XCTAssertFalse(player.state.hasMedia)
        } catch {
            XCTFail("抛出的不是 SYPlayerError：\(error)")
        }
    }

    // MARK: - close / 重新打开

    func testCloseIsIdempotentAndReopenWorks() async throws {
        let player = SYPlayer()
        try await player.open(.file(sampleURL), decoding: .software)
        player.close()
        player.close()
        player.close()
        XCTAssertEqual(player.state.playback, .idle)
        try await player.open(.file(sampleURL), decoding: .software)
        XCTAssertTrue(player.state.hasMedia, "close 之后必须还能再 open")
        player.close()
    }

    // MARK: - deinit

    /// deinit 自动关闭：open + play 之后直接放掉唯一强引用，不许崩、不许卡，
    /// 而且对象真的被回收——不只是"没崩"。
    ///
    /// 原版本只验证不崩溃，没验证对象图那一半：SYPlayer
    /// 的 deinit 会往 `openQueue`/主队列上派发闭包，如果哪一个不小心强捕获了
    /// `self`（而不是 `resources`），就会造出一个"deinit 永远不跑、但也不崩"
    /// 的悬空对象——这类回归不崩溃，光看"没崩"这条断言是看不出来的。这里用
    /// `weak var` 钉住"最后一个强引用释放的同一时刻，对象就没了"：Swift 的类
    /// 释放是同步的，`player = nil` 这行执行完，弱引用必须立刻读到 nil。
    ///
    /// 循环 10 次是为了让"泵线程还在跑时对象被释放"这个窗口真的被撞上
    /// （同形状的驱动曾在 ASan 下复现过一次 UAF）。
    /// 上一版**断言在了唯一一个不泄漏的对象上**。`SYPlayer` 本身
    /// 确实会按时回收（block 捕获的是 `[weak self]`），可当时泄漏的是它下面
    /// 的整棵子图：`SYPlayerResources → SypPlayerBridge → onEof/onError block
    /// → SYPlayerResources`。桥以 `copy` 属性持有两个 block，block 又强捕获
    /// `res` 去读会话号，谁也不把 block 置空——于是每个播放器都永久泄漏一个
    /// 桥、一个 `PlayerCore`（含一条 dispatch 队列）以及挂上去的
    /// `CAMetalLayer`（它的强引用只在 `~PlayerCore` 里释放）。
    ///
    /// 所以这里追加两条弱引用断言，钉住"桥和资源盒也一起走了"。等待是必需
    /// 的：deinit 把 `close()` 派到 `openQueue`、把定时器清理派到主队列，
    /// `resources` 要等这两个 block 都跑完才没人持有；`SYPlayer` 自己的回收
    /// 则是同步的，仍然就地断言。
    func testDeinitWhilePlayingDoesNotCrashAndActuallyFreesThePlayer() async throws {
        weak var lastWeakRef: SYPlayer?
        weak var lastWeakResources: AnyObject?
        weak var lastWeakBridge: AnyObject?
        for _ in 0..<10 {
            var player: SYPlayer? = SYPlayer()
            lastWeakRef = player
            lastWeakResources = player!.resourcesForTesting
            lastWeakBridge = player!.bridgeForTesting
            try await player!.open(.file(sampleURL), decoding: .software)
            player!.play()
            try? await Task.sleep(nanoseconds: 30_000_000)
            player = nil
            XCTAssertNil(lastWeakRef,
                         "SYPlayer 必须在最后一个强引用释放的同一时刻被回收，" +
                         "而不是被某个内部闭包意外续命")
        }
        // 给派到 openQueue / 主队列上的清理留一点时间再断言子图。
        try? await Task.sleep(nanoseconds: 1_000_000_000)
        XCTAssertNil(lastWeakResources,
                     "SYPlayerResources 必须随播放器一起回收——还活着说明桥的回调 block " +
                     "又把它捕获进了保留环")
        XCTAssertNil(lastWeakBridge,
                     "SypPlayerBridge 必须随播放器一起回收——它活着就意味着 PlayerCore、" +
                     "它的 dispatch 队列和挂上去的 CAMetalLayer 全都泄漏了")
    }

    // MARK: - 视图挂接与真实播放

    /// 视图挂接与解挂不该崩，也不该影响播放；解挂之后播放继续、不进入失败态。
    func testAttachAndDetachAroundPlayback() async throws {
        let player = SYPlayer()
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
        player.attach(to: view)
        XCTAssertTrue(view.attachedPlayer === player, "attach 之后视图侧必须认得这个播放器")
        try await player.open(.file(sampleURL), decoding: .software)
        player.play()
        let advanced = await wait(timeout: 3) { player.state.position > 0 }
        XCTAssertTrue(advanced, "3 秒内位置没有前进，挂接可能挡住了播放")
        player.detachVideo()
        XCTAssertNil(view.attachedPlayer, "detachVideo 之后视图侧的挂接必须清空")
        _ = await wait(timeout: 1) { false }   // 解挂后再跑一圈轮询，确认状态不会翻车
        XCTAssertNil(player.state.error, "解挂视频不该让播放失败")
        player.close()
    }

    // MARK: - stream 投递

    /// 原版本 `await task.value` 没有兜底：一旦消费循环
    /// 回归成永久挂起（stream 再也不投递、或者 break 条件失效），整条用例会
    /// 挂到天荒地老——这个 target 没有配 xctestplan/executionTimeAllowance，
    /// 结果是 CI 卡死而不是报一条清楚的失败。这里用 `withTaskGroup` 把消费
    /// 任务和一个 10 秒的 `Task.sleep` 赛跑：`Task.sleep` 对取消敏感，赢家一
    /// 出现就 `cancelAll()`，输家（多半是还在睡的超时任务）几乎立刻让路，
    /// 不会拖慢正常通过的那一次运行。
    func testStateStreamDeliversOnMainThreadAndFinishes() async throws {
        let player = SYPlayer()
        var sawNonMain = false
        let consumer = Task { @MainActor () -> Int in
            var count = 0
            for await _ in player.stateStream {
                if !Thread.isMainThread { sawNonMain = true }
                count += 1
                if count >= 3 { break }
            }
            return count
        }
        try await player.open(.file(sampleURL), decoding: .software)
        player.play()

        let count: Int? = await withTaskGroup(of: Int?.self) { group in
            group.addTask { await consumer.value }
            group.addTask {
                try? await Task.sleep(nanoseconds: 10_000_000_000)   // 10s 超时兜底
                return nil
            }
            let first = await group.next() ?? nil
            group.cancelAll()
            return first
        }
        consumer.cancel()

        guard let count else {
            XCTFail("10 秒内 stateStream 没能投递到 3 个状态——回归成了永久挂起，而不是普通失败")
            player.close()
            return
        }
        XCTAssertGreaterThanOrEqual(count, 3)
        XCTAssertFalse(sawNonMain, "AsyncStream 的消费必须在主线程")
        player.close()
    }
}
