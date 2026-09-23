// 公开音量 API 的夹取与独立性。
// 口径与 SYPlayerCacheConfiguration 同源（非有限值不得穿到桥下面去），
// 但**各写一份、不互相引用**。
import XCTest
@testable import SYPlayerKit
import SYPlayerKit_Private

final class SYPlayerVolumeTests: XCTestCase {
    @MainActor
    func testVolumeClampsToUnitRange() {
        let p = SYPlayer()
        p.volume = 2.0
        XCTAssertEqual(p.volume, 1.0)
        p.volume = -1.0
        XCTAssertEqual(p.volume, 0.0)
        p.volume = 0.5
        XCTAssertEqual(p.volume, 0.5)
    }

    @MainActor
    func testVolumeRejectsNonFiniteValues() {
        let p = SYPlayer()
        p.volume = 0.5
        p.volume = .nan
        XCTAssertEqual(p.volume, 0.0)
        p.volume = .infinity
        XCTAssertEqual(p.volume, 1.0)
        p.volume = -.infinity
        XCTAssertEqual(p.volume, 0.0)
    }

    @MainActor
    func testMuteDoesNotEraseVolume() {
        let p = SYPlayer()
        p.volume = 0.42
        p.isMuted = true
        XCTAssertEqual(p.volume, 0.42, "静音不该擦掉音量值")
        p.isMuted = false
        XCTAssertEqual(p.volume, 0.42)
    }
}

/// "跨 open 保留"与"运行中的 setter 真的落到了消费者"。
///
/// **这组用例读的是 `…InUseForTest`，不是 `SYPlayer.volume`**：后者的 getter 读
/// 桥 `PlayerCore` 上的持久副本，桥哪怕从来没把它下发给新建的 `TrackPlayer`，
/// `player.volume` 照样是 0.42——只断言它就是一种假绿。三条缝读的是
/// `player_`（消费者）自己手里的值，没有 `player_` 时返回 -1。
///
/// gravity 走的是公开路由（`SYPlayerView.videoGravity` → `SYPlayerVideoHost` →
/// `SYPlayer` → 桥），所以这里都是通过视图设 gravity，而不是直接
/// 摸桥——那样才测得到路由本身。
final class SYPlayerVolumePersistenceTests: XCTestCase {

    private var sampleURL: URL {
        let bundle = Bundle(for: SYPlayerVolumePersistenceTests.self)
        guard let url = bundle.url(forResource: "sample", withExtension: "mp4") else {
            fatalError("测试 bundle 里没有 sample.mp4——检查生成器的 resources_build_phase")
        }
        return url
    }

    @MainActor
    private func makeView() -> SYPlayerView {
        SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
    }

    /// open **之前**设的三个值，open 之后必须已经在新建的 TrackPlayer 手里；
    /// close 再 open 一次（又一个新 TrackPlayer），三者仍然是那三个值。
    @MainActor
    func testSettingsMadeBeforeOpenSurviveIntoEveryNewTrackPlayer() async throws {
        let p = SYPlayer()
        let view = makeView()
        p.attach(to: view)
        p.volume = 0.42
        p.isMuted = true
        view.videoGravity = .aspectFill

        // 还没有消费者：缝必须如实说"没有"，而不是回显持久副本。
        XCTAssertEqual(p.volumeInUseForTest, -1)
        XCTAssertEqual(p.mutedInUseForTest, -1)
        XCTAssertEqual(p.videoGravityInUseForTest, -1)

        try await p.open(.file(sampleURL), decoding: .software)
        XCTAssertEqual(p.volumeInUseForTest, 0.42, "open 之前设的音量没下发到新 TrackPlayer")
        XCTAssertEqual(p.mutedInUseForTest, 1, "open 之前设的静音没下发到新 TrackPlayer")
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFill.rawValue,
                       "open 之前设的 gravity 没下发到新 TrackPlayer")

        p.close()
        try await p.open(.file(sampleURL), decoding: .software)
        XCTAssertEqual(p.volumeInUseForTest, 0.42, "reopen 之后新 TrackPlayer 丢了音量")
        XCTAssertEqual(p.mutedInUseForTest, 1, "reopen 之后新 TrackPlayer 丢了静音")
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFill.rawValue,
                       "reopen 之后新 TrackPlayer 丢了 gravity")
        p.close()
    }

    /// 持久值必须在 sink **open 那一刻**就已落下，不能 open 之后再改。
    ///
    /// 读的是 `sinkGainAtOpenForTest`（AudioUnitSink 渲染线程的增益起点），不是
    /// `volumeInUseForTest`：桥若退回"create() 之后再 set_volume/set_muted"，后者照样
    /// 是 0.42 / 1，而 sink 是以 1.0 开的——开头约 15ms 近满音量。
    /// 期望值是字面量：静音 ⇒ 0；不静音 ⇒ Float(0.42)（sink 的增益是 float）。
    /// 第二次 open 验"每一次 open"而不只是第一次。
    @MainActor
    func testPersistedGainIsInTheSinkAtTheMomentItOpens() async throws {
        let p = SYPlayer()
        XCTAssertEqual(p.sinkGainAtOpenForTest, -1, "没打开时缝必须如实说没有")
        p.volume = 0.42
        p.isMuted = true
        try await p.open(.file(sampleURL), decoding: .software)
        XCTAssertEqual(p.sinkGainAtOpenForTest, 0.0,
                       "持久静音没在 sink open 之前落下——开头会漏出一段近满音量")
        p.close()

        p.isMuted = false
        try await p.open(.file(sampleURL), decoding: .software)
        XCTAssertEqual(p.sinkGainAtOpenForTest, Double(Float(0.42)),
                       "持久音量没在 sink open 之前落下——开头会从 1.0 斜坡下来")
        p.close()
    }

    /// 播放器已经开着时改三者：每一个都必须立刻落到当前的 TrackPlayer 上，
    /// 不能只写了持久副本、等下一次 open 才生效。
    @MainActor
    func testSettersWhileOpenReachTheCurrentTrackPlayer() async throws {
        let p = SYPlayer()
        let view = makeView()
        p.attach(to: view)
        try await p.open(.file(sampleURL), decoding: .software)
        // 默认值（桥把 PlayerCore 上的默认持久值作为 InitialSettings 传进 create()）。
        XCTAssertEqual(p.volumeInUseForTest, 1.0)
        XCTAssertEqual(p.mutedInUseForTest, 0)
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFit.rawValue)

        p.volume = 0.1
        XCTAssertEqual(p.volumeInUseForTest, 0.1, "运行中的 volume setter 没落到消费者")
        p.isMuted = true
        XCTAssertEqual(p.mutedInUseForTest, 1, "运行中的 isMuted setter 没落到消费者")
        view.videoGravity = .resize
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.resize.rawValue,
                       "运行中改视图的 gravity 没落到消费者")
        p.close()
    }

    /// `attach(to:)` 把新视图的 gravity 推给桥：换视图时 gravity
    /// 跟着新视图走。视图在 attach 之前设的 gravity（那时它没有 attachedPlayer，
    /// didSet 无处可去）也要在 attach 这一刻生效。
    @MainActor
    func testAttachPushesTheNewViewsGravity() async throws {
        let p = SYPlayer()
        try await p.open(.file(sampleURL), decoding: .software)
        let first = makeView()
        first.videoGravity = .resize
        p.attach(to: first)
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.resize.rawValue,
                       "attach 没把视图的 gravity 推给桥")

        let second = makeView()
        second.videoGravity = .aspectFill
        p.attach(to: second)
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFill.rawValue,
                       "换视图之后 gravity 没跟着新视图走")
        p.close()
    }

    /// 只有当前 attach 的那个视图能改 gravity：旧视图（已经被换掉的）
    /// 改自己的 gravity 不许影响新视图的画面。
    ///
    /// 两条路径都要测：一条走公开 API（旧视图的 attachedPlayer 已被 attach(to:)
    /// 清掉，didSet 本来就无处可去）；另一条直接调宿主方法、传一个**不是**当前
    /// 视图的 view——这才打得到 SYPlayer 里那道 `attachedView === view` 守卫，
    /// 第一条路径绕过了它。
    @MainActor
    func testOnlyTheCurrentlyAttachedViewCanChangeGravity() async throws {
        let p = SYPlayer()
        let old = makeView()
        let current = makeView()
        p.attach(to: old)
        p.attach(to: current)
        try await p.open(.file(sampleURL), decoding: .software)
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFit.rawValue)

        old.videoGravity = .resize
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFit.rawValue,
                       "被换掉的旧视图改 gravity 影响了当前画面")

        p.videoGravityDidChange(.aspectFill, from: old)
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFit.rawValue,
                       "宿主方法没挡住非当前视图的 gravity 变更")

        current.videoGravity = .aspectFill
        XCTAssertEqual(p.videoGravityInUseForTest, SypVideoGravity.aspectFill.rawValue)
        p.close()
    }

    /// `SYPlayerState.videoSize` 端到端：桥的 -snapshot 填 videoDisplayWidth/Height →
    /// SYPlayerRawSnapshot 搬运 → make(from:) 映射。映射那一步有纯函数用例，这一条
    /// 守的是前两跳（任何一跳漏接，公开状态里都恒为 nil）。sample.mp4 是 640×360、
    /// SAR 1:1、无旋转；close 之后回到 nil。
    @MainActor
    func testOpenReportsVideoDisplaySize() async throws {
        let p = SYPlayer()
        XCTAssertNil(p.state.videoSize)
        try await p.open(.file(sampleURL), decoding: .software)
        XCTAssertEqual(p.state.videoSize, CGSize(width: 640, height: 360))
        p.close()
        XCTAssertNil(p.state.videoSize)
    }
}

/// 桥里 `PresentTrackingRenderer`（装饰层）→ `MetalRenderer` 那一跳。
///
/// `-videoGravityInUseForTest` 读的是装饰层自己记下的值，记在转发**之前**：删掉装饰层
/// 里的 `real_->set_gravity(g)` / `real_->set_source_geometry(...)`，上面那组用例照样
/// 全绿，而这一跳正是主功能在生产上是否生效的唯一环节。
/// 这组用例经 `debugBlitPixelForTest` 回读 MetalRenderer 用**它自己手里**的几何与
/// gravity 画出来的像素（与生产 present() 同一个 encode_blit()），缝本身也穿过装饰层。
///
/// sample.mp4 是 640×360、SAR 1:1、不旋转；首帧在归一化坐标 (0.5,0.05)、(0.5,0.5)、
/// (0.2,0.5) 附近都是明显非黑的颜色（ffmpeg 缩到 8×4 看过）。
final class SYPlayerRendererForwardingTests: XCTestCase {

    private var sampleURL: URL {
        let bundle = Bundle(for: SYPlayerRendererForwardingTests.self)
        guard let url = bundle.url(forResource: "sample", withExtension: "mp4") else {
            fatalError("测试 bundle 里没有 sample.mp4——检查生成器的 resources_build_phase")
        }
        return url
    }

    /// 等到 predicate 成立或超时（同 SYPlayerLifecycleTests.wait：状态本来就是 10Hz 轮询）。
    @MainActor
    private func wait(timeout: TimeInterval, until predicate: @MainActor () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return predicate()
    }

    /// 与 tests/test_metal_renderer.cpp 的 is_black 同一阈值。
    private func isBlack(_ p: (b: UInt8, g: UInt8, r: UInt8)) -> Bool {
        max(p.b, p.g, p.r) <= 8
    }

    /// open → 等到已呈现过一帧（open 之后 SYPlayer 补了 pause()，暂停快速首帧仍会上屏）。
    @MainActor
    private func openAndWaitForFirstFrame(_ p: SYPlayer) async throws {
        try await p.open(.file(sampleURL), decoding: .software)
        let presented = await wait(timeout: 5) { p.state.statistics.presentedTime != nil }
        XCTAssertTrue(presented, "5s 内没有呈现任何帧，后面的像素断言没有意义")
    }

    @MainActor
    private func pixel(_ p: SYPlayer, _ w: Int32, _ h: Int32,
                       _ x: Double, _ y: Double,
                       file: StaticString = #filePath, line: UInt = #line) throws -> (b: UInt8, g: UInt8, r: UInt8) {
        try XCTUnwrap(p.debugBlitPixelForTest(width: w, height: h, x: x, y: y),
                      "离屏 blit 回读失败", file: file, line: line)
    }

    /// gravity 那一半：正方形目标里 16:9 的画面，AspectFit 时顶边中点是黑边；
    /// 经公开 API 切到 AspectFill 后画面铺满，同一点不再是黑的。
    @MainActor
    func testGravityChangeReachesMetalRenderer() async throws {
        let p = SYPlayer()
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
        p.attach(to: view)
        try await openAndWaitForFirstFrame(p)

        XCTAssertFalse(isBlack(try pixel(p, 64, 64, 0.5, 0.5)), "中心应当是画面，不是黑的")
        XCTAssertTrue(isBlack(try pixel(p, 64, 64, 0.5, 0.05)), "AspectFit：正方形目标顶边应是黑边")

        view.videoGravity = .aspectFill
        XCTAssertFalse(isBlack(try pixel(p, 64, 64, 0.5, 0.05)),
                       "切到 AspectFill 后顶边仍是黑边——gravity 没从装饰层转发到 MetalRenderer")
        p.close()
    }

    /// 几何那一半：64×32（2:1）目标 + AspectFit。不旋转时 16:9 的画面几乎占满宽度，
    /// (0.2, 0.5) 是画面；经装饰层下发 90° 旋转后画面变成竖的（9:16），左右出现宽黑边，
    /// 同一点变黑、中心仍是画面。
    @MainActor
    func testSourceGeometryReachesMetalRenderer() async throws {
        let p = SYPlayer()
        try await openAndWaitForFirstFrame(p)

        XCTAssertFalse(isBlack(try pixel(p, 64, 32, 0.2, 0.5)), "不旋转时 (0.2,0.5) 应当是画面")

        XCTAssertTrue(p.debugSetSourceGeometryForTest(sarNum: 1, sarDen: 1, rotationDeg: 90))
        XCTAssertTrue(isBlack(try pixel(p, 64, 32, 0.2, 0.5)),
                      "旋转 90° 后左侧应是黑边——几何没从装饰层转发到 MetalRenderer")
        XCTAssertTrue(isBlack(try pixel(p, 64, 32, 0.8, 0.5)), "旋转 90° 后右侧应是黑边")
        XCTAssertFalse(isBlack(try pixel(p, 64, 32, 0.5, 0.5)), "旋转后中心仍应是画面")
        p.close()
    }
}
