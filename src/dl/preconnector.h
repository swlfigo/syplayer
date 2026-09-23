// preconnector.h — 预连接：经全局 HTTP 后端发 1 字节 Range，预热连接池
//
// 做什么：对 url 所在源（scheme+host+port）发一个 `Range: bytes=0-0` 的 GET，
// 让后端完成 DNS/TCP/TLS 与一次 HTTP 往返，把连接留在后端自己的连接池里。
// 响应体丢弃、不写缓存、不受限速器准入、失败不重试、不回报结果。
//
// 策略：
//   - 同一源 kDedupeWindowMs（30 秒）内只发一次；满 30 秒（>=）再发。
//     过期的去重条目在每次 preconnect() 时清掉，表的大小受"30 秒内发过的源"
//     约束，不随调用次数无界增长。
//   - 同时在途上限 kMaxInflight（4）；超出直接丢弃、不排队，也不登记去重。
//   - 服务端不支持 Range 回 200 全量时，收到首块数据即 cancel，不拉整文件。
//     206（1 字节）不 cancel，让请求自然结束、连接还回后端连接池。
//
// 回收：本类没有自有线程。DLTask 不能在自己的回调里析构（dl_task.h 契约），
// 所以回调只置 done 标志；已结束的任务在**下一次 preconnect() 调用**时移出、
// 在锁外析构（与 Scheduler 的 reap 同一思路）。上限 4 保证在途的至多 4 个；
// 已结束未回收的对象会滞留到下一次调用（retained_for_test() 看得到）。
//
// 线程安全：
//   - preconnect() 可从任意线程并发调用。
//   - 回调（后端线程）只写 Entry 的原子标志、调 DLTask::cancel()，不取 mu_。
//   - ~Preconnector **不得**与 preconnect() 并发。进程单例永不析构，天然满足；
//     测试实例由用例保证（先停止调用再析构）。
//   - 析构会 cancel 全部在途并等它们结束（~DLTask 会等终态），所以析构期间
//     后端必须还活着、且守约回调 on_complete。
#pragma once

#include "clock.h"

#include <syplayer/syp_http.h>
#include <syplayer/syp_types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace syp::dl {

class Preconnector {
public:
    // 进程单例，永不析构（理由同 RateLimiter::instance()）。后端每次调用时
    // 取 current_http_backend()（未注册为 nullptr ⇒ 什么都不做），时钟为
    // system_clock()。
    static Preconnector& instance();

    // 测试用独立实例。backend_fn 在每次 preconnect() 时调用一次取后端，
    // 可返回 nullptr（等价于"未注册后端"）。backend_ctx 原样传给它。
    Preconnector(const syp_http_backend* (*backend_fn)(void*), void* backend_ctx, Clock clock);
    // cancel 全部在途，再析构 DLTask（~DLTask 等回调结束）。见类注释的并发约束。
    ~Preconnector();

    Preconnector(const Preconnector&)            = delete;
    Preconnector& operator=(const Preconnector&) = delete;

    // 尽力而为地预连接 url 所在源。真的发出了请求返回 true；url 不是 http(s)、
    // 解析不出 host、无后端、30 秒内发过、在途已满 ⇒ false。headers 可为
    // nullptr，调用期间有效即可（DLTask 会拷贝）。
    bool preconnect(std::string_view url, const syp_headers* headers);

    // 测试缝
    int32_t inflight_for_test() const;        // 尚未回收的 Entry 中 !done 的个数
    int32_t retained_for_test() const;        // 尚未回收的 Entry 总数（含 done）
    int32_t dedupe_entries_for_test() const;  // 去重表当前条目数

    static constexpr int64_t kDedupeWindowMs = 30000;
    static constexpr int32_t kMaxInflight    = 4;

private:
    struct Entry;

    static void cb_on_data(void* ctx, int64_t offset, const uint8_t* data, int32_t len);
    static void cb_on_finished(void* ctx, syp_status st, int32_t http_status);

    int64_t now_ms() const noexcept;
    int32_t inflight_locked() const noexcept;

    const syp_http_backend* (*backend_fn_)(void*);
    void*   backend_ctx_;
    Clock   clock_;

    mutable std::mutex                   mu_;
    std::map<std::string, int64_t>       last_sent_;   // 源 → 上次发出时刻（ms）
    std::vector<std::unique_ptr<Entry>>  entries_;     // 尚未回收的任务
};

namespace detail {

// 源归一化："http://User@Host:80/a?b#c" → "http://host:80"。
//   - scheme 只认 http / https（大小写不敏感），输出小写；
//   - host 小写化；IPv6 字面量保留方括号（"[::1]"）；
//   - 端口缺省 http→80、https→443；显式端口必须是 1..65535 的十进制
//     （":" 后为空按缺省端口，同 RFC 3986）；
//   - 去掉 userinfo、路径、查询、片段。
// 非 http(s)、缺 "://"、host 为空、端口非法 ⇒ 空串。
std::string origin_key(std::string_view url);

}  // namespace detail

}  // namespace syp::dl
