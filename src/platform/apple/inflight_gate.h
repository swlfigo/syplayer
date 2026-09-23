// inflight_gate.h —— 渲染在途帧上限。纯 C++，无 GPU 依赖，可单测。
// try_acquire 非阻塞：满了立刻返回 false，调用方据此返回 SYP_ERR_BUSY，而不是等。
// release 在 Metal 回调里调用（任意线程）：离屏在命令缓冲完成回调，挂 layer 时在
// drawable 的 presentedHandler（drawable 回到池子的时机），见 metal_renderer.mm。
#pragma once

#include <atomic>
#include <cstdint>

namespace syp::platform {

inline constexpr int32_t kMaxInflightFrames = 3;

class InflightGate {
public:
    explicit InflightGate(int32_t capacity) noexcept : capacity_(capacity) {}
    bool try_acquire() noexcept { return try_acquire(capacity_); }
    // 本次按更小的上限判定（limit 与构造容量取小者）：挂 CAMetalLayer 时上限受
    // drawable 池约束（maximumDrawableCount - 1），见 metal_renderer.mm present()。
    bool try_acquire(int32_t limit) noexcept {
        const int32_t cap = limit < capacity_ ? limit : capacity_;
        int32_t cur = in_flight_.load(std::memory_order_relaxed);
        while (cur < cap) {
            // 注：不用 memory_order_acq_rel——那是 libc++ 的非标准遗留扩展。
            if (in_flight_.compare_exchange_weak(cur, cur + 1, std::memory_order_seq_cst,
                                                 std::memory_order_seq_cst)) {
                return true;
            }
        }
        return false;
    }
    void release() noexcept { in_flight_.fetch_sub(1, std::memory_order_seq_cst); }
    int32_t in_flight() const noexcept { return in_flight_.load(std::memory_order_seq_cst); }
private:
    const int32_t        capacity_;
    std::atomic<int32_t> in_flight_{0};
};

}  // namespace syp::platform
