// playlist_fetcher.cpp —— 见 playlist_fetcher.h 顶部注释。
//
// 【错误码选择】原本想用 SYP_ERR_TOO_LARGE，但
// include/syplayer/syp_types.h 里没有这个码（冻结目录，禁止新增）。
// 现有码里选的是 SYP_ERR_OOM（-5，顶层通用错误组，不绑定磁盘语义）而不是
// 曾经用过的 SYP_ERR_NO_SPACE（-11）：NO_SPACE 归在「本地读写失败」那一组
// （挨着 IO=-10 / CACHE_CORRUPT=-12），字面强绑定磁盘——调用方看到它会去
// 查本地存储或缓存驱逐，而不是去看播放列表 URL 或 kMaxPlaylistBytes。
// OOM 不绑定磁盘，且跟下面 kMaxPlaylistBytes 注释里"会把任意大的响应体
// 读进内存"这句话字面对得上——这里拒绝的正是"读进内存会撑爆"这件事，不是
// "磁盘没地方放"。两个码都不完美（本来就缺一个 TOO_LARGE），选 OOM 是因为
// 它被误判后调用方走错排查方向的代价更轻。
#include "media/hls/playlist_fetcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace syp::media::hls {
namespace {

struct FetchState {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    done        = false;   // on_complete 到达过
    syp_status              status      = SYP_OK;
    int32_t                 http_status = 0;
    std::vector<uint8_t>    body;
    bool                    too_large   = false;
};

void on_response(void* ctx, int32_t http_status, const syp_headers* /*headers*/,
                 int64_t content_length, int64_t /*total_length*/) {
    auto* st = static_cast<FetchState*>(ctx);
    std::lock_guard<std::mutex> g(st->mu);
    st->http_status = http_status;
    // 已声明的长度就超限时，不必等它发完
    if (content_length > kMaxPlaylistBytes) {
        st->too_large = true;
    } else if (content_length > 0) {
        st->body.reserve(static_cast<std::size_t>(content_length));
    }
}

// 【当前套件无覆盖】这道累计长度检查是防"on_response 声明的
// content_length 未知或不可信、只能靠边收边数"这类场景——真实 HTTP 世界里对
// 应的是响应没有 Content-Length（连接关闭定界）或用 chunked 编码的情形，
// on_response 那道基于声明长度的检查在这些场景下形同虚设，全靠这里兜底。
//
// 现在的测试套件测不到这道检查：tests/support/loopback_server.cpp 的响应头
// 永远带一个准确的 Content-Length（等于它实际打算发送的字节数，见该文件
// "Content-Length: " << fmt_i64(body_len) 那一行），且不支持
// Transfer-Encoding: chunked 或"不声明长度、发到连接关闭为止"这种定界方式；
// close_after_bytes 只能让实际发送的字节数比声明的**少**，做不出"声明的比
// 实际发的少"或"完全不声明"。也就是说用现有旋钮，on_response 的声明长度
// 检查必然先于这里触发，`oversize_playlist_is_rejected` 测的其实是
// on_response 那道，不是这一道。
//
// 要真正覆盖这道检查，LoopbackServer 需要新增一个旋钮：要么允许发送的字节
// 数与声明的 Content-Length 不一致（且不因此提前关闭连接），要么支持完全
// 省略 Content-Length（走 chunked 或纯 connection-close 定界）。这不在本
// 任务范围内，如实记录在此，等以后哪个任务需要验证这条路径时再补。
void on_data(void* ctx, const uint8_t* data, int32_t len) {
    auto* st = static_cast<FetchState*>(ctx);
    if (len <= 0) return;
    std::lock_guard<std::mutex> g(st->mu);
    if (st->too_large) return;   // 已经判超限，后面的字节一概不收
    if (static_cast<int64_t>(st->body.size()) + len > kMaxPlaylistBytes) {
        st->too_large = true;
        st->body.clear();
        st->body.shrink_to_fit();
        return;
    }
    st->body.insert(st->body.end(), data, data + len);
}

bool on_redirect(void* /*ctx*/, const char* /*new_url*/) { return true; }

void on_complete(void* ctx, syp_status status, int32_t http_status) {
    auto* st = static_cast<FetchState*>(ctx);
    {
        std::lock_guard<std::mutex> g(st->mu);
        st->status = status;
        if (http_status != 0) st->http_status = http_status;
        st->done = true;
    }
    st->cv.notify_all();
}

}  // namespace

PlaylistFetchResult fetch_playlist(const syp_http_backend*  backend,
                                   const std::string&       url,
                                   int32_t                  connect_timeout_ms,
                                   int32_t                  read_timeout_ms,
                                   const std::atomic<bool>* abort) {
    PlaylistFetchResult out;
    if (backend == nullptr || url.empty()) {
        out.status = SYP_ERR_INVALID_ARG;
        return out;
    }
    const auto aborted = [abort] {
        return abort != nullptr && abort->load(std::memory_order_acquire);
    };
    // 进门就已经中止：一个请求都不发。直播下"看门狗先开火、hls 随后又要
    // 重拉一次播放列表"是真实顺序（hls 的重拉循环要下一次 ff_check_interrupt
    // 才看得到中止），这时候再去建一条注定被取消的连接纯属浪费。
    if (aborted()) {
        out.status = SYP_ERR_CANCELED;
        return out;
    }

    FetchState st;

    syp_http_request req{};
    req.url                = url.c_str();
    req.range_start        = 0;
    req.range_end          = -1;          // 整个资源
    req.connect_timeout_ms = connect_timeout_ms;
    req.read_timeout_ms    = read_timeout_ms;

    syp_response_sink sink{};
    sink.ctx         = &st;
    sink.on_response = on_response;
    sink.on_data     = on_data;
    sink.on_redirect = on_redirect;
    sink.on_complete = on_complete;

    syp_http_request_handle* h = backend->create(backend->backend_ctx, &req, &sink);
    if (h == nullptr) {
        out.status = SYP_ERR_IO;
        return out;
    }
    backend->start(h);

    {
        std::unique_lock<std::mutex> lk(st.mu);
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(read_timeout_ms > 0 ? read_timeout_ms : 15000);

        // 【为什么是分片轮询而不是一次 wait_until(deadline)】中止标志没有
        // 配套的 cv——它是 HlsSession 的 std::atomic<bool>，可能在任意线程
        // 被任意时刻置位，而 std::atomic::wait/notify 在本项目是禁用的
        // （iOS 13 下限）。要让看门狗在读超时（可能是 15~30 秒）到期之前
        // 就把这次抓取打断，唯一的办法是周期性回看。abort 为 nullptr 时
        // 退化成一次整段的等待，与本函数原来的行为逐字相同。
        bool timed_out = false;
        bool canceled  = false;
        while (!st.done) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { timed_out = true; break; }
            if (aborted())       { canceled  = true; break; }
            auto until = deadline;
            if (abort != nullptr) {
                const auto poll = now + std::chrono::milliseconds(kAbortPollMs);
                if (poll < until) until = poll;
            }
            st.cv.wait_until(lk, until, [&] { return st.done; });
        }

        if (timed_out || canceled) {
            // 取消，然后**必须等 on_complete 真的到达**再走。理由见
            // playlist_fetcher.h 里 fetch_playlist 声明上方的完整注释——
            // 简述：(1) syp_http.h 的 destroy() 契约要求调用前已收到
            // on_complete，当前 Apple 后端的 destroy() 会内部兜底等待，
            // 但那是该实现的额外保险，不是接口保证；(2) Apple 后端的超时是
            // 滚动空闲计时器，慢速持续来数据时永不自己触发，这里的绝对
            // 截止时间是唯一真正兑现 read_timeout_ms 承诺的地方。
            // 中止那条走同一段收尾，理由完全一样——差别只在最终报哪个码。
            lk.unlock();
            backend->cancel(h);
            lk.lock();
            st.cv.wait(lk, [&] { return st.done; });
            // 【中止优先于超时，也优先于后端自己报的码】cancel() 之后后端
            // 大概率会以 SYP_ERR_CANCELED 回调，但那不是保证；而调用方要
            // 分辨的恰恰是"是我自己叫停的"还是"网络真的超时了"。
            //
            // 【`if (canceled)` 这一支当前无覆盖，是纯防御——如实标注】把它整条去掉，
            // test_hls_playlist_fetcher 8/8 与 test_hls_e2e 11/11 全绿。
            // 原因是唯一的后端实现（Apple/NSURLSession）在 cancel() 之后
            // **恰好**也以 SYP_ERR_CANCELED 回调，于是 st.status 本来就
            // 已经是对的。
            // 要真正覆盖它，需要一个"cancel() 之后报别的码"的 stub 后端；
            // 而 fetch_playlist 存在的理由就是把**异步真后端**包成一次阻塞
            // 调用，本文件的用例刻意全部用真后端（见 test_hls_playlist_
            // fetcher.cpp 里 backend() 上方那段）——为这一支单独引进一个
            // stub 会把那条纪律撕开一个口子，代价大于收益。
            // 留着它的理由是接口契约：syp_http.h 没有承诺 cancel() 后一定
            // 报 CANCELED，换一个后端（未来的 Android/Linux 实现）完全可以
            // 合规地报 IO 或 TIMEOUT，那时少了这一支，调用方就会把一次
            // 主动中止误读成网络故障。
            if (canceled)                   st.status = SYP_ERR_CANCELED;
            else if (st.status == SYP_OK)   st.status = SYP_ERR_TIMEOUT;
        }
        out.status      = st.status;
        out.http_status = st.http_status;
        // 【m4：中止优先于 too_large —— 与上面那条纪律对齐】
        // 这里原来是 `if (st.too_large) out.status = SYP_ERR_OOM;`，
        // **无条件**覆盖，包括刚刚在几行之上被刻意定成 SYP_ERR_CANCELED
        // 的那个值。那跟紧邻的「中止优先于超时、也优先于后端自己报的码」
        // 直接冲突：一次用户主动中止会被报成"播放列表太大"，正是本项目
        // 反复在堵的那类误归因。判据统一成一条——**中止赢过一切**，
        // too_large 赢过后端自报的码（它是我们在传输途中自己看出来的，
        // 比后端那个笼统的码精确），HTTP 状态码兜底。
        //
        // 【当前无覆盖，按 #40/#41 的约定如实标注】要触发它得让"取回被
        // cancel"与"体积超限"同时成立，而本文件的用例刻意只用真后端
        // （见 fetch_playlist 上方那段），造不出这个交错。留着的理由是
        // 契约一致性，不是指望它被测到。
        if (st.too_large && out.status != SYP_ERR_CANCELED) {
            out.status = SYP_ERR_OOM;
        } else if (out.status == SYP_OK
                 && (out.http_status < 200 || out.http_status >= 300)) {
            out.status = SYP_ERR_HTTP_STATUS;
        }
        if (out.status == SYP_OK) out.body = std::move(st.body);
    }

    backend->destroy(h);
    return out;
}

}  // namespace syp::media::hls
