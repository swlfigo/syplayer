// SYPlayerNetworkTests.swift — 进程级限速的 Swift 入口。
// 断言读的是 C 层（桥的类方法 → syp_rate_limit_get），不读 Swift 侧副本。
import XCTest
@testable import SYPlayerKit

final class SYPlayerNetworkTests: XCTestCase {
    override func tearDown() {
        SYPlayerNetwork.maximumDownloadRate = nil     // 进程全局状态，必须复位
        super.tearDown()
    }

    @MainActor
    func testPositiveRateReachesTheCLayer() {
        SYPlayerNetwork.maximumDownloadRate = 1_000_000
        XCTAssertEqual(SYPlayerNetwork.rateLimitInCLayerForTest, 1_000_000)
        XCTAssertEqual(SYPlayerNetwork.maximumDownloadRate, 1_000_000)
    }

    @MainActor
    func testNilZeroAndNegativeAllMeanUnlimited() {
        for v: Int? in [nil, 0, -5] {
            SYPlayerNetwork.maximumDownloadRate = 1_000
            SYPlayerNetwork.maximumDownloadRate = v
            XCTAssertEqual(SYPlayerNetwork.rateLimitInCLayerForTest, 0, "输入 \(String(describing: v))")
            XCTAssertNil(SYPlayerNetwork.maximumDownloadRate)
        }
    }

    @MainActor
    func testHugeRateIsClampedByTheCLayer() {
        SYPlayerNetwork.maximumDownloadRate = Int.max
        XCTAssertEqual(SYPlayerNetwork.rateLimitInCLayerForTest, 1 << 40)   // RateLimiter::kMaxRate
    }

    // 预连接。非 http(s) 的 URL 必须在 Swift 侧就被挡住——用在途数不变
    // 证明桥调用根本没发生（C 层同样会挡，这条只钉 Swift 侧那道省调用的判断）。
    @MainActor
    func testPreconnectNonHTTPIsNoop() {
        let before = SYPlayerNetwork.preconnectInflightForTest
        SYPlayerNetwork.preconnect(URL(string: "file:///tmp/a.mp4")!)
        XCTAssertEqual(SYPlayerNetwork.preconnectInflightForTest, before)
    }

    // preconnectURL: 现在会惰性注册 HTTP 后端（不再依赖同进程
    // 里先跑过 SYPlayer/SYPlayerPreloader 的其它用例），所以这里不能再假设
    // "没有后端"——用一个本机丢端口（127.0.0.1:1）的地址，连接会快速失败，
    // 这条用例只钉"带 headers 调用不崩溃"，不断言在途数或结果。
    @MainActor
    func testPreconnectDoesNotCrashWithHeaders() {
        SYPlayerNetwork.preconnect(URL(string: "http://127.0.0.1:1/")!,
                                    headers: ["Authorization": "Bearer tok", "X-Trace": "42"])
    }

    // preconnectURL: 必须自己惰性注册后端，不能依赖调用方先
    // 创建过 SYPlayer/SYPlayerPreloader。用本机丢端口地址（不产生真实网络
    // 流量，连接快速失败）触发一次 preconnect，断言调用之后后端已注册。
    // 已知局限（诚实记录，不隐藏）：同进程内若本条用例之前已有别的用例
    // 注册过后端，即使把桥里新增的那行 ensure_http_backend_registered()
    // 删掉，本条用例也可能仍然绿——单独跑本测试类时能测出这一行被删掉，
    // 全量跑测试套件时测不出来。
    @MainActor
    func testPreconnectRegistersBackendLazily() {
        SYPlayerNetwork.preconnect(URL(string: "http://127.0.0.1:1/")!)
        XCTAssertTrue(SYPlayerNetwork.preconnectBackendRegisteredForTest)
    }
}
