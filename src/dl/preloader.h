// preloader.h — 预加载：把"下一个要播的资源的前若干秒"提前下进共享缓存。
//
// 【它不做什么】不建 Pipeline、不解码、不 read()、不预连接。只暖字节。
// 一条预加载最坏也只占几个 HTTP 连接和一份 CacheIndex，绝不会是 Pipeline
// 那量级的 160MB。
//
// 【它怎么下】每个条目持一个 SourceBridge，但**从不调它的 read()**
// （唯一的例外是 HLS 的播放列表抓取，见下面那段）。
// 下载窗口完全由每条目自己那份 syp_config 钉出来：
//   min_segment_size = ceil(target_bytes / quota)，segment_size_hint = 0，
//   max_concurrent_tasks = quota，first_buffer_ms = target_buffer_ms = 0。
// 于是 SourceBridge::lookahead_bytes_locked() 算出的窗口恰好是
//   per_conn × quota ∈ [target_bytes, target_bytes + quota - 1]，
// 右端再被资源总长夹住。达标（已缓存区间覆盖 [0, min(target, total))）就
// 关掉这个 source——缓存留着，下次播放直接命中。
//
// 不用 "seek(0) + update_playback 驱动窗口" 这条路：
// update_playback 那条路要靠 syp_playback_state::duration_ms，而它
// **生产环境无人填写**；用配置钉窗口不依赖任何外部
// 输入，且对"估不出时长"的资源同样成立。
//
// 【HLS：播放列表条目不下载自己，它展开成分片条目】
// URL 看起来是 m3u8/m3u（is_playlist_url）的条目走另一条路：
//   · 它**不开 SourceBridge**（开了就等于把播放列表下两遍——一遍给 source、
//     一遍给下面的 fetch_text）；
//   · 驱动线程在 execute() 里 fetch_text() 把它整份取回来（这是本类唯一
//     调 SourceBridge::read() 的地方），扫一遍：master 就再取一次第一条变体，
//     media 就按 EXTINF 累计时长挑前 K 个分片（至多 kMaxPreloadSegments），
//     连同 #EXT-X-MAP 的 init 段建成**子条目**（Entry::parent = 播放列表 URL），
//     子条目此后就是普通的按字节预加载条目；
//   · 展开一次就完（Entry::expanded）。直播（没有 ENDLIST）同样只暖这一轮：
//     续暖只会一直下很快就过期、无人会用的字节；
//   · 见到非 NONE 的 #EXT-X-KEY 就整条放弃（Failed），**密钥 URL 一次都不
//     请求**——与 media 侧 playlist_declares_encryption 同一条判据、同一个
//     时点（拿到 body 之后、发出任何子请求之前）；
//   · remove()/set_priority() 对播放列表条目同时作用于它的子条目；remove()
//     还会**打断这个条目正在进行的那次播放列表抓取**（只打断它自己的那次，
//     别的条目的展开不受影响），否则它的生效要等一次读超时。
// 扫描器本身在 m3u8_scan.h，那里写了它为什么可以这么糙。
//
// 【一条已知的、刻意留下的缺口：取回的播放列表进了缓存】fetch_text 用的是
// 普通的 SourceBridge，所以那份 m3u8 的字节会按 cache_ttl_ms 留在缓存里。
// 播放不受影响——HlsSession 的播放列表通道根本不读这份缓存（播放列表零
// 缓存）。受影响的只有**下一次对同一个直播 URL 的预加载**：它可能
// 扫到一份过期的分片列表，于是暖了几个已经滚出窗口的分片。代价是几百 KiB
// 的无用流量，不会错播、也不会让播放拿到旧数据。要修得给这条抓取一条
// "不缓存"的通道（或强制重验）。
//
// 【同一个 cache key 上出现真正的播放源时，预加载让路】
// 共享索引（CacheStore）让**读**路径看得见对等源下好的区间，但每个
// SourceBridge 的 Scheduler 只在 open 那一刻被喂了一次已有区间，之后
// 对等源新下的字节它看不见。于是一条预加载和一条在播的源挂在同一个 key 上
// 时，两边会对同一批字节各发一次 Range 请求——正好是预加载存在的意义的
// 反面。这里选的解法是**让路**而不是"把对等区间灌进调度器"：
// 驱动线程每一轮都问一次 CacheStore::open_count(key)，只要这个 key 上除了
// 自己还有别人打开着，这个条目就当作没有额度（interrupt + close），已下的
// 字节留在缓存里；对方 release 之后自动复工。
//   · 为什么不是灌区间：那要动 Scheduler 在途任务的取消/重排，是这个里程碑
//     里最危险的一块，而收益与让路相同——
//     让路之后那个 key 上只剩一条源在下，重复请求从源头没有了；
//   · 播放源 open 时会一次性读到预加载已经下好的区间（共享索引），所以
//     "让路"不丢已有成果；
//   · 代价：让路有一个探测延迟（下面 kPeerPollMs），窗口内可能重复下几段；
//     两个 Preloader 实例对同一个 URL 同时开源时可能互相让路、来回抖动
//     （单 preloader 是设计支持的拓扑，多实例本就"总并发翻倍"）。
//
// 【线程与锁序】
//   Preloader::mu_ → SourceBridge::mu_ → CacheStore::Handle::mu
//   Preloader::mu_ → Scheduler::mu_（**只有测试缝** rate_class_for_test
//     走这条：持 mu_ 进桥、拷出 Scheduler* 后放掉桥锁，再取 Scheduler::mu_
//     读类别。Scheduler 的回调都在放锁后发出，不会回头取本类 mu_，不成环）
//   · 公开方法（add/set_priority/remove/remove_all/get_stats）只在 mu_ 下
//     改表 + 唤醒驱动线程，**不做任何阻塞 IO**，可以在主线程调用；
//     【一个有界的例外】remove()/remove_all() 在**放开 mu_ 之后**
//     会去打断在途的播放列表抓取（interrupt_fetch，fetch_mu_ 下）。它必须
//     这么做：那次抓取就跑在驱动线程上，驱动线程阻塞在 read() 里的时候没法
//     自己响应 removing 标志，不打断的话 remove 的实际生效要等一次读超时
//     （实测 5,819ms），而 remove 要求打断在途下载。代价是
//     interrupt() 最坏要等一条桥的一次 fsync（p90 1.2ms、max 25.9ms）——
//     至多一条播放列表桥，条目源那一半仍然零 IO；
//   · 全部 SourceBridge::open/close 只发生在驱动线程、且**不持 mu_**——
//     close() 内部要等 backend 回调退出，而回调里要取 mu_，持锁调就是自死锁；
//   · CacheStore::open_count() 同样**不持 mu_** 调：CacheStore::mu_ 是另一棵
//     树，而 CacheStore 的某些路径会在自己的锁下调用户日志回调，
//     持 mu_ 去取它就给出了一个真实的反序；
//   · **interrupt() 不是例外**。它确实只是"置位 + notify"，但那要先取桥的
//     mu_，而 persist_chunk 每 256KiB 就握着那把 mu_ 做一次 sync()+save()。
//     所以 interrupt() 会等一次 fsync（在本机实测：p50 178us、p90 1.2ms、
//     max 25.9ms），在 mu_ 下调它就把这段等待传染给所有公开方法。
//     结论是一条**没有例外**的规矩：mu_ 下不调桥的任何方法。
//     打断在途请求这件事由驱动线程在 execute() 里（锁外）做，remove() /
//     remove_all() / 析构只置标志位并唤醒它；
//   · 需要在锁外用桥指针时，只有驱动线程可以用裸指针（measure()）——桥只由
//     它自己析构。别的线程放开 mu_ 之后那个指针就可能已经被 close + 析构，
//     那是 UAF，不是竞态窗口；
//   · SourceBridge 的回调在 backend 线程上到达，回调体里只做
//     "dirty_ = true; cv_.notify_one();"，实际工作全部推给驱动线程；
//   · Entry::source 这个指针**只在 mu_ 下读写**：开源/关源本身在锁外做，
//     但"装进去"和"摘出来"两个动作都在锁内。
//     （这里原本写着"get_stats 会在 mu_ 下解引用它"——**那已经不成立了**：
//      现在 get_stats() 只读已提交的计数器 + 上一轮缓存的
//      active_tasks，一条桥都不碰，正是为了兑现"公开方法不做阻塞 IO"这条
//      承诺。锁内读写的约束本身仍然成立，只是理由换成了驱动线程自己。）
//
// 【两个 Preloader 实例】允许。它们经 CacheStore 共享缓存，但各自独立
// 计额度——总并发可能翻倍。这是有意的取舍：跨实例
// 协调需要一个全局额度池，而典型接入方只有一个 preloader。
#pragma once

#include "clock.h"
#include "source_bridge.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>
#include <syplayer/syp_types.h>

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace syp::dl {

// 播放列表上限，与 media 层的 kMaxPlaylistBytes 取同一个数（8 MiB）：
// 一个恶意或错误的 URL 不该把任意大的响应体读进内存。
inline constexpr int64_t kMaxPlaylistBytes = 8 * 1024 * 1024;
// 一个播放列表最多展开多少个分片条目。
inline constexpr size_t  kMaxPreloadSegments = 16;
// 分片条目的字节目标：分片是不可变的小对象，"整段暖完"才有意义。
// 64 MiB 是一个够大的上界（真实分片通常几百 KiB 到几 MiB），实际窗口
// 还会被资源总长夹住，所以等价于"整段"。
inline constexpr int64_t kSegmentTargetBytes = 64 * 1024 * 1024;
// 抓播放列表用的超时，**不是** dl_cfg 里那一份。理由与 media 层
// kProbeReadTimeoutMs 同一条：这次抓取是**同步**发生在
// 驱动线程上的，一个只接受连接、永不响应的服务端会用默认配置
// （15s 读超时 × 重试）把驱动线程按住两分钟，而析构要等驱动线程。
// 播放列表是几 KiB 的小文件：3s 连不上 / 5s 没有一个字节，就是坏了。
// 抓失败的代价只是这一条 HLS 预加载不展开，不影响播放。
// max_retries 不能写 0：SourceBridge 把它分两路用，scheduler 那一路走的是
// clamp_i32_pos(max_retries, 5)，**0 被当成"没填"、退回默认的 5**，反而更松。
// 1 两边都是 1，才是真的"最多再试一次"。
inline constexpr int32_t kPlaylistConnectTimeoutMs = 3000;
inline constexpr int32_t kPlaylistReadTimeoutMs    = 5000;
inline constexpr int32_t kPlaylistMaxRetries       = 1;
// 【整次抓取的墙钟上界】上面三个超时都是**每次 read** 的；
// 在这条常数进来之前，整次 fetch_text 只有 kMaxPlaylistBytes = 8 MiB 这一个
// **字节**上界。于是一个"每 4000ms 给 4 个字节"的服务端既永远踩不到 5s 的
// 单次读超时，又要走 `播放列表长度 ÷ 滴流速率` 那么久，理论天花板是
// `8 MiB ÷ 滴流速率`。
//
// 真正的后果不是这条 HLS 预加载慢——是**让路（peer-yield）在这段时间里整个
// 失效**。抓取是同步跑在驱动线程上的，驱动线程卡在 read() 里就不会再去问
// CacheStore::open_count()，而那条预加载的 SourceBridge **一直开着**，和真正
// 在播的流在同一个 cache key 上抢带宽；下面第 53-56 行承诺的"让路有一个
// 50ms 的探测延迟"在这条路径上不成立。实测：正常让路 14ms，驱动线程扎在
// 滴流播放列表里时 116,547ms（8,300×）。本机在 160 字节播放列表 + 4B/4000ms
// 上复现：对照组 57ms，实验组 **>200,000ms（量测窗口耗尽，始终没让路）**。
//
// 取 2 × kPlaylistReadTimeoutMs：播放列表是几 KiB 的小文件，两个读超时还没
// 取完就是坏了。实际上界还要加最多一次在途 read 的超时（deadline 在两次
// read **之间**查），也就是最坏 ~15s——与"每次 read 5s + 一次重试"同一档，
// 不是新的一档。修复后同一形状实测见 tests/test_preloader_hls.cpp。
inline constexpr int64_t kPlaylistFetchDeadlineMs = 2 * kPlaylistReadTimeoutMs;

enum class PreloadPriority : int32_t {
    Background = 0,
    Next       = 1,
    Playing    = 2,
};

enum class PreloadState : int32_t {
    Pending    = 0,   // 有条目，但当前没有额度（或还没轮到、或正在给对等源让路）
    Estimating = 1,   // 正在问 provider 要 time→byte
    Running    = 2,   // source 开着，正在下
    Done       = 3,   // 已覆盖 [0, target)，source 已关
    Failed     = 4,   // 不可恢复错误，source 已关
};

// 【终态是粘的，这是一条已知的、刻意留下的缺口】条目一旦进 Done 或 Failed
// 就不再参与配额分配，而 add() 对已存在的条目只抬优先级、不重置状态。于是：
//   · 一次**瞬时**的磁盘满（SourceBridge 把写失败报成 fatal → 条目 Failed）
//     会让这个 URL 在本 Preloader 的余生里再也不预加载，哪怕一秒后
//     enforce_capacity 就腾出了几百 MiB——这正是警告过的
//     "据 NO_SPACE 永久停产"，只是从另一条路走进来的：不是谁读了 NO_SPACE，
//     而是错误被当成了不可恢复的终态；
//   · Done 的条目在缓存被淘汰之后也不会自动重下。
// 当前的出路只有调用方自己 **再 add() 一次**（remove() 与否都行）。
//
// 【这条出路一度是假的，已修】add() 原先撞上已存在的条目时
// 只做 `e->removing = false`，**不重置 state / last_error**。而 remove() 也
// 只是置 `removing = true`，条目要等驱动线程下一次 commit() 才真摘掉——于是
// 背靠背的 `remove(); add();`（调用方照着这段话最自然的写法）把那条 Failed
// 条目原封不动救活了还返回 SYP_OK，实测 **5/5 全中、一个新请求都没发**。
// 而"先等条目真的消失再 add"公开 C API **做不到**（syp_preload_stats 只有
// entries 总数，没有 per-URL 信号；state_for_test / wait_settled_for_test
// 都是测试缝）。现在 add() 撞上终态条目会就地复位（state=Pending、
// last_error=SYP_OK，播放列表条目还清 expanded），由
// tests/test_preloader.cpp 的 add_after_remove_revives_a_terminal_entry
// 钉住（5 轮）。
//
// 【为什么没顺手做成自动重试 —— 这是一个决定，不是遗漏】自动重试要三样
// 东西：区分可重试错误（NO_SPACE / IO / 5xx）与真正的终态（404 / 403）、
// 一个退避时钟、以及一个"重试几次就放弃"的上界。少任何一样都会变成
// "对着一个 404 的 URL 无限重发请求"——那比粘住的终态更坏，而且坏在对端。
// 这三样都属于接入层的策略（它才知道这个 URL 还值不值得要），所以仍然
// 不做。这里只把**文档已经承诺过**的那条出路变成真的。

struct PreloadConfig {
    // 【reserved_for_playing >= max_total_tasks 且一个 Playing 条目都没有时，
    // 预算是 0——一个条目都不会跑】这不是 bug，是"全部让路给播放"的正确
    // 结果，但它很反直觉，所以写在这里：没有 Playing 条目时留下的那几个额度
    // 是留给**本 preloader 之外的播放源**的（真正在播的那条流不归 preloader
    // 管，它的并发额度由它自己的 syp_source 决定；preloader 能做的只有自己
    // 少占几条）。
    int32_t max_total_tasks       = 6;
    int32_t reserved_for_playing  = 3;
    int64_t default_preload_bytes = 1 << 20;
    int64_t default_preload_ms    = 3000;
};

// 与公开 ABI 的 syp_media_info_provider 同形（syp_preload_api.cpp 逐字段转发）。
// dl 层只见函数指针，绝不认识 AVFormatContext。
struct MediaInfoProvider {
    void* ctx = nullptr;
    syp_status (*estimate_range_for_ms)(void* ctx, const char* url, int64_t ms,
                                        int64_t* out_start, int64_t* out_end) = nullptr;
};

struct PreloadStats {
    int64_t entries          = 0;
    int64_t active_tasks     = 0;
    int64_t downloaded_bytes = 0;
    int64_t completed        = 0;
    int64_t failed           = 0;
    int64_t provider_miss    = 0;   // 装了 provider 但该 URL 估不出的次数
};

class Preloader {
public:
    // backend 为空 / dl_cfg.cache_dir 为空 → SYP_ERR_INVALID_ARG。
    // provider 可空（空 = 只能按字节预加载）。provider->ctx 由安装方拥有，
    // 必须活过本对象。
    static std::unique_ptr<Preloader> create(const syp_config& dl_cfg,
                                             const PreloadConfig& cfg,
                                             const MediaInfoProvider* provider,
                                             const syp_http_backend* backend,
                                             Clock clock,
                                             syp_status* err);
    ~Preloader();

    Preloader(const Preloader&)            = delete;
    Preloader& operator=(const Preloader&) = delete;

    // ms_or_zero > 0 且装了 provider → 按时间；否则按 default_preload_bytes。
    // URL 已存在时**不重复建条目**，只把优先级抬到 max(旧, 新)（调用方常常
    // 在列表滚动时对同一个 URL 反复 add）。
    syp_status add(const std::string& url, PreloadPriority prio, int64_t ms_or_zero);
    syp_status set_priority(const std::string& url, PreloadPriority prio);
    // 立即返回；真正的 interrupt + close 由驱动线程做。已下字节留在缓存里。
    void       remove(const std::string& url);
    void       remove_all();
    void       get_stats(PreloadStats* out) const;

    // ---- 测试缝 ----
    // 等到驱动线程把当前这一轮计划执行完并睡下（!dirty_ && !working_）。
    void    wait_settled_for_test();
    // 等到某条目进入 Done/Failed；返回 PreloadState 的整数值，超时返回 -2，
    // 条目不存在返回 -1。超时只用来把挂死变成失败，不承担断言。
    int32_t wait_terminal_for_test(const std::string& url, int32_t timeout_ms);
    // 本对象**真正在用**的 cache_dir（构造时从 dl_cfg 拷下来的那一份）。
    // 存在的理由：syp_preload.h 那条"provider 与 preloader 必须同一个
    // cache_dir"的前置条件要可检验——拿它和 MediaInfoProvider::cache_dir()
    // 逐字节比，而不是只比"我们传了同一个变量进去"。
    const std::string& cache_dir_for_test() const noexcept { return cache_dir_; }
    // 本对象**真正会交给每一条 SourceBridge** 的那份 syp_config 的起点
    // （base_config()：dl_cfg_ 整份 + cache_dir 指向自己那份字符串）。
    //
    // 【为什么必须有这一条】
    // 容量/TTL 三个字段（max_cache_bytes / min_free_space_bytes / cache_ttl_ms）
    // 从 Swift 到 dl 层要过 bridged() → SypCacheSettings → prepare_cache_dir →
    // syp_config → PreloadStack::create → 这里，整整五跳。补的读回缝断在
    // **MediaInfoProvider::cfg_** 上，而那只是一条支流：真正承担全部预加载
    // 下载的是本类和它开出去的 SourceBridge。实测——在
    // media_info_provider.cpp 里 provider 构造之后、Preloader::create 之前把
    // 三个字段清零，`ctest 33/33` 与 `xcodebuild 82 tests, 0 failures`
    // **同时全绿**：缓存不再有上限、不再过期、无限涨，一条用例都不红。
    // 这条缝就是补那个洞的。按值返回：cache_dir 指向 cache_dir_，与本对象同寿。
    syp_config dl_config_for_test() const noexcept { return base_config(); }
    // 播放列表抓取那一条源**真正**用的配置。与上面那条是
    // 两条不同的路径：dl_config_for_test() 断的是起点，这一条断的是
    // fetch_text 实际交给 SourceBridge::open 的那份。容量三件套在这里必须
    // 原样通过——fetch_text 只该覆盖并发/超时/窗口。
    syp_config playlist_config_for_test() const noexcept { return playlist_config(); }
    int32_t quota_for_test(const std::string& url) const;   // 不存在返回 -1
    // 这个条目**当前打开着的那条 SourceBridge 真正收到的**那份 syp_config。
    // 条目不存在、或还没开过源时返回一个 struct_size == 0 的零值结构。
    // quota_for_test() 断的是算术，这一条断的是**抵达**：见 Entry::open_cfg。
    syp_config source_config_for_test(const std::string& url) const;
    int32_t state_for_test(const std::string& url) const;   // 不存在返回 -1
    // 这个条目当前是不是在给同一 cache key 上的对等源让路（1 是 / 0 否 /
    // -1 条目不存在）。存在的理由：让路的外在表现与"没轮到额度"一模一样
    // （quota=0 + Pending），没有这个缝，让路用例会被"恰好没额度"骗过。
    int32_t peer_yield_for_test(const std::string& url) const;
    // 这个条目**当前打开着的那条 SourceBridge 的调度器**里的限速类别
    // （static_cast<int32_t>(RateClass)）；条目不存在或没开源时返回 -1。
    // 读的是 SourceBridge::rate_class_for_test()（→ Scheduler::rate_class()），
    // 不是 Entry::sent_class——后者是"我们以为下发了什么"，缝要断的是抵达。
    //
    // 【这是本类唯一一处在 mu_ 下调桥的地方，只因为它是测试缝】持 mu_ 时
    // 条目的 source 不可能被关（关源先在 mu_ 下摘指针），所以指针一定活着；
    // 代价是多出两条锁序边：mu_ → 桥的 mu_（短暂持有，拷出 Scheduler* 就放），
    // 以及放开桥锁之后的 mu_ → Scheduler::mu_（Scheduler::rate_class() 取它）；
    // 最坏等一次桥内 fsync。不成环：Scheduler / SourceBridge 的回调都在各自
    // 放锁之后才发出，没有谁持着它们的锁回头取本类的 mu_。生产路径
    // 一条都不走这里。
    int32_t rate_class_for_test(const std::string& url) const;
    // 条目当前那条桥的 Scheduler **构造时**收到的类别（-1 同上）。
    // 与 rate_class_for_test 的区别：后者会被驱动线程随后的 SetClass 改掉，
    // "开源时传错了类别"只在一个轮询周期内可见；这一条不随时间变，断言它与
    // 时序无关。锁序同上（少一条 Scheduler::mu_：那个字段不取锁）。
    int32_t open_rate_class_for_test(const std::string& url) const;
    // 驱动线程当前是不是正卡在一轮里（working_）。用例用它把"驱动线程已经
    // 阻塞在某条桥的调用里"变成一个**稳定的谓词**：一旦它在句柄锁被外部
    // 握住的情况下变成真，就不会再变回去，于是"这一刻桥的 mu_ 被别人拿着"
    // 这件事可以被确定性地等到，而不用睡。
    bool    driver_busy_for_test() const;

private:
    // 一次锁外量测的结果。【为什么必须锁外量】SourceBridge::get_stats() 会取
    // 桥的 mu_ **和句柄锁**（它要读 index->ranges().total_bytes()），而
    // 后端线程会握着句柄锁做 file->sync() + index->save()——一次 fsync。
    // 在 Preloader::mu_ 下调它，等于让主线程的 add/set_priority/remove 和驱动
    // 线程整轮计划都跟着一次索引落盘走，而头文件恰恰承诺公开方法不做阻塞 IO。
    // length() / cached_ranges() 同理（都要句柄锁）。所以驱动线程的一轮是
    //   等 → 锁外探对等源 → 锁内定计划 → 锁外执行 → 锁内装 source+快照指针
    //   → **锁外量测** → 锁内提交
    // 六段，凡是会碰桥的动作全在锁外。
    struct Meas {
        int64_t    downloaded = 0;
        int32_t    active     = 0;
        int64_t    total      = -1;
        syp_range  first{};
        bool       have_range = false;
        bool       valid      = false;
    };

    struct Entry {
        std::string     url;
        std::string     key;                // CacheStore::make_key(cache_dir, url)
        PreloadPriority prio = PreloadPriority::Next;
        int64_t         seq  = 0;
        int64_t         want_ms      = 0;   // >0 = 按时间；0 = 按字节
        int64_t         target_bytes = 0;   // 0 = 还没定（要问 provider）
        PreloadState    state = PreloadState::Pending;
        int32_t         quota = 0;          // 本轮分到的额度
        int32_t         open_quota = 0;     // source 打开时用的额度
        // **真正交给 SourceBridge::open 的那一份 syp_config**（install 时抄下
        // 来的 Action::cfg，不是事后再算一遍 config_for）。只给读回缝用。
        // 【为什么要它】把 config_for 里那行
        // `c.max_concurrent_tasks = quota > 0 ? quota : 1;` 删掉后，ctest 33/33 +
        // xcodebuild 82/0 **双绿存活**。结构性原因：test_preloader.cpp 里 14 处
        // 额度断言**全部**读 quota_for_test()（算术缝），没有一条观察
        // config_for() 交出去的那份配置——分配到的额度从不抵达真正开出去的
        // 那条源，而这件事在任何缝上都不可见。
        syp_config      open_cfg{};
        syp_status      last_error = SYP_OK;
        int64_t         counted_bytes = 0;  // 已并入 downloaded_bytes_ 的快照
        bool            removing = false;
        bool            estimated = false;  // 已问过 provider（不论成败）
        bool            is_playlist = false;  // URL 看起来是 m3u8/m3u
        bool            expanded    = false;  // 已展开过（直播也只展开一次）
        std::string     parent;               // 非空 = 本条目是某播放列表展开出来的
        bool            peer_busy = false;  // 这个 key 上有别人（播放源）开着
        Meas            meas{};             // 本轮锁外量到的数，commit 里消费
        // **已经下发到** source 的限速类别（开源时传的、或之后 SetClass
        // 在锁外转发成功的那个）。规划时与 rate_class_for(prio) 比，不等就
        // 生成一条 SetClass。只在 mu_ 下读写，只由驱动线程写。
        RateClass       sent_class = RateClass::Playing;
        std::unique_ptr<SourceBridge> source;
        Preloader*      owner = nullptr;    // 回调 ctx
    };

    // SetClass：条目开着源、且期望的限速类别 ≠ 已下发的类别时，
    // 锁外对它调一次 SourceBridge::set_rate_class()。
    enum class ActionKind { Close, Open, Estimate, Expand, SetClass };
    // 【source 为什么长在 Action 上】开源/关源必须在锁外做，而 Entry::source
    // 会被**驱动线程自己**在 mu_ 下读写（不是 get_stats——它一条桥都不碰，
    // 上面那段注释里有订正）。于是：关源时在
    // mu_ 下把指针**摘**到
    // Action 里、锁外析构；开源时在锁外建好放进 Action、再在 mu_ 下**装**回
    // Entry。指针本身自始至终只在锁内被读写。
    struct Action {
        ActionKind kind = ActionKind::Close;
        Entry*     e    = nullptr;
        int32_t    quota = 0;
        syp_config cfg{};                          // 开源用，plan 时在锁下算好
        syp_status err = SYP_OK;                   // 开源失败的原因，commit 时并入
        // 关源时在**锁外**（close 之后、析构之前）读到的最终下载字节数，
        // commit 里并进总账。-1 = 没量到。close() 之后 get_stats() 仍然返回
        // downloaded_bytes_（source_bridge.cpp:1000 在早退之前就填好了）。
        int64_t    final_bytes = -1;
        std::unique_ptr<SourceBridge> source;      // 关：摘出来的；开：新建的
        // Open 传给 SourceBridge::open 的类别 / SetClass 要下发的类别。
        // 与 cfg 一样在 plan 时（mu_ 下）按 prio 算好，execute 不读 Entry。
        RateClass     cls    = RateClass::Playing;
        // SetClass 用：规划时在 mu_ 下从 Entry::source 拷出的裸指针。锁外用它
        // 是安全的——与 measure() 同一条理由：桥只由驱动线程（execute 的调用
        // 者）在关源时析构，而同一轮里被 SetClass 的条目不会同时被 Close。
        SourceBridge* bridge = nullptr;
    };

    Preloader(syp_config dl_cfg, PreloadConfig cfg, MediaInfoProvider provider,
              const syp_http_backend* backend, Clock clock);

    static void cb_cached_ranges(void* ctx, const syp_range* rs, int32_t n);
    static void cb_total_length(void* ctx, int64_t total);
    static void cb_error(void* ctx, syp_status st, int32_t http);
    void        note_progress();            // 三个回调共用：置 dirty_ + notify

    void driver_main();
    // 不持 mu_：问 CacheStore 这些 key 上有没有别人开着。
    std::vector<int64_t> probe_peers(const std::vector<Entry*>& es) const;
    std::vector<Entry*>  probe_list_locked() const;
    std::vector<Action>  compute_plan_locked();
    void execute(std::vector<Action>& plan);         // 不持 mu_
    // 把开好的 source 装回条目，并快照"条目 → 桥"供锁外量测。持 mu_。
    std::vector<std::pair<Entry*, SourceBridge*>> install_and_snapshot_locked(
        std::vector<Action>& plan);
    // 不持 mu_：对每一条打开着的桥调 get_stats/length/cached_ranges。
    // 这些 Entry* 只可能被驱动线程自己销毁，而本函数就在驱动线程上。
    void measure(const std::vector<std::pair<Entry*, SourceBridge*>>& srcs,
                 std::vector<Meas>* out) const;
    void commit(std::vector<Action>& plan,
                const std::vector<std::pair<Entry*, SourceBridge*>>& srcs,
                const std::vector<Meas>& meas);      // 持 mu_
    // 阻塞地把一个 URL 的全文取回来（上限 kMaxPlaylistBytes）。只在驱动线程
    // 上调用，**不持 mu_**。取回来的字节顺带进了缓存（播放通道自己不读这份
    // 缓存——HlsSession 的播放列表通道零缓存——但暖一份没坏处）。
    // e 只用来在两次 read 之间复查"这条目还要不要"（removing / stop_），
    // 指针在驱动线程上一定有效。
    // owner_url：这次抓取属于哪个条目（master 的第二次抓取取的是变体 URL，
    // 但它仍然属于父条目）——remove(url) 靠它只打断该打断的那一条。
    std::expected<std::string, syp_status> fetch_text(const std::string& url, Entry* e,
                                                      const std::string& owner_url);
    void expand_playlist(Entry* e);        // 不持 mu_；结果在内部短暂加锁写回
    // 打断在途的播放列表抓取。owner_url 非空 = 只打断该条目的那一次；
    // 空 = 打断任何一次。**持 fetch_mu_、不持 mu_**。
    void interrupt_fetch(const std::string* owner_url);
    // 展开失败的共同收尾（持 mu_）：expanded 置位 + 记错误，不重试。
    void finish_expand_failed(Entry* e, syp_status st);

    void allocate_locked();
    bool needs_peer_poll_locked() const;
    void close_all();                                // 不持 mu_
    Entry* find_locked(const std::string& url) const;
    // 纯算术，只看本轮量到的数，不碰任何桥、不取任何锁。
    static bool reached_target(const Entry& e);
    // 本类开出去的每一条 SourceBridge 都从这里起步（config_for 与 fetch_text
    // 各自只覆盖并发/超时/窗口相关的字段，容量三件套原样带下去）。
    // 【为什么提成一个函数】原先那两行
    // （`syp_config c = dl_cfg_; c.cache_dir = cache_dir_.c_str();`）在两处各写
    // 一遍，于是"把容量字段清零"这个改动有**两个**落点，而读回缝
    // （dl_config_for_test）只看得见 dl_cfg_ 那一个。合并成一处之后，
    // 读回缝读的就是这两条路径真正的起点。
    syp_config base_config() const noexcept;
    syp_config config_for(const Entry& e, int32_t quota) const;
    // 播放列表抓取（fetch_text）那一条源的配置。与 config_for 提成函数的
    // 理由相同：留在 fetch_text 函数体里的话，"把容量三件套清零"这个改动
    // 在这条路径上没有任何读回缝看得见（ctest 33/33 + xcodebuild 82/0 双绿存活）。
    syp_config playlist_config() const noexcept;

    syp_config        dl_cfg_{};
    std::string       cache_dir_;
    PreloadConfig     cfg_{};
    MediaInfoProvider provider_{};
    const syp_http_backend* backend_ = nullptr;
    Clock             clock_{};

    // 在途的播放列表抓取。**不由 mu_ 保护**，由它自己这把叶子锁保护——
    // 打断在途抓取要调 SourceBridge::interrupt()，而"mu_ 下不调桥的任何
    // 方法"是本类一条没有例外的规矩（见文件头）。fetch_mu_ 是一片真正的
    // 叶子：只在它下面做"读/写这个指针"和"调 interrupt()"，不再取任何别的锁。
    //
    // 生命周期：只有驱动线程写这个指针，且**先在 fetch_mu_ 下清空、再关源
    // 析构**，所以持有 fetch_mu_ 时这个指针要么是空的，要么指向一条还活着
    // 的桥——这正是 remove() 那条"锁外拿裸指针"路线做不到、因而被判为 UAF
    // 的那件事。
    mutable std::mutex      fetch_mu_;
    SourceBridge*           fetching_ = nullptr;
    // 这次在途抓取是**哪个条目**的展开（播放列表条目的 URL，不是正在抓的
    // 那个 URL——master 的第二次抓取取的是变体 URL，但它仍然属于父条目）。
    // remove(url) 靠它只打断该打断的那一条，不误伤别的条目的展开。用字符串
    // 而不是 Entry*：放开 mu_ 之后指针可能已经被驱动线程回收并重用（ABA），
    // 字符串没有这个问题。
    std::string             fetching_owner_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;       // 驱动线程等这个
    std::condition_variable idle_cv_;  // 测试缝等这个
    bool    stop_    = false;
    bool    dirty_   = false;
    bool    working_ = false;
    int64_t next_seq_ = 1;

    std::vector<std::unique_ptr<Entry>> entries_;
    std::map<std::string, Entry*>       by_url_;

    int64_t downloaded_bytes_ = 0;
    // 上一轮量到的活跃任务数之和。get_stats() 只读它，不再去碰任何桥——
    // 代价是这个数最陈旧到上一轮（有源开着时轮询周期是 kPeerPollMs），
    // 换来的是"公开方法绝不阻塞在索引落盘上"这条承诺真的成立。
    int64_t active_tasks_     = 0;
    int64_t completed_        = 0;
    int64_t failed_           = 0;
    int64_t provider_miss_    = 0;

    std::thread driver_;
};

}  // namespace syp::dl
