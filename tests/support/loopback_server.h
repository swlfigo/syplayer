// loopback_server.h — 测试用极简 HTTP/1.1 服务器：127.0.0.1 + 内核分配端口
#pragma once

#include "support/synthetic_byte.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace syp::dl::test {

// 放在类外：带默认成员初值的嵌套 struct 不能在 enclosing class 未完成时
// 做 {} / designated init（clang 会要那些默认初值）。
struct LoopbackConfig {
    int64_t     resource_length        = 1024;
    bool        support_range          = true;
    int32_t     status_code            = 0;     // 0 = 自动 200/206
    std::string redirect_to;                    // 非空则 302 + Location
    int64_t     close_after_bytes      = -1;    // -1 = 发完；否则发这么多就关
    int32_t     delay_ms               = 0;     // 读完请求后、写响应前
    std::string content_range_override;         // 非空则原样作为 Content-Range 值
    bool        hang                   = false; // 读请求后永不响应
    int32_t     pause_after_bytes      = -1;    // 发这么多后暂停
    int32_t     pause_ms               = 0;
    int32_t     body_chunk_bytes       = 0;     // >0 时按块发送
    int32_t     body_chunk_delay_ms    = 0;
    // 非 200/206 时作为响应体发出；空则 Content-Length: 0（保持旧行为）。
    std::string error_body;
    // 非空时从该文件供 body，resource_length 自动取文件大小（忽略配置值）。
    // 为空则走 synthetic_byte 合成公式（既有行为，不动）。
    std::string body_file;
    // 非空时发 ETag 响应头。set_config 可中途换值，用于「源文件变了」的场景。
    std::string etag;

    // 按路径路由。非空时**优先于** body_file/synthetic_byte：
    // 命中的路径用这里的字节，未命中的路径返回 404。
    // 空 map = 保持既有的单资源行为，一个字节都不变（既有用例全部走这条路）。
    std::map<std::string, std::vector<uint8_t>> routes;
    std::map<std::string, std::string>          route_content_types;

    // gzip 响应（Content-Encoding: gzip，整份；带 Range 的请求回 206 + 编码表示的
    // Content-Range，Content-Length 为编码后长度）。gzip_when_accepted：仅当请求头 Accept-Encoding 含 gzip 时编码（模拟 CDN）；
    // gzip_always：无视 Accept-Encoding 一律编码（模拟不守规矩的服务端）。编码用
    // deflate stored 块，不压缩但长度必然与原文不同，不依赖 zlib。只作用于路由/合成
    // 公式两种供体，body_file 不支持。
    bool gzip_when_accepted = false;
    bool gzip_always        = false;

    // 只对**第 1..slow_first_requests 个**供体请求（按收到顺序，1-based）
    // 生效的按块节流：每 slow_chunk_bytes 字节睡 slow_chunk_delay_ms。0 = 关。
    // 用来造"同一资源里只有一条连接是 slow-loris"——body_chunk_delay_ms 是
    // 全局的，做不出兄弟连接之间的速度差。命中的请求**替换**（不叠加）
    // body_chunk_bytes/body_chunk_delay_ms；没命中的照旧走全局那两个值。
    // 序号在"决定供体"那一刻分配（与 total_requests() 同口径：redirect、
    // 非 200/206、gzip 整份这几条路径不占序号）。
    int32_t slow_first_requests  = 0;
    int32_t slow_chunk_bytes     = 1024;
    int32_t slow_chunk_delay_ms  = 0;

    // HTTP/1.1 持久连接。false（默认）= 每个响应都写 Connection: close、
    // 写完即关，既有用例一个字节都不变。true 时**只有两条供体分支**支持保活：
    // 路由命中的 200/206 与合成公式（synthetic_byte）的 200/206——响应头写
    // Connection: keep-alive、Content-Length 如实，body 完整写完后在同一 fd 上
    // 继续读下一个请求，直到对端关闭或服务器析构。其余分支（redirect、
    // 路由未命中 404、非 200/206 错误状态、gzip、body_file、close_after_bytes
    // 截断、hang）在 keep-alive 下**照旧写 close 并关连接**：它们的 body 长度
    // 或者与 Content-Length 不符、或者从没被持久连接场景用到，没必要为它们
    // 保证 HTTP/1.1 持久连接的前提。
    bool keep_alive = false;
};

// 一次「决定要供体」的请求（跳过纯 redirect / 非 200-206 错误响应）留下的记录。
// start/end 是按 Range 头（或整文件）算出的**承诺**区间，不是实际发出的字节数——
// 截断与否看 server_capped / early_close / bytes_sent。
struct LoopbackRequestRecord {
    int64_t start            = 0;   // 半开区间起点
    int64_t end              = 0;   // 半开区间终点（承诺值，即 Content-Length 里声明的那个）
    int64_t bytes_sent       = 0;   // 实际写出的 body 字节数（<= end-start）
    // bytes_sent < (end-start) 有两种完全不同的成因，混在一个
    // "truncated" 标记里会把「服务端按 close_after_bytes 主动掐断」和
    // 「客户端提前 cancel() / 连接被打断」算成同一件事——这会污染任何
    // 依赖"服务端真的截断过几次"来做确定性构造的断言（例如场景 F）。
    // 两者可判定区分：close_after_bytes 主动生效时，实际发出的字节数
    // 恰好等于配置值本身（remaining_limit 被钉死成 close_after_bytes）；
    // 其它任何提前退出（写失败、stop_ 信号、客户端主动断开）几乎不会
    // 恰好卡在这个数字上。
    bool    server_capped    = false;  // 精确命中 close_after_bytes：服务端自己主动掐断
    bool    early_close      = false;  // bytes_sent < (end-start) 但不是上面那种：写失败/被取消/其它
    int32_t concurrent_at_start = 0;   // 本请求开始供体那一刻（含它自己）同时在供体的连接数
    std::string path;   // 请求行里的路径（含 query）
};

class LoopbackServer {
public:
    using Config = LoopbackConfig;

    explicit LoopbackServer(LoopbackConfig cfg = {});
    ~LoopbackServer();
    LoopbackServer(const LoopbackServer&)            = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    uint16_t    port() const noexcept;
    std::string base_url() const;  // http://127.0.0.1:<port>
    std::string url(std::string_view path = "/") const;

    void           set_config(LoopbackConfig cfg);
    LoopbackConfig config() const;

    // 反向自检：不靠「最终字节比对全等」侧面推断线上行为，
    // 直接数服务端收到过什么。全部线程安全，可在跑的过程中随时读。
    int64_t total_requests() const noexcept;       // 供过体的请求总数（不含 redirect/错误状态码）
    int64_t total_bytes_sent() const noexcept;      // 所有响应累计实际写出的 body 字节
    int64_t server_capped_count() const noexcept;   // 精确命中 close_after_bytes 的响应数
    int64_t early_close_count() const noexcept;     // 其它提前退出（写失败/取消/…）的响应数
    int32_t peak_concurrent_requests() const noexcept;  // 同时在供体的连接数峰值
    // accept() 成功的 TCP 连接总数（accept 那一刻 +1，与请求数无关）。
    // keep-alive 模式下用它断言"几个请求共用了几条连接"。
    int64_t accepted_connections() const noexcept;
    std::vector<LoopbackRequestRecord> requests_snapshot() const;

    // 线程安全，可在跑的过程中随时调用。
    // path 做**精确匹配**（含 query）——HLS 的分片 URL 带 query 时，
    // 断言方必须用完全一样的字符串，模糊匹配会把"请求了 A"和"请求了 A?x=1"
    // 混成一件事。
    void    set_route(std::string_view path, std::vector<uint8_t> body,
                      std::string_view content_type);
    // 撤掉一条路由，之后对它的请求走"路由模式下未命中 ⇒ 404"。用于在跑的
    // 过程中制造"原本可取的资源变成 404"（直播重拉失败）。
    void    remove_route(std::string_view path);
    // 【供过体的请求数】在响应写完之后才 +1（record_request）。
    int64_t requests_for(std::string_view path) const;

    // **收到**请求行的那一刻就 +1，跟"最后供没供体"无关。
    //
    // 存在的理由是 requests_for() 对 hang 住的请求**永远数不到**——
    // record_request() 跑在供完体之后，而 LoopbackConfig::hang 的分支在那
    // 之前就把连接晾住了。凡是要断言"卡住之后对方确实又发过一次请求"的
    // 用例（test_hls_e2e 的 request_abort_unblocks_a_hanging_playlist_fetch）
    // 只能用这个计数，用 requests_for() 会得到一条恒不成立的断言。
    //
    // 计数点在 parse_request() 成功之后、delay_ms/hang/redirect/路由之前，
    // 所以 404、重定向、被晾住的请求统统算数。路径同样是**精确匹配含
    // query**，跟 requests_for() 一致。
    int64_t requests_received_for(std::string_view path) const;

    // 按路径扣住请求：收到请求行（计入 requests_received_for）之后、写响应之前原地
    // 等待，直到 release_route() 或服务器析构。路径精确匹配含 query。用来确定性地制造
    // "加载线程阻塞在某个分片上"——比 delay_ms 可控：何时放行由用例决定。
    void    hold_route(std::string_view path);
    void    release_route(std::string_view path);

private:
    void accept_loop();
    void handle_conn(int fd);
    // 读并响应**一个**请求。返回 true 表示连接可以留着读下一个请求
    // （仅 keep_alive 且走了支持保活的供体分支、body 完整写完）。
    // carry：上次读多出来的字节（属于下一个请求），读新请求时先用它。
    bool serve_one(int fd, std::string& carry);
    void sleep_interruptible(int32_t ms);
    void add_client_fd(int fd);
    void remove_client_fd(int fd);
    void shutdown_all_clients();
    // concurrent_at_start：本请求开始供体那一刻（含它自己）同时在供体的连接数，
    // 供调用方判定「降级后是否已经收敛到单连接」（场景 E）。
    void record_request(int64_t start, int64_t end, int64_t bytes_sent,
                        bool server_capped, bool early_close, int32_t concurrent_at_start,
                        std::string path);

    LoopbackConfig       cfg_{};
    mutable std::mutex   mu_;
    std::atomic<bool>    stop_{false};
    uint16_t             port_ = 0;
    int                  listen_fd_ = -1;
    int                  wake_rd_   = -1;
    int                  wake_wr_   = -1;
    std::thread          acceptor_;
    std::mutex           clients_mu_;
    std::vector<int>     client_fds_;
    std::mutex           threads_mu_;
    std::vector<std::thread> conn_threads_;

    std::atomic<int64_t> accepted_connections_{0};
    std::atomic<int64_t> total_requests_{0};
    std::atomic<int64_t> total_bytes_sent_{0};
    std::atomic<int64_t> server_capped_count_{0};
    std::atomic<int64_t> early_close_count_{0};
    std::atomic<int32_t> inflight_requests_{0};
    std::atomic<int32_t> peak_inflight_requests_{0};
    // 供体序号（1-based，按决定供体的先后），给 slow_first_requests 用。
    std::atomic<int64_t> donor_seq_{0};
    mutable std::mutex   requests_mu_;
    std::vector<LoopbackRequestRecord> requests_;

    // 每路径请求计数，mu_ 保护（与 cfg_.routes 同一把锁，
    // set_route()/requests_for() 都在这把锁下读写）。
    std::map<std::string, int64_t> per_path_counts_;
    // 收到即计数，与 per_path_counts_ 同一把锁（mu_）。
    std::map<std::string, int64_t> per_path_received_;
    // hold_route() 扣住的路径集合，mu_ 保护。
    std::set<std::string> held_routes_;
};

}  // namespace syp::dl::test
