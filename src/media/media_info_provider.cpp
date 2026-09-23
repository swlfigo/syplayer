// media_info_provider.cpp — 只读 header 的一次性探测 + 按 URL 的内存缓存。
#include "media/media_info_provider.h"

#include "media/avio_bridge.h"
#include "media/demuxer.h"

#include <syplayer/syp_source.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

namespace syp::media {
namespace {

constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();

// a * b / c，溢出时退化为先除后乘。
int64_t mul_div(int64_t a, int64_t b, int64_t c) noexcept {
    if (a <= 0 || b <= 0 || c <= 0) return 0;
    if (a > kI64Max / b) return (a / c) * b;
    return (a * b) / c;
}

int64_t sat_add(int64_t a, int64_t b) noexcept {
    if (b > 0 && a > kI64Max - b) return kI64Max;
    return a + b;
}

int64_t covered_bytes(const syp_range* rs, int32_t n) noexcept {
    int64_t sum = 0;
    for (int32_t i = 0; i < n; ++i) {
        if (rs[i].end > rs[i].start) sum = sat_add(sum, rs[i].end - rs[i].start);
    }
    return sum;
}

std::vector<syp_range> read_ranges(syp_source* src) {
    const int32_t n = syp_source_cached_ranges(src, nullptr, 0);
    if (n <= 0) return {};
    std::vector<syp_range> rs(static_cast<size_t>(n));
    const int32_t got = syp_source_cached_ranges(src, rs.data(), n);
    rs.resize(static_cast<size_t>(got > 0 ? got : 0));
    return rs;
}

// 探测的字节上界看门。挂在 syp_source 的 on_cached_ranges 上：一旦这个源
// **这次探测新够到**的字节超过 kProbeMaxBytes，就打断它，让阻塞中的 FFmpeg
// read 立刻拿到 SYP_ERR_CANCELED，探测整体放弃。
//
// 【数的是增量，不是总量】已缓存字节里有一部分是**别人
// 早就下好的**——上一次预加载、上一次播放，甚至上一次探测自己。拿总量去比
// 上界，等于让一份热缓存悄悄关掉按时间预加载：一次按时间的预加载就会缓存
// 超过上界的字节，于是同一个 URL 的下一次探测在零网络字节处就放弃（实测
// 3.1MB 预热 → NOT_IMPLEMENTED）。所以 open 之后先量一次基线，之后一律比
// 差值。基线量到之前（open 还没返回时到达的那次回调）不做判定，由 open
// 返回后的复查兜底。
//
// 【为什么可以在回调里回头调 syp_source_interrupt】SourceBridge 的
// fire_cached_ranges() 在**锁外**调用户回调（source_bridge.cpp:625，mu_ 与
// 句柄锁都已释放），而 interrupt() 只取桥的 mu_ 置一个标志位 + notify，
// 是一片叶子——没有反序，也不会自死锁。
//
// 【这里绝不能碰 AvioBridge】probe() 的析构顺序是 Demuxer → AvioBridge →
// syp_source_close，也就是说**回调还可能到达时 avio 已经没了**。所以看门
// 只持有 syp_source* 与两个原子量，一个字都不碰 avio
// （AvioBridge::request_abort 的生命周期契约也正是这条）。
//
// src 是原子的：syp_source_open() 在返回之前就可能触发一次回调
// （source_bridge.cpp:266，热缓存时把已有区间报给调用方），那一刻我们还
// 没拿到 syp_source*。这种情况下只置 exceeded，由 open 返回后的复查兜底。
//
// 【墙钟看门狗也挂在这里】字节上界与三个超时之间有一条缝：
// "每次读都在读超时之前回来一点点"两边都够不着。实测 1 KiB/s 滴流的服务端
// **跑满 150 秒仍未结束**，理论上要 ~2,048 秒（~34 分钟）才够到 kProbeMaxBytes。
// 看门狗与字节上界共用同一条开火路径（置 exceeded + syp_source_interrupt），
// 所以后果完全一致：放弃探测、退化为按字节。取值与已知局限见
// media_info_provider.h 的 kProbeWallClockMs。
struct ProbeGuard {
    std::atomic<syp_source*> src{nullptr};
    std::atomic<bool>        exceeded{false};
    // open 那一刻这个 URL 已经缓存了多少字节。-1 = 还没量到。
    std::atomic<int64_t>     baseline{-1};
    // 整次探测的墙钟死线。构造即起算——连接与 open 也算在里面，它要管的是
    // "这次探测一共占了驱动线程多久"，不是"读了多久"。
    const std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kProbeWallClockMs)};
    // 供诊断与用例区分"超字节"与"超时"两种放弃原因。
    std::atomic<bool>        timed_out{false};

    // 超时则打断并返回 true。可从任意线程调用（回调在后端线程上到达，
    // probe() 自己在驱动线程上调）。
    bool trip_if_past_deadline() noexcept {
        if (std::chrono::steady_clock::now() < deadline) return false;
        timed_out.store(true, std::memory_order_release);
        exceeded.store(true, std::memory_order_release);
        syp_source* s = src.load(std::memory_order_acquire);
        if (s != nullptr) syp_source_interrupt(s);
        return true;
    }

    void note(const syp_range* rs, int32_t n) noexcept {
        // 墙钟检查排在**所有**前置 return 之前：一次没带区间的回调、或者
        // 基线还没量到时到达的回调，同样是"时间在流逝"的证据。滴流场景下
        // 每收到一块字节就来一次回调（source_bridge.cpp fire_cached_ranges
        // 在每次 on_data 上调），所以这条路径每秒都有机会开火。
        if (trip_if_past_deadline()) return;
        if (rs == nullptr || n <= 0) return;
        const int64_t base = baseline.load(std::memory_order_acquire);
        if (base < 0) return;                       // 基线未定：留给 open 后的复查
        if (covered_bytes(rs, n) - base <= kProbeMaxBytes) return;
        exceeded.store(true, std::memory_order_release);
        syp_source* s = src.load(std::memory_order_acquire);
        if (s != nullptr) syp_source_interrupt(s);
    }
    static void on_ranges(void* ctx, const syp_range* rs, int32_t n) {
        auto* self = static_cast<ProbeGuard*>(ctx);
        if (self != nullptr) self->note(rs, n);
    }
};

}  // namespace

int64_t estimate_bytes_for(int64_t duration_us, int64_t total_bytes,
                           int64_t bit_rate, int64_t header_bytes,
                           int64_t ms) noexcept {
    // ms 上界：下面要先算 ms * 1000（微秒），溢出的输入直接当"估不出"退化为
    // 按字节——一个大到会溢出的预加载时长本来也没有意义。
    if (ms <= 0 || ms > kI64Max / 1000) return 0;

    int64_t bytes = 0;
    if (duration_us > 0 && total_bytes > 0) {
        bytes = mul_div(total_bytes, ms * 1000, duration_us);
    } else if (bit_rate > 0) {
        bytes = mul_div(bit_rate / 8, ms, 1000);
    }
    if (bytes <= 0) return 0;

    // 宁可多下：CBR 折算对 VBR 素材可能显著偏低，而"少下"意味着首帧仍要
    // 等网络，"多下"只是多占一点缓存（明确只保证不少于这一侧）。
    bytes = mul_div(bytes, kEstimateSafetyNum, kEstimateSafetyDen);
    // 容器头的常数项。**加**在安全系数之后，不是跟它取 max——取 max 的话
    // 这一项在"估算值本来就比头部大"的区间里（也就是误差最要命的那一段）
    // 恒不生效，而且结果对 ms 不再单调。见头文件。
    if (header_bytes > 0) bytes = sat_add(bytes, header_bytes);
    if (bytes < kMinEstimateBytes) bytes = kMinEstimateBytes;
    if (total_bytes > 0 && bytes > total_bytes) bytes = total_bytes;
    return bytes;
}

MediaInfoProvider::MediaInfoProvider(const syp_config& dl_cfg) : cfg_(dl_cfg) {
    if (cfg_.cache_dir != nullptr) cache_dir_ = cfg_.cache_dir;
    cfg_.cache_dir = cache_dir_.c_str();
}

MediaInfoProvider::~MediaInfoProvider() = default;

syp_media_info_provider MediaInfoProvider::table() noexcept {
    syp_media_info_provider t{};
    t.ctx = this;
    t.estimate_range_for_ms = &MediaInfoProvider::c_estimate;
    return t;
}

syp_status MediaInfoProvider::c_estimate(void* ctx, const char* url, int64_t ms,
                                         int64_t* out_start, int64_t* out_end) {
    auto* self = static_cast<MediaInfoProvider*>(ctx);
    if (self == nullptr) return SYP_ERR_NOT_IMPLEMENTED;
    return self->estimate_range_for_ms(url, ms, out_start, out_end);
}

// 探测那一条源的配置。
//
// 【为什么提成一个函数】"读回缝拍照之后还能再改一手配置"这个问题此前在
// 三处地方发现过（HlsSession / fetch_text / config_for）。实测还有第四处，
// 而且它就在本文件里：config_for_test() 读的是 cfg_，而 probe() 原先是
// `syp_config c = cfg_;` 之后再覆盖若干字段——也就是说 cfg_ 这条缝同样只
// 断到起点：在那一行之后把容量三件套清零，用例仍然全绿，说明没有真正
// 钉住。提成函数之后，读回缝 probe_config_for_test() 断的是 probe() 真正
// 交给 syp_source_open 的那一份。
//
// 容量三件套在这里同样原样通过：探测源也会在关闭时走 enforce_capacity。
syp_config MediaInfoProvider::probe_config() const noexcept {
    // 探测不需要并发：一条连接顺序读 header 就够，多开只会抢播放的带宽。
    syp_config c = cfg_;
    c.cache_dir            = cache_dir_.c_str();
    c.max_concurrent_tasks = 1;
    // 【时间上界，与字节上界同等重要】字节上界只管住
    // "下太多"，管不住"一个字节都不来"：一个只接受连接、永不响应的服务端
    // 用默认配置（15s 读超时 × 重试）把这次探测按住实测 120,071ms，而这次
    // 探测同步占着 Preloader 的驱动线程，~Preloader 要等它——syp_preload.h
    // 对 provider 的头一条硬约束"必须尽快返回"正是这么被违反的。
    c.connect_timeout_ms   = kProbeConnectTimeoutMs;
    c.read_timeout_ms      = kProbeReadTimeoutMs;
    c.max_retries          = kProbeMaxRetries;
    return c;
}

std::optional<MediaInfoProvider::Info> MediaInfoProvider::probe(const std::string& url) {
    syp_source* src = nullptr;
    const syp_config c = probe_config();

    // guard 必须活过 syp_source_close：回调在后端线程上到达，close() 才是
    // "此后不会再有回调"的那条分界线。
    ProbeGuard guard;
    syp_source_callbacks cb{};
    cb.ctx              = &guard;
    cb.on_cached_ranges = &ProbeGuard::on_ranges;

    // 限速：公开 syp_source_open 没有类别参数，这次探测按 Playing 准入
    // （SourceBridge::open 的默认值）。有意如此——驱动线程上的同步阻塞读，按
    // Preload 准入时播放持续消费会让它几乎拿不到额度、撞 kProbeWallClockMs。
    // 与 Preloader::fetch_text 同一理由。
    if (syp_source_open(&src, url.c_str(), nullptr, &c, &cb) != SYP_OK
        || src == nullptr) {
        return std::nullopt;
    }
    guard.src.store(src, std::memory_order_release);

    Info info;
    bool over = false;
    // 基线：open 刚返回时这个 URL 已经缓存了多少字节。之后所有判定都比
    // "现状 - 基线"。这里可能已经混进了 open 之后这几微秒里新下的几个字节，
    // 那只会让上界**松**一点点（多允许几字节），不会让它失效。
    const int64_t baseline = [&] {
        const std::vector<syp_range> rs0 = read_ranges(src);
        return covered_bytes(rs0.data(), static_cast<int32_t>(rs0.size()));
    }();
    guard.baseline.store(baseline, std::memory_order_release);
    {
        // 基线量到之前到达的回调只置位、不判定（那时 baseline 还是 -1），
        // 所以这里复查一次现状，把那一段窗口补上。
        const std::vector<syp_range> rs0 = read_ranges(src);
        over = guard.exceeded.load(std::memory_order_acquire)
               || covered_bytes(rs0.data(), static_cast<int32_t>(rs0.size())) - baseline
                      > kProbeMaxBytes;
    }
    // 墙钟检查点 1/3（F5）：open 本身就可能花掉整条预算（连接超时 × 重试）。
    if (guard.trip_if_past_deadline()) over = true;

    std::unique_ptr<AvioBridge> avio;
    if (!over) {
        avio = AvioBridge::create(src, /*buffer_size=*/0);
        if (avio == nullptr) over = true;
    }
    std::unique_ptr<Demuxer> d;
    // 墙钟检查点 2/3（F5）：AvioBridge::create 本身不做 IO，这里主要是把
    // "已经超时了就别再往下走"这条判据放在每一段之间，而不是只放一处。
    if (!over && guard.trip_if_past_deadline()) over = true;
    if (!over) {
        syp_status derr = SYP_OK;
        d = Demuxer::open_avio(avio->ctx(), &derr);
        if (d != nullptr) {
            info.duration_us = d->duration_us();
            AVFormatContext* fmt = d->raw();
            info.bit_rate = fmt != nullptr ? static_cast<int64_t>(fmt->bit_rate) : 0;
        }
        info.total_bytes = syp_source_length(src);
        // 探测实际够到的前缀长度 = **FFmpeg 经 AvioBridge 读到过的最大偏移**。
        //
        // 【为什么不再取 cached_ranges[0].end】那是**缓存**的属性，不是这次
        // 解析的属性：缓存里本来就有 1MB，这个常数项就变成
        // 1MB，同一个 URL 冷热两次估出两个值（实测 2,229,233 vs 1,025,009）。
        // 读到的最大偏移只数经过本次解析的读，与缓存里原本有什么无关，而且
        // 比"已缓存前缀"更紧——source 是按不小于 min_segment_size 的块下的，
        // 已缓存前缀恒不小于真正读到的位置（N4 的 118% 超估就有它一份）。
        // 仍然夹在上界里：常数项不该比一次探测允许够到的字节还大。
        if (avio != nullptr) {
            const int64_t reach = avio->diag().max_read_end;
            info.header_bytes = reach > kProbeMaxBytes ? kProbeMaxBytes : reach;
        }
        const std::vector<syp_range> rs = read_ranges(src);
        over = guard.exceeded.load(std::memory_order_acquire)
               || covered_bytes(rs.data(), static_cast<int32_t>(rs.size())) - baseline
                      > kProbeMaxBytes;
        // 墙钟检查点 3/3（F5）：Demuxer::open_avio 里是一整段阻塞解析
        // （avformat_open_input + find_stream_info）。它期间的打断靠回调路径；
        // 这里兜住"它自己返回了，但整条预算已经花光"——那种结果不该被当成
        // 一次成功的探测记进 cache_（记进去等于把一个在超时边缘解析出来的
        // 半残结果永久化）。
        if (guard.trip_if_past_deadline()) over = true;
    }

    // 先拆 Demuxer（它持着 AVIOContext 的使用权），再拆 AvioBridge，
    // 最后关 source —— 与 SYPBridge.mm 的 close_internal 同一顺序。
    d.reset();
    avio.reset();
    syp_source_close(src);
    // close 之后回调不会再来，guard 可以安全离开作用域。

    if (over) return std::nullopt;           // 超上界：退化为按字节
    if (info.duration_us <= 0 && info.bit_rate <= 0) return std::nullopt;
    return info;
}

syp_status MediaInfoProvider::estimate_range_for_ms(const char* url, int64_t ms,
                                                    int64_t* out_start,
                                                    int64_t* out_end) {
    // ms 的上界检查在 estimate_bytes_for 里（它要算 ms * 1000）；这里只挡
    // 掉"连探测都不必发起"的输入，省一次网络往返。
    if (url == nullptr || url[0] == '\0' || ms <= 0) return SYP_ERR_NOT_IMPLEMENTED;
    const std::string key(url);

    std::optional<Info> info;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = cache_.find(key);
        if (it != cache_.end()) info = it->second;
    }
    if (!info) {
        // 探测在锁外：它是网络 IO，持锁会让同时来的别的 URL 全部排队。
        info = probe(key);
        if (!info) return SYP_ERR_NOT_IMPLEMENTED;
        std::lock_guard<std::mutex> g(mu_);
        cache_.emplace(key, *info);
    }

    const int64_t bytes = estimate_bytes_for(info->duration_us, info->total_bytes,
                                             info->bit_rate, info->header_bytes, ms);
    if (bytes <= 0) return SYP_ERR_NOT_IMPLEMENTED;

    if (out_start != nullptr) *out_start = 0;
    if (out_end != nullptr) *out_end = bytes;
    return SYP_OK;
}

// ------------------------------------------------------------- PreloadStack

std::unique_ptr<PreloadStack> PreloadStack::create(const syp_config& dl_cfg,
                                                   const syp::dl::PreloadConfig& cfg,
                                                   const syp_http_backend* backend,
                                                   syp_status* err) {
    if (err != nullptr) *err = SYP_OK;
    // 归一化一次：尾斜杠是同一个目录的另一种拼法，而 CacheStore::make_key
    // 不归一化目录——两种拼法就是两份缓存（见 dl/cache_store.h）。
    // 实现搬到了 normalize_cache_dir()：接入层（SYPBridge.mm）的
    // 播放侧要走同一份归一化，而它够不到这个函数体。之后那一份又下沉进了
    // src/dl，于是 C ABI 也被收口。
    // 这里**仍然要自己调一次**，不是冗余：下面 dir.empty() 的判空、以及
    // self->cache_dir_ 这个"两个对象共享的同一份字符串"都要用归一化后的结果。
    // 与 Preloader 构造函数里那一次叠加无害——它是幂等的。
    std::string dir = syp::dl::normalize_cache_dir(
        dl_cfg.cache_dir != nullptr ? std::string(dl_cfg.cache_dir) : std::string());
    if (dir.empty()) {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }
    const syp_http_backend* be = backend != nullptr ? backend
                                                    : syp::dl::current_http_backend();
    if (be == nullptr) {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    auto self = std::unique_ptr<PreloadStack>(new (std::nothrow) PreloadStack());
    if (!self) {
        if (err != nullptr) *err = SYP_ERR_OOM;
        return nullptr;
    }
    self->cache_dir_ = std::move(dir);

    // 这一份 syp_config 是两边唯一的输入：cache_dir 指向 self->cache_dir_，
    // 两个对象各自把它拷进自己的 std::string，于是内容逐字节相同。
    syp_config c = dl_cfg;
    c.cache_dir = self->cache_dir_.c_str();

    self->provider_ = std::make_unique<MediaInfoProvider>(c);
    const syp_media_info_provider tbl = self->provider_->table();
    syp::dl::MediaInfoProvider dlp{};
    dlp.ctx = tbl.ctx;
    dlp.estimate_range_for_ms = tbl.estimate_range_for_ms;

    syp_status perr = SYP_OK;
    self->preloader_ = syp::dl::Preloader::create(c, cfg, &dlp, be,
                                                  syp::dl::system_clock(), &perr);
    if (self->preloader_ == nullptr) {
        if (err != nullptr) *err = perr != SYP_OK ? perr : SYP_ERR_INVALID_ARG;
        return nullptr;
    }
    return self;
}

PreloadStack::~PreloadStack() {
    // 显式，不靠成员顺序的隐式规则读者自己推：preloader 的驱动线程可能正
    // 卡在 provider 的 estimate_range_for_ms 里，它必须先死透。
    preloader_.reset();
    provider_.reset();
}

const std::string& PreloadStack::provider_cache_dir_for_test() const noexcept {
    return provider_->cache_dir();
}

const std::string& PreloadStack::preloader_cache_dir_for_test() const noexcept {
    return preloader_->cache_dir_for_test();
}

const syp_config& PreloadStack::provider_config_for_test() const noexcept {
    return provider_->config_for_test();
}

syp_config PreloadStack::provider_probe_config_for_test() const noexcept {
    return provider_->probe_config_for_test();
}

syp_config PreloadStack::preloader_config_for_test() const noexcept {
    return preloader_->dl_config_for_test();
}

}  // namespace syp::media
