// avio_bridge.h — 把 syp_source 包成 FFmpeg 的 AVIOContext
//
// 这是 dl 层与 FFmpeg 之间的缝。Demuxer 接的是同一条缝，
// 所以按生产代码标准写，不是验证工具的一次性胶水。
//
// 不接管 syp_source 的生命周期（谁开谁关）。不做写入路径（本项目只读）。
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <syplayer/syp_source.h>

extern "C" {
#include <libavformat/avio.h>
}

namespace syp::media {

std::string ffmpeg_version_string();

// syp_status → AVERROR。绝不返回 0——0 会被 FFmpeg 当成成功。
int syp_status_to_averror(int32_t status) noexcept;

// FFmpeg 的 whence → SYP_SEEK_*。内部先 mask 掉 AVSEEK_FORCE。
// AVSEEK_SIZE 不是 whence，由调用方先行处理，这里返回 false。
// 未知 whence 也返回 false。
bool avio_whence_to_syp(int whence, int32_t* out) noexcept;

class AvioBridge {
public:
    // src 须在 bridge 存活期间保持有效。buffer_size 传 0 用默认 64 KiB。
    // 失败返回 nullptr。
    static std::unique_ptr<AvioBridge> create(syp_source* src, int buffer_size);

    ~AvioBridge();
    AvioBridge(const AvioBridge&) = delete;
    AvioBridge& operator=(const AvioBridge&) = delete;

    AVIOContext* ctx() const noexcept { return ctx_; }

    // 挂到 AVFormatContext::interrupt_callback。没有它，看门狗打不断阻塞中的 read。
    AVIOInterruptCB interrupt_cb() noexcept;

    // 看门狗调用：置中止标志并打断底层阻塞的 read。可在任意线程调用。
    //
    // 生命周期契约：调用方须保证 request_abort() 的调用不会与 ~AvioBridge()
    // 重叠，也不会发生在析构之后——这条缝一旦跨过就是 UAF。典型编排：
    // 看门狗线程持有的是裸指针/非拥有引用，析构前必须先让看门狗停止调用
    // （join 或其它同步手段），而不是指望 request_abort() 自己去做同步。
    void request_abort() noexcept;

    struct Diag {
        int64_t reads = 0;
        int64_t seeks = 0;
        int     last_averror = 0;
        // FFmpeg 经这条缝读到过的**最大文件偏移**（读完那一刻的位置），
        // 也就是"这次解析真正够到了多远"。0 = 一个字节都没读过。
        //
        // 存在的理由：MediaInfoProvider 要拿"容器头有
        // 多大"当线性估算的常数项，之前取的是 syp_source 的已缓存区间右端
        // ——那是**缓存**的属性，不是这次解析的属性：同一个 URL 冷热两次
        // 估出两个值。这个数只数经过本桥的读，与缓存里原本有什么无关。
        int64_t max_read_end = 0;
    };
    Diag diag() const noexcept;

private:
    AvioBridge() = default;

    static int     on_read(void* opaque, uint8_t* buf, int buf_size);
    static int64_t on_seek(void* opaque, int64_t offset, int whence);
    static int     on_interrupt(void* opaque);

    syp_source*       src_ = nullptr;
    AVIOContext*      ctx_ = nullptr;
    std::atomic<bool> abort_{false};
    std::atomic<int64_t> reads_{0};
    std::atomic<int64_t> seeks_{0};
    std::atomic<int>     last_averror_{0};
    // 本桥自己跟的文件位置：on_seek 把它设成 seek 的落点，on_read 把它
    // 往前推 n。所有 IO 都经过这两个回调，所以它就是准确的当前位置。
    std::atomic<int64_t> pos_{0};
    std::atomic<int64_t> max_read_end_{0};
};

}  // namespace syp::media
