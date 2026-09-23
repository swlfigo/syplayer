// SYPlayerViewRepresentable+AppKit.swift — 原生 macOS 的 SwiftUI 接入。
//
// 与 UIKit 版（SYPlayerViewRepresentable.swift）同名、公开成员对称：
// makeNSView / updateNSView / dismantleNSView 对应 makeUIView / updateUIView /
// dismantleUIView。"换播放器重新挂接"的判据不在这里重复，两版共用
// SYPlayerViewRepresentable.swift 末尾的 syncAttachment(to:)。
#if os(macOS) && !targetEnvironment(macCatalyst)
import AppKit
import SwiftUI

public struct SYPlayerViewRepresentable: NSViewRepresentable {
    /// internal 而非 private：共享扩展 syncAttachment(to:) 在另一个文件里读它。
    let player: SYPlayer

    public init(player: SYPlayer) {
        self.player = player
    }

    public func makeNSView(context: Context) -> SYPlayerView {
        let view = SYPlayerView(frame: .zero)
        player.attach(to: view)
        return view
    }

    /// SwiftUI 会复用同一个 NSView 而只重建表示层：换了播放器时 makeNSView 不会
    /// 再跑，必须在这里补挂，否则画面还挂在旧播放器的渲染器上、声音已经是新的。
    public func updateNSView(_ nsView: SYPlayerView, context: Context) {
        syncAttachment(to: nsView)
    }

    /// SwiftUI 销毁这个表示层时解挂——不解的话渲染器还挂着一个已经不在层级里的
    /// layer，每帧白跑一次 present。
    public static func dismantleNSView(_ nsView: SYPlayerView, coordinator: ()) {
        nsView.attachedPlayer?.videoViewDidLeaveWindow(nsView)
    }
}
#endif
