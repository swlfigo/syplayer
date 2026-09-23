#include "media/packet_queue.h"

extern "C" {
#include <libavutil/mathematics.h>
}

namespace syp::media {

namespace {

constexpr AVRational kMicros{1, 1000000};

// 包的开始时刻（µs）：pts 优先，缺失时退到 dts；时基无效或两者都缺失为 AV_NOPTS_VALUE。
int64_t packet_start_us(const AVPacket* p, AVRational tb) noexcept {
    if (tb.num <= 0 || tb.den <= 0) return AV_NOPTS_VALUE;
    const int64_t ts = (p->pts != AV_NOPTS_VALUE) ? p->pts : p->dts;
    if (ts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    return av_rescale_q(ts, tb, kMicros);
}

}  // namespace

bool PacketQueue::push(AVPacket* owned) {
    if (owned == nullptr) return false;
    // 时刻换算不碰队列状态，放在锁外。
    const int64_t start = packet_start_us(owned, time_base_);
    int64_t       end   = start;
    if (start != AV_NOPTS_VALUE && owned->duration > 0) {
        end = start + av_rescale_q(owned->duration, time_base_, kMicros);
    }
    std::lock_guard<std::mutex> g(mu_);
    if (full_locked()) return false;
    q_.push_back(Entry{owned, start});
    bytes_ += owned->size;
    if (end != AV_NOPTS_VALUE &&
        (buffered_until_us_ == AV_NOPTS_VALUE || end > buffered_until_us_)) {
        buffered_until_us_ = end;
    }
    return true;
}

AVPacket* PacketQueue::pop() {
    std::lock_guard<std::mutex> g(mu_);
    if (q_.empty()) return nullptr;
    AVPacket* p = q_.front().pkt;
    q_.pop_front();
    bytes_ -= p->size;
    return p;
}

void PacketQueue::clear() noexcept {
    std::lock_guard<std::mutex> g(mu_);
    for (Entry& e : q_) av_packet_free(&e.pkt);
    q_.clear();
    bytes_             = 0;
    buffered_until_us_ = AV_NOPTS_VALUE;
}

std::deque<AVPacket*> PacketQueue::take_all() {
    std::deque<Entry> taken;
    {
        std::lock_guard<std::mutex> g(mu_);
        taken.swap(q_);
        bytes_             = 0;
        buffered_until_us_ = AV_NOPTS_VALUE;
    }
    std::deque<AVPacket*> out;
    for (const Entry& e : taken) out.push_back(e.pkt);
    return out;
}

std::size_t PacketQueue::size() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return q_.size();
}

int64_t PacketQueue::bytes() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return bytes_;
}

bool PacketQueue::full() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return full_locked();
}

bool PacketQueue::empty() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return q_.empty();
}

int64_t PacketQueue::buffered_until_us() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return buffered_until_us_;
}

int64_t PacketQueue::queued_duration_us() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    if (q_.empty()) return 0;
    const int64_t head = q_.front().start_us;
    if (head == AV_NOPTS_VALUE || buffered_until_us_ == AV_NOPTS_VALUE) return 0;
    return buffered_until_us_ > head ? buffered_until_us_ - head : 0;
}

}  // namespace syp::media
