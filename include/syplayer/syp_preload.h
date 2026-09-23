// syp_preload.h — 预加载：把"下一个要播的资源的前若干秒"提前下进共享缓存。
//
// 一个 syp_preloader 管一组"等着被播"的 URL。它**只暖字节**：不建管线、
// 不解码、不预连接。暖好的字节落在与播放同一份缓存里（同一个
// cache_dir + 同一个 URL ⇒ 同一份索引），播放时直接命中。
//
// 典型用法：
//     syp_config dl; syp_config_init(&dl); dl.cache_dir = "...";
//     syp_preload_config pc; syp_preload_config_init(&pc);
//     syp_preloader* p = syp_preloader_create(&dl, &pc, NULL);
//     syp_preloader_add(p, next_url, SYP_PRELOAD_PRIORITY_NEXT, 0);
//     ...
//     syp_preloader_destroy(p);
//
// 线程：所有函数可在任意线程调用，且**都不做阻塞 IO**（真正的下载在内部
// 驱动线程上），可以直接在 UI 线程调。syp_preloader_destroy 会阻塞等内部
// 线程退出。
#ifndef SYPLAYER_SYP_PRELOAD_H
#define SYPLAYER_SYP_PRELOAD_H

#include "syp_config.h"
#include "syp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct syp_preloader syp_preloader;

// 只影响**并发额度的分配**，不改写下载窗口语义。
typedef enum {
    SYP_PRELOAD_PRIORITY_BACKGROUND = 0,   // 更远的候选
    SYP_PRELOAD_PRIORITY_NEXT       = 1,   // 下一个要播的
    SYP_PRELOAD_PRIORITY_PLAYING    = 2,   // 正在播的那条（若也交给本对象管）
} syp_preload_priority;

typedef struct {
    // ABI 前向兼容：调用方必须填 sizeof(syp_preload_config)。
    // 类型与 syp_config::struct_size 一致（uint32_t）——这两个字段是同一条
    // 约定的两个实例，类型不一致会让绑定层（Swift/Rust/JNI）各写一套转换。
    uint32_t struct_size;

    // 本 preloader 能同时占用的下载任务总数上限。默认 6
    int32_t max_total_tasks;

    // 留给"正在播放的那条流"的额度。**当本对象里没有任何
    // SYP_PRELOAD_PRIORITY_PLAYING 条目时**，预加载最多只用
    // max_total_tasks - reserved_for_playing 个额度，剩下的留给
    // 真正在播的那个 syp_source（它不归本对象管，只能靠"自己少占"让路）。
    // 有 Playing 条目时整份 max_total_tasks 都可以分——那条目本身排在最前。
    // ⚠️ reserved_for_playing >= max_total_tasks 且没有 Playing 条目时，
    //    预加载一个条目都不会跑。这是合法配置（全部让路），不是 bug。
    // 默认 3
    int32_t reserved_for_playing;

    // 没装 provider（或该 URL 估不出时长）时，每个条目暖多少字节。默认 1 MiB
    int64_t default_preload_bytes;

    // 装了 provider 且 syp_preloader_add 的 ms_or_zero 传 0 时，暖多少毫秒。
    // 默认 3000
    int64_t default_preload_ms;
} syp_preload_config;

void syp_preload_config_init(syp_preload_config* out);

// media 层注入的"毫秒 → 字节区间"能力。**可选**：不装就只能按字节预加载。
// ctx 由安装方拥有，必须活过 preloader。
//
// estimate_range_for_ms 会被内部的驱动线程调用，且允许阻塞（内部会把它排在
// 不持锁的位置），但不得回调任何 syp_preloader_* 函数。除此之外还有两条
// **实现依赖的硬约束**，违反了不会报错，只会表现成"预加载卡住/主线程卡住"：
//
//   1. **必须尽快返回。** 它跑在驱动线程上，这期间 syp_preloader_add /
//      set_priority / remove 只是置标志位并立刻返回（它们仍然不阻塞），但
//      这些请求要等本次调用返回之后才会被执行；syp_preloader_destroy 更要
//      **等它返回**才能 join 掉驱动线程。一次拖到几秒的实现 = 一次几秒的
//      destroy，在 Swift 的 deinit 里就是一次主线程卡顿。所以实现方必须给
//      自己的探测加上界（syp::media::MediaInfoProvider 用的是
//      kProbeMaxBytes：够到的字节超过上界就放弃、返回
//      SYP_ERR_NOT_IMPLEMENTED，让调用方退化为按字节）。
//
//   2. **返回之后不得再持有同一个 URL 的 syp_source。** preloader 靠
//      "这个 cache key 上还有没有别人开着"来给真正的播放源让路；一个在返回
//      后仍然开着源的 provider（比如异步预取、或缓存住一个 handle）会被
//      **它自己**看成竞争的播放源，那个条目就永远停在 Pending 上不再下载。
//
// 【前置条件】provider 探测用的 cache_dir 必须与传给 syp_preloader_create
// 的 dl_cfg.cache_dir **逐字节相同**。两者是同一份缓存才有意义：探测读下来
// 的头部字节要能被随后的预加载与播放命中，否则那些字节纯属白下。
typedef struct {
    void* ctx;
    // 成功：返回 SYP_OK 并写出 [*out_start, *out_end)（本版本 start 恒为 0）。
    // 不支持该 URL / 估不出：返回 SYP_ERR_NOT_IMPLEMENTED，调用方退化为按字节。
    syp_status (*estimate_range_for_ms)(void* ctx, const char* url, int64_t ms,
                                        int64_t* out_start, int64_t* out_end);
} syp_media_info_provider;

// dl_cfg 的内容会被拷贝（cache_dir 指向的字符串也会拷）。
// 需要先注册好全局 HTTP 后端（syp_set_http_backend），否则返回 NULL。
syp_preloader* syp_preloader_create(const syp_config* dl_cfg,
                                    const syp_preload_config* cfg,
                                    const syp_media_info_provider* provider);

// 打断所有在途下载、等内部线程退出、释放。已下字节留在缓存里。
void syp_preloader_destroy(syp_preloader* p);

// ms_or_zero > 0 且装了 provider → 按时间暖；否则按 default_preload_bytes。
// URL 已存在时不重复建条目，只把优先级抬到 max(旧, 新)。
syp_status syp_preloader_add(syp_preloader* p, const char* url,
                             syp_preload_priority prio, int64_t ms_or_zero);

// URL 不存在返回 SYP_ERR_INVALID_ARG。
syp_status syp_preloader_set_priority(syp_preloader* p, const char* url,
                                      syp_preload_priority prio);

// 立即返回；真正的打断与关闭在内部线程上完成。URL 不存在是 no-op。
void       syp_preloader_remove(syp_preloader* p, const char* url);
void       syp_preloader_remove_all(syp_preloader* p);

typedef struct {
    int64_t entries;          // 当前条目数
    int64_t active_tasks;     // 正在下载的任务数
    // 本 preloader 的**条目源**累计从网络下载的字节数。
    //
    // 计入的只有 Entry::source 这些条目源（含 HLS 展开出的 EXT-X-MAP 与前
    // 若干分片）。**两类字节不计入，它们都真的走了网络**：
    //   1. provider 的探测源 —— 上界 kProbeMaxBytes（syp::media 是 2 MiB），
    //      **单位是"每次探测"，不是"每个 URL"**；实测一个 1,440,157 B 的
    //      faststart MP4，探测落盘 524,288 B。kProbeMaxBytes 的单位是"每次
    //      探测"，不是"每个 URL"——
    //      失败的探测**不记忆**（media_info_provider.cpp:271 直接返回，
    //      不往 cache_ 里 insert；只有成功那一支在 273 行 insert），而按字节
    //      的基线每次探测都重取一遍。于是同一个 moov-at-end 的 URL 连探三次
    //      合计会超过 kProbeMaxBytes。两次独立实测：
    //        第一次：#1 st=-99 2,244,608 B | #2 st=0 84,509 B | #3 0 B = 2,329,117 B
    //        第二次：#1 st=-99 2,215,936 B | #2 st=0 49,669 B | #3 0 B = 2,265,605 B
    //      两次都 > kProbeMaxBytes = 2,097,152，而这只是**一个** URL。
    //      顺带一提，"那就把失败也记进缓存"是错的修法：让失败
    //      那一支也 emplace 之后，#2/#3 永远停在 st=-99——那个 URL 从此再也
    //      估不出来。所以这是一条**文档订正，不是行为变更**。
    //   2. fetch_text 为**每一张播放列表**另开的一次性 SourceBridge（master
    //      一次 + 选中的 media 一次）—— 上界 kMaxPlaylistBytes 8 MiB/次；
    //      实测 120 B + 271 B。
    // ⇒ 它**低报**真实网络用量。实测一例：本字段报 545,588 B，缓存目录同期
    //   实际落盘 1,070,267 B，**低报 49.0%**。
    //
    // ⚠️ **反方向也不成立：这个字段不是缓存目录的占用量。** 目录里还有历次
    //    会话留下的字节，以及播放侧写进去的字节（同目录时两者共享一份缓存）。
    //    要"本次新增了多少"，只能自己在开始前记一个基线再做减法，而且减法
    //    **只该数 .dat**——.idx 每轮几百字节，算进去就把低报算成了假低报。
    int64_t downloaded_bytes;
    int64_t completed;        // 已达标条目数（累计，不随 remove 减少）
    int64_t failed;           // 已失败条目数（累计）
    int64_t provider_miss;    // 装了 provider 但该 URL 估不出的次数（累计）
} syp_preload_stats;

void syp_preloader_get_stats(const syp_preloader* p, syp_preload_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SYPLAYER_SYP_PRELOAD_H
