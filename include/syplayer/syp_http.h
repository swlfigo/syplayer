// syp_http.h — HTTP 后端接入点（跨平台的关键缝）
//
// dl 层**绝不直接调用任何平台网络 API**。它只认下面这张函数表。
//   - iOS/macOS：用 NSURLSession 实现
//   - Android：用 OkHttp / Cronet 实现（未来）
//   - 桌面/测试：libcurl 或内存桩
//
// 线程模型：
//   - dl 层可能在任意内部线程调用 syp_http_backend 的方法
//   - 后端可在任意线程回调 syp_response_sink，但**同一个请求的回调必须串行**
//     （on_response → on_data* → on_complete，顺序不可乱、不可并发）
//   - on_complete 之后不得再对该请求发起任何回调

#ifndef SYPLAYER_SYP_HTTP_H
#define SYPLAYER_SYP_HTTP_H

#include "syp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- 请求描述
typedef struct {
    const char* url;
    syp_headers headers;        // 不含 Range，由下面两个字段表达
    int64_t     range_start;    // 起始字节，>= 0
    int64_t     range_end;      // 结束字节（含），-1 表示到文件尾
    int32_t     connect_timeout_ms;
    int32_t     read_timeout_ms;
} syp_http_request;

// ---------------------------------------------------------------- 回调入口
// dl 层提供，后端调用。所有指针参数的生命周期仅限回调期间，需要留存必须拷贝。
typedef struct {
    void* ctx;

    // 收到响应头。content_length 为本次响应体长度（不是文件总长），未知填 -1。
    // total_length 为资源总长（从 Content-Range 解析），未知填 -1。
    void (*on_response)(void* ctx, int32_t http_status,
                        const syp_headers* headers,
                        int64_t content_length, int64_t total_length);

    // 收到数据。后端不必保证分片大小。
    void (*on_data)(void* ctx, const uint8_t* data, int32_t len);

    // 发生重定向。返回 true 表示继续跟随，false 表示中止。
    // 重定向计数由 dl 层维护（cfg.max_redirects）。
    bool (*on_redirect)(void* ctx, const char* new_url);

    // 请求结束。status 为 SYP_OK 或负错误码；
    // 若是 SYP_ERR_HTTP_STATUS，http_status 带上具体状态码，否则填 0。
    void (*on_complete)(void* ctx, syp_status status, int32_t http_status);
} syp_response_sink;

// ---------------------------------------------------------------- 后端函数表
typedef struct syp_http_request_handle syp_http_request_handle;

typedef struct {
    void* backend_ctx;

    // 创建请求。不发起传输。返回 NULL 表示创建失败。
    // sink 由 dl 层保证在 destroy 之前一直有效。
    syp_http_request_handle* (*create)(void* backend_ctx,
                                       const syp_http_request* req,
                                       const syp_response_sink* sink);

    // 发起传输。异步，立即返回。
    void (*start)(syp_http_request_handle* h);

    // 请求取消。可在任意线程调用，可重入。
    // 取消后后端仍须回调一次 on_complete(SYP_ERR_CANCELED, 0)。
    //
    // cancel 可能在 start 之前被调用（dl 层在 create 与 start 之间收到取消
    // 时就会这样）。无论 start 是否调用过，cancel 之后都必须回调恰好一次
    // on_complete(SYP_ERR_CANCELED, 0)。
    // 从未 start 过的请求被 destroy 时，不得回调。
    //
    // 生命周期重入规则（三条，写清楚是为了让新实现的后端不必重新踩坑）：
    //   1. cancel 在 on_complete 已经回调完成之后、destroy 调用之前再被
    //      调用，必须是 no-op —— 不得再触发任何回调（也不得补发第二次
    //      on_complete）。这段窗口里 dl 层可能仍持有非空句柄并合法地调
    //      一次 cancel（例如另一线程的收尾路径），后端不能把它当成一次
    //      新的取消。
    //   2. destroy 可以在 on_complete 回调**内部**、由后端自己投递该回调
    //      的那条线程同步调用（dl 层的正常用法就是这样：收到 on_complete
    //      后立即同步 destroy）。后端在 destroy 里等待"没有回调仍在途"时，
    //      判据必须能识别"调用 destroy 的这条线程，是不是正好就在本请求
    //      自己的回调帧里"——如果是，必须跳过等待直接返回，不能去等调用
    //      线程自己下方那一帧才会释放的状态（那是等自己，永远等不到）。
    //      逃生口的判据看的是 destroy **调用者**所在的线程/帧，与 cancel
    //      是谁发起的、从哪条线程发起的无关。
    //   3. destroy 返回之后，后端不得再以任何方式触碰 sink（包括不能有
    //      任何已提交但尚未执行的回调在 destroy 返回后才触发）。
    void (*cancel)(syp_http_request_handle* h);

    // 释放。调用前 dl 层保证已收到 on_complete；可能在 on_complete 回调
    // 内部同步调用，也可能在回调之外的任意线程调用——见上面 cancel 附近
    // 的三条生命周期重入规则，destroy 必须同时正确处理这两种调用形状。
    void (*destroy)(syp_http_request_handle* h);
} syp_http_backend;

// 注册全局 HTTP 后端。必须在创建任何 syp_source、预加载器，或调用
// syp_preconnect 之前调用一次；注册之后不得再替换——已在途的 DLTask 读的是
// 就地覆盖的同一份全局存储（源码侧证据：src/dl/source_bridge.h 关于
// current_http_backend() 的注释），运行中换后端会让它们读到半新半旧的状态。
// 传 NULL 恢复为内置后端（若该平台有）。
syp_status syp_set_http_backend(const syp_http_backend* backend);

#ifdef __cplusplus
}
#endif

#endif  // SYPLAYER_SYP_HTTP_H
