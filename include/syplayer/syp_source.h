// syp_source.h — 媒体源：核心对象
//
// 一个 syp_source 代表"一个可边下边读的媒体资源"。它对上提供 read/seek
// （形状刻意对齐 FFmpeg 的 AVIOContext 回调），对下管理分段并发下载、
// 洞调度、内存/磁盘缓存。
//
// 典型用法：
//     syp_config cfg; syp_config_init(&cfg); cfg.cache_dir = "...";
//     syp_source* s = NULL;
//     syp_source_open(&s, url, NULL, &cfg, &callbacks);
//     // 交给 FFmpeg：avio_alloc_context(buf, sz, 0, s, read_cb, NULL, seek_cb)
//     ...
//     syp_source_close(s);

#ifndef SYPLAYER_SYP_SOURCE_H
#define SYPLAYER_SYP_SOURCE_H

#include "syp_config.h"
#include "syp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct syp_source syp_source;

// ---------------------------------------------------------------- 回调
// 全部可为 NULL。**均可能在内部线程被调用**，实现方需自行保证线程安全，
// 且不得在回调里再调用 syp_source_* 的阻塞接口（会死锁）。
typedef struct {
    void* ctx;

    // 缓冲状态变化。buffering=true 表示读操作正在等待数据。
    void (*on_buffering)(void* ctx, bool buffering);

    // 下载速度，字节/秒。按 cfg.speed_window_ms 的窗口平滑。
    void (*on_speed)(void* ctx, int64_t bytes_per_sec);

    // 已缓存区间发生变化。ranges 仅在回调期间有效。
    void (*on_cached_ranges)(void* ctx, const syp_range* ranges, int32_t count);

    // 拿到资源总长度（首个响应解析出 Content-Range 之后）。
    void (*on_total_length)(void* ctx, int64_t total_length);

    // 不可恢复的错误。可恢复错误内部重试，不走这里。
    void (*on_error)(void* ctx, syp_status status, int32_t http_status);
} syp_source_callbacks;

// ---------------------------------------------------------------- 生命周期
// headers 可为 NULL。cfg 内容会被拷贝，调用后可释放。
// cb 可为 NULL；若非 NULL，其内容会被拷贝（但 ctx 指向的对象须存活到 close）。
syp_status syp_source_open(syp_source**              out,
                           const char*               url,
                           const syp_headers*        headers,
                           const syp_config*         cfg,
                           const syp_source_callbacks* cb);

// 关闭并释放。会阻塞等待内部线程退出。
// 若有 read 阻塞在别的线程，须先 syp_source_interrupt。
void syp_source_close(syp_source* s);

// ---------------------------------------------------------------- 读取
// 形状对齐 FFmpeg AVIOContext：
//   >0  实际读到的字节数
//   0   不返回（用 SYP_ERR_EOF 表示尾部）
//   <0  错误码
// 阻塞直到有数据可读 / 到达 EOF / 出错 / 被 interrupt。
int32_t syp_source_read(syp_source* s, uint8_t* buf, int32_t size);

// whence 用 SYP_SEEK_*。返回新的绝对位置，或负错误码。
// seek 到未缓存区间会触发调度器补洞，本身不阻塞等待数据。
int64_t syp_source_seek(syp_source* s, int64_t offset, int32_t whence);

// 资源总长度；未知返回 -1。
int64_t syp_source_length(syp_source* s);

// 打断当前阻塞中的 read。可在任意线程调用。
// 之后 read 返回 SYP_ERR_CANCELED，直到 syp_source_resume。
void syp_source_interrupt(syp_source* s);
void syp_source_resume(syp_source* s);

// ---------------------------------------------------------------- 调度反馈
// 调度器需要知道播放进度才能做决策（该优先补哪个洞、要不要限速）。
// 播放器应在播放过程中周期性调用（建议 200~500ms 一次）。
// 不调用也能工作，但调度质量会下降。
typedef struct {
    int64_t play_position_ms;    // 当前播放时间点
    int64_t duration_ms;         // 总时长，未知填 -1
    bool    is_playing;          // 暂停时调度器可以更保守
    bool    is_seeking;
} syp_playback_state;

void syp_source_update_playback(syp_source* s, const syp_playback_state* st);

// ---------------------------------------------------------------- 查询
// 把已缓存区间写入 out，最多 max 个，返回实际写入数；out 为 NULL 时只返回总数。
int32_t syp_source_cached_ranges(syp_source* s, syp_range* out, int32_t max);

typedef struct {
    int64_t downloaded_bytes;    // 本次会话从网络下载的字节数
    int64_t cache_hit_bytes;     // 从缓存直接读到的字节数
    int64_t cached_bytes;        // 当前已缓存总量
    int32_t active_tasks;        // 进行中的下载任务数
    int32_t completed_tasks;
    int32_t failed_tasks;
    int32_t redirect_count;
    int64_t current_speed_bps;
} syp_source_stats;

void syp_source_get_stats(syp_source* s, syp_source_stats* out);

// ---------------------------------------------------------------- 缓存管理
// 全局操作，不依赖 syp_source 实例。
int64_t    syp_cache_size(const char* cache_dir);
syp_status syp_cache_evict(const char* cache_dir, int64_t target_bytes);
syp_status syp_cache_clear(const char* cache_dir);
syp_status syp_cache_remove(const char* cache_dir, const char* url);

#ifdef __cplusplus
}
#endif

#endif  // SYPLAYER_SYP_SOURCE_H
