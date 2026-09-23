// ExampleApp.swift — 原生 macOS（AppKit，不是 Mac Catalyst）示例 App 入口。
//
// macOS 11 起有 SwiftUI App 生命周期，直接用它；播放页与 iOS 示例共用。
import SwiftUI
import SYPlayerKit

@main
struct ExampleApp: App {
    @StateObject private var player = SYPlayer()

    var body: some Scene {
        WindowGroup {
            PlayerScreen(player: player)
                .frame(minWidth: 480, minHeight: 360)
        }
    }
}
