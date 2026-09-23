// preloader.cpp — 条目表在 mu_ 下，全部阻塞动作在驱动线程上、锁外做。
#include "preloader.h"

#include "cache_store.h"
#include "m3u8_scan.h"

#include <algorithm>
#include <chrono>
#include <expected>
#include <limits>
#include <new>
#include <utility>

namespace syp::dl {
namespace {

constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr int32_t kMaxTotalTasksCap = 64;

// 【为什么需要一个轮询周期】"同一个 cache key 上出现了/离开了播放源"这件事
// **没有事件源**：CacheStore 不回调任何人（它的 mu_ 下不许调外面的代码），
// SourceBridge 也不广播 open/close。所以
// 驱动线程只能自己隔一会儿问一句 open_count()。50ms：让路的延迟上界（这段
// 时间里最多重复下一个窗口的字节），代价是每 50ms 一次 map::find。
// 只有"有条目正开着源"或"有条目正在让路"时才轮询；两者都没有时照旧无限等，
// 空闲的 preloader 不会有周期性唤醒。
constexpr int32_t kPeerPollMs = 50;

int64_t ceil_div_pos(int64_t a, int64_t b) noexcept {
    if (b <= 0) return a;
    if (a <= 0) return 0;
    return (a + b - 1) / b;
}

int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int64_t sat_add(int64_t a, int64_t b) noexcept {
    if (b > 0 && a > kI64Max - b) return kI64Max;
    return a + b;
}

// 限速类别映射：Playing 条目与播放同级；Next / Background
// 只用播放用剩的额度，并给播放留半桶。
RateClass rate_class_for(PreloadPriority p) noexcept {
    return p == PreloadPriority::Playing ? RateClass::Playing : RateClass::Preload;
}

}  // namespace

std::unique_ptr<Preloader> Preloader::create(const syp_config& dl_cfg,
                                             const PreloadConfig& cfg,
                                             const MediaInfoProvider* provider,
                                             const syp_http_backend* backend,
                                             Clock clock,
                                             syp_status* err) {
    if (err != nullptr) *err = SYP_OK;
    if (backend == nullptr || backend->create == nullptr || backend->start == nullptr
        || backend->cancel == nullptr || backend->destroy == nullptr
        || dl_cfg.cache_dir == nullptr || dl_cfg.cache_dir[0] == '\0') {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }
    MediaInfoProvider pv{};
    if (provider != nullptr && provider->estimate_range_for_ms != nullptr) pv = *provider;

    auto self = std::unique_ptr<Preloader>(
        new (std::nothrow) Preloader(dl_cfg, cfg, pv, backend,
                                     clock.now_ms != nullptr ? clock : system_clock()));
    if (!self) {
        if (err != nullptr) *err = SYP_ERR_OOM;
        return nullptr;
    }
    // 【这一行会抛，而它下面就是 extern "C"】
    // 仓库没有 -fno-exceptions，C ABI 层一律 `new (std::nothrow)`，唯独
    // std::thread 的构造是个破例：线程数到上界（EAGAIN）时它抛
    // std::system_error，而 syp_preloader_create 与本函数都没有 try/catch，
    // 异常会**穿出 extern "C"**——那是未定义行为，实践上就是 std::terminate。
    // 在内存/线程吃紧的 iOS 设备上这不是"这次预加载失败"，是整个 app 挂掉。
    // 捕获之后照原路返回 nullptr + SYP_ERR_OOM：与本函数已有的失败形状一致，
    // 调用方不需要多认一种错误。
    // self 在这里析构是安全的：~Preloader 的 interrupt_fetch(nullptr) 见到
    // fetching_ == nullptr 直接返回，driver_ 不 joinable，close_all() 走空表。
    try {
        self->driver_ = std::thread([p = self.get()] { p->driver_main(); });
    } catch (...) {
        if (err != nullptr) *err = SYP_ERR_OOM;
        return nullptr;
    }
    return self;
}

Preloader::Preloader(syp_config dl_cfg, PreloadConfig cfg, MediaInfoProvider provider,
                     const syp_http_backend* backend, Clock clock)
    : dl_cfg_(dl_cfg)
    , cfg_(cfg)
    , provider_(provider)
    , backend_(backend)
    , clock_(clock) {
    // 【C ABI 这一侧的归一化闸】
    // 与 SourceBridge 的构造函数同一条理由（那里有完整版）：cache_dir_ 既是
    // make_key 的入参也是落盘路径的来源，syp_preloader_create 之前没有任何
    // 一处会归一化它。幂等，所以 PreloadStack::create 已经做过的那一次叠加无害。
    if (dl_cfg_.cache_dir != nullptr) cache_dir_ = normalize_cache_dir(dl_cfg_.cache_dir);
    dl_cfg_.cache_dir = cache_dir_.c_str();
}

Preloader::~Preloader() {
    {
        std::lock_guard<std::mutex> g(mu_);
        stop_ = true;
        // 【这里**不**再逐条 interrupt()】interrupt() 要取桥的 mu_，而
        // persist_chunk 每 256KiB 就握着那把 mu_ 做一次 sync()+save()——在
        // 本类的 mu_ 下调它，等于让析构方的线程连着 add/set_priority/remove
        // 一起排在一次 fsync 后面（实测 p50 178us / p90 1.2ms / max 25.9ms）。
        // 打断也不需要在这里做：驱动线程看见 stop_ 就从等待里退出，它正在
        // 执行的那条 close() 自己会先 interrupt；剩下还开着的源由 join 之后
        // 的 close_all() 逐条 interrupt + close（那时没有任何锁、也没有并发）。
        cv_.notify_all();
    }
    // 驱动线程可能正阻塞在一次播放列表抓取的 read() 里（HLS 展开，见头文件）。
    // 那条 read 只有 interrupt() 能立刻打断，否则析构要等一次读超时——在
    // Swift 的 deinit 上那就是一次主线程卡顿。
    interrupt_fetch(nullptr);
    if (driver_.joinable()) driver_.join();
    // 驱动线程已退出，此后没有任何人会再碰 entries_ / source。
    close_all();
    entries_.clear();
    by_url_.clear();
}

Preloader::Entry* Preloader::find_locked(const std::string& url) const {
    auto it = by_url_.find(url);
    return it == by_url_.end() ? nullptr : it->second;
}

syp_status Preloader::add(const std::string& url, PreloadPriority prio,
                          int64_t ms_or_zero) {
    if (url.empty()) return SYP_ERR_INVALID_ARG;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stop_) return SYP_ERR_CANCELED;
        if (Entry* e = find_locked(url); e != nullptr) {
            // 已存在：只抬优先级，不重复建条目，也不重置已有的目标字节数。
            if (static_cast<int32_t>(prio) > static_cast<int32_t>(e->prio)) {
                e->prio = prio;
            }
            e->removing = false;
            // 【撞上终态条目就地复位】
            // 终态是粘的（见头文件那段），而头文件给出的**唯一**出路是
            // "调用方自己 remove() + 再 add()"。可是 remove() 只置
            // removing=true，条目要等驱动线程下一次 commit() 才真摘掉；
            // 这里原先只写 `e->removing = false`，于是背靠背的 remove+add
            // （调用方最自然的写法）把那条 Failed 条目**原封不动救活**了，
            // 还返回 SYP_OK——实测 5/5 全中、一个新请求都没发。而
            // "先等条目真的消失再 add"公开 C API 做不到（syp_preload_stats
            // 只有 entries 总数，没有 per-URL 信号）。所以那条出路按字面
            // 照做是无效的，这几行就是把它变成真的。
            if (e->state == PreloadState::Done || e->state == PreloadState::Failed) {
                e->state      = PreloadState::Pending;
                e->last_error = SYP_OK;
                // 播放列表条目从不开源，只靠 Expand 推进（compute_plan_locked
                // 的判据里有 `!e->expanded`）。只复位 state 的话它会**永远停在
                // Pending**——比原来的 Failed 更糟：wait_terminal 再也不返回。
                // 所以这一类还要把"已展开"一起清掉，让它重新抓一次播放列表。
                // 这不违反"直播只暖一轮"（hls_live_playlist_is_warmed_once_only
                // 钉的是**驱动线程自己**不续暖），这里是调用方显式又 add 了一次。
                if (e->is_playlist) e->expanded = false;
                // 【estimated 不复位，是刻意的】复位它等于每次 re-add 都要
                // 再跑一次 provider 探测（一次网络往返 + 最多 2 MiB 的字节
                // 代价）。target_bytes 已经定下来了，重下用同一个目标是对的；
                // 真要换目标，调用方给的 ms_or_zero 本来就不会改已有条目的
                // 目标（上面那行注释说的"不重置已有的目标字节数"）。
            }
            dirty_ = true;
            cv_.notify_one();
            return SYP_OK;
        }
        auto up = std::make_unique<Entry>();
        up->url   = url;
        // key 在这里算一次就够：cache_dir_ 与 url 都不会再变。用它向
        // CacheStore 问"这个 key 上还有谁开着"，锁外问（见头文件锁序）。
        up->key   = CacheStore::make_key(cache_dir_, url);
        up->prio  = prio;
        up->seq   = next_seq_++;
        up->owner = this;
        up->is_playlist = is_playlist_url(url);
        up->want_ms = ms_or_zero > 0 ? ms_or_zero
                                     : ((provider_.estimate_range_for_ms != nullptr
                                         || up->is_playlist)
                                            ? cfg_.default_preload_ms
                                            : 0);
        if (up->is_playlist) {
            // 播放列表条目从不开源、也不问 provider：它的"时长 → 字节"换算
            // 由 EXTINF 直接给出（挑前 K 个分片那一步），不需要探测容器头，
            // 拿一个 m3u8 去喂 FFmpeg 的 header 探测更是纯浪费。
            up->target_bytes = kMaxPlaylistBytes;
            up->estimated = true;
        } else if (provider_.estimate_range_for_ms == nullptr || up->want_ms <= 0) {
            // 没装 provider 就直接定死按字节；装了则留到驱动线程去估。
            up->target_bytes = cfg_.default_preload_bytes > 0 ? cfg_.default_preload_bytes
                                                             : (1 << 20);
            up->estimated = true;
        }
        by_url_.emplace(url, up.get());
        entries_.push_back(std::move(up));
        dirty_ = true;
        cv_.notify_one();
    }
    return SYP_OK;
}

syp_status Preloader::set_priority(const std::string& url, PreloadPriority prio) {
    std::lock_guard<std::mutex> g(mu_);
    Entry* e = find_locked(url);
    if (e == nullptr) return SYP_ERR_INVALID_ARG;
    if (e->prio == prio) return SYP_OK;
    e->prio = prio;
    // HLS 展开出来的子条目跟着父条目走：调用方只知道播放列表那一个 URL，
    // 分片 URL 是我们自己造出来的，它没法单独给它们调优先级。
    for (auto& up : entries_) {
        if (up->parent == url) up->prio = prio;
    }
    dirty_ = true;
    cv_.notify_one();
    return SYP_OK;
}

// 打断在途的播放列表抓取。**持 fetch_mu_，不持 mu_**。
//
// fetch_mu_ 是一片叶子：驱动线程**先在它下面清空 fetching_、再关源析构**，
// 所以持有它时这个指针要么是空的、要么指向一条还活着的桥——这正是
// 判定 remove() 那条"锁外拿裸指针"路线为 UAF 时缺的那个保证。
void Preloader::interrupt_fetch(const std::string* owner_url) {
    std::lock_guard<std::mutex> g(fetch_mu_);
    if (fetching_ == nullptr) return;
    // 只打断该打断的那一条：别的条目的展开不能被误伤（误伤的后果是那条
    // 播放列表被判 Failed 且 expanded，此后再也不展开）。
    if (owner_url != nullptr && fetching_owner_ != *owner_url) return;
    fetching_->interrupt();
}

// 【remove / remove_all 对条目源一行桥调用都没有；唯一的例外是在途的
//   播放列表抓取，而且它不在 mu_ 下】
//
// 【为什么这一条必须是例外】条目源的打断可以交给驱动线程（它每轮都会
// interrupt+close 该关的源），但**播放列表抓取本身就跑在驱动线程上**：
// 它阻塞在 read() 里的时候，驱动线程不可能同时去响应 removing 标志。
// 不打断的话 remove() 的实际生效要等一次读超时——实测 5,819ms，而
// remove 需要能打断在途下载。所以这里在**锁外**（fetch_mu_ 下、
// mu_ 之外）打断它，并且只打断属于这个 URL 的那一次。
//
// 【代价，说清楚】interrupt() 要取桥的 mu_，而 persist_chunk 每 256KiB 握着
// 那把 mu_ 做一次 sync()+save()，所以这一步最坏要等一次 fsync（实测
// p50 178us、p90 1.2ms、max 25.9ms；对析构那条同形路径实测 6ms）。
// 也就是说"公开方法不做任何阻塞 IO"这句话，对 remove()/remove_all() 有一个
// **有界的例外**：至多一条播放列表桥的一次 fsync。换来的是 remove 打断
// 语义真的成立。条目源那一半仍然一个字节的 IO 都不做。
//
// 原来这里在 mu_ 下调 `source->interrupt()`。interrupt() 取的是桥的 mu_，
// 而 persist_chunk 每 256KiB 就握着那把 mu_ 做一次 sync()+save()——于是
// 一次 remove() 会在调用方线程上等一次 fsync（实测 p50 178us、p90 1.2ms、
// max 25.9ms），而且是**在 mu_ 下等**，并发的 get_stats/add/set_priority
// 全部排在后面。这与本文件承诺的"公开方法不做任何阻塞 IO"直接冲突。
//
// 【为什么不是"锁外拿裸指针再 interrupt"】那是一个真实的 UAF：放开 mu_ 之后，
// 驱动线程随时可能在 execute() 里 close() 并析构同一条桥。measure() 能用裸
// 指针，只因为它**就跑在驱动线程上**；remove() 来自任意线程，没有这个保证。
// 换成 shared_ptr 又会把析构点搬到调用方线程上。
//
// 所以打断交还给驱动线程：置 removing + dirty_ 并唤醒，驱动线程这一轮的
// execute() 本来就是 `interrupt(); close();`（在锁外）。头文件里
// "立即返回；真正的 interrupt + close 由驱动线程做"说的就是这个语义，
// 原来的实现反而是多余且有害的那一半。
void Preloader::remove(const std::string& url) {
    {
        std::lock_guard<std::mutex> g(mu_);
        Entry* e = find_locked(url);
        if (e == nullptr) return;
        e->removing = true;
        // 父条目走了，它展开出来的分片条目也一并走——它们是这次预加载的
        // 一部分，调用方从来没有单独 add 过它们，也就无从单独 remove。
        for (auto& up : entries_) {
            if (up->parent == url) up->removing = true;
        }
        dirty_ = true;
        cv_.notify_one();
    }
    interrupt_fetch(&url);   // 锁外
}

void Preloader::remove_all() {
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& up : entries_) up->removing = true;
        dirty_ = true;
        cv_.notify_one();
    }
    interrupt_fetch(nullptr);   // 锁外：谁的抓取都该停
}

void Preloader::get_stats(PreloadStats* out) const {
    if (out == nullptr) return;
    std::lock_guard<std::mutex> g(mu_);
    *out = PreloadStats{};
    out->entries = static_cast<int64_t>(entries_.size());
    out->downloaded_bytes = downloaded_bytes_;
    out->active_tasks = active_tasks_;
    out->completed = completed_;
    out->failed = failed_;
    out->provider_miss = provider_miss_;
    // 【这里一条桥都不碰】SourceBridge::get_stats() 要取桥的 mu_ **和句柄锁**
    // （读 index->ranges().total_bytes()），而后端线程会握着句柄锁做
    // sync()+save() 一次 fsync。在本类的 mu_ 下调它，主线程的一次 get_stats
    // 就会把 add/set_priority/remove 和驱动线程整轮一起拖进那次落盘——而
    // 头文件承诺的是"公开方法不做任何阻塞 IO"。所以统计只读驱动线程在
    // commit 里结算好的计数器，代价是最陈旧到上一轮（见 active_tasks_）。
}

// ---------------------------------------------------------------- 回调
void Preloader::note_progress() {
    std::lock_guard<std::mutex> g(mu_);
    dirty_ = true;
    cv_.notify_one();
}

void Preloader::cb_cached_ranges(void* ctx, const syp_range*, int32_t) {
    auto* e = static_cast<Entry*>(ctx);
    if (e != nullptr && e->owner != nullptr) e->owner->note_progress();
}
void Preloader::cb_total_length(void* ctx, int64_t) {
    auto* e = static_cast<Entry*>(ctx);
    if (e != nullptr && e->owner != nullptr) e->owner->note_progress();
}
void Preloader::cb_error(void* ctx, syp_status st, int32_t) {
    auto* e = static_cast<Entry*>(ctx);
    if (e == nullptr || e->owner == nullptr) return;
    {
        std::lock_guard<std::mutex> g(e->owner->mu_);
        if (e->last_error == SYP_OK) e->last_error = st;
        e->owner->dirty_ = true;
    }
    e->owner->cv_.notify_one();
}

// ---------------------------------------------------------------- 配额
void Preloader::allocate_locked() {
    const int32_t total   = clamp_i32(cfg_.max_total_tasks, 1, kMaxTotalTasksCap);
    const int32_t reserve = clamp_i32(cfg_.reserved_for_playing, 0, total);
    const int32_t per_max = clamp_i32(dl_cfg_.max_concurrent_tasks, 1, total);

    std::vector<Entry*> live;
    live.reserve(entries_.size());
    for (auto& up : entries_) {
        up->quota = 0;
        if (up->removing) continue;
        if (up->state == PreloadState::Done || up->state == PreloadState::Failed) continue;
        // 让路中的条目不参与分配：它这一轮无论如何都不会开源，占着额度只会
        // 让排在后面的条目白等。
        if (up->peer_busy) continue;
        live.push_back(up.get());
    }
    std::sort(live.begin(), live.end(), [](const Entry* a, const Entry* b) {
        if (a->prio != b->prio) {
            return static_cast<int32_t>(a->prio) > static_cast<int32_t>(b->prio);
        }
        return a->seq < b->seq;   // 同级 FIFO，不轮转（轮转会让每条都只下一半）
    });

    // has_playing 取的是 live —— 也就是**这一轮我们真的会去下的那些条目**，
    // 让路中的（peer_busy）不算。
    bool has_playing = false;
    for (const Entry* e : live) {
        if (e->prio == PreloadPriority::Playing) has_playing = true;
    }
    // 没有 Playing 条目时留 reserve 个额度给"本 preloader 之外的播放源"
    // ——真正在播的那条流不归我们管，我们能做的只有自己少占几条。
    //
    // 【让路与保底额度的耦合，是刻意的，方向也必须是这个】一条 Playing 条目
    // 让路（peer_busy），意味着这个 key 上**真的有一个本 preloader 之外的
    // 播放源打开着**——那正是 reserved_for_playing 存在的场景。所以它一退出
    // live，has_playing 跟着变 false、budget 从 total 收到 total - reserve，
    // 保底额度**开始生效**。反过来（让路中的条目照样计入 has_playing）会在
    // "真播放源刚开起来"的那一刻把整份 total 发给后台预加载，默认配置下是
    // 3~6 条连接去抢在播那条流的带宽，正好违背保底额度存在的意图。
    // 这条耦合由 reserve_still_applies_while_a_peer_plays_the_key 钉住。
    int32_t budget = has_playing ? total : (total - reserve);
    if (budget < 0) budget = 0;

    for (Entry* e : live) {
        if (budget <= 0) {
            e->quota = 0;
            continue;
        }
        const int32_t g = per_max < budget ? per_max : budget;
        e->quota = g;
        budget -= g;
    }
}

bool Preloader::needs_peer_poll_locked() const {
    for (const auto& up : entries_) {
        if (up->removing) continue;
        if (up->state == PreloadState::Done || up->state == PreloadState::Failed) continue;
        // 只有"正开着源"（可能撞上新来的播放源）或"正在让路"（等对方走）
        // 这两种条目需要周期性复查；其余情形驱动线程照旧无限等。
        if (up->peer_busy || up->source != nullptr) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 驱动线程
std::vector<Preloader::Entry*> Preloader::probe_list_locked() const {
    std::vector<Entry*> out;
    out.reserve(entries_.size());
    for (const auto& up : entries_) {
        if (up->removing) continue;
        if (up->state == PreloadState::Done || up->state == PreloadState::Failed) continue;
        out.push_back(up.get());
    }
    return out;
}

std::vector<int64_t> Preloader::probe_peers(const std::vector<Entry*>& es) const {
    // 【不持 mu_】open_count 要取 CacheStore::mu_，那是另一棵树的叶子；
    // 而 CacheStore 的个别路径会在自己的锁下调用户日志回调，
    // 所以"持 mu_ 去取 CacheStore::mu_"是一条真实的反序，不能走。
    // es 里的 Entry* 只可能被驱动线程自己在 commit() 里销毁，本函数就跑在
    // 驱动线程上，所以这些指针在这里一定有效。key 在 add() 之后不再变。
    std::vector<int64_t> counts;
    counts.reserve(es.size());
    for (const Entry* e : es) counts.push_back(CacheStore::get().open_count(e->key));
    return counts;
}

std::vector<Preloader::Action> Preloader::compute_plan_locked() {
    allocate_locked();
    std::vector<Action> plan;

    for (auto& up : entries_) {
        Entry* e = up.get();
        // 1) 该关的：被移除、掉到 0 额度（含给对等源让路）、已达标、已失败。
        const bool want_closed = e->removing || e->quota == 0
                                 || e->state == PreloadState::Done
                                 || e->state == PreloadState::Failed;
        if (e->source != nullptr && want_closed) {
            // 这条 source 的最后一次结账在 execute() 里（锁外、close 之后）做，
            // 不在这儿——get_stats() 会取句柄锁，那是一次可能撞上 fsync 的等待。
            Action a;
            a.kind   = ActionKind::Close;
            a.e      = e;
            a.source = std::move(e->source); // 锁内摘出来，锁外关
            plan.push_back(std::move(a));
            continue;
        }
        if (e->removing) continue;
        // 2) 该开的：有额度、没开、目标已定。
        // 【播放列表条目永不开源】它要的是"整份取回来再扫一遍"，那是下面
        // 第 4 步的 Expand 做的事（fetch_text）。这里若也给它开一条 source，
        // 同一份播放列表就会被下两遍——一遍进 source 的窗口、一遍进
        // fetch_text，而直播播放列表被重复请求本身就是错的
        // （hls_live_playlist_is_warmed_once_only 钉住这一条）。
        if (e->is_playlist) continue;
        if (e->source == nullptr && e->quota > 0 && e->estimated
            && e->state != PreloadState::Done && e->state != PreloadState::Failed) {
            Action a;
            a.kind  = ActionKind::Open;
            a.e     = e;
            a.quota = e->quota;
            a.cfg   = config_for(*e, e->quota);   // 锁内算好，execute 不读 Entry
            a.cls   = rate_class_for(e->prio);    // 同上：类别也在锁内定
            plan.push_back(std::move(a));
        } else if (e->source != nullptr) {
            // 2b) 开着且不关：优先级变过（set_priority 只改表 + 唤醒，不碰桥）
            // 导致期望类别 ≠ 已下发类别，就在这里生成一条同步动作，由
            // execute() 锁外转发，install_and_snapshot_locked 记下已下发。
            const RateClass want = rate_class_for(e->prio);
            if (want != e->sent_class) {
                Action a;
                a.kind   = ActionKind::SetClass;
                a.e      = e;
                a.cls    = want;
                a.bridge = e->source.get();
                plan.push_back(std::move(a));
            }
        }
    }
    // 3) 至多一次估算。挑选条件是 `!estimated && quota > 0`——**没有额度的
    // 条目不会去估算**，省掉一次网络往返；它拿到额度时自然会被估。
    for (auto& up : entries_) {
        Entry* e = up.get();
        if (e->removing || e->estimated) continue;
        if (e->quota <= 0) continue;
        e->state = PreloadState::Estimating;
        Action a;
        a.kind = ActionKind::Estimate;
        a.e    = e;
        plan.push_back(std::move(a));
        break;
    }
    // 4) 至多一次 HLS 展开（阻塞抓一到两次播放列表，与估算同一档慢活，
    //    所以同样每轮至多一条）。
    for (auto& up : entries_) {
        Entry* e = up.get();
        if (e->removing || !e->is_playlist || e->expanded) continue;
        if (e->state == PreloadState::Done || e->state == PreloadState::Failed) continue;
        if (e->quota <= 0) continue;         // 没额度就先不占用网络
        Action a;
        a.kind = ActionKind::Expand;
        a.e    = e;
        plan.push_back(std::move(a));
        break;
    }
    return plan;
}

void Preloader::execute(std::vector<Action>& plan) {
    // 先全部 Close（让路必须先于占用），再 Open，最后才是慢活。
    for (Action& a : plan) {
        if (a.kind != ActionKind::Close || a.source == nullptr) continue;
        a.source->interrupt();
        a.source->close();
        // 关掉之后、析构之前把最终字节数读出来（锁外，所以撞上 fsync 也只是
        // 拖慢驱动线程自己）。close() 之后 get_stats() 仍然填 downloaded_bytes。
        syp_source_stats ss{};
        a.source->get_stats(&ss);
        a.final_bytes = ss.downloaded_bytes;
        // 析构点也放在锁外：~SourceBridge 里的 CacheStore::release() 要取
        // CacheStore::mu_，那把锁下不许有人持有 mu_。
        a.source.reset();
    }
    for (Action& a : plan) {
        if (a.kind != ActionKind::Open) continue;
        syp_source_callbacks cb{};
        cb.ctx              = a.e;
        cb.on_cached_ranges = &Preloader::cb_cached_ranges;
        cb.on_total_length  = &Preloader::cb_total_length;
        cb.on_error         = &Preloader::cb_error;
        // a.e->url 在 add() 之后不再变，且条目只可能被驱动线程自己销毁。
        auto opened = SourceBridge::open(a.e->url, nullptr, a.cfg, &cb, backend_, clock_,
                                         a.cls);
        if (opened) {
            a.source = std::move(*opened);
        } else {
            a.err = opened.error();
        }
    }
    // 类别同步：锁外转发。SourceBridge::set_rate_class 会在本线程上
    // 同步跑一轮 schedule()，可能经回调进 Preloader 拿 mu_（cb_error）——
    // 所以绝不能在 mu_ 下做。放在慢活之前：升级为 Playing 是急事。
    for (Action& a : plan) {
        if (a.kind != ActionKind::SetClass || a.bridge == nullptr) continue;
        a.bridge->set_rate_class(a.cls);
    }
    // 至多一次估算：它是阻塞网络 IO（provider 要开一次连接读 header），
    // 放在锁外、且每轮只做一次——否则一次慢估算会把"高优先级要停低优先级"
    // 这件急事排到几秒之后。
    for (const Action& a : plan) {
        if (a.kind != ActionKind::Estimate) continue;
        Entry* e = a.e;
        int64_t s = 0;
        int64_t end = 0;
        // 【锁外调 provider、锁内落地结果】与"execute 不持锁"的总纪律不冲突：
        // 持锁的只有下面写回那几行，不含任何 IO。e 这个指针在这里一定有效——
        // 条目只可能被驱动线程自己在 commit() 里销毁，而本函数就跑在驱动线程上。
        // 函数指针的空判在这里是**冗余**的（add() 对没装 provider 的条目
        // 直接置 estimated=true，永远走不到 Estimate 分支），留着是因为这一
        // 行是整个类里唯一一处调用外来函数指针，代价是一次分支。
        //
        // 【这一次调用要单独兜一次异常】它是本类唯一一处
        // **同步**调用外来代码的地方（接入层那份 provider 在 src/media，
        // 再上去还有 ObjC++ 与 Swift），而 dl 层管不到它抛不抛。只靠
        // driver_main 那张网兜不住这一条：那张网的处理是"放弃这一轮再回去等"，
        // 而 `estimated` 要到下面那几行才置位——于是这条目下一轮会被再次
        // 选去估算、再抛一次，驱动线程每被唤醒一次就白跑一轮（活锁的形状）。
        // 所以在这里就地按"provider 估不出"处理：与 st != SYP_OK 那一支
        // 走**完全相同**的收尾（记 provider_miss、退回按字节、estimated 置位），
        // 调用方看到的与"provider 返回了错误"没有区别。
        syp_status st = SYP_ERR_NOT_IMPLEMENTED;
        if (provider_.estimate_range_for_ms != nullptr) {
            try {
                st = provider_.estimate_range_for_ms(provider_.ctx, e->url.c_str(),
                                                     e->want_ms, &s, &end);
            } catch (...) {
                st  = SYP_ERR_NOT_IMPLEMENTED;
                end = 0;
            }
        }
        std::lock_guard<std::mutex> g(mu_);
        e->estimated = true;
        if (st == SYP_OK && end > 0) {
            // 本版本只支持 [0, end)；provider 若写了非 0 的 start，
            // 仍然从 0 开始暖——首帧要的是头部，不是中间某一段。
            e->target_bytes = end;
        } else {
            ++provider_miss_;
            e->target_bytes = cfg_.default_preload_bytes > 0
                                  ? cfg_.default_preload_bytes : (1 << 20);
        }
        e->state = PreloadState::Pending;
        dirty_ = true;
        break;
    }
    // 至多一次 HLS 展开，同样是阻塞网络 IO、同样在锁外。放在最后：Close 让
    // 出的额度与 Open 建好的连接都不该等一次播放列表往返。
    for (const Action& a : plan) {
        if (a.kind != ActionKind::Expand) continue;
        expand_playlist(a.e);
        break;
    }
}

std::expected<std::string, syp_status> Preloader::fetch_text(const std::string& url,
                                                             Entry* e,
                                                             const std::string& owner_url) {
    const syp_config c = playlist_config();
    // 【有意用 Playing，不要"顺手改成按条目优先级"】这是驱动线程
    // 上的同步阻塞读；按 Preload 准入时播放持续消费会让它几乎拿不到额度 ⇒ 撞
    // kPlaylistFetchDeadlineMs、条目判 Failed 不重试，整个预加载器也被按住。
    // 与预加载的元数据探测（media_info_provider.cpp 经 syp_source_open）同一
    // 理由。
    auto opened = SourceBridge::open(url, nullptr, c, nullptr, backend_, clock_,
                                     RateClass::Playing);
    if (!opened) return std::unexpected<syp_status>(opened.error());
    std::unique_ptr<SourceBridge> src = std::move(*opened);
    {
        // fetch_mu_ 是叶子锁：装指针 + 记下它属于哪个条目（remove(url) 靠
        // 后者只打断该打断的那一条）。
        std::lock_guard<std::mutex> g(fetch_mu_);
        fetching_       = src.get();
        fetching_owner_ = owner_url;
    }
    {
        // stop_ 在 mu_ 下读；此时**不持** fetch_mu_，也不碰桥。
        bool stopping = false;
        {
            std::lock_guard<std::mutex> g(mu_);
            stopping = stop_;
        }
        if (stopping) src->interrupt();   // 锁外调，不持任何锁
    }

    std::string text;
    std::vector<uint8_t> buf(64 * 1024);
    syp_status st = SYP_OK;
    // 【整次抓取的墙钟起点】见 preloader.h 的
    // kPlaylistFetchDeadlineMs：三个超时都是每次 read 的，没有这条 deadline
    // 时一个滴流的服务端能把驱动线程按住 `8 MiB ÷ 滴流速率`，而让路在这段
    // 时间里整个失效。走 clock_ 而不是 chrono::now：clock.h 的规矩是
    //"超时与速度窗口只走注入式时钟"。
    const int64_t fetch_t0 = clock_.now_ms(clock_.ctx);
    for (;;) {
        {
            // 每读一块复查一次"这条目还要不要"：remove()/析构只置标志位
            // （它们一个桥调用都不做，见上面 remove 的注释），真正的放弃
            // 在这里发生。停在两次 read 之间，所以最坏等一次读超时。
            std::lock_guard<std::mutex> g(mu_);
            if (stop_ || (e != nullptr && e->removing)) {
                st = SYP_ERR_CANCELED;
                break;
            }
        }
        // 墙钟 deadline 与 stop_/removing 查在同一处、同一个节拍上：它们要
        // 兑现的是同一条承诺——"驱动线程不会被一次播放列表抓取无界地按住"。
        // 报 TIMEOUT 而不是 CANCELED：这不是谁打断了它，是它自己超了时；
        // expand_playlist 会把它落到 last_error 上，条目判 Failed 不重试
        // （与其它抓取失败同一条路，见 finish_expand_failed）。
        if (clock_.now_ms(clock_.ctx) - fetch_t0 > kPlaylistFetchDeadlineMs) {
            st = SYP_ERR_TIMEOUT;
            break;
        }
        const int32_t n = src->read(buf.data(), static_cast<int32_t>(buf.size()));
        if (n == SYP_ERR_EOF) break;
        if (n < 0) { st = static_cast<syp_status>(n); break; }
        text.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
        if (static_cast<int64_t>(text.size()) > kMaxPlaylistBytes) {
            st = SYP_ERR_OOM;
            break;
        }
    }
    {
        // 先在 fetch_mu_ 下摘掉指针，之后才 close + 析构——这个顺序就是
        // "持有 fetch_mu_ 时 fetching_ 一定指向活着的桥"这条不变式本身。
        std::lock_guard<std::mutex> g(fetch_mu_);
        fetching_ = nullptr;
        fetching_owner_.clear();
    }
    src->interrupt();
    src->close();
    src.reset();
    if (st != SYP_OK) return std::unexpected<syp_status>(st);
    return text;
}

// 展开失败的共同收尾：这条播放列表就此作罢（expanded 置位，不重试），
// 错误落到 last_error 上，commit() 会把它翻成 Failed。
void Preloader::finish_expand_failed(Entry* e, syp_status st) {
    std::lock_guard<std::mutex> g(mu_);
    e->expanded = true;
    if (e->last_error == SYP_OK) e->last_error = st;
    dirty_ = true;
}

void Preloader::expand_playlist(Entry* e) {
    std::string base;
    int64_t want_ms = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        base    = e->url;
        want_ms = e->want_ms > 0 ? e->want_ms : cfg_.default_preload_ms;
    }
    auto first = fetch_text(base, e, base);
    if (!first) return finish_expand_failed(e, first.error());

    PlaylistScan scan = scan_playlist(*first, base);
    std::string media_base = base;
    if (scan.is_master) {
        if (scan.first_variant.empty()) return finish_expand_failed(e, SYP_ERR_NOT_IMPLEMENTED);
        auto second = fetch_text(scan.first_variant, e, base);
        if (!second) return finish_expand_failed(e, second.error());
        media_base = scan.first_variant;
        scan = scan_playlist(*second, media_base);
        // 变体又是 master：不递归（一层就够，再往下多半是畸形播放列表）。
        if (scan.is_master) return finish_expand_failed(e, SYP_ERR_NOT_IMPLEMENTED);
    }
    if (scan.encrypted) {
        // 一个分片、一次密钥请求都不发：判定发生在 body 到手之后、
        // 任何子条目建立之前，而密钥 URI 本身从来不进 children。
        return finish_expand_failed(e, SYP_ERR_NOT_IMPLEMENTED);
    }

    const size_t k = segments_for_ms(scan.segments, want_ms, kMaxPreloadSegments);
    std::vector<std::string> children;
    if (!scan.map_uri.empty()) children.push_back(scan.map_uri);
    for (size_t i = 0; i < k; ++i) children.push_back(scan.segments[i].url);

    std::lock_guard<std::mutex> g(mu_);
    e->expanded = true;
    // 播放列表本身到此为止：它已经被取回并落进缓存，分片由子条目去暖。
    // 直播（无 ENDLIST）同样只走这一轮，绝不续暖——前几个分片很快过期，
    // 续暖只会一直下无人会用的字节。
    if (e->state != PreloadState::Failed) {
        e->state = PreloadState::Done;
        ++completed_;
    }
    for (const std::string& u : children) {
        if (u.empty()) continue;
        if (by_url_.find(u) != by_url_.end()) continue;   // 已有（多个播放列表共享分片）
        auto up = std::make_unique<Entry>();
        up->url    = u;
        up->key    = CacheStore::make_key(cache_dir_, u);
        up->prio   = e->prio;
        up->seq    = next_seq_++;
        up->owner  = this;
        up->parent = base;
        up->target_bytes = kSegmentTargetBytes;   // 会被资源总长夹住 ⇒ 整段
        up->estimated = true;
        by_url_.emplace(u, up.get());
        entries_.push_back(std::move(up));
    }
    dirty_ = true;
}

std::vector<std::pair<Preloader::Entry*, SourceBridge*>>
Preloader::install_and_snapshot_locked(std::vector<Action>& plan) {
    // 先把开好的 source 装回条目（锁内装、锁内读，别人才不会读到半个指针）。
    for (Action& a : plan) {
        if (a.kind != ActionKind::Open) continue;
        if (a.source != nullptr) {
            a.e->source        = std::move(a.source);
            a.e->open_quota    = a.quota;
            // 【读回缝的落点】抄下 **真正交给
            // SourceBridge::open 的那一份**（a.cfg 就是那份，execute() 里
            // 原样传进去的），不是事后再调一次 config_for 算出来的。
            // 差别是要害的：事后再算一遍的缝，对"config_for 里那行赋值被
            // 删掉"这个改动照样测不出来——它读的是同一个坏函数。
            a.e->open_cfg      = a.cfg;
            a.e->counted_bytes = 0;    // 新 source，字节从 0 数起
            a.e->sent_class    = a.cls;  // open 时交给桥的那个类别
        } else if (a.err != SYP_OK && a.e->last_error == SYP_OK) {
            a.e->last_error = a.err;
        }
    }
    // 类别同步已在 execute() 里锁外下发：记下已下发的值。若这期间优先级又
    // 变了，set_priority 已置 dirty_，下一轮规划会再比一次、再发一条。
    for (const Action& a : plan) {
        if (a.kind == ActionKind::SetClass) a.e->sent_class = a.cls;
    }
    std::vector<std::pair<Entry*, SourceBridge*>> srcs;
    srcs.reserve(entries_.size());
    for (auto& up : entries_) {
        up->meas = Meas{};
        if (up->source != nullptr) srcs.emplace_back(up.get(), up->source.get());
    }
    return srcs;
}

void Preloader::measure(const std::vector<std::pair<Entry*, SourceBridge*>>& srcs,
                        std::vector<Meas>* out) const {
    // 【锁外】这三个调用都会取桥的 mu_，get_stats/cached_ranges 还会取句柄锁，
    // 而句柄锁会被后端线程握着做 fsync。裸指针在这里一定有效：桥只由驱动
    // 线程（也就是本函数的调用者）在 execute() 里析构。
    out->clear();
    out->reserve(srcs.size());
    for (const auto& pr : srcs) {
        Meas m;
        // 【采样次序是 load-bearing 的：区间在前、字节计数在后】
        //
        // 这三个调用各取一次桥的 mu_，所以它们**不是**一个原子快照。而
        // SourceBridge::persist_chunk 在**同一个** mu_ 临界区里做
        // `index->add_range(r)` 与 `downloaded_bytes_ += n`，两者永远同步更新。
        // 于是采样次序直接决定了谁更新：
        //   · 原先是"先 get_stats、后 cached_ranges"——reached_target 判的是
        //     **更新的**区间，commit 记的是**更旧的**字节数。条目进 Done 的
        //     那一刻，downloaded_bytes 可以比区间少一整块（256KiB 以内）。
        //     外部完全可观测：wait_terminal_for_test 返回 Done 之后立刻读
        //     get_stats，就会读到一个对不上账的数。曾经在干净树上因此吃了一次
        //     **假红**（6 个并发 ./test_preloader → 1/6 红在
        //     `st.downloaded_bytes >= 32000`）；本机复现 1/6 红在
        //     byte_target_downloads_then_stops 的 `>= 16 * 1024`。
        //   · 现在反过来：区间取在前（t1）、字节计数取在后（t2 > t1）。两者都
        //     单调不减，所以 downloaded(t2) >= downloaded(t1) >= 区间(t1) 所
        //     对应的字节（冷缓存下两者相等；热缓存下命中的那部分记在
        //     cache_hit_bytes 里，不在 downloaded 里，方向仍然安全）。
        //     也就是说"条目进 Done"⇒"downloaded_bytes 已经把判定所依据的那些
        //     区间全部算进去了"，这条蕴含现在是真的。
        // 代价：达标的判定最多**晚一轮**（用的是稍旧的区间快照）。一轮就是
        // 一次驱动循环，条目本来就还开着源，没有任何损失。
        m.have_range = pr.second->cached_ranges(&m.first, 1) >= 1;
        m.total      = pr.second->length();
        syp_source_stats ss{};
        pr.second->get_stats(&ss);
        m.downloaded = ss.downloaded_bytes;
        m.active     = ss.active_tasks;
        m.valid      = true;
        out->push_back(m);
    }
}

void Preloader::commit(std::vector<Action>& plan,
                       const std::vector<std::pair<Entry*, SourceBridge*>>& srcs,
                       const std::vector<Meas>& meas) {
    // 【PreloadState::Estimating 在这里不需要任何特殊处理】估算的结果
    // （target_bytes + 回到 Pending）是 execute() 在 provider 返回之后**当场**
    // 写回的，所以走到这里时状态已经是 Pending。就算将来 Estimating 真的
    // 跨了轮（异步 provider），把它打回 Pending 也不丢任何东西：重发
    // Estimate 的判据是 `!estimated`，与状态无关。早先这里有一个
    // `state != Estimating` 的排除条件，实测在所有路径（含上面那个假想
    // 路径）都是死代码，已删。
    // 1) 关掉的那些 source：把 execute() 在锁外读到的最终字节数并进总账。
    for (const Action& a : plan) {
        if (a.kind != ActionKind::Close || a.final_bytes < 0) continue;
        downloaded_bytes_ = sat_add(downloaded_bytes_,
                                    a.final_bytes - a.e->counted_bytes);
        a.e->counted_bytes = a.final_bytes;
    }
    // 2) 还开着的那些：把锁外量到的数落到条目上，并重算活跃任务数。
    int64_t active = 0;
    for (size_t i = 0; i < srcs.size() && i < meas.size(); ++i) {
        Entry* e = srcs[i].first;
        // 这一轮里 source 只会被"装回"，不会被摘掉，所以 srcs 的条目一定还在。
        e->meas = meas[i];
        downloaded_bytes_ = sat_add(downloaded_bytes_,
                                    e->meas.downloaded - e->counted_bytes);
        e->counted_bytes = e->meas.downloaded;
        active += e->meas.active;
    }
    active_tasks_ = active;

    bool again = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
        Entry* e = it->get();
        if (e->removing && e->source == nullptr) {
            by_url_.erase(e->url);
            it = entries_.erase(it);
            again = true;
            continue;
        }
        const bool open = e->source != nullptr;
        const bool terminal = e->state == PreloadState::Done
                              || e->state == PreloadState::Failed;
        if (!terminal && e->last_error != SYP_OK) {
            e->state = PreloadState::Failed;
            ++failed_;
            again = true;                      // 下一轮把 source 关掉
        } else if (!terminal && open && reached_target(*e)) {   // 只看本轮量到的数
            e->state = PreloadState::Done;
            ++completed_;
            again = true;                      // 下一轮关源，腾出额度
        } else if (!terminal && open) {
            e->state = PreloadState::Running;
        } else if (!terminal) {
            e->state = PreloadState::Pending;
        }
        ++it;
    }
    if (again) dirty_ = true;   // 状态变了 → 再跑一轮（关掉达标的源、腾出额度）
}

// 【整条驱动线程包在 try/catch 里】
//
// 这条线程上有 push_back / make_unique<Entry> / by_url_.emplace，以及
// fetch_text 里最大 kMaxPlaylistBytes = 8 MiB 的 `text.append`。仓库没有
// -fno-exceptions，所以其中任何一次 bad_alloc 在**没有 catch 的线程函数**里
// 都不是"这条预加载失败"——它是 **std::terminate**，在内存吃紧的 iOS 设备上
// 就是整个 app 挂掉。C ABI 层其余地方一律 `new (std::nothrow)`，唯独这条
// 线程是破例。
//
// 【接住之后做什么：放弃这一轮，然后回去等】不继续往下跑，也不无限重试：
//   · working_ 必须复位并叫醒 idle_cv_，否则 wait_settled_for_test 永远
//     等不到（析构那条路不受影响，它走的是 stop_ + join）；
//   · **不置 dirty_**。这一轮开头已经把 dirty_ 清掉了，所以下一圈会停在
//     cv_.wait 上老实等着，不会变成一个烧 CPU 的重试循环；下一次 add() /
//     进度回调自然会把它叫醒，那时内存可能已经宽松了。
//   · 已经开出去的 source 随 plan 的局部析构在**本线程**上关掉，与正常
//     路径同一个线程、同一个时机。
//
// 【剩下的那个尖角，如实记着】catch 里那次 lock_guard 自己理论上也能抛
// （system_error）。那一条没有更外层的防线——真到那一步，进程已经没有可用的
// 互斥量了，任何"优雅处理"都是自欺。
void Preloader::driver_main() {
    for (;;) {
      try {
        std::vector<Entry*> probes;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (needs_peer_poll_locked()) {
                // 超时不是断言、也不承担正确性：它只是"去问一句 open_count"的
                // 周期。超时醒来照样跑一轮（此时 dirty_ 可能是 false）。
                cv_.wait_for(lk, std::chrono::milliseconds(kPeerPollMs),
                             [this] { return stop_ || dirty_; });
            } else {
                cv_.wait(lk, [this] { return stop_ || dirty_; });
            }
            if (stop_) break;
            dirty_   = false;
            working_ = true;
            probes   = probe_list_locked();
        }
        const std::vector<int64_t> counts = probe_peers(probes);
        std::vector<Action> plan;
        {
            std::lock_guard<std::mutex> g(mu_);
            // 先全清：没被探测的条目（终态 / removing）不该留着上一轮的结论，
            // 否则 peer_yield_for_test 会对一条早已 Done 的条目报"正在让路"。
            for (auto& up : entries_) up->peer_busy = false;
            for (size_t i = 0; i < probes.size() && i < counts.size(); ++i) {
                Entry* e = probes[i];
                // 自己开着的那一份引用不算"别人"。
                const int64_t self = e->source != nullptr ? 1 : 0;
                e->peer_busy = counts[i] > self;
            }
            plan = compute_plan_locked();
        }
        execute(plan);
        std::vector<std::pair<Entry*, SourceBridge*>> srcs;
        {
            std::lock_guard<std::mutex> g(mu_);
            srcs = install_and_snapshot_locked(plan);
        }
        std::vector<Meas> meas;
        measure(srcs, &meas);      // 锁外：会撞句柄锁/fsync 的三个调用全在这里
        {
            std::lock_guard<std::mutex> g(mu_);
            commit(plan, srcs, meas);
            working_ = false;
        }
        plan.clear();       // 锁外析构（Close 的 source 已经在 execute 里放掉了）
        idle_cv_.notify_all();
      } catch (...) {
        // 见上面那段：放弃这一轮，复位 working_，**不置 dirty_**，回去等。
        {
            std::lock_guard<std::mutex> g(mu_);
            working_ = false;
        }
        idle_cv_.notify_all();
        log_msg(SYP_LOG_WARN, "preload", "driver round aborted by an exception");
      }
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        working_ = false;
    }
    idle_cv_.notify_all();
}

void Preloader::close_all() {
    // 只在析构、驱动线程已 join 之后调用，所以不需要持锁。
    for (auto& up : entries_) {
        if (up->source == nullptr) continue;
        up->source->interrupt();
        up->source->close();
        up->source.reset();
    }
}

bool Preloader::reached_target(const Entry& e) {
    // 纯算术：只看 measure() 在锁外量到的那一份快照，不碰桥、不取锁。
    if (!e.meas.valid) return false;
    int64_t goal = e.target_bytes;
    const int64_t total = e.meas.total;
    if (total >= 0 && goal > total) goal = total;
    if (goal <= 0) return total == 0;
    if (!e.meas.have_range) return false;
    // HoleSet 不变式保证区间升序且两两不相邻，所以"覆盖 [0, goal)"
    // 等价于"第一段从 0 开始且至少到 goal"。
    return e.meas.first.start <= 0 && e.meas.first.end >= goal;
}

syp_config Preloader::base_config() const noexcept {
    // 容量三件套（max_cache_bytes / min_free_space_bytes / cache_ttl_ms）在这里
    // **原样带下去**，config_for 与 fetch_text 都不碰它们——它们只覆盖并发、
    // 超时、窗口这几类字段。读回缝 dl_config_for_test() 读的就是这个函数的
    // 输出，所以"谁在这条路径上把容量清零"是可被用例杀掉的。
    syp_config c = dl_cfg_;
    c.cache_dir = cache_dir_.c_str();
    return c;
}

// 播放列表抓取那一条源的配置。
//
// 【容量三件套在这里原样通过】max_cache_bytes /
// min_free_space_bytes / cache_ttl_ms 一个都不碰：这条路径开出去的
// SourceBridge 关闭时同样会走 enforce_capacity，它是"缓存有上限、会过期"
// 这件事在播放列表这一支上的唯一依据。MUT_R8f（在这里把三个字段清零）此前是
// ctest 33/33 + xcodebuild 82/0 双绿存活的；现在由读回缝 playlist_config_for_test() 与行为用例
// hls_playlist_fetch_carries_capacity_and_evicts_an_unreferenced_entry
// 两条一起守着。
syp_config Preloader::playlist_config() const noexcept {
    syp_config c = base_config();
    c.max_concurrent_tasks = 1;      // 播放列表小，一条连接顺序读最快
    c.first_buffer_ms      = 0;
    c.target_buffer_ms     = 0;
    // 时间上界（见 preloader.h 的三个常量）：这次抓取同步占着驱动线程，
    // 默认配置下一个只接受连接不响应的服务端能把它按住两分钟。
    c.connect_timeout_ms   = kPlaylistConnectTimeoutMs;
    c.read_timeout_ms      = kPlaylistReadTimeoutMs;
    c.max_retries          = kPlaylistMaxRetries;
    return c;
}

syp_config Preloader::config_for(const Entry& e, int32_t quota) const {
    syp_config c = base_config();
    c.max_concurrent_tasks = quota > 0 ? quota : 1;
    int64_t goal = e.target_bytes > 0 ? e.target_bytes : cfg_.default_preload_bytes;
    if (goal <= 0) goal = 1 << 20;
    const int64_t per_conn = ceil_div_pos(goal, c.max_concurrent_tasks);
    c.min_segment_size  = per_conn > 0 ? per_conn : 1;
    c.segment_size_hint = 0;
    // 两个 *_buffer_ms 显式清零：lookahead_bytes_locked() 的"按时长换算"
    // 分支要求 duration_ms > 0 且 buf_ms > 0，预加载从不调 update_playback，
    // 前者恒不成立；清零是为了将来即便有人给 duration_ms 填了值，窗口也
    // 仍然由这里的配置钉死。
    c.first_buffer_ms  = 0;
    c.target_buffer_ms = 0;
    return c;
}

// ---------------------------------------------------------------- 测试缝
void Preloader::wait_settled_for_test() {
    std::unique_lock<std::mutex> lk(mu_);
    idle_cv_.wait(lk, [this] { return stop_ || (!dirty_ && !working_); });
}

int32_t Preloader::wait_terminal_for_test(const std::string& url, int32_t timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    const bool ok = idle_cv_.wait_for(
        lk, std::chrono::milliseconds(timeout_ms), [this, &url] {
            const Entry* e = find_locked(url);
            if (e == nullptr) return true;
            return e->state == PreloadState::Done || e->state == PreloadState::Failed;
        });
    if (!ok) return -2;
    const Entry* e = find_locked(url);
    return e == nullptr ? -1 : static_cast<int32_t>(e->state);
}

int32_t Preloader::quota_for_test(const std::string& url) const {
    std::lock_guard<std::mutex> g(mu_);
    const Entry* e = find_locked(url);
    return e == nullptr ? -1 : e->quota;
}

syp_config Preloader::source_config_for_test(const std::string& url) const {
    std::lock_guard<std::mutex> g(mu_);
    const Entry* e = find_locked(url);
    return e == nullptr ? syp_config{} : e->open_cfg;
}

int32_t Preloader::state_for_test(const std::string& url) const {
    std::lock_guard<std::mutex> g(mu_);
    const Entry* e = find_locked(url);
    return e == nullptr ? -1 : static_cast<int32_t>(e->state);
}

bool Preloader::driver_busy_for_test() const {
    std::lock_guard<std::mutex> g(mu_);
    return working_;
}

int32_t Preloader::rate_class_for_test(const std::string& url) const {
    // 持 mu_ 调桥：只此一处，只因为是测试缝（理由见头文件）。
    std::lock_guard<std::mutex> g(mu_);
    const Entry* e = find_locked(url);
    if (e == nullptr || e->source == nullptr) return -1;
    return static_cast<int32_t>(e->source->rate_class_for_test());
}

int32_t Preloader::open_rate_class_for_test(const std::string& url) const {
    std::lock_guard<std::mutex> g(mu_);
    const Entry* e = find_locked(url);
    if (e == nullptr || e->source == nullptr) return -1;
    return static_cast<int32_t>(e->source->open_rate_class_for_test());
}

int32_t Preloader::peer_yield_for_test(const std::string& url) const {
    std::lock_guard<std::mutex> g(mu_);
    const Entry* e = find_locked(url);
    if (e == nullptr) return -1;
    return e->peer_busy ? 1 : 0;
}

}  // namespace syp::dl
