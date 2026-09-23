// syp_net.h — 进程级网络设置：下行限速

#ifndef SYPLAYER_SYP_NET_H
#define SYPLAYER_SYP_NET_H

#include "syp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 进程级下行限速（字节/秒），0 = 不限（默认）。线程安全，随时可调，立即生效。
// 只控**平均**速率：单个在途请求内部全速，突发上限约为"在途请求数 × 1 秒额度"。
// 不受限的路径：**播放过程中**抓取的 HLS 播放列表（含直播刷新）；服务端不
// 支持 Range 时的单连接整文件下载；预连接请求（见下方 syp_preconnect）。
// 预加载器抓取的播放列表与预加载的元数据探测**受限**（按播放优先级走正常
// 的准入判定，与其余下载同一份预算）。
// 播放优先：预加载只在余额高于保留线时才能**开始新请求**；已发出的预加载
// 请求透支的额度由后续时间偿还，播放在预加载突发之后可能短暂等待（最坏约
// "同时在途的预加载请求数 × 1 秒"量级）。
// 负数按 0（不限）处理。
// ⚠️ 通常是一次原子写、立即返回；但极少数情况下（内部唤醒线程未能启动，
// 限速会自动关闭以避免永久卡住——fail-open）本调用会**在调用它的这个线程上
// 同步执行**已登记的调度回调，可能触发新下载任务的发起。这条路径下调用方
// 不得持有自己的锁再调用本函数（正常路径——线程已起或本来就不需要起——
// 没有这条限制，因为回调要么发生在异步线程上，要么根本不发生）。
void    syp_rate_limit_set(int64_t bytes_per_sec);
int64_t syp_rate_limit_get(void);

// 预连接：尽力而为地把 url 所在源（scheme+host+port）的 DNS/TCP/TLS
// 预热进 HTTP 后端的连接池，缩短随后首个请求的建连时间。立即返回、不阻塞。
// 实现：经 syp_set_http_backend 注册的后端发一个 Range: bytes=0-0 的 GET，
// 丢弃响应、不写缓存、不受下行限速；同一源 30 秒内只发一次；同时在途上限 4，
// 超出静默丢弃；失败不重试、不回报。未注册后端或 url 不是 http(s) 时什么都不做。
// headers 可为 NULL（需要鉴权头的 CDN 应传与播放相同的头）。线程安全。
// 与 syp_set_http_backend 同一条契约（syp_http.h）：后端必须在第一次调用
// syp_preconnect 之前注册好，且注册之后不得再替换。
void syp_preconnect(const char* url, const syp_headers* headers);

#ifdef __cplusplus
}
#endif

#endif  // SYPLAYER_SYP_NET_H
