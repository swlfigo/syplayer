// syp_net_api.cpp — syp_net.h 的实现：进程级限速的 C 入口。
#include <syplayer/syp_net.h>

#include "dl/preconnector.h"
#include "dl/rate_limiter.h"

namespace syp::dl {
// 定义在 source_bridge.cpp（同一静态库、同一命名空间），与 rate_limiter.cpp /
// cache_store.h 同法前置声明，不为一行日志多拉一个头。
void log_msg(syp_log_level lvl, const char* tag, const char* msg);
}  // namespace syp::dl

extern "C" {

// RateLimiter::instance() 不是 noexcept：懒初始化里的
// `new RateLimiter(...)` 可能抛 std::bad_alloc，任其穿过这层 extern "C"
// 边界就是未定义行为（实践上是 std::terminate），与
// src/dl/syp_preload_api.cpp 的 syp_preloader_remove 等入口同一处理方式。
// 两个入口都没有 status 出参可用，降级口径：set 静默放弃（等价于"没调
// 过"——进程限速本就默认 0/不限，不是中间态）；get 按"不限"返回 0。
//
// set 这一路补一条 WARN 日志（锁外——这里本来就没持
// 任何锁，log_msg 同步调用户回调也不会有重入锁的风险；跟 rate_limiter.cpp
// fail-open 那条 WARN 是同一个理由，只是这里没有锁可担心）。get 这一路没有
// 补：取不到时静默降级为"不限"就是它的正常返回值，不是错误。
void syp_rate_limit_set(int64_t bytes_per_sec) {
    try {
        syp::dl::RateLimiter::instance().set_rate(bytes_per_sec);   // 夹取在 set_rate 里
    } catch (...) {
        syp::dl::log_msg(SYP_LOG_WARN, "ratelimit",
                          "syp_rate_limit_set: RateLimiter::instance() threw; call ignored");
    }
}

int64_t syp_rate_limit_get(void) {
    try {
        return syp::dl::RateLimiter::instance().rate();
    } catch (...) {
        return 0;   // 取不到时按"不限"处理
    }
}

// 【同 syp_rate_limit_set 的异常兜底】Preconnector::instance() 同样不是
// noexcept（懒初始化的 new 可能抛 bad_alloc），穿过 extern "C" 边界是未定义
// 行为，同法夹住、WARN 一条、调用方当"什么都没发生"处理——预连接本就是
// 尽力而为、无回报，这与"没调过"在语义上等价。
// url == nullptr 直接返回：不进 try，Preconnector::preconnect
// 接的是 std::string_view，从空指针构造是未定义行为，必须在这一层挡住。
void syp_preconnect(const char* url, const syp_headers* headers) {
    if (url == nullptr) return;
    try {
        syp::dl::Preconnector::instance().preconnect(url, headers);
    } catch (...) {
        syp::dl::log_msg(SYP_LOG_WARN, "preconnect",
                          "syp_preconnect threw; call ignored");
    }
}

}  // extern "C"
