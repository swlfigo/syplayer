// SYPlayerViewRepresentable.swift — SwiftUI 接入。包的是同一个 SYPlayerView，
// 与 UIKit 页共用一份 SYPlayerLayer 逻辑，不做第二套。
//
// 【改名】原名 `SYPlayerVideoView` 与 UIKit 侧的 `SYPlayerView` 只差
// 一个 "Video"，在补全列表里几乎是同形词——两者却分属两套 UI 体系、用法完全
// 不同（一个是 UIView 子类，一个是 SwiftUI 的表示层），选错编译期未必报错、
// 运行期表现为"画面不出来"。改成 `…Representable` 让类型名自己说清它是哪一侧。
// 此刻尚无外部消费者，改名零代价，晚一步就要背兼容包袱。
//
// 本文件的 UIViewRepresentable 版只在有 UIKit 的平台编译；原生 macOS 的
// NSViewRepresentable 版在 SYPlayerViewRepresentable+AppKit.swift，类型名与
// 公开成员一致。"换播放器重新挂接"的判断两版共用文件末尾的 syncAttachment(to:)。
#if canImport(UIKit)
import SwiftUI
import UIKit

public struct SYPlayerViewRepresentable: UIViewRepresentable {
    /// internal 而非 private：文件末尾的共享扩展 syncAttachment(to:) 要读它，
    /// 而 AppKit 版的同名结构体在另一个文件里，同样经那份扩展读它。
    let player: SYPlayer

    public init(player: SYPlayer) {
        self.player = player
    }

    public func makeUIView(context: Context) -> SYPlayerView {
        let view = SYPlayerView()
        player.attach(to: view)
        return view
    }

    public func updateUIView(_ uiView: SYPlayerView, context: Context) {
        // 尺寸与窗口进出由 SYPlayerLayer / SYPlayerView 自己处理，这里不管。
        //
        // 要管的是**换播放器**：SwiftUI 会复用同一个 UIView 而只重建表示层，
        // 所以 `SYPlayerViewRepresentable(player: a)` 换成 `…(player: b)` 时 makeUIView
        // 不会再跑一次。不在这里补挂，画面就还挂在 a 的渲染器上，而声音已经是
        // b 的——表现为"换了个播放器之后黑屏 + 串音"，而且看起来像渲染 bug。
        // attach(to:) 本身幂等（同一个视图重复 attach 不会把自己摘掉），所以这里
        // 只需要挡住"没换"的常见情况，省掉每次 update 都重挂一次 layer。
        syncAttachment(to: uiView)
    }

    /// SwiftUI 销毁这个表示层时解挂——不解的话渲染器还挂着一个已经不在层级里的
    /// layer，每帧白跑一次 present。
    public static func dismantleUIView(_ uiView: SYPlayerView, coordinator: ()) {
        uiView.attachedPlayer?.videoViewDidLeaveWindow(uiView)
    }
}
#endif

#if canImport(UIKit) || os(macOS)
extension SYPlayerViewRepresentable {
    /// update 回调（updateUIView / updateNSView）的全部逻辑，单独拎出来只为可测：
    /// `UIViewRepresentableContext` / `NSViewRepresentableContext` 都没有公开
    /// 构造器，测试造不出 `Context`，也就没法直接调 update 回调。
    /// 两个平台的表示层共用这一份，保证"换播放器重新挂接"的判据只有一处。
    @MainActor
    func syncAttachment(to view: SYPlayerView) {
        if view.attachedPlayer !== player {
            player.attach(to: view)
        }
    }
}
#endif
