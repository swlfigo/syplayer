// AppDelegate.swift — 两个壳共用。不用 Storyboard、不声明
// UIApplicationSceneManifest（Info.plist 里没有这个 key），所以系统按
// iOS 12 之前的经典生命周期跑：window 由 AppDelegate 自己建，不需要额外
// 一个 SceneDelegate.swift。Mac Catalyst 下同样支持这条路径。
import UIKit

@UIApplicationMain
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
    ) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        // 两页并存就是"SwiftUI 与 UIKit 两种接入都成立"最直接的证据：
        // 同一个 App、同一个 SYPlayerKit，切一下 tab 就能对比。
        // 顺带也是视图离开/回到窗口要对称解挂重挂的唯一手工验证入口：
        // 切 tab 就是让一整棵视图树离开窗口再回来。
        let uikitPage = UINavigationController(rootViewController: PlayerViewController())
        uikitPage.tabBarItem = UITabBarItem(title: "UIKit", image: nil, tag: 0)
        let swiftUIPage = UINavigationController(rootViewController: SwiftUIPlayerViewController())
        swiftUIPage.tabBarItem = UITabBarItem(title: "SwiftUI", image: nil, tag: 1)
        // 第三页演示预加载。它与两个播放页共用同一个默认缓存目录，所以
        // 在这里暖过的 URL，切回 UIKit 页用同一个地址打开会直接命中缓存。
        let preloadController = PreloadViewController()
        let preloadPage = UINavigationController(rootViewController: preloadController)
        preloadPage.tabBarItem = UITabBarItem(title: "预加载", image: nil, tag: 2)
        let tabs = UITabBarController()
        tabs.viewControllers = [uikitPage, swiftUIPage, preloadPage]
        window.rootViewController = tabs
        window.makeKeyAndVisible()
        self.window = window

        #if DEBUG
        // 无人值守自检入口。**不是测试目标，是"这一页真的被跑过"
        // 的证据**：设了环境变量就自动走一遍"起本地样例服务器 → 填三条 URL →
        // 两种统计口径各跑一段 → 全部移除 → 把页面摘掉验证释放 → 退出"，
        // 并把页面上渲染出来的文字打到 stdout。没设环境变量时这一段一行都不跑，
        // 手工操作的行为与没有它时完全一样。
        //
        //   SYPLAYER_DEMO_PRELOAD_AUTORUN=1 \
        //   SYPLAYER_DEMO_PRELOAD_SECONDS=8 \
        //   SYPLAYER_DEMO_PRELOAD_PHASES=merged|perrow|both \
        //   <path>/syplayer-mac.app/Contents/MacOS/syplayer-mac
        let env = ProcessInfo.processInfo.environment
        // 播放页同形的无人值守自检入口，与预加载
        // 那段并列、同一个 #if DEBUG 块、各自的环境变量互不影响。两个变量
        // 同时设不是需求，写成 else if：只设一个就只跑那一个。
        if env["SYPLAYER_DEMO_PLAYER_AUTORUN"] == "1" {
            tabs.selectedIndex = 0
            guard let playerController = uikitPage.viewControllers.first as? PlayerViewController else {
                print("[player-autorun] 找不到 PlayerViewController，退出。")
                exit(1)
            }
            playerController.startAutorunProbe { ok in
                print("[player-autorun] 结束　结果=\(ok ? "OK" : "FAILED")")
                exit(ok ? 0 : 1)
            }
        } else if env["SYPLAYER_DEMO_PRELOAD_AUTORUN"] == "1" {
            tabs.selectedIndex = 2
            let seconds = Double(env["SYPLAYER_DEMO_PRELOAD_SECONDS"] ?? "") ?? 8
            let phases = env["SYPLAYER_DEMO_PRELOAD_PHASES"] ?? "both"
            // 闭包里**一个字都不提 preloadController / preloadPage**：它被
            // 存在那个 controller 上，强捕获就会做出一个环，而这一段的最后
            // 一步恰恰是验证"页面被摘掉后真的释放了"（见 PreloadViewController
            // 的 deinit 打印）。tabs / 另外两页也用 weak，理由同上。
            preloadController.startAutorunProbe(seconds: seconds, phases: phases) {
                [weak tabs, weak uikitPage, weak swiftUIPage] ok in
                if let tabs = tabs, let a = uikitPage, let b = swiftUIPage {
                    tabs.viewControllers = [a, b]
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                    print("[preload-autorun] 结束（若上面没有出现 deinit 那一行，说明页面被吊住了）"
                          + "　结果=\(ok ? "OK" : "FAILED")")
                    // **退出码要跟着结果走。** 上一版恒退 0，
                    // 于是这条自检根本不是闸门：实测把 PreloadViewController
                    // 里 5 条 addTarget 全注释掉（屏幕上一个控件都不响应），
                    // 自检照样 EXIT=0、输出与基线逐字节相同。现在自检经
                    // sendActions 驱动控件，接线断了就走不到最后，这里退 1。
                    exit(ok ? 0 : 1)
                }
            }
        }
        #endif
        return true
    }
}
