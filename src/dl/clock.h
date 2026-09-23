// clock.h — 注入式时钟。超时与速度窗口都只走这里，禁止直接 chrono::now。
#pragma once

#include <chrono>
#include <cstdint>

namespace syp::dl {

// 函数指针而不是 std::function：零分配，和 syp_http_backend 的函数表风格一致。
struct Clock {
    int64_t (*now_ms)(void* ctx);
    void*   ctx;
};

inline int64_t system_clock_now_ms(void* /*ctx*/) noexcept {
    using clock = std::chrono::steady_clock;
    const auto n = clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(n).count();
}

inline Clock system_clock() noexcept {
    return Clock{&system_clock_now_ms, nullptr};
}

}  // namespace syp::dl
