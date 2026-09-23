// SYPlayerErrorTests.swift — 每个 SYP_ERR_* ↔ SYPlayerError
// 双向一致，未知码走 .unknown，statusCode 往返。
//
// 为什么用 SypStatusCode 而不是抄一遍字面量：那些数值的唯一权威是
// include/syplayer/syp_types.h，SYPBridge.mm 里的 static_assert 已经把
// SypStatusCode 钉在上面；测试再抄一份魔数的话，两边一起抄错就测不出来。
import XCTest
@testable import SYPlayerKit
import SYPlayerKit_Private

final class SYPlayerErrorTests: XCTestCase {
    /// 每个已知状态码都要映射到一个**具名** case（不是 .unknown），并且 statusCode 能转回去。
    func testKnownStatusCodesRoundTrip() {
        let expected: [(SypStatusCode, SYPlayerError)] = [
            (.invalidArg, .invalidArgument),
            (.canceled, .canceled),
            (.timeout, .timeout),
            (.outOfMemory, .outOfMemory),
            // 左边是 SypStatusCode.IO（Swift 对"成员名整个就是缩写"的 NS_ENUM
            // 成员保留原大小写），右边是 SYPlayerError.io。
            (.IO, .io),
            (.noSpace, .noSpace),
            (.cacheCorrupt, .cacheCorrupt),
            (.network, .network),
            (.httpStatus, .httpStatus(0)),
            (.tooManyRedirects, .tooManyRedirects),
            (.rangeUnsupported, .rangeUnsupported),
            (.contentChanged, .contentChanged),
            (.notImplemented, .notImplemented),
        ]
        for (code, error) in expected {
            XCTAssertEqual(SYPlayerError(statusCode: code.rawValue), error, "码 \(code.rawValue) 映射错了")
            XCTAssertEqual(error.statusCode, code.rawValue, "\(error) 的 statusCode 回不去")
        }
    }

    /// EOF 与 BUSY 不是错误语义，但 init(statusCode:) 仍必须给出一个确定结果
    /// （调用方不该在这两个码上拿到 nil 或崩溃）——归入 .unknown，由上层
    /// 决定怎么处理；SYPlayer 自己根本不会用这两个码构造错误。
    func testEofAndBusyFallIntoUnknown() {
        XCTAssertEqual(SYPlayerError(statusCode: SypStatusCode.eof.rawValue), .unknown(-1))
        XCTAssertEqual(SYPlayerError(statusCode: SypStatusCode.busy.rawValue), .unknown(-6))
    }

    func testUnknownCodeKeepsItsValue() {
        XCTAssertEqual(SYPlayerError(statusCode: -12345), .unknown(-12345))
        XCTAssertEqual(SYPlayerError(statusCode: -12345).statusCode, -12345)
    }

    /// httpStatus 的载荷是 HTTP 状态码，statusCode 轴上永远是 -21：
    /// 往返只在 syp_status 轴上闭合，这是有意的，不是 bug。
    func testHTTPStatusPayloadDoesNotChangeStatusCode() {
        XCTAssertEqual(SYPlayerError.httpStatus(404).statusCode, SypStatusCode.httpStatus.rawValue)
        XCTAssertNotEqual(SYPlayerError.httpStatus(404), SYPlayerError.httpStatus(0))
    }

    /// 文案在框架内，业务不该再看见 -99（旧 PlayerViewController 那份硬编码映射）。
    func testLocalizedDescriptionIsNonEmptyForEveryCase() {
        let all: [SYPlayerError] = [.invalidArgument, .canceled, .timeout, .outOfMemory, .io,
                                    .noSpace, .cacheCorrupt, .network, .httpStatus(404),
                                    .tooManyRedirects, .rangeUnsupported, .contentChanged,
                                    .notImplemented, .unknown(-7)]
        for e in all {
            XCTAssertFalse(e.localizedDescription.isEmpty, "\(e) 没有文案")
        }
    }
}
