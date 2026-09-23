// test_hls_playlist_fetcher.cpp —— 播放列表通道。
//
// 这条通道存在的唯一理由是**零缓存**：直播的播放列表同一个 URL 内容每几秒
// 变一次，而 dl 层在区间已缓存时一个请求都不发（source_bridge.cpp:911）。
// 所以本文件最重要的一条是 fetches_fresh_content_every_time。
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <syplayer/syp_http.h>

#include "media/hls/playlist_fetcher.h"
#include "platform/apple/apple_http_backend.h"
#include "support/loopback_server.h"
#include "tiny_test.h"

using namespace syp::media::hls;
using syp::dl::test::LoopbackServer;

namespace {
std::vector<uint8_t> bytes_of(const char* s) {
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(s),
                                reinterpret_cast<const uint8_t*>(s) + std::char_traits<char>::length(s));
}
std::string as_string(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}
// 这些用例用真实的平台后端（Apple 上是 NSURLSession），打的是 127.0.0.1。
// 用真后端而不是 stub：本函数的职责就是把**异步后端**包成一次阻塞调用，
// 用一个同步 stub 测它等于把被测的那件事测掉了。
const syp_http_backend* backend() { return syp_apple_http_backend(); }
}  // namespace

TEST_CASE(fetches_a_small_playlist) {
    LoopbackServer srv;
    srv.set_route("/a.m3u8", bytes_of("#EXTM3U\n#EXT-X-VERSION:6\n"), "application/x-mpegURL");

    const auto r = fetch_playlist(backend(), srv.url("/a.m3u8"), 3000, 3000);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(SYP_OK));
    // apple_http_backend.mm 对每个请求都无条件带 "Range: bytes=<start>-"
    // （即便 range_start=0/range_end=-1 意在"整个资源"）——这是既有的、
    // 分片通道也在用的平台后端行为，不在本任务改动范围内。LoopbackServer
    // 忠实模拟真实服务端：只要带了 Range 头就回 206（Content-Range 覆盖
    // 全量字节),不是 200。所以这里接受 200/206 两种，不是最初写的
    // 严格 200——判据仍然是"2xx 即成功"，跟 fetch_playlist 内部的判定一致。
    CHECK(r.http_status == 200 || r.http_status == 206);
    CHECK_EQ(as_string(r.body), std::string("#EXTM3U\n#EXT-X-VERSION:6\n"));
}

TEST_CASE(fetches_fresh_content_every_time) {
    // 直播语义：同一个 URL，内容在两次取之间变了，第二次必须拿到新的。
    // 这条用例是整条通道存在的理由——走 syp_source 的话第二次会命中缓存，
    // 一个请求都不发。
    LoopbackServer srv;
    srv.set_route("/live.m3u8", bytes_of("#EXTM3U\nseq=1\n"), "application/x-mpegURL");

    const auto first = fetch_playlist(backend(), srv.url("/live.m3u8"), 3000, 3000);
    CHECK_EQ(as_string(first.body), std::string("#EXTM3U\nseq=1\n"));

    srv.set_route("/live.m3u8", bytes_of("#EXTM3U\nseq=2\n"), "application/x-mpegURL");

    const auto second = fetch_playlist(backend(), srv.url("/live.m3u8"), 3000, 3000);
    CHECK_EQ(as_string(second.body), std::string("#EXTM3U\nseq=2\n"));

    // 服务端侧实证：真的发了两次，不是"内容不同所以大概发了两次"
    CHECK_EQ(srv.requests_for("/live.m3u8"), 2);
}

TEST_CASE(non_2xx_is_an_error_with_the_status_carried) {
    LoopbackServer srv;
    // 没 set_route 的路径在路由模式下返回 404；先建一个别的路由进入路由模式
    srv.set_route("/exists.m3u8", bytes_of("#EXTM3U\n"), "application/x-mpegURL");

    const auto r = fetch_playlist(backend(), srv.url("/missing.m3u8"), 3000, 3000);
    CHECK(r.status != SYP_OK);
    CHECK_EQ(r.http_status, 404);
    CHECK(r.body.empty());
}

TEST_CASE(non_redirect_3xx_terminal_status_is_still_an_error) {
    // fetch_playlist 自己那段"2xx 才算成功"的判定
    // （out.http_status < 200 || out.http_status >= 300）此前零增量覆盖：
    // 唯一在跑的非 2xx 用例（404）已经被 apple_http_backend.mm 的
    // did_complete() 自己判定成 SYP_ERR_HTTP_STATUS 了（http_status_>=400），
    // 那段判定形同虚设。它真正拦的是"后端自己判定成功（http_status<400），
    // 但状态码落在 [100,199]∪[300,399] 这个终态"——例如 304 Not Modified：
    // 没有 Location，NSURLSession 不会当重定向跟随，did_complete() 里
    // http_status_ < 400 那个分支会给出 st=SYP_OK；如果没有 fetch_playlist
    // 自己这段判定，调用方会把一个空播放列表当成"取成功了"。
    LoopbackServer srv;
    syp::dl::test::LoopbackConfig cfg = srv.config();
    cfg.status_code = 304;   // 不带 Location 的终态，不会被当重定向跟随
    srv.set_config(cfg);
    srv.set_route("/cached.m3u8", bytes_of("#EXTM3U\n"), "application/x-mpegURL");

    const auto r = fetch_playlist(backend(), srv.url("/cached.m3u8"), 3000, 3000);
    CHECK(r.status != SYP_OK);
    CHECK_EQ(r.http_status, 304);
}

TEST_CASE(oversize_playlist_is_rejected) {
    LoopbackServer srv;
    // kMaxPlaylistBytes + 1，逐字节合成，确认上限是**闭区间之外**才拒
    std::vector<uint8_t> big(static_cast<std::size_t>(kMaxPlaylistBytes) + 1u, 'x');
    srv.set_route("/big.m3u8", std::move(big), "application/x-mpegURL");

    const auto r = fetch_playlist(backend(), srv.url("/big.m3u8"), 3000, 5000);
    CHECK(r.status != SYP_OK);
    // 不是超时也不是网络错，是被我们自己的上限拒掉的。
    // 最初设想用 SYP_ERR_TOO_LARGE，但 syp_types.h 里没有这个码
    // （冻结目录，不能新增）；playlist_fetcher.cpp 顶部注释解释了为什么
    // 选 SYP_ERR_OOM 而不是最初用过的 SYP_ERR_NO_SPACE（
    // NO_SPACE 强绑定磁盘语义，OOM 不绑定磁盘且跟"读进内存会撑爆"更贴）。
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(SYP_ERR_OOM));
}

TEST_CASE(hang_times_out_and_does_not_leak_the_handle) {
    LoopbackServer srv;
    syp::dl::test::LoopbackConfig cfg = srv.config();
    cfg.hang = true;                       // 读完请求后永不响应
    srv.set_config(cfg);
    srv.set_route("/hang.m3u8", bytes_of("#EXTM3U\n"), "application/x-mpegURL");

    const auto r = fetch_playlist(backend(), srv.url("/hang.m3u8"), 1000, 500);
    CHECK(r.status != SYP_OK);
    // 关键不在返回值，在于函数**真的返回了**，且没有在后台留一个还会回调
    // 到已析构状态上的 handle —— 后者 ASan 会在进程退出时抓到。
    // 这条用例必须在 build-asan 下也跑。
}


// 【本条钉的是新补的那个洞】fetch_playlist 此前只受自身
// read_timeout_ms 约束：看门狗（HlsSession::request_abort）在播放列表抓取
// 期间开火**完全没有效果**——分片那条在 AvioBridge 里有传播，播放列表这条
// 没有。直播每几秒就要重拉一次播放列表，用户导航离开时撞上一次正在进行的
// 重拉是常态，不是边角。
//
// read_timeout_ms 故意设成 30 秒：断言是"3 秒内返回"，只有中止标志真的被
// 轮询到才可能做到。把 fetch_playlist 里那段轮询去掉，这条要等满 30 秒。
TEST_CASE(abort_flag_cancels_a_hanging_fetch) {
    LoopbackServer srv;
    srv.set_route("/hang.m3u8", bytes_of("#EXTM3U\n"), "application/x-mpegURL");
    syp::dl::test::LoopbackConfig cfg = srv.config();
    cfg.hang = true;                       // 读完请求后永不响应
    srv.set_config(cfg);

    std::atomic<bool> abort_flag{false};
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        abort_flag.store(true, std::memory_order_release);
    });

    const auto t0 = std::chrono::steady_clock::now();
    const auto r  = fetch_playlist(backend(), srv.url("/hang.m3u8"), 30000, 30000,
                                   &abort_flag);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    killer.join();

    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(SYP_ERR_CANCELED));
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 3000);
}

// 标志**一开始就是置位的**：一次请求都不该发出去。直播下看门狗先开火、
// hls 随后又要重拉一次播放列表是真实顺序，这时候再去建一条注定要被取消的
// 连接纯属浪费。
TEST_CASE(already_aborted_fetch_sends_no_request) {
    LoopbackServer srv;
    srv.set_route("/a.m3u8", bytes_of("#EXTM3U\n"), "application/x-mpegURL");

    std::atomic<bool> abort_flag{true};
    const auto r = fetch_playlist(backend(), srv.url("/a.m3u8"), 1000, 1000, &abort_flag);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(SYP_ERR_CANCELED));
    CHECK_EQ(srv.requests_for("/a.m3u8"), static_cast<int64_t>(0));
}

int main() { return tiny_test_main(); }
