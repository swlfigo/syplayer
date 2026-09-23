// stub_backend.h — syp_http_backend 的纯内存桩：同步泵 / 异步线程，契约自查
#pragma once

#include "support/synthetic_byte.h"

#include <syplayer/syp_http.h>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace syp::dl::test {

class StubBackend {
public:
    enum class Mode { Sync, Async };

    enum class HeaderPolicy : uint8_t { Auto, Omit, Custom };

    struct Script {
        int64_t resource_length     = 1024;
        bool    support_range       = true;
        int32_t http_status         = 206;
        std::string etag;
        std::string last_modified;
        HeaderPolicy content_length = HeaderPolicy::Auto;
        std::string  content_length_custom;
        HeaderPolicy content_range  = HeaderPolicy::Auto;
        std::string  content_range_custom;
        // on_response 的 total_length / content_length 参数。true 则传 -1，
        // 用来模拟真实后端「206 缺 Content-Range / 200 chunked 无 CL」。
        bool unknown_total_length   = false;
        bool unknown_content_length = false;
        std::vector<std::string> redirect_urls;
        int64_t break_after_bytes   = -1;   // -1 = 不断；发够后 on_complete(NETWORK)
        int64_t max_body_bytes      = -1;   // -1 = 按 Range 发满；否则发这么多后 on_complete(OK)
        bool    timeout             = false;
        int32_t chunk_size          = 4096; // 必须能设成 1
        // 非 200/206 时作为响应体发出；空则不发 body（保持旧行为）。
        std::string error_body;
        // 测试专用（配合 wait_paused()/release_paused()）：Body 阶段第一次
        // sink.on_data(...) 调用返回之后、检查是否被取消之前，阻塞一次。
        // 只在 Async 模式下有意义——用来把"cancel 落在 on_data 已经交付、
        // 但后端自己的收尾还没检查取消状态"这个窗口钉死，构造确定性的
        // 跨线程交错（见 test_scheduler.cpp 里回归 dl 层级重入路径的用例）。仅第一次
        // on_data 生效；false 时对现有用例零影响。
        bool pause_after_first_on_data_once = false;
    };

    struct CapturedRequest {
        std::string url;
        int64_t     range_start = 0;
        int64_t     range_end   = -1;
        int32_t     connect_timeout_ms = 0;
        int32_t     read_timeout_ms    = 0;
        // create 时按值拷贝的请求头（name, value），顺序同 req->headers。
        std::vector<std::pair<std::string, std::string>> headers;
    };

    StubBackend();
    ~StubBackend();
    StubBackend(const StubBackend&)            = delete;
    StubBackend& operator=(const StubBackend&) = delete;

    const syp_http_backend* backend() const noexcept { return &table_; }

    void set_mode(Mode m);
    void set_default_script(Script s);

    // 「URL → 完整字节」表。**这张表非空时桩换一种工作方式**：
    //   · URL 命中 ⇒ 按这份字节应答，resource_length 自动取它的长度
    //     （Script 里其余字段——状态码、Range 支持、chunk_size、断流点——
    //      仍然照常生效，所以既有的那些失效注入对资源表同样可用）；
    //   · URL 未命中 ⇒ 一律 404。
    // 表为空时行为与以前逐字节相同（Script::resource_length + 合成字节），
    // 既有用例零影响。
    //
    // 【为什么必须补这张表，不能用既有的 Script】Script 只能表达"一份长度
    // 为 N 的合成资源"（synthetic_byte.h 的公式），它按**请求序号**挂脚本
    // 而不是按 URL，也拿不出真实文件字节。而这里要喂给 FFmpeg 的是
    // 真的 m3u8 文本 + 真的 fMP4 分片，且"哪个 URL 给哪份字节"必须由 URL
    // 决定——请求顺序是 FFmpeg 内部的事，不是用例能钉死的。
    void add_resource(std::string url, std::vector<uint8_t> bytes);

    // 这个 URL 被 create() 过几次（含未 start 的）。
    int requests_for(const std::string& url) const;
    // 第 n 次 create（从 1 开始）使用该脚本；未指定的走 default。
    void set_script_for_request(int n, Script s);

    // 同步泵：推进一步（一次回调）。没有在途请求时返回 false。
    bool pump();
    void pump_all();
    // 只推进第 n1 次 create 的请求一步（1-based，与 request_at(n1-1) 同一个）。
    // 用于构造"多条连接速度不同"的确定性场景。该请求已完成、未 start
    // 或不存在时返回 false。
    bool pump_request(int n1);
    // true：pump 在所有在途请求里均匀抽一个（乱序到达）。false：永远排干
    // live_ 里第一个（默认，确定性用例依赖这个顺序）。
    void set_random_pump(bool on, uint32_t seed = 1);

    int              request_count() const;
    CapturedRequest  request_at(int i) const;  // 0-based
    // 实际交给 sink.on_data 的字节合计。错误页测试用来证明桩真的发了 body。
    int64_t          sink_on_data_bytes() const;
    // trampoline_cancel 被调用的总次数（含 complete 之后的 no-op cancel）。
    // 预连接用例用它证明"206 路径上一次 cancel 都没发"。
    int              cancel_calls() const;

    // 与 Script::pause_after_first_on_data_once 配对的跨线程闩锁。
    // wait_paused()：阻塞至某个 worker 线程进入暂停点（已经把第一块
    // 数据交给 sink.on_data 并返回，还没检查是否被取消）；超时返回 false。
    // release_paused()：放行被暂停的 worker 线程，可在暂停发生之前调用
    // （标准的"先置位再等谓词"写法，不要求调用顺序）。全局只支持一次
    // 暂停/放行（本文件的用例每条用一个 StubBackend 实例，够用）。
    bool wait_paused(int timeout_ms = 8000);
    void release_paused();

    // 契约自查：on_complete 后再回调、handle 泄漏、cancel 后没 complete。
    // cancel 计数在 destroy 之后仍保留，所以「create→cancel（未 start）→destroy」
    // 这种活句柄已经不在 live_ 里的违约也能查到。
    bool contract_ok(std::string* message = nullptr) const;

private:
    struct Req;

    static syp_http_request_handle* trampoline_create(
        void* ctx, const syp_http_request* req, const syp_response_sink* sink);
    static void trampoline_start(syp_http_request_handle* h);
    static void trampoline_cancel(syp_http_request_handle* h);
    static void trampoline_destroy(syp_http_request_handle* h);

    std::shared_ptr<Req> lock_req(syp_http_request_handle* h);
    Script script_for_index_locked(int n1) const;  // n1 从 1
    bool   step(const std::shared_ptr<Req>& r);
    void   complete(const std::shared_ptr<Req>& r, syp_status st, int32_t http);
    void   note_violation(const char* msg);
    // 发出 on_response / on_data / on_redirect 之前：已 complete 则记违约并返回 false。
    bool   prepare_sink_callback(const std::shared_ptr<Req>& r);
    void   spawn_worker(const std::shared_ptr<Req>& r);  // 调用方持有 mu_
    std::shared_ptr<Req> pick_pumpable();

    syp_http_backend table_{};
    // shared_ptr：worker 可能在 ~StubBackend 之后仍锁一次；持有副本避免
    // mutex lock failed: Invalid argument。
    mutable std::shared_ptr<std::mutex> mu_ = std::make_shared<std::mutex>();
    Mode   mode_ = Mode::Sync;
    Script default_script_{};
    std::map<int, Script> per_request_;  // 1-based
    // shared_ptr：Req 在 worker 线程上按 URL 持有一份，不跟着表拷贝字节。
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> resources_;
    int    next_request_index_ = 1;
    std::vector<std::shared_ptr<Req>> live_;
    std::vector<CapturedRequest> captured_;
    std::vector<std::string> violations_;
    int created_n_   = 0;
    int destroyed_n_ = 0;
    int64_t sink_on_data_bytes_ = 0;
    int     cancel_calls_ = 0;
    bool     random_pump_ = false;
    std::mt19937 pump_rng_{};
    // 第一次在未 complete 时 cancel 的次数 / 这些请求随后 on_complete 的次数。
    // 不相等即「cancel 后没 complete」，即使 handle 已 destroy 也能查到。
    int canceled_need_complete_ = 0;
    int canceled_got_complete_  = 0;
    // destroy 在 worker 自己的线程上不能 join；挂在这里，~StubBackend 再收。
    std::vector<std::thread> join_later_;
    int workers_live_ = 0;

    // pause_after_first_on_data_once 的实现：独立于 mu_，避免暂停期间
    // worker 线程持锁堵住其它请求。pause_armed_ 保证全局只暂停一次。
    mutable std::mutex     pause_mu_;
    std::condition_variable pause_cv_;
    bool pause_armed_    = true;   // 尚未消费过一次暂停配额
    bool pause_entered_  = false;
    bool pause_release_  = false;
};

}  // namespace syp::dl::test
