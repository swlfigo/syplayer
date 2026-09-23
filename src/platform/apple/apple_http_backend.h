// apple_http_backend.h — NSURLSession 实现的 syp_http_backend（内部头，C 链接）
#pragma once

#include <syplayer/syp_http.h>

#ifdef __cplusplus
extern "C" {
#endif

// 返回进程内静态的函数表，永远非 NULL。调用方不得释放。
const syp_http_backend* syp_apple_http_backend(void);

#ifdef __cplusplus
}
#endif
