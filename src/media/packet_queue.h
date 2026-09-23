// packet_queue.h — 有界 packet 队列，数量与字节数双重设限。
//
// 只按个数限会被大 packet 撑爆内存；只按字节限会被海量小 packet 拖垮。
// pop() 转移所有权，调用方负责 av_packet_free。
//
// 线程安全：全部方法内部加锁。线程模式下它是加载线程（push）与泵线程
// （pop/clear）之间唯一的跨线程数据边界。mu_ 是叶子锁——持有期间不调用任何
// 外部代码；Pipeline 的锁序是 load_mu_ → 本类 mu_。
//
// buffered_until_us()：已入队包结束时刻（(pts，缺失时 dts) + duration，
// µs）的最大值。取最大值而不是"最后一个包"：B 帧 pts 乱序时仍单调。pop 不回退
// ——已送进解码器的数据仍算"已缓冲到"；clear() 复位为 AV_NOPTS_VALUE。时基未知
// （构造时没给）时恒为 AV_NOPTS_VALUE。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
}

namespace syp::media {

class PacketQueue {
public:
    PacketQueue(std::size_t max_packets, int64_t max_bytes, AVRational time_base = AVRational{0, 1})
        : max_packets_(max_packets), max_bytes_(max_bytes), time_base_(time_base) {}
    ~PacketQueue() { clear(); }

    PacketQueue(const PacketQueue&)            = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    bool       push(AVPacket* owned);   // 失败时不接管所有权
    AVPacket*  pop();                   // 转移所有权，空时返回 nullptr
    void       clear() noexcept;
    // 摘出全部包（按队列顺序，所有权转给调用方），队列状态复位同 clear()。给"持外层锁
    // 摘、解锁后再释放"的调用方用（Pipeline::seek_async）：av_packet_free 不在任何锁内。
    std::deque<AVPacket*> take_all();

    std::size_t size()  const noexcept;
    int64_t     bytes() const noexcept;
    bool        full()  const noexcept;
    bool        empty() const noexcept;

    int64_t buffered_until_us() const noexcept;
    // 队尾结束时刻 − 队首包开始时刻（≥0）；空队列或任一端未知为 0。线程模式的
    // 停读水位用它（加载线程不知道播放位置，见 pipeline.cpp water_full_locked()）。
    int64_t queued_duration_us() const noexcept;

private:
    struct Entry {
        AVPacket* pkt;
        int64_t   start_us;   // AV_NOPTS_VALUE 表示未知
    };
    bool full_locked() const noexcept {
        return q_.size() >= max_packets_ || bytes_ >= max_bytes_;
    }

    mutable std::mutex    mu_;
    std::deque<Entry>     q_;
    std::size_t           max_packets_;
    int64_t               max_bytes_;
    AVRational            time_base_;
    int64_t               bytes_             = 0;
    int64_t               buffered_until_us_ = AV_NOPTS_VALUE;
};

}  // namespace syp::media
