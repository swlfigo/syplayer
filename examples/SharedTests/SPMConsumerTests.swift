// SPMConsumerTests.swift — 经 SwiftPM 接入 SYPlayerKit 之后，接入方视角的冒烟
// 用例。iOS 与原生 macOS 两个示例的测试目标共用这一份。
//
// 测试 bundle 挂在示例 App 里跑（TEST_HOST），所以：
//   · `Bundle.main` 就是示例 App——样片是否被正确打进 App 资源，这里一并验证；
//   · SYPlayerKit.framework 与 SYFFmpeg.framework 是 SwiftPM 内嵌进 App 的那两份，
//     链接或内嵌出错会在进程启动时就崩，而不是等到某条断言。
//
// 只用公开 API（`import SYPlayerKit`，不 @testable、不碰私有模块）：接入方拿到
// 的就是这些，用例能编过本身就在钉公开面。
//
// 不断言出画：模拟器上的 Metal 与硬解受限，出不出画不是接入是否成功的判据；
// 断言只看打开、状态与时间推进。音频设备起不来时播放器会切到系统时钟继续走，
// 所以也不断言时钟来源。
import CoreGraphics
import Foundation
import SYPlayerKit
import XCTest

@MainActor
final class SPMConsumerTests: XCTestCase {

    /// 等到 predicate 成立或超时。SYPlayer 的状态本身是 10Hz 轮询出来的，
    /// 这里按 50ms 轮询足够。
    private func wait(timeout: TimeInterval,
                      until predicate: @MainActor () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return predicate()
    }

    /// 样片 demo/shared/Resources/sample.mp4 的实测参数（ffprobe）：
    /// 视频 640×360、SAR 1:1、无旋转，时长 12.000000 秒。
    /// SAR 1:1 且无旋转，所以显示尺寸就是编码尺寸 640×360。
    func testPlaysBundledSampleThroughSPM() async throws {
        let url = try XCTUnwrap(Bundle.main.url(forResource: "sample", withExtension: "mp4"),
                                "示例 App 的 bundle 里没有 sample.mp4——检查示例工程的资源阶段")

        let player = SYPlayer()
        try await player.open(.file(url),
                              decoding: SYPlayer.isHardwareDecodeAvailable ? .hardwarePreferred : .software)

        let ready = await wait(timeout: 10) {
            player.state.hasMedia && player.state.videoSize != nil
        }
        XCTAssertTrue(ready, "10 秒内没有拿到 hasMedia 与 videoSize，最后状态：\(player.state)")
        XCTAssertEqual(player.state.videoSize, CGSize(width: 640, height: 360))
        XCTAssertTrue(player.state.hasVideo)
        XCTAssertEqual(player.state.duration ?? -1, 12.0, accuracy: 0.1)

        // open 之后停在暂停态；起点取 play() 之前的位置（open 与首次暂停之间泵线程
        // 可能已经走过一点点，所以不假定它恰好是 0），要求真的往前走过 0.2 秒以上，
        // 而不是只看"大于 0"。
        let start = player.state.position
        player.play()
        let advanced = await wait(timeout: 5) { player.state.position > start + 0.2 }
        XCTAssertTrue(advanced, "play() 之后 5 秒内播放位置没有从 \(start) 推进 0.2 秒以上，最后状态：\(player.state)")

        player.close()
        XCTAssertFalse(player.state.hasMedia, "close() 之后应回到未打开状态")
    }

    func testPublicAPISurfaceIsReachable() {
        addTeardownBlock { SYPlayerNetwork.maximumDownloadRate = nil }

        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("spm-consumer-cache-\(UUID().uuidString)", isDirectory: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: dir) }

        let cache = SYPlayerCacheConfiguration(directory: dir, maxBytes: 8 * 1024 * 1024,
                                               minFreeSpaceBytes: 0, timeToLive: 60)
        XCTAssertEqual(cache.maxBytes, 8_388_608)
        XCTAssertEqual(cache.timeToLive, 60)

        let preloader = SYPlayerPreloader(cache: cache)
        XCTAssertEqual(preloader.cacheConfiguration, cache)
        XCTAssertEqual(preloader.statistics.entries, 0)

        XCTAssertNil(SYPlayerNetwork.maximumDownloadRate, "默认不限速")
        SYPlayerNetwork.maximumDownloadRate = 1_000_000
        XCTAssertEqual(SYPlayerNetwork.maximumDownloadRate, 1_000_000)
        SYPlayerNetwork.maximumDownloadRate = nil
        XCTAssertNil(SYPlayerNetwork.maximumDownloadRate)

        // 端口 1 上没有服务：连接立即被拒，不打外网。preconnect 立即返回、尽力而为。
        SYPlayerNetwork.preconnect(URL(string: "http://127.0.0.1:1/")!)

        // 视图与 SwiftUI 包装在两个平台上同名、同构造方式。
        let player = SYPlayer()
        let view = SYPlayerView(frame: CGRect(x: 0, y: 0, width: 160, height: 90))
        player.attach(to: view)
        view.videoGravity = .aspectFill
        XCTAssertEqual(view.videoGravity, .aspectFill)
        _ = SYPlayerViewRepresentable(player: player)
        player.detachVideo()
        XCTAssertEqual(player.state, SYPlayerState.idle)
    }
}
