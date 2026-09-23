// audio_ring.h — 单生产者单消费者无锁环形缓冲。
//
// 这是内核与实时音频线程之间**唯一**的接触面。AudioUnit 的 render callback
// 只碰这个对象：从环里拷走字节、把已消费字节数原子自增。它拿不到 TrackPlayer、
// 拿不到 Pipeline、不上锁、不分配内存、不调 FFmpeg —— 这不是纪律要求，是结构
// 事实（回调的 inRefCon 指向的结构体里只有这个环和几个原子量）。
//
// 这是吸取了 Apple 后端单线程自死锁的教训：能靠结构挡住的，别靠记性。
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace syp::media {

class AudioRing {
public:
    explicit AudioRing(int32_t capacity_bytes);

    AudioRing(const AudioRing&)            = delete;
    AudioRing& operator=(const AudioRing&) = delete;

    int32_t write(const uint8_t* src, int32_t bytes) noexcept;
    int32_t read(uint8_t* dst, int32_t bytes) noexcept;

    int32_t capacity() const noexcept { return static_cast<int32_t>(buf_.size()); }
    int32_t readable() const noexcept;
    int32_t writable() const noexcept { return capacity() - readable(); }

    // 单调累计，永不回退。时钟换算靠它，所以 reset() 也不清它 —— 清了的话
    // seek/变速之后时钟会倒退，而调用方用的是「基准 + 增量」的模型。
    int64_t consumed_bytes() const noexcept {
        return read_pos_.load(std::memory_order_acquire);
    }

    // 生产者侧独占，调用前必须确保消费者已停止（AudioUnit 已 stop）。
    // 不清 consumed_bytes（见上）。
    //
    // 这是本类**唯一**一处靠调用方纪律而非结构保证的地方：这个类的自我
    // 定位是「能靠结构挡住的，别靠记性」（见文件头注释），但要在这里无锁地
    // 校验「消费者确实已停止」本身需要引入额外状态（比如一个只给消费者侧
    // 翻转的运行标记），不值得为这一处牺牲整个类的极简。调用方
    // （AudioUnitSink）必须先 AudioOutputUnitStop 确保回调不再触发，
    // 再调本函数。
    void reset() noexcept;

private:
    std::vector<uint8_t> buf_;
    std::atomic<int64_t> write_pos_{0};   // 单调累计写入字节数
    std::atomic<int64_t> read_pos_{0};    // 单调累计读出字节数
};

}  // namespace syp::media
