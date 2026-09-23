// frame_queue.h — 有界帧队列。满了 push 返回 false，由调用方决定怎么办
//（管线会把它翻译成 StepOutcome::Blocked）。不阻塞、不持锁等待。
#pragma once

#include <cstddef>
#include <deque>
#include <optional>

#include "media/frame.h"

namespace syp::media {

class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity) : capacity_(capacity) {}

    bool                 push(Frame&& f);
    std::optional<Frame> pop();
    void                 clear() noexcept { q_.clear(); }

    std::size_t size()     const noexcept { return q_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    bool        full()     const noexcept { return q_.size() >= capacity_; }
    bool        empty()    const noexcept { return q_.empty(); }

private:
    std::deque<Frame> q_;
    std::size_t       capacity_;
};

}  // namespace syp::media
