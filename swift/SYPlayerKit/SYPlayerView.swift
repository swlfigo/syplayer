// SYPlayerView.swift — UIKit 侧的视频视图。业务只需要把它摆进视图层级，
// 再 player.attach(to:) 一次；"必须主线程配置 CAMetalLayer"与"必须同步
// drawableSize"两条隐式约束都已经收进 SYPlayerLayer。
//
// 本文件里的 SYPlayerVideoGravity 与 SYPlayerVideoHost 与平台无关，两套视图
// （UIKit 版在本文件，原生 macOS 的 AppKit 版在 SYPlayerView+AppKit.swift）
// 共用；只有 SYPlayerView 类本身按平台分开定义，类名与公开成员保持一致，
// 接入方在两个平台上写同样的代码。
import Foundation
#if canImport(UIKit)
import UIKit
#endif

/// 画面填充方式。三种都由 MetalRenderer 的 blit pass 实现，参数由
/// src/media/video_geometry.h 的 blit_transform() 统一算出。
public enum SYPlayerVideoGravity: Equatable, Sendable {
    /// 等比缩放、完整显示，留黑边（默认）。
    case aspectFit
    /// 等比缩放、铺满视图，超出部分居中裁掉。
    case aspectFill
    /// 拉伸铺满视图，不保持宽高比。
    case resize
}

/// SYPlayer 用它接收视图的窗口进出事件。不直接写成 `weak var player: SYPlayer?`
/// 是为了让本文件不依赖 SYPlayer（编译顺序上视图先于播放器成立），也让
/// 视图层可以被单独测试。
///
/// **internal**：唯一的实现方是框架自己的 SYPlayer，唯一的
/// 调用方是 SYPlayerView 的窗口进出回调（UIKit 的 didMoveToWindow / AppKit 的
/// viewDidMoveToWindow）与 SYPlayerViewRepresentable 的 dismantle，都在框架内；持有它的 `attachedPlayer` 本来就是 internal，公开这个协议
/// 只会让业务侧多看见一对"不该自己调"的方法（调错了就是画面挂错渲染器）。
///
/// **@MainActor**：这两个方法都会去动 CAMetalLayer 的挂接，本来就只能在主线程；
/// 调用点（窗口进出回调与 dismantle）也都在主线程。不标的话
/// @MainActor 的 SYPlayer 去实现它就是一次跨隔离的越界（Swift 6 语言模式下直接
/// 报错），而用 nonisolated 去凑只是把越界藏起来。
@MainActor
protocol SYPlayerVideoHost: AnyObject {
    func videoViewDidEnterWindow(_ view: SYPlayerView)
    func videoViewDidLeaveWindow(_ view: SYPlayerView)

    /// 视图的 `videoGravity` 变了。视图不持有桥，
    /// 只能经这个协议把变化交给播放器；实现方只在 `view` 正是自己当前 attach
    /// 的那个视图时才生效——防止一个已经被换掉的旧视图改 gravity 影响新视图。
    func videoGravityDidChange(_ gravity: SYPlayerVideoGravity, from view: SYPlayerView)

    /// 解挂当前视频输出。放进协议是为了让 `SYPlayer.attach(to:)` 能把**别的**
    /// 播放器从这个视图上摘干净：它只拿得到
    /// `view.attachedPlayer` 这个协议引用，而"摘干净"必须由那个播放器自己做
    /// （要清它的 attachedView，也要让它的桥松开这个 layer）。
    func detachVideo()
}

#if canImport(UIKit)
public final class SYPlayerView: UIView {
    public override class var layerClass: AnyClass { SYPlayerLayer.self }

    public var playerLayer: SYPlayerLayer {
        // layerClass 已经钉死了类型，这个强制转换不可能失败。
        layer as! SYPlayerLayer
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

    public override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
    }

    public required init?(coder: NSCoder) {
        super.init(coder: coder)
        backgroundColor = .black
    }

    /// 视图离开窗口时解挂、回到窗口时重挂。
    ///
    /// 原始设计写的是"detachVideo() 或 view 的 willMove(toWindow: nil) 时解挂"，
    /// 这里做成**对称的一对**而不是单向解挂：UITabBarController 切页会把非当前
    /// 页的视图移出窗口，只解挂不重挂的话切回来就永远黑屏——而 demo 的验收
    /// 恰恰要求 UIKit 页与 SwiftUI 页并存。解挂本身仍然必要：离屏的
    /// CAMetalLayer 拿不到 drawable，继续挂着只会让渲染器每帧白跑一次。
    public override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil {
            attachedPlayer?.videoViewDidLeaveWindow(self)
        } else {
            playerLayer.contentsScale = window?.screen.scale ?? playerLayer.contentsScale
            playerLayer.syncDrawableSize()
            attachedPlayer?.videoViewDidEnterWindow(self)
        }
    }
}
#endif
