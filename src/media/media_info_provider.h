// media_info_provider.h — 把"毫秒 → 字节区间"的能力从 media 层注入 dl 层。
//
// 【为什么是一张函数表而不是让 dl 直接用 FFmpeg】dl 层必须零平台依赖、
// 也不该认识容器格式（syp_dl_purity_check 盯着这条）。接口里同样不出现
// AVFormatContext*——它只见 const char* url
// 与两个 int64_t 出参。
//
// 【它做什么】开一次 syp_source + AvioBridge + avformat_open_input +
// find_stream_info，**只读 header，不解码、不建 Pipeline**，拿到
// duration_us / bit_rate / 总字节数，再按 CBR 线性折算。探测下来的头部
// 字节留在同一份缓存里，播放时直接命中，所以不算白下。
//
// 【前置条件：cache_dir 必须与 Preloader 的那一份逐字节相同】上一句"不算
// 白下"整个建立在这条上：探测走的是自己这份 syp_config，落盘的目录由它的
// cache_dir 决定，而 CacheStore 的 key 是 make_key(cache_dir, url)——目录
// 一字之差（比如多一个尾斜杠）就是两份缓存，探测下来的头部字节播放时命中
// 不了，而且下面那个头部常数项也就白加了（多下的那些字节落在了另一个
// 目录里）。
//   **这条不再是一句无人检查的约定**：
//   下面的 PreloadStack 把两个对象一起造出来，cache_dir 只在它那里归一化
//   一次、再分发给两边，接入层（SYPBridge）用它而不是各造各的。
//   直接 new 一个 MediaInfoProvider 再 new 一个 Preloader 仍然是可以的，
//   那时这句前置条件就回到"调用方自己保证"。
//
// 【探测的字节数是有上界的：kProbeMaxBytes】"只读 header"这句话对
// **moov 在文件尾**的 MP4 不成立：FFmpeg 会 seek 到尾部去取 moov，而
// syp_source 把 [0, total) 当成一个窗口来填，于是"读个头"变成"下整个文件"
// （实测 moovend.mp4 22832904 B、bframes.mp4 22188359 B，即整份；对照
// faststart.mp4 只要 512KiB）。那对一个以"省流量"为目的的功能是最坏结果，
// 而且这次下载是**同步**发生在 Preloader 的驱动线程上（add/set_priority/
// remove 全部排在它后面），~Preloader 也打断不了它（源归 provider 所有）。
// 所以探测自带一个上界：**这次探测新够到**的字节一旦超过 kProbeMaxBytes
// 就打断并放弃，返回 SYP_ERR_NOT_IMPLEMENTED，调用方退化为按字节预加载
// ——对一个索引在文件末尾、前缀根本暖不出首帧的容器来说，按字节正是对的答案。
//
// 【订正：这个退化**不是永久的**，而且上界不是"每 URL 一份"】
// 上一版这里写的是"moov 在尾部的容器永久退化为按字节"，还有三处公开文档
// 写着"每个 URL 最多一次探测上界（2 MiB）"。两句都是假的，实测（moovend.mp4
// 经回环 HTTP，同一个 provider、同一个 URL 连探三次，判据是**服务端实际
// 写出的 body 字节数**）：
//     probe#1 st=-99 bytes=2,215,936  ← 放弃，但那 ~2.1MB 前缀留在缓存里
//     probe#2 st=0   bytes=  49,669   ← **成功**：从暖缓存起步，只需再新下
//                                        49KB 就够 FFmpeg 解析出流信息
//     probe#3 st=0   bytes=       0   ← 记忆命中，零请求
//     合计 probe#1+#2 = 2,265,605 B > kProbeMaxBytes(2,097,152)
// （独立测到 2,244,608 + 84,509 = 2,329,117 B，同一结论。）
//
// 成因有两条，缺一不可：
//   1. **失败的探测不进 cache_**（probe() 返回 nullopt，estimate_range_for_ms
//      只在成功时 emplace），所以同一个 URL 会被反复探测；
//   2. **字节上界的基线是每次探测重新量的**（那是对的，见下面"上界数的是
//      增量"那一段），所以每次探测各自拿到一个完整的上界额度。
// 于是上界的正确口径是"**每次探测**最多 kProbeMaxBytes（且因为判定是事后
// 的，还会超一个在途窗口，实测单次超出 118,784 B）"，不是"每个 URL"。
//
// 【为什么不顺手把失败也记忆掉 —— 不记忆是更好的行为】记忆失败等于把"第
// 一次没成"变成"这个 URL 从此永远按字节"。实测（在失败分支上 emplace
// 一个空 Info）：probe#2/#3 双双变成 st=-99，**这个 URL 终身估不出**。
// 所以这里修的是文档与用例，不是行为。由
// a_failed_probe_is_not_memoized_so_one_url_can_exceed_the_probe_bound 钉住。
//
// 【推论，必须一起说】`syp_preload_stats::provider_miss` 被三处文档宣传成
// "按时间预加载有没有真的生效的唯一信号"——它**依赖缓存冷热**：同一个
// moov-at-end 的 URL，冷缓存下第一次会让它涨，热缓存下（探测已经暖过一轮）
// 就不涨了。拿它做"这个 URL 支不支持按时间预加载"的判据会得到与缓存状态
// 有关的两个不同答案。
//
// 【上界数的是增量，不是缓存总量】已缓存的字节里有一部分
// 是别人早就下好的——上一次预加载、上一次播放、甚至上一次探测自己。拿总量
// 去比上界，就等于让一份热缓存**悄悄关掉**按时间预加载：一次按时间的预加载
// 缓存的字节就可能超过上界，于是同一个 URL 下一次探测在零网络字节处放弃
// （实测 3.1MB 预热 → NOT_IMPLEMENTED）。所以 open 之后先量一次基线，之后
// 一律比差值，由 probe_cap_and_estimate_ignore_bytes_cached_before_the_probe
// 钉住。
//
// 【探测也有时间上界：kProbe*TimeoutMs】字节上界只管住
// "下太多"，管不住"一个字节都不来"。一个只接受连接、永不响应的服务端用
// **默认**配置（15s 读超时 × 重试）把一次探测按住了实测 120,071ms——而这次
// 探测同步占着 Preloader 的驱动线程，~Preloader 要等它返回才能 join，在
// Swift 的 deinit 上就是一次两分钟的主线程卡顿。syp_preload.h 对 provider
// 的头一条硬约束"必须尽快返回"说的正是这件事，所以 probe() 这份自定义配置
// 里除了并发数，还必须有自己的超时。
//   【不要把它读成"上界 = read_timeout"】超时管的是**单次读**，不是整次
//   探测：一次读超时之后还会再发一次请求，所以墙钟是这个数的一个小倍数。
//   同一台机器上实测（read_timeout=5000）：max_retries=1 → **6,015–6,021ms**
//   （早先记的 6,013ms 是同一次测量的下沿，本轮重测 6,015 / 6,021）；
//   写成 0 反而是 15,021ms（0 被 clamp 回调度器默认的 5，见下面常量处）。
//
// 【订正：上面那三个超时管不住"慢速滴水"，墙钟看门狗已补上】
//   此前这里说"120s 变成了 6s"，而那只对"一个字节都不来"这一类成立。
//   **"每次读都在读超时之前回来一点点"两边都够不着**：实测服务端按
//   1 KiB/s 滴流（body_chunk_bytes=1024 / body_chunk_delay_ms=1000，永远
//   踩不到 5s 的单次读超时）⇒ 跑满 **150 秒仍未结束**，同一条
//   路径上也曾跑过 **60 秒仍未结束**；按这个速率够到 kProbeMaxBytes 要
//   ~2,048 秒（~34 分钟）。
//   现在由 kProbeWallClockMs（15,000ms）这条墙钟看门狗兜住，同一条滴流
//   路径实测 **15,047 / 15,065ms** 返回 SYP_ERR_NOT_IMPLEMENTED。
//   取值理由与它仍然存在的局限见下面 kProbeWallClockMs 处。
//
// 【精度】CBR 折算对 VBR 素材可能显著偏低。本版本保证的是**不少于按
// (总字节 / 总时长) 线性折算的值**（统一乘 5/4 的安全系数），偏多的一侧
// 不保证。要做准需要读 sidx/索引表，属于独立课题。
//
// 【线性模型漏掉的那一项：容器头的常数偏移，已修】"字节位置 ∝ 时间"这个
// 假设在 t=0 附近不成立——头部要先读完，才轮到第一个媒体字节，那是一个
// 与 ms 无关的**加数**。只乘安全系数盖不住它：实测 faststart.mp4
// （22832904 B / 57s）解出前 N 毫秒真正需要读到的文件偏移 vs 纯线性估算，
// 500ms 时少 7.8%（1000ms +6.6%、3000ms +15.7%、5000ms +17.7%），也就是
// 预加载"完成"之后首帧仍然要等一次网络。
//
// 修法是把这个常数项**加**进去：探测结束时取 AvioBridge::Diag::max_read_end
// （这次解析经本桥读到过的最大偏移，也就是探测实际够到的前缀长度，再夹到
// kProbeMaxBytes），作为 header_bytes 加在安全系数之后。
//   · 为什么是加数而不是抬 kMinEstimateBytes：下限是个 max，只在**整个
//     估算值都低于它**时才起作用，在"高码率 / 大头部"这个误差最大的区间
//     里它恒不生效。本素材上 moov 结束于 49701、第一个媒体字节在 49717，
//     两者都**小于** 64KiB——下限的量级没问题，它失效是因为它是下限而不是
//     加数；
//   · 为什么不取 cached_ranges[0].end（本轮之前的写法）也不取
//     downloaded_bytes：两者都是**缓存**的属性而不是这次解析的属性。
//     downloaded_bytes 在热缓存下是 0；cached_ranges[0].end 在热缓存下是
//     "别人下过多少"，同一个 URL 冷热两次能估出 1,025,009 与 2,229,233 两个
//     值。读到的最大偏移只数这次解析自己的读，冷热一致，而且比已缓存前缀更
//     紧（source 按不小于 min_segment_size 的块下，前缀恒不小于真实读到点）。
//
// 线程安全：estimate_range_for_ms 可从任意线程调用（Preloader 的驱动线程
// 会调它），内部用一把 mutex 保护 URL → 结果的内存缓存。**同一个 URL 的
// 两次并发调用可能各探测一次**（不做 in-flight 去重），代价只是多一次
// header 请求，不影响正确性。
//
// 【已知限制：cache_ 没有淘汰】成功的探测结果一
// 条不落地留在 cache_ 里，没有上限、没有 LRU、没有 TTL。一个长寿的
// Preloader 每见过一个 URL 就攒一条 Info（约 4 个 int64 + 一份 URL 字符串）。
// 对 App 的实际用法（一次会话里几十到几百个 URL）量级可忽略；但"一个进程
// 里滚动播上万条短视频"这种用法会线性增长。这里不修：加淘汰要先想清楚
// "淘汰之后同一个 URL 会重新探测一次"这件事与上面那条"失败不记忆"叠加起来
// 的流量代价，不是一行的事。
#pragma once

#include "dl/preloader.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>
#include <syplayer/syp_preload.h>
#include <syplayer/syp_types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace syp::media {

// 估算结果的下限。注意它是**下限**（一个 max），只在整个估算值都低于它时
// 起作用；容器头的常数开销由 header_bytes 这个加数负责，不要靠抬这个数去
// 修那件事（见文件头）。
inline constexpr int64_t kMinEstimateBytes = 64 * 1024;
// VBR 安全系数（分子/分母）：宁可多下。
inline constexpr int64_t kEstimateSafetyNum = 5;
inline constexpr int64_t kEstimateSafetyDen = 4;
// 探测允许够到的字节上界。超过就打断、放弃、退化为按字节（见文件头）。
// 取值理由：faststart.mp4 的一次完整 header 探测实测够到 512KiB，2MiB 留了
// 4 倍余量；同时它也是 header_bytes 这个加数的上界——探测够得越远，加上去
// 的常数项越大，所以这个数不能给得太松。FFmpeg 自己的 probesize 上限是
// kDefaultProbeSize（5MiB），本上界比它紧，是**故意**的：真需要 5MiB 才能
// 解析出流信息的素材，前缀预加载本来也救不了首帧。
inline constexpr int64_t kProbeMaxBytes = 2 * 1024 * 1024;
// 探测自己那份超时，**不是** dl_cfg 里的那份（见文件头"探测也有时间上界"）。
// 3s 连不上 / 5s 一个字节都不来，就按"这个 URL 估不出"处理，退化为按字节。
//
// 【max_retries 是 1 而不是 0，0 会适得其反】syp_config::max_retries 在
// SourceBridge 里分两路用：task.max_retries 直接用（0 = 单任务不重试），
// 而 scheduler 的 max_consecutive_errors 走的是
// clamp_i32_pos(cfg_.max_retries, 5)——**0 被当成"没填"，退回默认的 5**。
// 也就是说写 0 反而让调度器多起 5 轮任务。1 两边都是 1，才是真的"最多再
// 试一次"。（dl 侧的 kPlaylistMaxRetries=1 是碰巧落对了。）
inline constexpr int32_t kProbeConnectTimeoutMs = 3000;
inline constexpr int32_t kProbeReadTimeoutMs    = 5000;
inline constexpr int32_t kProbeMaxRetries       = 1;

// 【整次探测的墙钟硬死线】上面三个超时管的都是**单次读**，
// 管不住"每次读都在超时之前回来一点点"。实测：服务端 1 KiB/s 滴流
// （body_chunk_bytes=1024, body_chunk_delay_ms=1000，永远踩不到 5s 的单次
// 读超时）⇒ **跑满 150 秒仍未结束**；按这个速率够到 kProbeMaxBytes 需要
// ~2,048 秒（~34 分钟）。这期间驱动线程被占死：add/setPriority/remove 全部
// 空转、~PreloadStack 无法 join，而且 SYPlayerPreloader.deinit 那个后台块
// 会让一条线程 + 一条连接在 App 认为 preloader 已经没了之后继续往缓存目录
// 里写最多 ~34 分钟。
//
// 取 15,000ms 的理由（量出来的，不是拍的）：
//   · 必须 **大于** 现有那条"只接受连接、永不响应"路径的实测最坏值
//     6,016–6,017ms，否则会改掉那条路径的行为、并让
//     probe_gives_up_quickly_on_a_stalled_server（门槛 10,000ms）变成在
//     量看门狗而不是在量重试上界。15,000 留了 2.5 倍余量；
//   · 必须 **小于** 一个人能接受的驱动线程占用。15s 相对 2,048s 是 136×。
// 超时的后果与字节上界完全相同：打断、放弃、返回 SYP_ERR_NOT_IMPLEMENTED，
// 调用方退化为按字节预加载——不是错误，是降级。
//
// 【已知局限，别把它读成"任何情况下 15s 必返回"】看门狗的开火点是
// on_cached_ranges 回调（每收到一块字节就来一次，source_bridge.cpp
// fire_cached_ranges）加上 probe() 里三处顺序检查点。"字节一直在来"的滴流
// 正是它要治的那一类，回调每秒都有，所以实测收敛在 15s 附近；而"一个字节
// 都不来"那一类根本不产生回调，它由上面三个超时管（实测 6,016ms），两者
// 合起来才是完整的上界。真正的硬死线要把探测挪出驱动线程做成可取消的异步
// 任务，那是重构不是修补。
inline constexpr int32_t kProbeWallClockMs = 15000;

// 【cache_dir 的归一化搬走了】
// 实现现在住在 src/dl/cache_store.h 的 syp::dl::normalize_cache_dir()，紧挨
// CacheStore::make_key。搬的理由与完整的实测表、顺序论证、两条已知局限都在
// 那里，不在这里重复一遍（抄第二份注释与抄第二份实现是同一类错误）。
//
// 这里只留下 src/media 需要知道的那一句：**PreloadStack::create 与接入层
// SYPBridge.mm 的 prepare_cache_dir 都调它**，provider 与 preloader 拿到的是
// 同一份归一化后的串。搬下去之后 C ABI（syp_source_open / syp_preloader_create）
// 也由 SourceBridge / Preloader 的构造函数收口，不再是一条没有闸的路。

// 纯算术，脱离素材可测（与 demuxer.h 的 normalize_duration 同一个理由：
// 真实触发场景——直播源、chunked 传输、时长未知的流——做不成 fixture，
// 而这段换算本身与素材无关）。返回 0 = 估不出。
//   duration_us > 0 且 total_bytes > 0 → total × ms / duration
//   否则 bit_rate > 0                  → bit_rate / 8 × ms / 1000
//   都没有                             → 0
// 再乘安全系数、加 header_bytes、夹到 [kMinEstimateBytes, total_bytes]。
int64_t estimate_bytes_for(int64_t duration_us, int64_t total_bytes,
                           int64_t bit_rate, int64_t header_bytes,
                           int64_t ms) noexcept;

class MediaInfoProvider {
public:
    // dl_cfg 的内容会被拷贝（含 cache_dir 指向的字符串）；探测用的
    // syp_source 用这份配置打开，所以探测下来的字节落在同一份缓存里。
    explicit MediaInfoProvider(const syp_config& dl_cfg);
    ~MediaInfoProvider();

    MediaInfoProvider(const MediaInfoProvider&)            = delete;
    MediaInfoProvider& operator=(const MediaInfoProvider&) = delete;

    // 装给 syp_preloader_create 的函数表；ctx 指向 this，**必须活过 preloader**。
    syp_media_info_provider table() noexcept;

    // 成功写出 [0, *out_end) 并返回 SYP_OK；打不开 / 时长与码率都未知
    // → SYP_ERR_NOT_IMPLEMENTED（调用方退化为按字节）。
    syp_status estimate_range_for_ms(const char* url, int64_t ms,
                                     int64_t* out_start, int64_t* out_end);

    // 探测真正用的那个目录（构造时从 dl_cfg 拷下来的那一份）。存在的理由是
    // 上面那条前置条件要**可检验**：拿它和 Preloader 的那一份逐字节比。
    const std::string& cache_dir() const noexcept { return cache_dir_; }

    // 检验缝：本对象**真正在用**的
    // 那份 syp_config——构造时从 dl_cfg 整份拷下来的那一个，probe() 里
    // `syp_config c = cfg_` 用的就是它。
    //
    // 为什么需要它：容量/TTL（max_cache_bytes / min_free_space_bytes /
    // cache_ttl_ms）从 Swift 到 C 要过 bridged() → SypCacheSettings →
    // prepare_cache_dir → syp_config 三跳，而**只有第一跳有用例**。曾经把
    // prepare_cache_dir 里那三行改成硬 0（缓存无上限、永不过期、无限涨），
    // 74 条用例一条都不红。目录那一半有 cache_dir() 这条缝，容量这一半没有，
    // 现在补上——形状与 cache_dir() 一致：断的是**对象自己手里那份**，不是
    // 中间变量。
    const syp_config& config_for_test() const noexcept { return cfg_; }

    // 探测那一条源**真正**用的配置（见 .cpp 里
    // probe_config() 上方）。上面那条断的是起点 cfg_，这一条断的是
    // probe() 交给 syp_source_open 的那一份——两者之间隔着几行覆盖，而
    // 在容量三件套上它们必须逐字段相等。
    syp_config probe_config_for_test() const noexcept { return probe_config(); }

private:
    struct Info {
        int64_t duration_us = 0;
        int64_t total_bytes = -1;
        int64_t bit_rate    = 0;
        // 探测实际够到的前缀长度（AvioBridge::Diag::max_read_end，夹到
        // kProbeMaxBytes）。这就是线性模型漏掉的那个常数项，见文件头。
        int64_t header_bytes = 0;
    };

    static syp_status c_estimate(void* ctx, const char* url, int64_t ms,
                                 int64_t* out_start, int64_t* out_end);
    std::optional<Info> probe(const std::string& url);
    syp_config probe_config() const noexcept;

    syp_config                  cfg_{};
    std::string                 cache_dir_;
    mutable std::mutex          mu_;
    std::map<std::string, Info> cache_;
};

// PreloadStack —— provider 与 preloader 的共同拥有者，也是"两边同一个
// cache_dir"这条前置条件在代码里的落实点。
//
// 它保证三件光靠注释保证不了的事：
//   1. **目录只有一个来源**：dl_cfg.cache_dir 在这里被拷进一个 std::string、
//      归一化（剥掉尾部多余的 '/'）一次，两个对象拿到的是同一个
//      `c_str()`。为什么要归一化：CacheStore::make_key 不归一化目录，
//      而 Apple 的 NSTemporaryDirectory() 带尾
//      斜杠、URL.path 往返之后又不带——同一个目录的两种拼法就是两份缓存。
//   2. **析构顺序**：syp_preload.h 明写 provider->ctx 必须活过 preloader
//      （驱动线程可能正卡在 estimate_range_for_ms 里）。成员声明顺序
//      provider_ 在前、preloader_ 在后，于是析构时 preloader_ 先走。
//   3. **provider 必装**：这个组合的全部意义就是"按时间预加载"。
//
// 不想要 provider（比如不链 FFmpeg 的构建）就别用这个类，直接建 Preloader。
class PreloadStack {
public:
    // backend 为空则用当前注册的全局后端。cache_dir 为空 → SYP_ERR_INVALID_ARG。
    static std::unique_ptr<PreloadStack> create(const syp_config& dl_cfg,
                                                const syp::dl::PreloadConfig& cfg,
                                                const syp_http_backend* backend,
                                                syp_status* err);
    ~PreloadStack();

    PreloadStack(const PreloadStack&)            = delete;
    PreloadStack& operator=(const PreloadStack&) = delete;

    syp::dl::Preloader* preloader() const noexcept { return preloader_.get(); }
    // 两边共用的那一份目录（已归一化）。
    const std::string&  cache_dir() const noexcept { return cache_dir_; }
    // 检验缝：provider 与 preloader 各自**真正在用**的目录，用来断言它们
    // 逐字节相同——而不是断言"我们传了同一个变量进去"。
    const std::string&  provider_cache_dir_for_test() const noexcept;
    const std::string&  preloader_cache_dir_for_test() const noexcept;
    // 检验缝：create() 交给两个对象的那份 syp_config，从
    // **provider 自己手里**读回来（不是 create() 的局部变量——那就又成了
    // "我们传了同一个变量进去"这种不鉴别的断言）。容量/TTL 三个字段是否真的
    // 从接入层穿到了 dl 层，只有这条缝看得见。preloader 那一侧拿的是同一个
    // `c`（create() 里相邻两句）；它自己那条缝见下面的
    // preloader_config_for_test()。
    const syp_config&   provider_config_for_test() const noexcept;
    // 检验缝：provider 那一支**再往下一跳**——probe() 真正交给
    // syp_source_open 的那一份。上面那条断在 cfg_（起点），而 probe() 在
    // 那之后还要覆盖并发与三个超时字段；容量三件套必须穿过那几行不变。
    // MUT_PROBE（在那几行旁边清零三字段）此前 ctest 33/33 +
    // xcodebuild 82/0 双绿存活，这条缝就是补它的。
    syp_config          provider_probe_config_for_test() const noexcept;
    // 检验缝：**preloader 那一支**真正会交给每一条 SourceBridge
    // 的那份 syp_config。上面那条读的是 provider（探测那一支），而承担全部
    // 预加载下载的是这一支——之前只补了前者的缝，这里再把三个容量字段在 provider
    // 构造之后、Preloader::create 之前清零，`ctest 33/33` 与
    // `xcodebuild 82 tests, 0 failures` 同时全绿。两条缝都要，形状与
    // provider_/preloader_cache_dir_for_test() 那一对一致。
    syp_config          preloader_config_for_test() const noexcept;

private:
    PreloadStack() = default;

    std::string                         cache_dir_;
    std::unique_ptr<MediaInfoProvider>  provider_;    // 先声明 ⇒ 后析构
    std::unique_ptr<syp::dl::Preloader> preloader_;   // 后声明 ⇒ 先析构
};

}  // namespace syp::media
