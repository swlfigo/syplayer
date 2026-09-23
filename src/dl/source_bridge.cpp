// source_bridge.cpp — 落盘后再算已缓存；read 用 cv 等数据，seek 不阻塞
#include "source_bridge.h"

#include "cache_store.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <utility>

namespace syp::dl {
namespace {

constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();
constexpr int64_t kSaveEveryBytes = 256 * 1024;
constexpr int64_t kMiB = 1024 * 1024;
// enforce_capacity 的节流阈值。
constexpr int64_t kEnforceMinIntervalMs = 1000;
constexpr int64_t kEnforceEveryBytes    = 8 * 1024 * 1024;

int64_t sat_add(int64_t a, int64_t b) noexcept {
    if (b > 0 && a > kI64Max - b) return kI64Max;
    if (b < 0 && a < kI64Min - b) return kI64Min;
    return a + b;
}

int32_t clamp_i32_pos(int32_t v, int32_t fallback) noexcept {
    return v > 0 ? v : fallback;
}

// total * ms / duration，溢出时退化为先除后乘。
int64_t bytes_for_ms(int64_t total, int64_t duration_ms, int32_t ms) noexcept {
    if (total <= 0 || duration_ms <= 0 || ms <= 0) return 0;
    const int64_t ms64 = static_cast<int64_t>(ms);
    if (total > kI64Max / ms64) {
        return (total / duration_ms) * ms64;
    }
    return (total * ms64) / duration_ms;
}

// 缓存目录扫描（CacheDirEntry / scan_cache_dir / remove_cache_entry）与它
// 依赖的那几个文件名、大小、mtime helper 已经上提到 cache_store.cpp——
// cache_dir_evict() 与 CacheStore 的容量策略必须用同一套扫描与 LRU 判据，
// 两处各写一份必然漂移。只被它们用到的 status_from_ec / remove_path 一并
// 上提：本文件里已经没有别的调用方，留着会撞 -Wunused-function -Werror。

// 日志 / 全局 HTTP 后端。指针拷贝出去后调用方保证对象存活。
std::mutex      g_log_mu;
syp_log_fn      g_log_fn  = nullptr;
void*           g_log_ctx = nullptr;
syp_log_level   g_log_max = SYP_LOG_INFO;

std::mutex          g_backend_mu;
syp_http_backend    g_backend{};
bool                g_backend_set = false;

}  // namespace

void SourceBridge::HeaderCopy::rebuild_ptrs() {
    name_c.clear();
    value_c.clear();
    name_c.reserve(names.size());
    value_c.reserve(values.size());
    for (size_t i = 0; i < names.size(); ++i) {
        name_c.push_back(names[i].c_str());
        value_c.push_back(values[i].c_str());
    }
}

syp_headers SourceBridge::HeaderCopy::view() const noexcept {
    syp_headers h{};
    h.names  = name_c.empty() ? nullptr : name_c.data();
    h.values = value_c.empty() ? nullptr : value_c.data();
    h.count  = static_cast<int32_t>(names.size());
    return h;
}

void SourceBridge::HeaderCopy::assign(const syp_headers* h) {
    names.clear();
    values.clear();
    name_c.clear();
    value_c.clear();
    if (h == nullptr || h->count <= 0 || h->names == nullptr || h->values == nullptr) {
        return;
    }
    names.reserve(static_cast<size_t>(h->count));
    values.reserve(static_cast<size_t>(h->count));
    for (int32_t i = 0; i < h->count; ++i) {
        names.emplace_back(h->names[i] ? h->names[i] : "");
        values.emplace_back(h->values[i] ? h->values[i] : "");
    }
    rebuild_ptrs();
}

syp_config default_config() {
    syp_config c{};
    c.struct_size               = static_cast<uint32_t>(sizeof(syp_config));
    c.cache_dir                 = nullptr;
    c.max_cache_bytes           = 512 * kMiB;
    c.max_memory_bytes          = 16 * kMiB;
    c.min_free_space_bytes      = 256 * kMiB;
    c.cache_ttl_ms              = 7LL * 24 * 60 * 60 * 1000;
    c.max_concurrent_tasks      = 3;
    c.min_segment_size          = 512 * 1024;
    c.segment_size_hint         = 0;
    c.reuse_max_remaining_bytes = 1 * kMiB;
    c.connect_estimate_max_ms   = 2000;
    c.speed_window_ms           = 3000;
    c.connect_timeout_ms        = 10000;
    c.read_timeout_ms           = 15000;
    c.max_redirects             = 8;
    c.max_retries               = 3;
    // 保留字段，库不读取（见 syp_config.h 顶注）：填默认值只为 ABI 前向
    // 兼容，不代表这三个数会被任何代码路径消费。
    c.idle_task_keep            = 2;
    c.idle_task_ttl_ms          = 60000;
    c.enable_socket_pool        = true;
    c.first_buffer_ms           = 500;
    c.target_buffer_ms          = 10000;
    c.allow_no_range_fallback   = true;
    return c;
}

syp_config copy_config(const syp_config* cfg, syp_status* err) {
    syp_config out = default_config();
    if (err != nullptr) *err = SYP_OK;
    if (cfg == nullptr) {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return out;
    }
    uint32_t sz = cfg->struct_size;
    if (sz < static_cast<uint32_t>(sizeof(uint32_t))) {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return out;
    }
    const uint32_t cap = static_cast<uint32_t>(sizeof(syp_config));
    if (sz > cap) sz = cap;
    std::memcpy(&out, cfg, sz);
    out.struct_size = cap;
    return out;
}

void set_log_callback(syp_log_fn fn, void* ctx, syp_log_level max_level) {
    std::lock_guard<std::mutex> g(g_log_mu);
    g_log_fn  = fn;
    g_log_ctx = ctx;
    g_log_max = max_level;
}

void log_msg(syp_log_level lvl, const char* tag, const char* msg) {
    syp_log_fn fn = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> g(g_log_mu);
        if (g_log_fn == nullptr) return;
        if (static_cast<int>(lvl) > static_cast<int>(g_log_max)) return;
        fn  = g_log_fn;
        ctx = g_log_ctx;
    }
    fn(ctx, lvl, tag != nullptr ? tag : "syp", msg != nullptr ? msg : "");
}

syp_status set_http_backend(const syp_http_backend* backend) {
    std::lock_guard<std::mutex> g(g_backend_mu);
    if (backend == nullptr) {
        g_backend = {};
        g_backend_set = false;
        return SYP_OK;
    }
    g_backend = *backend;
    g_backend_set = true;
    return SYP_OK;
}

const syp_http_backend* current_http_backend() {
    std::lock_guard<std::mutex> g(g_backend_mu);
    return g_backend_set ? &g_backend : nullptr;
}

SourceBridge::SourceBridge(const syp_http_backend* backend, Clock clock,
                           syp_config cfg, syp_source_callbacks cb,
                           std::string url, HeaderCopy headers, RateClass cls)
    : backend_(backend)
    , clock_(clock)
    , cfg_(cfg)
    , cb_(cb)
    , url_(std::move(url))
    , extra_headers_(std::move(headers))
    , rate_class_(cls) {
    // 【C ABI 这一侧的归一化闸】
    // cache_dir_ 是本对象唯一的那份目录字符串，它同时是
    //   · CacheStore::make_key(cache_dir_, url) 的入参（注册表的 key），
    //   · CacheStore::acquire 里 std::filesystem::path(cache_dir) 的来源（落盘路径），
    //   · enforce_capacity 里手工拼的那个 key（cache_store.cpp 的 cache_dir + US + hash）。
    // 三者同源，所以归一化只能放在这一行——放在 syp_source_open 的函数体里
    // 要另起一个必须活过整次调用的局部 std::string（copy_config 只 memcpy，
    // cfg.cache_dir 仍指向调用方的缓冲区），而且拦不住绕过 C ABI 的调用方。
    // 在此之前，直接用 syp_source_open 的调用方混用 "…/d" 与 "…//d" 会静默
    // 分裂成两份缓存：同一组 .idx/.dat 被两个 CacheIndex 打开互相盖写。
    // 归一化是幂等的，上游（SYPBridge.mm / PreloadStack::create）已经做过的
    // 那一次与这一次叠加无害。
    if (cfg_.cache_dir != nullptr) cache_dir_ = normalize_cache_dir(cfg_.cache_dir);
    cfg_.cache_dir = cache_dir_.c_str();
    play_.duration_ms = -1;
    if (clock_.now_ms == nullptr) clock_ = system_clock();
}

SourceBridge::~SourceBridge() {
    close();
}

std::expected<std::unique_ptr<SourceBridge>, syp_status> SourceBridge::open(
    std::string url,
    const syp_headers* headers,
    const syp_config& cfg,
    const syp_source_callbacks* cb,
    const syp_http_backend* backend,
    Clock clock,
    RateClass cls) {
    if (url.empty()) {
        return std::unexpected<syp_status>(SYP_ERR_INVALID_ARG);
    }
    if (cfg.cache_dir == nullptr || cfg.cache_dir[0] == '\0') {
        return std::unexpected<syp_status>(SYP_ERR_INVALID_ARG);
    }
    if (backend == nullptr || backend->create == nullptr || backend->start == nullptr
        || backend->cancel == nullptr || backend->destroy == nullptr) {
        return std::unexpected<syp_status>(SYP_ERR_INVALID_ARG);
    }

    HeaderCopy hc;
    hc.assign(headers);
    syp_source_callbacks cbc{};
    if (cb != nullptr) cbc = *cb;

    auto self = std::unique_ptr<SourceBridge>(
        new (std::nothrow) SourceBridge(backend, clock, cfg, cbc, std::move(url),
                                        std::move(hc), cls));
    if (!self) return std::unexpected<syp_status>(SYP_ERR_OOM);

    // 不持 mu_ 调 acquire()：它自己要取 CacheStore::mu_（另一棵树的叶子锁），
    // 而且这里 self 还没被任何别的线程看见。
    syp_status aerr = SYP_OK;
    self->handle_ = CacheStore::get().acquire(self->cache_dir_, self->url_,
                                              self->cfg_, &aerr);
    if (!self->handle_.valid()) {
        return std::unexpected<syp_status>(aerr != SYP_OK ? aerr : SYP_ERR_IO);
    }

    HoleSet already;
    int64_t total = -1;
    {
        std::lock_guard<std::mutex> hg(*self->handle_.mu);
        already = self->handle_.index->ranges();
        const auto& meta = self->handle_.index->meta();
        total = meta.total_length;
        // 索引里已有 etag/lm 时，等本会话的 on_validators 走过
        // validate_and_update 再落盘。这只是与校验回调的顺序：门禁强度完全
        // 等于 validate_and_update（新响应 etag 为空时它返回 OK，新字节仍会
        // 写入旧文件）。不靠「再等一次总长」加码——同一条 DLTask 的
        // on_response 先于 on_data，长度变化在任何字节到达之前就已经 fatal。
        self->need_validators_ = !meta.etag.empty() || !meta.last_modified.empty();
    }
    self->total_length_ = total;
    self->identity_ok_  = !self->need_validators_;

    // start_scheduler 失败时 self（unique_ptr）析构会走 close()，close() 里的
    // release() 负责把引用计数还回去——**不要**在这里额外写一次 release，
    // 会双减。
    auto st = self->start_scheduler(total, already);
    if (st != SYP_OK) return std::unexpected<syp_status>(st);

    if (total >= 0 && self->cb_.on_total_length != nullptr && !self->total_notified_) {
        self->total_notified_ = true;
        self->cb_.on_total_length(self->cb_.ctx, total);
    }
    if (!already.empty()) {
        self->fire_cached_ranges();
    }
    return self;
}

syp_status SourceBridge::start_scheduler(int64_t total, const HoleSet& already) {
    SchedulerConfig sc;
    sc.max_concurrent_tasks      = cfg_.max_concurrent_tasks;
    sc.min_segment_size          = cfg_.min_segment_size;
    sc.segment_size_hint         = cfg_.segment_size_hint;
    sc.reuse_max_remaining_bytes = cfg_.reuse_max_remaining_bytes;
    sc.connect_estimate_max_ms   = cfg_.connect_estimate_max_ms;
    sc.max_consecutive_errors    = clamp_i32_pos(cfg_.max_retries, 5);
    sc.allow_no_range_fallback   = cfg_.allow_no_range_fallback;
    sc.task.connect_timeout_ms   = cfg_.connect_timeout_ms;
    sc.task.read_timeout_ms      = cfg_.read_timeout_ms;
    sc.task.max_redirects        = cfg_.max_redirects;
    sc.task.max_retries          = cfg_.max_retries;
    sc.task.speed_window_ms      = cfg_.speed_window_ms;
    // 降级由 Scheduler 统一做：任务侧关掉，这样 200 + 非全文件会走到 fallback。
    sc.task.allow_no_range_fallback = false;
    // 限速准入类别。此刻 self 还没被别的线程看见（open() 里调），
    // 不持 mu_ 读 rate_class_ 没有竞争。limiter 留空 = 进程单例。
    sc.rate_class = rate_class_;

    SchedulerCallbacks scb{};
    scb.ctx             = this;
    scb.on_data         = &SourceBridge::cb_on_data;
    scb.on_total_length = &SourceBridge::cb_on_total;
    scb.on_validators   = &SourceBridge::cb_on_validators;
    scb.on_error        = &SourceBridge::cb_on_error;
    scb.on_idle         = &SourceBridge::cb_on_idle;

    sched_ = std::make_unique<Scheduler>(backend_, clock_, sc, scb);

    const int64_t ahead = lookahead_bytes_locked();
    int64_t end = sat_add(pos_, ahead);
    if (total >= 0 && end > total) end = total;
    sched_->set_target_end(end);

    syp_headers hv = extra_headers_.view();
    const syp_headers* hp = extra_headers_.names.empty() ? nullptr : &hv;
    // 总长不传给调度器：它若带着索引里的旧值，响应里变了的 Content-Length
    // 会被吞掉，validate_and_update 看不到。未知让首个响应上报真实值。
    sched_->start(url_, hp, -1, already);
    return SYP_OK;
}

int64_t SourceBridge::now_ms() const noexcept {
    if (clock_.now_ms == nullptr) return 0;
    return clock_.now_ms(clock_.ctx);
}

int64_t SourceBridge::lookahead_bytes_locked() const noexcept {
    const int32_t buf_ms = (!seen_playback_ && pos_ == 0)
                               ? cfg_.first_buffer_ms
                               : cfg_.target_buffer_ms;
    if (play_.duration_ms > 0 && total_length_ > 0 && buf_ms > 0) {
        const int64_t n = bytes_for_ms(total_length_, play_.duration_ms, buf_ms);
        if (n > 0) return n;
    }
    int64_t w = cfg_.min_segment_size > 0 ? cfg_.min_segment_size : (512 * 1024);
    const int32_t conc = clamp_i32_pos(cfg_.max_concurrent_tasks, 1);
    if (conc > 1) {
        if (w > kI64Max / static_cast<int64_t>(conc)) return kI64Max;
        w *= static_cast<int64_t>(conc);
    }
    if (cfg_.segment_size_hint > w) w = cfg_.segment_size_hint;
    return w;
}

void SourceBridge::apply_window() {
    Scheduler* s = nullptr;
    int64_t pos = 0;
    int64_t end = -1;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (sched_ == nullptr || closing_ || fatal_ != SYP_OK) return;
        s = sched_.get();
        ++in_public_;
        pos = pos_;
        const int64_t ahead = lookahead_bytes_locked();
        end = sat_add(pos, ahead);
        if (total_length_ >= 0 && end > total_length_) end = total_length_;
    }
    PublicOpGuard guard(this);
    s->set_read_position(pos);
    s->set_target_end(end);
    // 纯兜底，无测试覆盖。两个 setter 在值没变时早退，不会调 schedule()；
    // 防的是双双早退时把该填的洞丢掉。每次成功的 read() 都会再走一趟
    // occupied_locked() + holes_in()。
    s->request_schedule();
}

void SourceBridge::save_index_locked() {
    if (!handle_.valid() || !index_dirty_) return;
    // 数据先于索引落稳：没 fsync 成功就不把内存里的区间写进 .idx，
    // 崩溃后最多漏缓存、不会把未落稳的洞当成已缓存。
    // sync 与 save 必须在**同一次**句柄锁持有里完成：另一个共享
    // 同一份索引的 SourceBridge 可能正要 save，中间放锁会让它把一份
    // "字节还没 fsync"的区间表写出去。
    //
    // 【日志必须在放开句柄锁之后才打】log_msg() 会调用户装的 syp_log_fn，
    // 还会取 g_log_mu（第三把锁）。在 mu_ + *handle_.mu 都握着的时候调用户
    // 回调，等于把"同 key 的两个源"这件事变成自死锁：日志回调里碰一下共享
    // 同一 cache key 的另一个 SourceBridge（查 cached_ranges / get_stats），
    // 那条路径要先取它自己的 mu_ 再取**同一把** *handle_.mu —— 同线程重入，
    // 直接挂死。预加载正好会造出这种同 key 的对等配置。
    // 所以锁内只记下结论，锁外再打。
    const char* warn = nullptr;
    bool saved = false;
    {
        std::lock_guard<std::mutex> hg(*handle_.mu);
        auto sy = handle_.file->sync();
        if (!sy) {
            warn = "cache data sync failed";
        } else {
            auto r = handle_.index->save();
            if (!r) {
                warn = "cache index save failed";
            } else {
                index_dirty_  = false;
                unsaved_bytes_ = 0;
                saved = true;
            }
        }
    }
    // 仍在 mu_ 下（三个调用点都持着它），但已经放开了句柄锁——这与此前
    // 的形状一致，没有扩大暴露面。
    if (warn != nullptr) log_msg(SYP_LOG_WARN, "source", warn);

    // 【落盘成功之后才谈淘汰，而且只置位】enforce_capacity 要取
    // CacheStore::mu_，那是另一棵树的叶子锁，不能在 mu_（更不能在句柄锁）
    // 下调。这里只记下"该扫一轮了"，由锁外的 maybe_enforce_capacity() 消费。
    // 节流本身也放在 mu_ 下算：last_enforce_ms_ / bytes_since_enforce_ 与
    // unsaved_bytes_ 一样由 mu_ 保护。
    if (!saved) return;
    const int64_t now = now_ms();
    if (now - last_enforce_ms_ >= kEnforceMinIntervalMs
        || bytes_since_enforce_ >= kEnforceEveryBytes) {
        last_enforce_ms_     = now;
        bytes_since_enforce_ = 0;
        want_enforce_        = true;
    }
}

void SourceBridge::maybe_enforce_capacity() {
    bool go = false;
    std::string dir;
    syp_config cfg_copy{};
    {
        std::lock_guard<std::mutex> g(mu_);
        go = want_enforce_;
        want_enforce_ = false;
        dir = cache_dir_;
        cfg_copy = cfg_;
    }
    if (!go || dir.empty()) return;
    // cfg_copy.cache_dir 指向本对象的 cache_dir_，enforce_capacity 只读
    // 三个整数字段，不碰这个指针；但仍然把它指到局部 dir 上，免得将来
    // 有人在 enforce_capacity 里用它时踩到悬垂。
    cfg_copy.cache_dir = dir.c_str();
    // 这一句在 mu_ **之外**（上面那个块已经析构）、也不持句柄锁：
    // enforce_capacity 内部会取 CacheStore::mu_。
    //
    // 返回值本层先不消费：SYP_ERR_BUSY 就是"下一轮接着删"，而我们本来就在
    // 反复调；SYP_ERR_NO_SPACE 是"腾不出来了"，该据此减产的是**预加载**，
    // 不是正在播放的这个源。
    //
    // 【谁来消费 —— 答案是"没有人"】这个 `(void)` 是全仓唯一的调用点，
    // `SYP_ERR_NO_SPACE` / `SYP_ERR_BUSY`
    // 在 preloader / api / media / swift 里一次都没为它出现过 —— 也就是说
    // 这个返回值在外部完全不可观测。
    // 将来真要接的时候照 cache_store.h 的契约理解：BUSY = 下一轮还有得做，
    // NO_SPACE = **在外部条件变化之前**再调没用，不是"这个目录从此无药可救"。
    // 这里打日志则会在低磁盘设备上按 1s 刷屏。
    (void)CacheStore::get().enforce_capacity(dir, cfg_copy);
}

std::shared_ptr<CacheFile> SourceBridge::file_ref_locked() const {
    return handle_.file;
}

bool SourceBridge::identity_ready_locked() const noexcept {
    // 只等 validators。同一条 DLTask 的 on_response 一定先于它自己的
    // on_data，Scheduler 又用 total_notified_ 全局去重，所以长度变化
    // 在任何字节落盘前就已经 fatal。这里再等 seen_total_ 只会在响应
    // 解析不出总长时把数据永远堵在 pending_。
    if (need_validators_) return seen_validators_;
    return true;
}

void SourceBridge::set_rate_class(RateClass c) {
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        rate_class_ = c;
        // 已 close（或正在 close）：只记下来。closing_ 之后 sched_ 随时会被
        // drain 掉，不再登记新的 in_public_。
        if (sched_ == nullptr || closing_) return;
        s = sched_.get();
        ++in_public_;
    }
    // 【锁外转发】Scheduler::set_rate_class 放锁后会在本线程上同步跑一轮
    // schedule()，可能经 on_idle / on_error 回到本类拿 mu_——持着 mu_ 调
    // 就是同线程自锁。与 apply_window() 同一体例：in_public_ 挡住 close()
    // 拆 Scheduler，PublicOpGuard 析构时归还。
    PublicOpGuard guard(this);
    s->set_rate_class(c);
}

RateClass SourceBridge::rate_class_for_test() const {
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (sched_ == nullptr || closing_) return rate_class_;
        s = sched_.get();
        ++in_public_;
    }
    PublicOpGuard guard(this);
    return s->rate_class();
}

RateClass SourceBridge::open_rate_class_for_test() const {
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (sched_ == nullptr || closing_) return rate_class_;
        s = sched_.get();
        ++in_public_;
    }
    PublicOpGuard guard(this);
    return s->initial_rate_class_for_test();
}

HoleSet SourceBridge::scheduler_cached_for_test() const {
    std::lock_guard<std::mutex> g(mu_);
    if (sched_ == nullptr) return {};
    return sched_->cached_ranges();
}

void SourceBridge::set_fatal_locked(syp_status st, int32_t http) {
    if (fatal_ != SYP_OK) return;
    fatal_ = st;
    fatal_http_ = http;
    cv_.notify_all();
}

void SourceBridge::fire_buffering(bool on) {
    syp_source_callbacks cb;
    {
        std::lock_guard<std::mutex> g(mu_);
        cb = cb_;
    }
    if (cb.on_buffering != nullptr) cb.on_buffering(cb.ctx, on);
}

void SourceBridge::fire_cached_ranges() {
    std::vector<syp_range> rs;
    syp_source_callbacks cb;
    {
        std::lock_guard<std::mutex> g(mu_);
        cb = cb_;
        if (cb.on_cached_ranges == nullptr || !handle_.valid()) return;
        std::lock_guard<std::mutex> hg(*handle_.mu);
        const auto sp = handle_.index->ranges().ranges();
        rs.resize(sp.size());
        for (size_t i = 0; i < sp.size(); ++i) {
            rs[i].start = sp[i].start;
            rs[i].end   = sp[i].end;
        }
    }
    const int32_t n = static_cast<int32_t>(rs.size());
    cb.on_cached_ranges(cb.ctx, rs.empty() ? nullptr : rs.data(), n);
}

void SourceBridge::record_speed_locked(int32_t n) noexcept {
    if (n <= 0) return;
    const int64_t t = now_ms();
    speed_samples_.push_back(SpeedSample{t, n});
    const int32_t win = cfg_.speed_window_ms;
    if (win > 0) {
        const int64_t left = t - static_cast<int64_t>(win);
        while (!speed_samples_.empty() && speed_samples_.front().t_ms < left) {
            speed_samples_.pop_front();
        }
    }
    const int64_t bps = speed_bps_locked();
    if (bps >= 0) current_speed_bps_ = bps;
}

int64_t SourceBridge::speed_bps_locked() const noexcept {
    const int32_t win = cfg_.speed_window_ms;
    if (win <= 0 || speed_samples_.empty()) return 0;
    const int64_t t = now_ms();
    const int64_t left = t - static_cast<int64_t>(win);
    int64_t sum = 0;
    int64_t first_t = t;
    for (const auto& s : speed_samples_) {
        if (s.t_ms < left) continue;
        sum += static_cast<int64_t>(s.bytes);
        if (s.t_ms < first_t) first_t = s.t_ms;
    }
    int64_t span = t - left;
    if (span < 1) span = 1;
    // 起步阶段不要把字节摊到整个窗口上。
    const int64_t seen = t - first_t;
    if (seen > 0 && seen < span) span = seen;
    if (span < 1) span = 1;
    if (sum > kI64Max / 1000) return kI64Max;
    return (sum * 1000) / span;
}

void SourceBridge::cb_on_data(void* ctx, int64_t offset, const uint8_t* data, int32_t len) {
    auto* self = static_cast<SourceBridge*>(ctx);
    if (self != nullptr) self->on_data(offset, data, len);
}

void SourceBridge::cb_on_total(void* ctx, int64_t total) {
    auto* self = static_cast<SourceBridge*>(ctx);
    if (self != nullptr) self->on_total(total);
}

void SourceBridge::cb_on_validators(void* ctx, const char* etag, const char* last_modified) {
    auto* self = static_cast<SourceBridge*>(ctx);
    if (self != nullptr) self->on_validators(etag, last_modified);
}

void SourceBridge::cb_on_error(void* ctx, syp_status st, int32_t http_status) {
    auto* self = static_cast<SourceBridge*>(ctx);
    if (self != nullptr) self->on_error(st, http_status);
}

void SourceBridge::cb_on_idle(void* ctx) {
    auto* self = static_cast<SourceBridge*>(ctx);
    if (self != nullptr) self->on_idle();
}

void SourceBridge::persist_chunk(int64_t offset, const uint8_t* data, int32_t len) {
    if (data == nullptr || len <= 0 || offset < 0) return;
    const int64_t n64 = static_cast<int64_t>(len);
    if (n64 > kI64Max - offset) return;
    const Range r{offset, offset + n64};

    std::shared_ptr<CacheFile> f;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_ || closing_ || fatal_ != SYP_OK || !identity_ok_
            || !handle_.valid()) {
            return;
        }
        f = file_ref_locked();
    }

    // 锁外写：CacheFile 的 pread/pwrite 并发安全（cache_file.h），
    // 这份局部 shared_ptr 保证对象在写期间不会被 close 拆掉。
    auto wr = f->write_at(offset, std::span<const uint8_t>(
                                  data, static_cast<size_t>(len)));
    if (!wr) {
        on_error(wr.error(), 0);
        return;
    }

    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        // write_at 期间可能已经 fatal（etag 变了）：字节在文件空洞里，
        // 但不记进索引、不 notify，崩溃/重启也看不到这段。
        if (closed_ || closing_ || fatal_ != SYP_OK || !identity_ok_
            || !handle_.valid()) {
            return;
        }
        s = sched_.get();
    }

    // 文件已写入。notify_persisted 与 add_range 的次序是顺带整理：
    // 先通知调度器、再把区间记进索引。两种次序下持久性不变量都成立
    // （字节已在文件里；未 save 的区间崩溃后当洞重下）。
    // fsync 仍推迟到 save_index_locked。
    if (s != nullptr) s->notify_persisted(r);

    int64_t speed = 0;
    bool fire_speed = false;
    syp_source_callbacks cb{};
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_ || closing_ || fatal_ != SYP_OK || !identity_ok_
            || !handle_.valid()) {
            return;
        }
        {
            std::lock_guard<std::mutex> hg(*handle_.mu);
            handle_.index->add_range(r);
        }
        index_dirty_ = true;
        downloaded_bytes_ = sat_add(downloaded_bytes_, n64);
        unsaved_bytes_    = sat_add(unsaved_bytes_, n64);
        bytes_since_enforce_ = sat_add(bytes_since_enforce_, n64);
        record_speed_locked(len);
        speed = current_speed_bps_;
        fire_speed = cb_.on_speed != nullptr;
        cb = cb_;
        if (unsaved_bytes_ >= kSaveEveryBytes) save_index_locked();
        cv_.notify_all();
    }

    fire_cached_ranges();
    if (fire_speed && cb.on_speed != nullptr) cb.on_speed(cb.ctx, speed);
    maybe_enforce_capacity();   // 锁外：上面那个 mu_ 块早已析构
}

void SourceBridge::flush_pending(std::vector<PendingChunk> chunks) {
    for (const auto& c : chunks) {
        if (c.bytes.empty()) continue;
        if (c.bytes.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            continue;
        }
        persist_chunk(c.offset, c.bytes.data(),
                      static_cast<int32_t>(c.bytes.size()));
    }
}

void SourceBridge::on_data(int64_t offset, const uint8_t* data, int32_t len) {
    if (data == nullptr || len <= 0 || offset < 0) return;
    const int64_t n64 = static_cast<int64_t>(len);
    if (n64 > kI64Max - offset) return;

    // 索引已有 etag/lm 时，on_validators 里 validate_and_update 返回之前
    // 不落盘。这只保证「先校验回调、后落盘」的顺序，不提供超出
    // validate_and_update 的身份检测（新 etag 为空时它返回 OK）。
    // 源变了之后旧缓存整份失效；把新版本写进旧文件会拼出坏数据，
    // add_range 之后还会随索引跨进程存活。公开 API 把 CONTENT_CHANGED
    // 当致命错误交给调用方，本层不作废重建。失败或已 fatal 则丢弃。
    std::vector<PendingChunk> leftover;
    bool oom = false;
    syp_source_callbacks cb{};
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_ || closing_ || fatal_ != SYP_OK || !handle_.valid()) return;
        if (!identity_ok_) {
            const int64_t cap = cfg_.max_memory_bytes;
            if (cap > 0 && (n64 > cap || pending_bytes_ > cap - n64)) {
                pending_.clear();
                pending_bytes_ = 0;
                set_fatal_locked(SYP_ERR_OOM, 0);
                oom = true;
                s = sched_.get();
                cb = cb_;
            } else {
                pending_bytes_ = sat_add(pending_bytes_, n64);
                PendingChunk p;
                p.offset = offset;
                p.bytes.assign(data, data + static_cast<size_t>(len));
                pending_.push_back(std::move(p));
                return;
            }
        } else {
            leftover.swap(pending_);
            pending_bytes_ = 0;
        }
    }
    if (oom) {
        if (s != nullptr) s->stop();
        if (cb.on_error != nullptr) cb.on_error(cb.ctx, SYP_ERR_OOM, 0);
        return;
    }
    flush_pending(std::move(leftover));
    persist_chunk(offset, data, len);
}

void SourceBridge::on_total(int64_t total) {
    syp_status changed = SYP_OK;
    bool fire_total = false;
    int64_t tv = total;
    syp_source_callbacks cb{};
    std::vector<PendingChunk> to_flush;
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closing_ || closed_) return;
        if (handle_.valid() && total >= 0) {
            std::expected<void, syp_status> v;
            {
                std::lock_guard<std::mutex> hg(*handle_.mu);
                v = handle_.index->validate_and_update("", "", total);
            }
            if (!v) {
                set_fatal_locked(v.error(), 0);
                changed = v.error();
                pending_.clear();
                pending_bytes_ = 0;
            } else {
                index_dirty_ = true;
                if (!identity_ok_ && identity_ready_locked()) {
                    identity_ok_ = true;
                    to_flush.swap(pending_);
                    pending_bytes_ = 0;
                }
            }
        }
        if (changed == SYP_OK && total >= 0) {
            if (total_length_ < 0) total_length_ = total;
            tv = total_length_;
            if (!total_notified_) {
                total_notified_ = true;
                fire_total = cb_.on_total_length != nullptr;
            }
        }
        cb = cb_;
        s = sched_.get();
        cv_.notify_all();
    }
    if (changed != SYP_OK) {
        if (s != nullptr) s->stop();
        if (cb.on_error != nullptr) cb.on_error(cb.ctx, changed, 0);
        return;
    }
    flush_pending(std::move(to_flush));
    if (fire_total && cb.on_total_length != nullptr) {
        cb.on_total_length(cb.ctx, tv);
    }
    apply_window();
}

void SourceBridge::on_validators(const char* etag, const char* last_modified) {
    syp_status changed = SYP_OK;
    syp_source_callbacks cb{};
    std::vector<PendingChunk> to_flush;
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closing_ || closed_) return;
        if (handle_.valid()) {
            std::expected<void, syp_status> v;
            {
                std::lock_guard<std::mutex> hg(*handle_.mu);
                v = handle_.index->validate_and_update(
                    etag ? etag : "", last_modified ? last_modified : "", -1);
            }
            if (!v) {
                set_fatal_locked(v.error(), 0);
                changed = v.error();
                pending_.clear();
                pending_bytes_ = 0;
            } else {
                index_dirty_ = true;
                seen_validators_ = true;
                if (!identity_ok_ && identity_ready_locked()) {
                    identity_ok_ = true;
                    to_flush.swap(pending_);
                    pending_bytes_ = 0;
                }
            }
        }
        cb = cb_;
        s = sched_.get();
        cv_.notify_all();
    }
    if (changed != SYP_OK) {
        if (s != nullptr) s->stop();
        if (cb.on_error != nullptr) cb.on_error(cb.ctx, changed, 0);
        return;
    }
    flush_pending(std::move(to_flush));
}

void SourceBridge::on_error(syp_status st, int32_t http_status) {
    syp_source_callbacks cb{};
    bool fire = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_) return;
        const bool first = (fatal_ == SYP_OK);
        set_fatal_locked(st, http_status);
        if (first) {
            ++failed_tasks_;
            fire = cb_.on_error != nullptr;
        }
        cb = cb_;
    }
    if (fire && cb.on_error != nullptr) cb.on_error(cb.ctx, st, http_status);
}

void SourceBridge::on_idle() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_) return;
        save_index_locked();
        cv_.notify_all();
    }
    maybe_enforce_capacity();   // 锁外
}

int32_t SourceBridge::read(uint8_t* buf, int32_t size) {
    if (buf == nullptr || size <= 0) return SYP_ERR_INVALID_ARG;

    {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_) return SYP_ERR_CANCELED;
        ++in_public_;
    }
    PublicOpGuard guard(this);

    bool buffering_this = false;
    bool waited = false;

    for (;;) {
        int64_t pos = 0;
        int64_t nwant = 0;
        syp_status early = SYP_OK;
        bool do_io = false;
        std::shared_ptr<CacheFile> f;
        {
            std::unique_lock<std::mutex> lk(mu_);
            auto finish_buf = [&]() {
                if (buffering_) {
                    buffering_ = false;
                    buffering_this = true;
                }
            };
            if (interrupted_) {
                finish_buf();
                early = SYP_ERR_CANCELED;
            } else if (fatal_ != SYP_OK) {
                finish_buf();
                early = fatal_;
            } else if (closing_) {
                finish_buf();
                early = SYP_ERR_CANCELED;
            } else {
                pos = pos_;
                if (total_length_ >= 0 && pos >= total_length_) {
                    finish_buf();
                    early = SYP_ERR_EOF;
                } else {
                    int64_t avail = 0;
                    if (handle_.valid()) {
                        std::lock_guard<std::mutex> hg(*handle_.mu);
                        avail = handle_.index->ranges().contiguous_from(pos);
                    }
                    if (total_length_ >= 0) {
                        const int64_t remain = total_length_ - pos;
                        if (remain < 0) avail = 0;
                        else if (avail > remain) avail = remain;
                    }
                    if (avail > static_cast<int64_t>(size)) {
                        avail = static_cast<int64_t>(size);
                    }
                    if (avail > 0) {
                        nwant = avail;
                        finish_buf();
                        do_io = true;
                        // 在 mu_ 下拷一份文件引用出去，锁外 read_at 用它。
                        f = file_ref_locked();
                    } else {
                        waited = true;
                        const bool need_buf = !buffering_;
                        if (need_buf) buffering_ = true;
                        lk.unlock();
                        if (need_buf) fire_buffering(true);
                        apply_window();
                        lk.lock();
                        cv_.wait(lk, [this] {
                            if (interrupted_ || closing_ || fatal_ != SYP_OK) {
                                return true;
                            }
                            if (total_length_ >= 0 && pos_ >= total_length_) {
                                return true;
                            }
                            // 谓词在持 mu_ 时求值，这里再取句柄锁，偏序
                            // mu_ → *handle_.mu 成立（句柄锁是叶子，持有它
                            // 时不会回头取 mu_，也不会调任何回调）。
                            if (!handle_.valid()) return false;
                            std::lock_guard<std::mutex> hg(*handle_.mu);
                            return handle_.index->ranges().contiguous_from(pos_) > 0;
                        });
                        continue;
                    }
                }
            }
        }

        if (early != SYP_OK) {
            if (buffering_this) fire_buffering(false);
            return early;
        }
        if (!do_io) continue;

        if (!f) {
            if (buffering_this) fire_buffering(false);
            return SYP_ERR_IO;
        }
        auto n = f->read_at(pos, std::span<uint8_t>(buf, static_cast<size_t>(nwant)));
        if (!n) {
            if (buffering_this) fire_buffering(false);
            return n.error();
        }
        if (*n <= 0) {
            if (buffering_this) fire_buffering(false);
            return SYP_ERR_IO;
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            if (pos_ == pos) {
                pos_ = sat_add(pos_, *n);
                if (!waited) cache_hit_bytes_ = sat_add(cache_hit_bytes_, *n);
            }
        }
        if (buffering_this) fire_buffering(false);
        apply_window();
        return static_cast<int32_t>(*n);
    }
}

int64_t SourceBridge::seek(int64_t offset, int32_t whence) {
    int64_t np = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closing_ || closed_) return SYP_ERR_CANCELED;
        int64_t base = 0;
        if (whence == SYP_SEEK_SET) {
            base = 0;
        } else if (whence == SYP_SEEK_CUR) {
            base = pos_;
        } else if (whence == SYP_SEEK_END) {
            if (total_length_ < 0) return SYP_ERR_INVALID_ARG;
            base = total_length_;
        } else {
            return SYP_ERR_INVALID_ARG;
        }
        np = sat_add(base, offset);
        if (np < 0) return SYP_ERR_INVALID_ARG;
        pos_ = np;
        cv_.notify_all();
    }
    apply_window();
    return np;
}

int64_t SourceBridge::length() const {
    std::lock_guard<std::mutex> g(mu_);
    return total_length_;
}

void SourceBridge::interrupt() {
    std::lock_guard<std::mutex> g(mu_);
    interrupted_ = true;
    cv_.notify_all();
}

void SourceBridge::resume() {
    std::lock_guard<std::mutex> g(mu_);
    interrupted_ = false;
}

void SourceBridge::update_playback(const syp_playback_state* st) {
    if (st == nullptr) return;
    {
        std::lock_guard<std::mutex> g(mu_);
        play_ = *st;
        seen_playback_ = true;
    }
    apply_window();
}

int32_t SourceBridge::cached_ranges(syp_range* out, int32_t max) const {
    std::lock_guard<std::mutex> g(mu_);
    if (!handle_.valid()) return 0;
    std::lock_guard<std::mutex> hg(*handle_.mu);
    const auto sp = handle_.index->ranges().ranges();
    const int32_t n = static_cast<int32_t>(sp.size());
    if (out == nullptr || max <= 0) return n;
    const int32_t w = n < max ? n : max;
    for (int32_t i = 0; i < w; ++i) {
        out[static_cast<size_t>(i)].start = sp[static_cast<size_t>(i)].start;
        out[static_cast<size_t>(i)].end   = sp[static_cast<size_t>(i)].end;
    }
    return w;
}

void SourceBridge::get_stats(syp_source_stats* out) const {
    if (out == nullptr) return;
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        std::memset(out, 0, sizeof(*out));
        out->downloaded_bytes  = downloaded_bytes_;
        out->cache_hit_bytes   = cache_hit_bytes_;
        if (handle_.valid()) {
            std::lock_guard<std::mutex> hg(*handle_.mu);
            out->cached_bytes = handle_.index->ranges().total_bytes();
        }
        out->completed_tasks   = completed_tasks_;
        out->failed_tasks      = failed_tasks_;
        out->current_speed_bps = current_speed_bps_;
        if (sched_ == nullptr || closing_) return;
        s = sched_.get();
        ++in_public_;
    }
    PublicOpGuard guard(this);
    out->active_tasks = s->active_task_count();
}

void SourceBridge::close() {
    Scheduler* s = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_) return;
        closing_ = true;
        interrupted_ = true;
        pending_.clear();
        pending_bytes_ = 0;
        cv_.notify_all();
        s = sched_.get();
    }
    if (s != nullptr) s->stop();
    {
        std::lock_guard<std::mutex> g(mu_);
        index_dirty_ = true;
        save_index_locked();
        closed_ = true;
    }
    // 【必须在上面那个块**之外**】enforce_capacity 取 CacheStore::mu_，
    // 不能在 mu_ 下调；那个块又不能提前结束，closed_ = true 必须和
    // save_index_locked 在同一次持有里（否则中间会被别人插进来再落一次盘）。
    // 【也必须在下面的 release() 之前】此刻本 key 的引用计数还是 ≥1，
    // enforce_capacity 因此一定会跳过自己正在收尾的这份缓存。
    maybe_enforce_capacity();
    // drain 公开调用：apply_window / get_stats 放锁后还握着 s。
    std::unique_ptr<Scheduler> dying;
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return in_public_ == 0; });
        dying = std::move(sched_);
    }
    // 锁外拆 Scheduler：析构会等回调，回调里还要拿 mu_。
    dying.reset();
    // 【CacheFile/CacheIndex 必须在 dying.reset() **之后**才销毁】
    // in_public_ 挡的是公开 API 的重入，挡不住 backend 回调线程：
    // persist_chunk() 是"锁内查 closing_、放锁、锁外 write_at"，查完放锁的
    // 那条回调此刻仍可能在写 file_。~Scheduler 等到每个 Slot 的 in_cb 归零
    // 才返回（scheduler.cpp 析构里的 dtor_idle_locked），那之后才没有回调
    // 能再碰它们。修复前 file_.reset() 在上面那个锁块里、早于 dying.reset()，
    // TSan 报过 CacheFile::close_fd ↔ CacheFile::write_at。
    //
    // 换成 shared_ptr 之后这条**仍然成立，而且仍然必要**：
    // shared_ptr 保证的是"对象不会在别人手里消失"，挡不住"一条回调线程正在
    // 读成员 handle_.file 这个 shared_ptr 本身、而我们同时在写它"——那是对
    // 指针变量的数据竞争，TSan 照样会报。所以位置一个字不动。
    // release() 放在锁外：它要取 CacheStore::mu_，而那是另一棵树的叶子锁，
    // 持有 mu_ 时不得触碰（cache_store.h 顶部的锁纪律第 3 条）。
    CacheStore::Handle dying_handle;
    {
        std::lock_guard<std::mutex> g(mu_);
        dying_handle = std::move(handle_);
        handle_ = CacheStore::Handle{};
    }
    if (!dying_handle.key.empty()) {
        CacheStore::get().release(dying_handle.key);
    }
    // dying_handle 在这里析构：若刚才那次 release 把引用计数清零，
    // 真正的销毁发生在此刻，且此刻已经没有任何回调能碰到它们。
}

int64_t cache_dir_size(const char* cache_dir) {
    if (cache_dir == nullptr || cache_dir[0] == '\0') return SYP_ERR_INVALID_ARG;
    syp_status err = SYP_OK;
    auto entries = scan_cache_dir(std::filesystem::path(cache_dir), &err);
    if (err != SYP_OK) return err;
    int64_t sum = 0;
    for (const auto& e : entries) sum = sat_add(sum, e.bytes);
    return sum;
}

syp_status cache_dir_evict(const char* cache_dir, int64_t target_bytes) {
    if (cache_dir == nullptr || cache_dir[0] == '\0') return SYP_ERR_INVALID_ARG;
    if (target_bytes < 0) return SYP_ERR_INVALID_ARG;
    syp_status err = SYP_OK;
    auto entries = scan_cache_dir(std::filesystem::path(cache_dir), &err);
    if (err != SYP_OK) return err;
    int64_t sum = 0;
    for (const auto& e : entries) sum = sat_add(sum, e.bytes);
    if (sum <= target_bytes) return SYP_OK;

    std::sort(entries.begin(), entries.end(),
              [](const CacheDirEntry& a, const CacheDirEntry& b) {
                  return a.mtime < b.mtime;
              });
    for (const auto& e : entries) {
        if (sum <= target_bytes) break;
        const syp_status st = remove_cache_entry(e);
        if (st != SYP_OK) return st;
        sum -= e.bytes;
        if (sum < 0) sum = 0;
    }
    return SYP_OK;
}

syp_status cache_dir_clear(const char* cache_dir) {
    return cache_dir_evict(cache_dir, 0);
}

syp_status cache_dir_remove(const char* cache_dir, const char* url) {
    if (cache_dir == nullptr || cache_dir[0] == '\0' || url == nullptr) {
        return SYP_ERR_INVALID_ARG;
    }
    auto idx = CacheIndex::create(std::filesystem::path(cache_dir), url);
    auto r = idx.remove_files();
    if (!r) return r.error();
    return SYP_OK;
}

}  // namespace syp::dl
