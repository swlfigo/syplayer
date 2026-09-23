// syp_config.h — 配置
//
// 设计取舍：**不做云端下发、不做 key-value 字符串配置系统。**
// 那类机制是为运营需求服务的，通用播放器不需要。
// 这里就是一个普通 POD 结构体，创建时传入，之后不可变。
//
// 下面的默认值是经验起点而非教条——它们针对的是移动网络与 CDN 的常见行为，
// 换场景需要实测调整。

#ifndef SYPLAYER_SYP_CONFIG_H
#define SYPLAYER_SYP_CONFIG_H

#include "syp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // ABI 前向兼容：调用方必须填 sizeof(syp_config)。
    // 库据此判断调用方编译时见到的结构体版本，新增字段一律追加在末尾。
    uint32_t struct_size;

    // ---- 缓存 ----
    const char* cache_dir;            // 必填。索引与内容文件的根目录
    int64_t  max_cache_bytes;         // 磁盘缓存上限，0 = 不限。默认 512 MiB
    int64_t  max_memory_bytes;        // 内存缓存上限。默认 16 MiB
    int64_t  min_free_space_bytes;    // 磁盘可用空间低于此值时触发淘汰。默认 256 MiB
    int64_t  cache_ttl_ms;            // 缓存条目存活时间，0 = 不按时间淘汰。默认 7 天

    // ---- 下载调度 ----
    int32_t  max_concurrent_tasks;    // 单文件最大并发连接数。默认 3
    int64_t  min_segment_size;        // 分片下限，避免切得过碎。默认 512 KiB
                                      //   ⚠️ 512 KiB 缺少实测依据，需按实际网络调整
    int64_t  segment_size_hint;       // 期望分片大小，0 = 自动
                                      //   （自动 = ceil(剩余长度 / 并发数)，再夹在 min 与本值之间）

    // 判断"等现有任务下完" vs "另起一条连接"：
    //   estimate_ms = remaining_bytes / recent_speed
    //   若 remaining_bytes > reuse_max_remaining_bytes  → 不等
    //   若 estimate_ms > connect_estimate_max_ms        → 不等
    int64_t  reuse_max_remaining_bytes;  // 默认 1 MiB
    int32_t  connect_estimate_max_ms;    // 默认 2000
    int32_t  speed_window_ms;            // 近期速度统计窗口。默认 3000

    // ---- 单任务 ----
    int32_t  connect_timeout_ms;      // 默认 10000
    int32_t  read_timeout_ms;         // 默认 15000
    int32_t  max_redirects;           // 默认 8。超过返回 SYP_ERR_TOO_MANY_REDIRECTS
    int32_t  max_retries;             // 单任务连续无进展的重试次数（推进过字节即清零）。默认 3

    // ---- 保留字段（当前忽略）----
    // 连接复用由 HTTP 后端负责（Apple 后端：进程内唯一 NSURLSession，同主机
    // 连接自动保活）。这三个字段保留只为 ABI 兼容，库不读取；需要提前建连
    // 请用 syp_preconnect()（syp_net.h）。
    int32_t  idle_task_keep;          // 保留，忽略
    int32_t  idle_task_ttl_ms;        // 保留，忽略
    bool     enable_socket_pool;      // 保留，忽略

    // ---- 缓冲水位（供调度决策，不负责播放器起播判定）----
    int32_t  first_buffer_ms;         // 起播目标缓冲时长。默认 500
    int32_t  target_buffer_ms;        // 稳态目标缓冲时长。默认 10000

    // ---- 降级 ----
    bool     allow_no_range_fallback; // 服务端不支持 Range 时退回单连接全量下载。默认 true

} syp_config;

// 用默认值填充。调用方随后覆盖需要改的字段。
// cache_dir 不会被填充，必须自行设置。
void syp_config_init(syp_config* out);

#ifdef __cplusplus
}
#endif

#endif  // SYPLAYER_SYP_CONFIG_H
