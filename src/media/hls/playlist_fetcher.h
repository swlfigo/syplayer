// playlist_fetcher.h —— HLS 播放列表通道：把异步的 syp_http_backend
// 包成一次阻塞的“取回整个播放列表”。
//
// 【为什么不走 syp_source】播放列表在直播下**必须永不缓存**（同一 URL
// 内容每几秒变一次），而 dl 层在区间已缓存时一个请求都不发
// （src/dl/source_bridge.cpp:911），校验只在有新响应时才跑
// （cache_index.cpp:484）—— 走 syp_source 会让直播冻在第一次拉到的那份上，
// 最长 cache_ttl_ms（默认 7 天）。
//
// 而 syp_source 提供的一切（Range、洞调度、分段并发、磁盘索引）播放列表
// 都不需要：它小、一次取完、且必须每次都新鲜。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <syplayer/syp_http.h>
#include <syplayer/syp_types.h>

namespace syp::media::hls {

// 播放列表是文本，正常在几 KB 到几百 KB。没有上限的话，一个恶意或错误的
// URL 会把任意大的响应体读进内存。
inline constexpr int64_t kMaxPlaylistBytes = 8 * 1024 * 1024;

// 中止标志的回看周期。50ms：对"用户导航离开"这个尺度完全够用，而
// std::atomic::wait/notify 在本项目是禁用的（iOS 13 下限，见 CLAUDE 纪律），
// 所以只能轮询——但轮询的是一个已经在等 cv 的线程，不额外占 CPU。
inline constexpr int kAbortPollMs = 50;

struct PlaylistFetchResult {
    syp_status           status      = SYP_OK;
    int32_t              http_status = 0;   // 拿到响应头才有意义，否则 0
    std::vector<uint8_t> body;              // status != SYP_OK 时为空
};

// 阻塞直到取完、失败、或超过 read_timeout_ms。
//
// backend 为 nullptr 时返回 SYP_ERR_INVALID_ARG（调用方应传
// syp::dl::current_http_backend()，与分片通道用的是同一个后端——
// 这让“两条通道用同一个 HTTP 后端”成为结构事实而不是约定）。
//
// 【超时的正确做法】超时后**必须 cancel 并等 on_complete 真的到达**再返回。
// 两条独立理由，缺一都不够：
//
//   1. 契约层面（决定性）：include/syplayer/syp_http.h:97 明文写着
//      destroy() 的前置条件是「调用前 dl 层保证已收到 on_complete」——这是
//      我们对**任何**后端都要遵守的接口保证，不是"这个后端凑巧安全所以可
//      以不管"。Apple 后端当前的 destroy() 实现
//      （apple_http_backend.mm:616-681 的 Handle::destroy()，内部用
//      cv_.wait_for 循环等到 inflight_==0 && !in_callback_ 才真正返回，
//      之后才 [t cancel]）会自己兜底等待，但那是**该实现给违约调用方的额
//      外保险，不是接口承诺的一部分**——换一个后端（未来的 Android/Linux
//      实现）完全可以合规地在 destroy() 里立刻释放资源而不等，届时不遵守
//      这条契约就是真 UAF。反向自检已经验证过这个区
//      别：跳过本函数的 cancel+wait、但仍调用 backend->destroy(h) 时，在
//      当前 Apple 后端上并不会产生 ASan 能抓到的 UAF；必须进一步绕过
//      destroy()（如真的泄漏 handle）才能人为构造出可观测的崩溃。
//   2. 计时语义层面（与 UAF 无关，纯粹的超时契约正确性）：Apple 后端自己
//      的超时是**滚动空闲计时器**而不是总时长上限——did_receive_response
//      （apple_http_backend.mm:849）和 did_receive_data（:927）每次都会
//      重新 arm_timer(read_timeout_ms)。只要服务端以小于 read_timeout_ms
//      的间隔持续"挤牙膏"式发送，后端内部计时器永远不会触发，destroy()
//      会一直挂着等。fetch_playlist 自己 wait_until 用的是绝对截止时间，
//      专门用来防住这类慢速蹭网络的情况，让函数真正兑现本注释开头"或超过
//      read_timeout_ms"这句承诺——没有它，头文件字面上的超时承诺在这种
//      场景下不成立。
//
// 【abort：播放列表通道的中止口】非 nullptr 时本函数会
// 周期性（kAbortPollMs）回看它一眼；置位就 cancel + 等 on_complete，返回
// SYP_ERR_CANCELED。调用前就已经置位的话，一个请求都不发。
//
// 为什么是"传一个标志指针 + 轮询"而不是"把 handle 交出去让外面 cancel"：
// 后者要把一个后端 handle 的生命周期跨线程暴露出去，而 syp_http.h:97 的
// destroy() 契约（调用前必须已收到 on_complete）就会变成两个线程的共同
// 责任——那是本文件用一整段注释在防的那类事故。轮询把所有权整个留在本
// 函数里，代价只是最多 kAbortPollMs 的延迟。
//
// **为什么非要有它**：分片那条通道的中止走 AvioBridge::request_abort() →
// syp_source_interrupt()，一路是通的；播放列表这条在此之前只受
// read_timeout_ms 约束，看门狗在播放列表抓取期间开火完全没有效果。直播每
// 几秒重拉一次播放列表，用户导航离开时撞上一次正在进行的重拉是常态。
// 生命周期契约：*abort 必须在本函数返回前一直有效（HlsSession 里它是会话
// 自己的成员，天然满足）。
PlaylistFetchResult fetch_playlist(const syp_http_backend* backend,
                                   const std::string&      url,
                                   int32_t                 connect_timeout_ms,
                                   int32_t                 read_timeout_ms,
                                   const std::atomic<bool>* abort = nullptr);

}  // namespace syp::media::hls
