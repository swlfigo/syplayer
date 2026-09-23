#include "media/audio_ring.h"

#include <algorithm>
#include <cstring>

namespace syp::media {

AudioRing::AudioRing(int32_t capacity_bytes)
    : buf_(static_cast<size_t>(capacity_bytes > 0 ? capacity_bytes : 1)) {}

int32_t AudioRing::readable() const noexcept {
    const int64_t w = write_pos_.load(std::memory_order_acquire);
    const int64_t r = read_pos_.load(std::memory_order_acquire);
    return static_cast<int32_t>(w - r);
}

int32_t AudioRing::write(const uint8_t* src, int32_t bytes) noexcept {
    if (src == nullptr || bytes <= 0) return 0;

    const int64_t w   = write_pos_.load(std::memory_order_relaxed);
    const int64_t r   = read_pos_.load(std::memory_order_acquire);
    const int32_t cap = capacity();
    const int32_t free_bytes = cap - static_cast<int32_t>(w - r);
    const int32_t n = std::min(bytes, free_bytes);
    if (n <= 0) return 0;

    const int32_t off   = static_cast<int32_t>(w % cap);
    const int32_t first = std::min(n, cap - off);
    std::memcpy(buf_.data() + off, src, static_cast<size_t>(first));
    if (n > first)
        std::memcpy(buf_.data(), src + first, static_cast<size_t>(n - first));

    // release：保证上面的 memcpy 对消费者可见，之后消费者才看得到新的 write_pos_
    write_pos_.store(w + n, std::memory_order_release);
    return n;
}

int32_t AudioRing::read(uint8_t* dst, int32_t bytes) noexcept {
    if (dst == nullptr || bytes <= 0) return 0;

    const int64_t r   = read_pos_.load(std::memory_order_relaxed);
    const int64_t w   = write_pos_.load(std::memory_order_acquire);
    const int32_t cap = capacity();
    const int32_t avail = static_cast<int32_t>(w - r);
    const int32_t n = std::min(bytes, avail);
    if (n <= 0) return 0;

    const int32_t off   = static_cast<int32_t>(r % cap);
    const int32_t first = std::min(n, cap - off);
    std::memcpy(dst, buf_.data() + off, static_cast<size_t>(first));
    if (n > first)
        std::memcpy(dst + first, buf_.data(), static_cast<size_t>(n - first));

    read_pos_.store(r + n, std::memory_order_release);
    return n;
}

void AudioRing::reset() noexcept {
    // 把读指针推到写指针处 = 丢弃全部未读内容，且 consumed_bytes 不回退。
    write_pos_.store(read_pos_.load(std::memory_order_acquire), std::memory_order_release);
}

}  // namespace syp::media
