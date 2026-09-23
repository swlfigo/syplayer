#include "media/frame_queue.h"

#include <utility>

namespace syp::media {

bool FrameQueue::push(Frame&& f) {
    if (full()) return false;      // 不吞掉传入的帧：调用方仍持有它
    q_.push_back(std::move(f));
    return true;
}

std::optional<Frame> FrameQueue::pop() {
    if (q_.empty()) return std::nullopt;
    Frame f = std::move(q_.front());
    q_.pop_front();
    return f;
}

}  // namespace syp::media
