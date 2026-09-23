// frame.h — 解码帧的所有权基石
//
// Frame 独占持有底层资源，析构即释放；不可拷贝、只可移动。队列里存 Frame
// 本身而非指针，所以「谁持有」在类型上就是确定的。
//
// pts_us() 在解码器出口就换算成微秒，之后全管线统一 —— 避免「同一个量两种
// 口径」，那是 dl 层已知缺口的典型来源。
//
// Backend::Pixel（CVPixelBuffer 硬解路径）刻意不在本头里出现：
// src/media/ 必须保持平台无关。硬解加的是新分支，不是改接口。
#pragma once

#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/rational.h>
}

namespace syp::media {

class Frame {
public:
    // 硬解帧不新增 Backend——它仍是 AVFrame（pix_fmt 为
    // AV_PIX_FMT_VIDEOTOOLBOX / 将来 AV_PIX_FMT_MEDIACODEC），平台句柄经 hw_handle() 取。
    enum class Backend { Av };

    Frame() noexcept = default;
    ~Frame();

    Frame(const Frame&)            = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;

    // 接管 owned 的所有权。time_base 用于把 pts/duration 换算成微秒。
    static Frame from_av(AVFrame* owned, AVRational time_base, bool is_video);

    Backend backend() const noexcept { return Backend::Av; }
    bool    valid()   const noexcept { return av_ != nullptr; }

    int64_t pts_us()      const noexcept { return pts_us_; }
    int64_t duration_us() const noexcept { return duration_us_; }
    bool    is_video()    const noexcept { return is_video_; }

    // 视频
    int32_t        width()  const noexcept;
    int32_t        height() const noexcept;
    int32_t        pix_fmt() const noexcept;
    const uint8_t* plane(int i) const noexcept;
    int32_t        stride(int i) const noexcept;

    // 硬解帧的平台句柄：Apple 上是 CVPixelBufferRef，Android 上将是 AVMediaCodecBuffer*；
    // 软件帧与空帧返回 nullptr。生命周期随本 Frame（AVFrame 的 buf[0] 持有引用）。
    // 硬解帧的 plane()/stride() 无意义，调用方不得解引用。
    void* hw_handle() const noexcept;

    // AVColorSpace / AVColorRange，直接映射 AVFrame 对应字段；取不到
    // （av_ 为空）时分别返回 AVCOL_SPC_UNSPECIFIED / AVCOL_RANGE_UNSPECIFIED
    // ——跟这两个枚举本身"未标注"的哨兵值一致，调用方不需要额外判空。
    // MetalRenderer::present()
    // 需要从真实 Frame 读出 colorspace/color_range 再传给
    // select_color_matrix()，此前这两个字段在 Frame 的公开缝上不可见。
    int32_t colorspace()  const noexcept;
    int32_t color_range() const noexcept;

    // 音频
    int32_t sample_rate() const noexcept;
    int32_t channels()    const noexcept;   // 走 ch_layout.nb_channels
    int32_t sample_fmt()  const noexcept;
    int32_t nb_samples()  const noexcept;

private:
    AVFrame* av_          = nullptr;
    int64_t  pts_us_      = 0;
    int64_t  duration_us_ = 0;
    bool     is_video_    = false;
};

}  // namespace syp::media
