// SYPlayerView+AppKit.swift — 原生 macOS（AppKit）的视频视图。
//
// 与 UIKit 版（SYPlayerView.swift）同名、公开成员一致：接入方在 iOS / Mac
// Catalyst 与原生 macOS 上写同样的 `SYPlayerView(frame:)`、`playerLayer`、
// `videoGravity`、`player.attach(to:)`。Mac Catalyst 走的是 UIKit 版，这里用
// `!targetEnvironment(macCatalyst)` 排除。
//
// 与 UIKit 版的对应关系：
//   · `layerClass`         ↔ `makeBackingLayer()`：返回自己持有的 SYPlayerLayer，
//                            视图是 layer-backed（wantsLayer = true），由 AppKit 管
//                            这个 layer 的 frame 与层级。
//   · `didMoveToWindow`    ↔ `viewDidMoveToWindow`：离开窗口解挂、回到窗口重挂。
//   · `window.screen.scale`↔ `window.backingScaleFactor`：写进 contentsScale，
//                            drawableSize = bounds × contentsScale 由 SYPlayerLayer 算。
//   · AppKit 没有 UIKit 那样可靠的"尺寸变了就 layoutSublayers"时机，所以
//     `layout()`、`setFrameSize(_:)` 与 `viewDidChangeBackingProperties()`（换到
//     不同缩放比的屏幕）三处都显式同步一次 drawableSize。
#if os(macOS) && !targetEnvironment(macCatalyst)
import AppKit

public final class SYPlayerView: NSView {
    /// 在 super.init 之前就建好，保证 `playerLayer` 在任何时刻都拿得到同一个对象，
    /// 不依赖 AppKit 何时调用 makeBackingLayer()。
    private let hostedLayer = SYPlayerLayer()

    /// 视频上屏用的 layer。与 UIKit 版一样是视图的 backing layer 本身。
    public var playerLayer: SYPlayerLayer {
        hostedLayer
    }

    /// 画面填充方式，默认 `.aspectFit`。可以在任意时刻改，下一帧生效（已经上屏的
    /// 那一帧不重画）。
    ///
    /// 视图不持有桥：变化经 `attachedPlayer`（`SYPlayerVideoHost`）
    /// 交给播放器，由它写桥。还没 attach 时改了也不丢——`SYPlayer.attach(to:)`
    /// 会在挂上的那一刻把视图当前的值推给桥；换视图时 gravity 跟着新视图走。
    public var videoGravity: SYPlayerVideoGravity = .aspectFit {
        didSet {
            attachedPlayer?.videoGravityDidChange(videoGravity, from: self)
        }
    }

    /// 由 SYPlayer.attach(to:) 填，attach 到别的视图或 detachVideo() 时清。
    weak var attachedPlayer: SYPlayerVideoHost?

    public override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        configure()
    }

    public required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        // 先设 wantsLayer，AppKit 才会调 makeBackingLayer() 把 hostedLayer 装成
        // backing layer。背景色写在 layer 上：NSView 没有 backgroundColor。
        wantsLayer = true
        hostedLayer.backgroundColor = NSColor.black.cgColor
    }

    public override func makeBackingLayer() -> CALayer {
        hostedLayer
    }

    /// 视图离开窗口时解挂、回到窗口时重挂。对称的一对而不是单向解挂：窗口里的
    /// 标签页切换会把非当前页的视图移出窗口，只解挂不重挂的话切回来就永远黑屏。
    /// 解挂本身仍然必要：离屏的 CAMetalLayer 拿不到 drawable，继续挂着只会让
    /// 渲染器每帧白跑一次。
    public override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil {
            attachedPlayer?.videoViewDidLeaveWindow(self)
        } else {
            syncContentsScale()
            playerLayer.syncDrawableSize()
            attachedPlayer?.videoViewDidEnterWindow(self)
        }
    }

    public override func layout() {
        super.layout()
        playerLayer.syncDrawableSize()
    }

    public override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        playerLayer.syncDrawableSize()
    }

    /// 窗口被拖到缩放比不同的屏幕上时触发：contentsScale 跟着 backingScaleFactor
    /// 走，drawableSize 随之重算，否则 Retina ↔ 非 Retina 切换后画面发糊或裁切。
    public override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        syncContentsScale()
        playerLayer.syncDrawableSize()
    }

    private func syncContentsScale() {
        guard let scale = window?.backingScaleFactor, scale > 0 else { return }
        if playerLayer.contentsScale != scale { playerLayer.contentsScale = scale }
    }
}
#endif
