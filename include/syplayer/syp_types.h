// syp_types.h — 基础类型与错误码
//
// ⚠️ 本目录下所有头文件是 syplayer 的公开 C ABI：
//    只允许 C 基本类型 / 不透明指针 / POD struct / 函数指针。
//    禁止出现 C++ 类型、Objective-C 类型、Metal/CoreVideo 类型。
//    这条纪律决定了 Swift 能否直接绑定、将来能否换 Rust 实现、Android 能否接入。

#ifndef SYPLAYER_SYP_TYPES_H
#define SYPLAYER_SYP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- 错误码
// 约定：0 成功，负数失败。read 类接口用 >0 表示字节数。
typedef int32_t syp_status;

enum {
    SYP_OK                    =  0,

    SYP_ERR_EOF               = -1,   // 正常读到文件尾
    SYP_ERR_INVALID_ARG       = -2,
    SYP_ERR_CANCELED          = -3,   // 被 syp_source_interrupt 打断
    SYP_ERR_TIMEOUT           = -4,
    SYP_ERR_OOM               = -5,
    // 瞬态、不是故障：调用方保留本次的输入稍后重试（如渲染在途帧已满时重试同一帧）。
    SYP_ERR_BUSY              = -6,   // 暂时没有资源处理（如渲染在途帧已满），本次未执行，不是故障

    SYP_ERR_IO                = -10,  // 本地读写失败
    SYP_ERR_NO_SPACE          = -11,
    SYP_ERR_CACHE_CORRUPT     = -12,  // 索引损坏 / 校验失败

    SYP_ERR_NETWORK           = -20,  // 传输层失败（连不上、断流）
    SYP_ERR_HTTP_STATUS       = -21,  // 服务端返回错误状态码，详见 syp_error_info.http_status
    SYP_ERR_TOO_MANY_REDIRECTS= -22,  // 超过 cfg.max_redirects
    SYP_ERR_RANGE_UNSUPPORTED = -23,  // 服务端不支持 Range 且配置不允许降级
    SYP_ERR_CONTENT_CHANGED   = -24,  // etag/length 与缓存索引不一致，源文件变了

    SYP_ERR_NOT_IMPLEMENTED   = -99,
};

// 返回错误码的静态描述串（只读，不需要释放；未知码返回 "unknown"）
const char* syp_status_str(syp_status s);

// ---------------------------------------------------------------- 基础结构
// 字节区间，半开区间 [start, end)
typedef struct {
    int64_t start;
    int64_t end;
} syp_range;

// seek 的 whence，取值与 <stdio.h> 一致，便于直接转给 FFmpeg
enum {
    SYP_SEEK_SET = 0,
    SYP_SEEK_CUR = 1,
    SYP_SEEK_END = 2,
};

// 一组 HTTP 头。names/values 平行数组，均为 NUL 结尾字符串。
// 生命周期由调用方持有，被调方若需保留必须自行拷贝。
typedef struct {
    const char* const* names;
    const char* const* values;
    int32_t            count;
} syp_headers;

// ---------------------------------------------------------------- 日志
typedef enum {
    SYP_LOG_ERROR = 0,
    SYP_LOG_WARN  = 1,
    SYP_LOG_INFO  = 2,
    SYP_LOG_DEBUG = 3,
} syp_log_level;

// 设置全局日志回调。传 NULL 关闭。可能在任意内部线程被调用。
typedef void (*syp_log_fn)(void* ctx, syp_log_level lvl, const char* tag, const char* msg);
void syp_set_log_callback(syp_log_fn fn, void* ctx, syp_log_level max_level);

// 库版本，形如 "0.1.0"
const char* syp_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SYPLAYER_SYP_TYPES_H
