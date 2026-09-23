// AppDelegate.swift — iOS 示例 App 入口。
//
// 部署目标取 iOS 13.0，与 SYPlayerKit 包声明的下限一致：接入方在最低支持
// 版本上能否解析并链接这个包，本身就是这个示例要验证的事。SwiftUI 的 App
// 生命周期要 iOS 14，所以入口用 UIKit 的 AppDelegate + UIHostingController，
// 播放页本身仍是与 macOS 共用的 SwiftUI 视图。
import SwiftUI
import SYPlayerKit
import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    /// 播放器由 App 持有，生命周期与进程一致；播放页只观察它。
    private let player = SYPlayer()

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = UIHostingController(rootView: PlayerScreen(player: player))
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}
