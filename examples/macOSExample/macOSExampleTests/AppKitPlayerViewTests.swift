// AppKitPlayerViewTests.swift — 原生 macOS 的 AppKit 版 SYPlayerView，经 SwiftPM
// 接入后在真实窗口里的行为。
//
// 视图离开窗口会解挂、回到窗口会重挂；drawableSize 由视图在 frame 变化与
// 进出窗口时同步为 bounds × backingScaleFactor。这里把视图放进一个离屏
// NSWindow，挂上正在播放的播放器，拿下再放回、再改 frame，实际钉住的是：
// 整个过程不崩；drawableSize 与 contentsScale 每一步都等于"点尺寸 × 窗口缩放比"；
// 播放器在这一串操作之后仍在推进。
//
// **解挂/重挂本身从公开状态上观察不到**：渲染器在没有输出 layer 时照样渲染
// 到内部纹理、present 照样成功，`statistics.presentedTime` 在视图离开窗口期间
// 也在前进（实测离窗 0.5 秒内 0.40 → 0.93），所以这里不对它做断言。
//
// 缩放比取窗口实际的 backingScaleFactor（Retina 屏为 2、普通屏为 1），
// 点尺寸是字面量；两者乘出来的就是期望的像素尺寸。
import AppKit
import SYPlayerKit
import XCTest

@MainActor
final class AppKitPlayerViewTests: XCTestCase {

    func testAppKitPlayerViewSurvivesWindowReattach() async throws {
        let url = try XCTUnwrap(Bundle.main.url(forResource: "sample", withExtension: "mp4"))

        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 600),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 800, height: 600))
        window.contentView = container

        let scale = window.backingScaleFactor
        XCTAssertTrue(scale == 1 || scale == 2, "意外的缩放比 \(scale)")

        let view = SYPlayerView(frame: NSRect(x: 0, y: 0, width: 320, height: 180))
        container.addSubview(view)
        XCTAssertEqual(view.playerLayer.contentsScale, scale)
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 320 * scale, height: 180 * scale))

        let player = SYPlayer()
        player.attach(to: view)
        try await player.open(.file(url),
                              decoding: SYPlayer.isHardwareDecodeAvailable ? .hardwarePreferred : .software)
        player.play()

        // 拿下：视图离开窗口（框架在这里解挂输出 layer），不该崩、也不该丢掉已有的 drawableSize。
        view.removeFromSuperview()
        XCTAssertNil(view.window)
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 320 * scale, height: 180 * scale))

        // 离开窗口期间改 frame：drawableSize 按 layer 当前的 contentsScale 更新。
        view.frame = NSRect(x: 0, y: 0, width: 400, height: 300)
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 400 * scale, height: 300 * scale))

        // 放回（框架在这里重挂输出 layer）：drawableSize 仍按窗口缩放比。
        container.addSubview(view)
        XCTAssertNotNil(view.window)
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 400 * scale, height: 300 * scale))

        // 在窗口里再改一次 frame。
        view.setFrameSize(NSSize(width: 640, height: 360))
        XCTAssertEqual(view.playerLayer.drawableSize, CGSize(width: 640 * scale, height: 360 * scale))

        // 这一串操作之后播放器仍在走。
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline, player.state.position <= 0 {
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTAssertGreaterThan(player.state.position, 0, "移出/放回窗口之后播放位置没有推进：\(player.state)")

        player.close()
        view.removeFromSuperview()
        window.close()
    }
}
