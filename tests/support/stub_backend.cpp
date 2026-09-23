// stub_backend.cpp — 内存 HTTP 桩。回调串行；on_complete 之后不再回调。
#include "stub_backend.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <utility>

namespace syp::dl::test {
namespace {

struct HeaderSet {
    std::vector<std::string> names;
    std::vector<std::string> values;
    std::vector<const char*> name_c;
    std::vector<const char*> value_c;

    void add(std::string n, std::string v) {
        names.push_back(std::move(n));
        values.push_back(std::move(v));
    }
    syp_headers view() {
        name_c.clear();
        value_c.clear();
        name_c.reserve(names.size());
        value_c.reserve(values.size());
        for (size_t i = 0; i < names.size(); ++i) {
            name_c.push_back(names[i].c_str());
            value_c.push_back(values[i].c_str());
        }
        syp_headers h{};
        h.names  = name_c.empty() ? nullptr : name_c.data();
        h.values = value_c.empty() ? nullptr : value_c.data();
        h.count  = static_cast<int32_t>(names.size());
        return h;
    }
};

std::string fmt_i64(int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%" PRId64, v);
    return std::string(buf);
}

}  // namespace

struct StubBackend::Req {
    StubBackend*        owner = nullptr;
    syp_response_sink   sink{};
    Script              script{};
    CapturedRequest     captured{};
    enum class Phase {
        Created,
        Running,
        Redirecting,
        Responding,
        Body,
        Completing,
        Done
    };
    Phase   phase           = Phase::Created;
    int     redirect_i      = 0;
    int64_t body_start      = 0;   // 绝对偏移，含
    int64_t body_end        = 0;   // 绝对偏移，不含
    int64_t sent            = 0;   // 已回调的 body 字节
    int64_t break_at        = -1;
    int32_t chunk           = 4096;
    int32_t http_status     = 206;
    syp_status complete_st  = SYP_OK;
    int32_t complete_http   = 0;
    bool    canceled        = false;
    bool    completed       = false;
    bool    destroyed       = false;
    bool    in_callback     = false;
    bool    started         = false;
    int     index1          = 0;      // 第几次 create（1-based），pump_request 用
    bool    cancel_counted  = false;  // 未 complete 时第一次 cancel 已计入契约计数
    bool    sending_error_body = false;
    std::string error_body;
    // 非空 ⇒ body 从这份真实字节里发，而不是 synthetic_byte() 合成。
    // create() 时按 URL 定好，之后只读。
    std::shared_ptr<const std::vector<uint8_t>> resource;
    std::thread worker;
};

StubBackend::StubBackend() {
    table_.backend_ctx = this;
    table_.create  = &StubBackend::trampoline_create;
    table_.start   = &StubBackend::trampoline_start;
    table_.cancel  = &StubBackend::trampoline_cancel;
    table_.destroy = &StubBackend::trampoline_destroy;
}

StubBackend::~StubBackend() {
    for (;;) {
        std::vector<std::shared_ptr<Req>> leftover;
        std::vector<std::thread> later;
        {
            std::lock_guard<std::mutex> g(*mu_);
            leftover.swap(live_);
            later.swap(join_later_);
            for (const auto& r : leftover) {
                if (r && !r->destroyed) {
                    violations_.push_back("handle not destroyed before StubBackend dtor");
                    r->destroyed = true;
                }
                if (r) {
                    r->phase = Req::Phase::Done;
                    r->canceled = true;
                }
            }
        }
        for (auto& r : leftover) {
            std::thread w;
            {
                std::lock_guard<std::mutex> g(*mu_);
                w = std::move(r->worker);
            }
            if (w.joinable() && w.get_id() != std::this_thread::get_id()) w.join();
            else if (w.joinable()) {
                std::lock_guard<std::mutex> g(*mu_);
                join_later_.push_back(std::move(w));
            }
        }
        for (auto& w : later) {
            if (w.joinable() && w.get_id() != std::this_thread::get_id()) w.join();
        }
        {
            std::lock_guard<std::mutex> g(*mu_);
            if (live_.empty() && join_later_.empty() && workers_live_ == 0) break;
        }
        std::this_thread::yield();
    }
}

void StubBackend::set_mode(Mode m) {
    std::lock_guard<std::mutex> g(*mu_);
    mode_ = m;
}

bool StubBackend::wait_paused(int timeout_ms) {
    std::unique_lock<std::mutex> lk(pause_mu_);
    return pause_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                              [this] { return pause_entered_; });
}

void StubBackend::release_paused() {
    std::lock_guard<std::mutex> g(pause_mu_);
    pause_release_ = true;
    pause_cv_.notify_all();
}

void StubBackend::set_random_pump(bool on, uint32_t seed) {
    std::lock_guard<std::mutex> g(*mu_);
    random_pump_ = on;
    pump_rng_.seed(seed);
}

void StubBackend::set_default_script(Script s) {
    if (s.chunk_size <= 0) s.chunk_size = 1;
    std::lock_guard<std::mutex> g(*mu_);
    default_script_ = std::move(s);
}

void StubBackend::set_script_for_request(int n, Script s) {
    if (s.chunk_size <= 0) s.chunk_size = 1;
    std::lock_guard<std::mutex> g(*mu_);
    per_request_[n] = std::move(s);
}

void StubBackend::add_resource(std::string url, std::vector<uint8_t> bytes) {
    auto p = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    std::lock_guard<std::mutex> g(*mu_);
    resources_[std::move(url)] = std::move(p);
}

int StubBackend::requests_for(const std::string& url) const {
    std::lock_guard<std::mutex> g(*mu_);
    int n = 0;
    for (const CapturedRequest& c : captured_) {
        if (c.url == url) ++n;
    }
    return n;
}

StubBackend::Script StubBackend::script_for_index_locked(int n1) const {
    auto it = per_request_.find(n1);
    if (it != per_request_.end()) return it->second;
    return default_script_;
}

void StubBackend::note_violation(const char* msg) {
    violations_.emplace_back(msg);
}

bool StubBackend::prepare_sink_callback(const std::shared_ptr<Req>& r) {
    std::lock_guard<std::mutex> g(*mu_);
    if (r->completed) {
        note_violation("callback after on_complete");
        r->in_callback = false;
        return false;
    }
    return true;
}

std::shared_ptr<StubBackend::Req> StubBackend::lock_req(syp_http_request_handle* h) {
    auto* raw = reinterpret_cast<Req*>(h);
    std::lock_guard<std::mutex> g(*mu_);
    for (auto& r : live_) {
        if (r.get() == raw) return r;
    }
    return nullptr;
}

syp_http_request_handle* StubBackend::trampoline_create(
    void* ctx, const syp_http_request* req, const syp_response_sink* sink) {
    auto* self = static_cast<StubBackend*>(ctx);
    if (req == nullptr || sink == nullptr) return nullptr;
    auto r = std::make_shared<Req>();
    r->owner = self;
    r->sink  = *sink;
    {
        std::lock_guard<std::mutex> g(*self->mu_);
        r->script = self->script_for_index_locked(self->next_request_index_);
        r->index1 = self->next_request_index_;
        ++self->next_request_index_;
        r->captured.url = req->url ? req->url : "";
        r->captured.range_start = req->range_start;
        r->captured.range_end   = req->range_end;
        r->captured.connect_timeout_ms = req->connect_timeout_ms;
        r->captured.read_timeout_ms    = req->read_timeout_ms;
        const syp_headers& rh = req->headers;
        if (rh.names != nullptr && rh.values != nullptr) {
            for (int32_t i = 0; i < rh.count; ++i) {
                const size_t k = static_cast<size_t>(i);
                r->captured.headers.emplace_back(rh.names[k] ? rh.names[k] : "",
                                                 rh.values[k] ? rh.values[k] : "");
            }
        }
        // 资源表非空 ⇒ 由 URL（而不是请求序号）决定发什么。未命中的 URL
        // 一律 404：这正是"真实网络不可达"在桩这一侧的表达。
        if (!self->resources_.empty()) {
            auto it = self->resources_.find(r->captured.url);
            if (it != self->resources_.end()) {
                r->resource = it->second;
                r->script.resource_length = static_cast<int64_t>(it->second->size());
            } else {
                r->script.http_status     = 404;
                r->script.resource_length = 0;
            }
        }
        self->captured_.push_back(r->captured);
        ++self->created_n_;
        self->live_.push_back(r);
    }
    return reinterpret_cast<syp_http_request_handle*>(r.get());
}

void StubBackend::trampoline_start(syp_http_request_handle* h) {
    auto* raw = reinterpret_cast<Req*>(h);
    if (raw == nullptr || raw->owner == nullptr) return;
    StubBackend* self = raw->owner;
    std::shared_ptr<Req> r = self->lock_req(h);
    if (!r) return;
    {
        std::lock_guard<std::mutex> g(*self->mu_);
        if (r->started || r->destroyed || r->completed || r->canceled) return;
        r->started = true;
        r->phase = Req::Phase::Running;
        // 必须在同一把锁里把 worker 存进 r->worker。若先放锁再 spawn，
        // cancel/destroy 会看到 started 却 join 不到线程。
        if (self->mode_ == Mode::Async) self->spawn_worker(r);
    }
}

void StubBackend::trampoline_cancel(syp_http_request_handle* h) {
    auto* raw = reinterpret_cast<Req*>(h);
    if (raw == nullptr || raw->owner == nullptr) return;
    StubBackend* self = raw->owner;
    std::shared_ptr<Req> r = self->lock_req(h);
    if (!r) return;

    bool emit_now = false;
    {
        std::lock_guard<std::mutex> g(*self->mu_);
        ++self->cancel_calls_;
        r->canceled = true;
        if (r->completed) return;
        if (!r->cancel_counted) {
            r->cancel_counted = true;
            ++self->canceled_need_complete_;
        }
        // 同步且不在回调里：cancel 自己完成，避免析构在没人 pump 时卡死。
        // 异步已 start：只由 worker 投递，保证同一请求串行。
        if (self->mode_ == Mode::Sync && !r->in_callback && r->started) {
            emit_now = true;
        }
        if (!r->started && !r->completed) {
            // create 了但还没 start：没有 worker 会被 spawn，同步和异步
            // 都必须在这里 complete，否则 ~DLTask 的 cv_.wait 永久阻塞。
            emit_now = true;
        }
    }
    if (emit_now) self->complete(r, SYP_ERR_CANCELED, 0);
}

void StubBackend::trampoline_destroy(syp_http_request_handle* h) {
    auto* raw = reinterpret_cast<Req*>(h);
    if (raw == nullptr || raw->owner == nullptr) return;
    StubBackend* self = raw->owner;
    std::shared_ptr<Req> r;
    std::thread w;
    {
        std::lock_guard<std::mutex> g(*self->mu_);
        for (auto it = self->live_.begin(); it != self->live_.end(); ++it) {
            if (it->get() == raw) {
                r = *it;
                self->live_.erase(it);
                break;
            }
        }
        if (!r) {
            self->note_violation("destroy of unknown handle");
            return;
        }
        if (r->destroyed) {
            self->note_violation("double destroy");
            return;
        }
        if (!r->completed) {
            self->note_violation("destroy before on_complete");
        }
        r->destroyed = true;
        ++self->destroyed_n_;
        r->phase = Req::Phase::Done;
        w = std::move(r->worker);
        if (w.joinable() && w.get_id() == std::this_thread::get_id()) {
            self->join_later_.push_back(std::move(w));
        }
    }
    if (w.joinable()) {
        if (w.get_id() != std::this_thread::get_id()) w.join();
        else {
            std::lock_guard<std::mutex> g(*self->mu_);
            self->join_later_.push_back(std::move(w));
        }
    }
}

void StubBackend::spawn_worker(const std::shared_ptr<Req>& r) {
    // 调用方必须持有 mu_。新线程首个动作再取 mu_，保证 assignment 对
    // destroy 的 joinable()/join 可见。
    if (r->destroyed || r->completed || r->worker.joinable()) return;
    ++workers_live_;
    std::shared_ptr<std::mutex> mu = mu_;
    try {
        r->worker = std::thread([this, r, mu] {
            { std::lock_guard<std::mutex> ready(*mu); }
            while (true) {
                bool done = false;
                {
                    std::lock_guard<std::mutex> g(*mu);
                    done = (r->phase == Req::Phase::Done || r->destroyed || r->completed);
                }
                if (done) break;
                if (!step(r)) break;
                std::this_thread::yield();
            }
            std::lock_guard<std::mutex> g(*mu);
            --workers_live_;
        });
    } catch (...) {
        --workers_live_;
        throw;
    }
}

void StubBackend::complete(const std::shared_ptr<Req>& r, syp_status st, int32_t http) {
    syp_response_sink sink{};
    {
        std::lock_guard<std::mutex> g(*mu_);
        if (r->completed) return;
        if (r->destroyed) {
            note_violation("complete after destroy");
            return;
        }
        r->completed = true;
        r->phase = Req::Phase::Done;
        r->complete_st = st;
        r->complete_http = http;
        if (r->cancel_counted) ++canceled_got_complete_;
        r->in_callback = true;
        sink = r->sink;
    }
    if (sink.on_complete != nullptr) {
        sink.on_complete(sink.ctx, st, http);
    }
    {
        std::lock_guard<std::mutex> g(*mu_);
        r->in_callback = false;
    }
}

bool StubBackend::step(const std::shared_ptr<Req>& r) {
    syp_response_sink sink{};
    Req::Phase phase;
    bool canceled = false;
    Script script;
    CapturedRequest cap;
    {
        std::lock_guard<std::mutex> g(*mu_);
        if (r->destroyed || r->completed || r->phase == Req::Phase::Done) return false;
        canceled = r->canceled;
        phase = r->phase;
        sink = r->sink;
        script = r->script;
        cap = r->captured;
        if (canceled) {
            r->in_callback = true;
        } else {
            r->in_callback = true;
        }
    }

    if (canceled) {
        {
            std::lock_guard<std::mutex> g(*mu_);
            r->in_callback = false;
        }
        complete(r, SYP_ERR_CANCELED, 0);
        return false;
    }

    auto end_cb = [&] {
        std::lock_guard<std::mutex> g(*mu_);
        r->in_callback = false;
        const bool c = r->canceled && !r->completed;
        return c;
    };

    if (phase == Req::Phase::Running) {
        if (script.timeout) {
            if (end_cb()) {
                complete(r, SYP_ERR_CANCELED, 0);
                return false;
            }
            complete(r, SYP_ERR_TIMEOUT, 0);
            return false;
        }
        {
            std::lock_guard<std::mutex> g(*mu_);
            r->phase = script.redirect_urls.empty() ? Req::Phase::Responding
                                                    : Req::Phase::Redirecting;
            r->in_callback = false;
        }
        return true;
    }

    if (phase == Req::Phase::Redirecting) {
        std::string url;
        {
            std::lock_guard<std::mutex> g(*mu_);
            if (r->redirect_i >= static_cast<int>(script.redirect_urls.size())) {
                r->phase = Req::Phase::Responding;
                r->in_callback = false;
                return true;
            }
            url = script.redirect_urls[static_cast<size_t>(r->redirect_i)];
            ++r->redirect_i;
        }
        bool follow = true;
        if (sink.on_redirect != nullptr) {
            if (!prepare_sink_callback(r)) return false;
            follow = sink.on_redirect(sink.ctx, url.c_str());
        }
        const bool want_cancel = end_cb();
        if (!follow) {
            complete(r, SYP_ERR_CANCELED, 0);
            return false;
        }
        if (want_cancel) {
            complete(r, SYP_ERR_CANCELED, 0);
            return false;
        }
        return true;
    }

    if (phase == Req::Phase::Responding) {
        int64_t serve_start = 0;
        int64_t serve_end   = 0;  // exclusive
        int32_t status      = script.http_status;
        const int64_t L = std::max<int64_t>(script.resource_length, 0);

        if (status == 200 || (!script.support_range && (status == 206))) {
            status = 200;
            serve_start = 0;
            serve_end = L;
        } else if (status == 206) {
            serve_start = cap.range_start < 0 ? 0 : cap.range_start;
            if (cap.range_end < 0) serve_end = L;
            else {
                if (cap.range_end >= std::numeric_limits<int64_t>::max()) {
                    serve_end = L;
                } else {
                    serve_end = cap.range_end + 1;
                }
            }
            if (serve_start < 0) serve_start = 0;
            if (serve_end > L) serve_end = L;
            if (serve_start > L) serve_start = L;
            if (serve_end < serve_start) serve_end = serve_start;
        } else {
            serve_start = 0;
            serve_end = static_cast<int64_t>(script.error_body.size());
        }

        HeaderSet hs;
        if (!script.etag.empty()) hs.add("ETag", script.etag);
        if (!script.last_modified.empty()) hs.add("Last-Modified", script.last_modified);

        const int64_t body_len = serve_end - serve_start;
        if (script.content_length == HeaderPolicy::Auto) {
            if (status == 200 || status == 206) hs.add("Content-Length", fmt_i64(body_len));
            else if (body_len > 0) hs.add("Content-Length", fmt_i64(body_len));
        } else if (script.content_length == HeaderPolicy::Custom) {
            hs.add("Content-Length", script.content_length_custom);
        }

        if (script.content_range == HeaderPolicy::Auto) {
            if (status == 206 && body_len > 0) {
                hs.add("Content-Range",
                       "bytes " + fmt_i64(serve_start) + "-" + fmt_i64(serve_end - 1)
                           + "/" + fmt_i64(L));
            } else if (status == 206 && body_len == 0 && L >= 0) {
                hs.add("Content-Range", "bytes */" + fmt_i64(L));
            } else if (status == 416) {
                hs.add("Content-Range", "bytes */" + fmt_i64(L));
            }
        } else if (script.content_range == HeaderPolicy::Custom) {
            hs.add("Content-Range", script.content_range_custom);
        }

        const syp_headers view = hs.view();
        const int64_t cl_arg = script.unknown_content_length
            ? int64_t{-1}
            : ((status == 200 || status == 206) ? body_len : int64_t{-1});
        const int64_t tot_arg = script.unknown_total_length
            ? int64_t{-1}
            : ((status == 206) ? L : (status == 200 ? L : int64_t{-1}));

        {
            std::lock_guard<std::mutex> g(*mu_);
            r->http_status = status;
            r->body_start = serve_start;
            r->body_end   = serve_end;
            r->sent       = 0;
            r->break_at   = script.break_after_bytes;
            r->chunk      = script.chunk_size > 0 ? script.chunk_size : 1;
            r->error_body = script.error_body;
            r->sending_error_body =
                (status != 200 && status != 206) && !script.error_body.empty();
            if (script.max_body_bytes >= 0) {
                const int64_t cap_end = serve_start + script.max_body_bytes;
                if (r->body_end > cap_end) r->body_end = cap_end;
            }
        }

        if (sink.on_response != nullptr) {
            if (!prepare_sink_callback(r)) return false;
            sink.on_response(sink.ctx, status, &view, cl_arg, tot_arg);
        }
        const bool want_cancel = end_cb();
        if (want_cancel) {
            complete(r, SYP_ERR_CANCELED, 0);
            return false;
        }

        {
            std::lock_guard<std::mutex> g(*mu_);
            if (status == 200 || status == 206) {
                r->phase = Req::Phase::Body;
            } else {
                r->complete_st = SYP_ERR_HTTP_STATUS;
                r->complete_http = status;
                r->phase = r->sending_error_body ? Req::Phase::Body
                                                 : Req::Phase::Completing;
            }
        }
        return true;
    }

    if (phase == Req::Phase::Body) {
        int64_t start = 0, sent = 0, end = 0, break_at = -1;
        int32_t chunk = 1;
        {
            std::lock_guard<std::mutex> g(*mu_);
            start = r->body_start;
            sent  = r->sent;
            end   = r->body_end;
            break_at = r->break_at;
            chunk = r->chunk;
        }
        const int64_t remaining = end - (start + sent);
        if (remaining <= 0) {
            if (end_cb()) {
                complete(r, SYP_ERR_CANCELED, 0);
                return false;
            }
            {
                std::lock_guard<std::mutex> g(*mu_);
                r->phase = Req::Phase::Completing;
                if (!r->sending_error_body) {
                    r->complete_st = SYP_OK;
                    r->complete_http = 0;
                }
            }
            return true;
        }

        int64_t n = remaining;
        if (n > static_cast<int64_t>(chunk)) n = chunk;
        if (break_at >= 0) {
            const int64_t left_to_break = break_at - sent;
            if (left_to_break <= 0) {
                if (end_cb()) {
                    complete(r, SYP_ERR_CANCELED, 0);
                    return false;
                }
                complete(r, SYP_ERR_NETWORK, 0);
                return false;
            }
            if (n > left_to_break) n = left_to_break;
        }

        std::vector<uint8_t> buf(static_cast<size_t>(n));
        const int64_t off = start + sent;
        std::string error_copy;
        bool use_error = false;
        std::shared_ptr<const std::vector<uint8_t>> res;
        {
            std::lock_guard<std::mutex> g(*mu_);
            use_error = r->sending_error_body;
            if (use_error) error_copy = r->error_body;
            res = r->resource;
        }
        if (use_error) {
            for (int64_t i = 0; i < n; ++i) {
                buf[static_cast<size_t>(i)] = static_cast<uint8_t>(
                    static_cast<unsigned char>(error_copy[static_cast<size_t>(off + i)]));
            }
        } else if (res) {
            // body_end 已按 resource_length（= res->size()）夹过，越界只可能
            // 是桩自己算错了——当成契约违约记下来，别静默读越界。
            if (off < 0 || off + n > static_cast<int64_t>(res->size())) {
                // 发 0 会让 Body 阶段原地打转，所以照发同样长度的零字节，
                // 只把违约记下来——contract_ok() 会让用例红，而不是挂死。
                std::lock_guard<std::mutex> g(*mu_);
                note_violation("resource body range out of bounds");
            } else {
                std::memcpy(buf.data(), res->data() + off, static_cast<size_t>(n));
            }
        } else {
            for (int64_t i = 0; i < n; ++i) {
                buf[static_cast<size_t>(i)] = synthetic_byte(off + i);
            }
        }
        {
            std::lock_guard<std::mutex> g(*mu_);
            r->sent = sent + n;
        }
        if (sink.on_data != nullptr) {
            if (!prepare_sink_callback(r)) return false;
            sink.on_data(sink.ctx, buf.data(), static_cast<int32_t>(n));
            {
                std::lock_guard<std::mutex> g(*mu_);
                sink_on_data_bytes_ += n;
            }
        }
        // 思路 3（dl 层级）的确定性交错点：on_data 已经返回（不再持有任何
        // Scheduler 侧的回调记账，见 SlotCbGuard），但本请求自己的收尾
        // （下面的 end_cb() 检查取消状态）还没跑。真实后端（Apple 的
        // Handle::emit_data）在这个位置也是"数据已经交给 dl 层、收尾检查
        // 还没做"，cancel 落在这个窗口内正是原始死锁的触发条件。
        // 只在脚本要求、且全局配额未消费时暂停一次；不要求调用方先
        // wait_paused() 再 release_paused()——两者顺序不敏感。
        bool do_pause = false;
        if (script.pause_after_first_on_data_once) {
            std::lock_guard<std::mutex> pg(pause_mu_);
            if (pause_armed_) {
                pause_armed_ = false;
                do_pause = true;
            }
        }
        if (do_pause) {
            std::unique_lock<std::mutex> lk(pause_mu_);
            pause_entered_ = true;
            pause_cv_.notify_all();
            pause_cv_.wait(lk, [this] { return pause_release_; });
        }
        const bool want_cancel = end_cb();
        if (want_cancel) {
            complete(r, SYP_ERR_CANCELED, 0);
            return false;
        }
        if (break_at >= 0 && sent + n >= break_at) {
            complete(r, SYP_ERR_NETWORK, 0);
            return false;
        }
        return true;
    }

    if (phase == Req::Phase::Completing) {
        syp_status st = SYP_OK;
        int32_t http = 0;
        {
            std::lock_guard<std::mutex> g(*mu_);
            st = r->complete_st;
            http = r->complete_http;
            r->in_callback = false;
        }
        complete(r, st, http);
        return false;
    }

    {
        std::lock_guard<std::mutex> g(*mu_);
        r->in_callback = false;
    }
    return false;
}

std::shared_ptr<StubBackend::Req> StubBackend::pick_pumpable() {
    std::lock_guard<std::mutex> g(*mu_);
    std::vector<std::shared_ptr<Req>> cand;
    for (auto& r : live_) {
        if (r && r->started && !r->completed && !r->destroyed
            && r->phase != Req::Phase::Done) {
            if (!random_pump_) return r;
            cand.push_back(r);
        }
    }
    if (cand.empty()) return nullptr;
    std::uniform_int_distribution<size_t> dist(0, cand.size() - 1);
    return cand[dist(pump_rng_)];
}

bool StubBackend::pump_request(int n1) {
    std::shared_ptr<Req> r;
    {
        std::lock_guard<std::mutex> g(*mu_);
        for (auto& x : live_) {
            if (x && x->index1 == n1 && x->started && !x->completed && !x->destroyed
                && x->phase != Req::Phase::Done) {
                r = x;
                break;
            }
        }
    }
    if (!r) return false;
    step(r);
    return true;
}

bool StubBackend::pump() {
    auto r = pick_pumpable();
    if (!r) return false;
    step(r);
    return pick_pumpable() != nullptr;
}

void StubBackend::pump_all() {
    int guard = 0;
    while (pump()) {
        if (++guard > 10000000) {
            note_violation("pump_all exceeded step guard");
            break;
        }
    }
}

int StubBackend::request_count() const {
    std::lock_guard<std::mutex> g(*mu_);
    return static_cast<int>(captured_.size());
}

StubBackend::CapturedRequest StubBackend::request_at(int i) const {
    std::lock_guard<std::mutex> g(*mu_);
    if (i < 0 || i >= static_cast<int>(captured_.size())) return {};
    return captured_[static_cast<size_t>(i)];
}

int64_t StubBackend::sink_on_data_bytes() const {
    std::lock_guard<std::mutex> g(*mu_);
    return sink_on_data_bytes_;
}

int StubBackend::cancel_calls() const {
    std::lock_guard<std::mutex> g(*mu_);
    return cancel_calls_;
}

bool StubBackend::contract_ok(std::string* message) const {
    std::lock_guard<std::mutex> g(*mu_);
    if (!violations_.empty()) {
        if (message) *message = violations_.front();
        return false;
    }
    if (created_n_ != destroyed_n_) {
        if (message) {
            *message = "leaked handles: created=" + std::to_string(created_n_)
                       + " destroyed=" + std::to_string(destroyed_n_);
        }
        return false;
    }
    if (canceled_need_complete_ != canceled_got_complete_) {
        if (message) {
            *message = "cancel without on_complete: canceled="
                       + std::to_string(canceled_need_complete_)
                       + " completed=" + std::to_string(canceled_got_complete_);
        }
        return false;
    }
    for (const auto& r : live_) {
        if (r && r->canceled && !r->completed) {
            if (message) *message = "cancel without on_complete";
            return false;
        }
    }
    return true;
}

}  // namespace syp::dl::test
