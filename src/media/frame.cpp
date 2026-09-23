#include "media/frame.h"

extern "C" {
#include <libavutil/mathematics.h>
}

namespace syp::media {
namespace {
constexpr AVRational kMicros{1, 1000000};
}

Frame::~Frame() {
    if (av_ != nullptr) av_frame_free(&av_);
}

Frame::Frame(Frame&& o) noexcept
    : av_(o.av_), pts_us_(o.pts_us_), duration_us_(o.duration_us_), is_video_(o.is_video_) {
    o.av_ = nullptr;
}

Frame& Frame::operator=(Frame&& o) noexcept {
    if (this != &o) {
        if (av_ != nullptr) av_frame_free(&av_);
        av_ = o.av_; pts_us_ = o.pts_us_; duration_us_ = o.duration_us_; is_video_ = o.is_video_;
        o.av_ = nullptr;
    }
    return *this;
}

Frame Frame::from_av(AVFrame* owned, AVRational tb, bool is_video) {
    Frame f;
    f.av_       = owned;
    f.is_video_ = is_video;
    if (owned != nullptr) {
        // AV_NOPTS_VALUE 保持原样传递，不要换算成一个看起来合法的数字 ——
        // 上层需要能分辨「没有 pts」与「pts 是 0」。
        f.pts_us_      = (owned->pts == AV_NOPTS_VALUE)
                             ? AV_NOPTS_VALUE
                             : av_rescale_q(owned->pts, tb, kMicros);
        f.duration_us_ = (owned->duration > 0) ? av_rescale_q(owned->duration, tb, kMicros) : 0;
    }
    return f;
}

int32_t Frame::width()  const noexcept { return av_ ? av_->width  : 0; }
int32_t Frame::height() const noexcept { return av_ ? av_->height : 0; }
int32_t Frame::pix_fmt() const noexcept { return av_ ? av_->format : -1; }

int32_t Frame::colorspace() const noexcept {
    return av_ ? static_cast<int32_t>(av_->colorspace) : static_cast<int32_t>(AVCOL_SPC_UNSPECIFIED);
}
int32_t Frame::color_range() const noexcept {
    return av_ ? static_cast<int32_t>(av_->color_range) : static_cast<int32_t>(AVCOL_RANGE_UNSPECIFIED);
}

const uint8_t* Frame::plane(int i) const noexcept {
    if (av_ == nullptr || i < 0 || i >= AV_NUM_DATA_POINTERS) return nullptr;
    return av_->data[i];
}

int32_t Frame::stride(int i) const noexcept {
    if (av_ == nullptr || i < 0 || i >= AV_NUM_DATA_POINTERS) return 0;
    return av_->linesize[i];
}

void* Frame::hw_handle() const noexcept {
    if (av_ == nullptr) return nullptr;
    const auto fmt = static_cast<AVPixelFormat>(av_->format);
    if (fmt == AV_PIX_FMT_VIDEOTOOLBOX || fmt == AV_PIX_FMT_MEDIACODEC) {
        return static_cast<void*>(av_->data[3]);
    }
    return nullptr;
}

int32_t Frame::sample_rate() const noexcept { return av_ ? av_->sample_rate : 0; }
// FFmpeg 7+ 把声道数移进了 ch_layout；旧的 channels 字段已废弃，不要用。
int32_t Frame::channels()    const noexcept { return av_ ? av_->ch_layout.nb_channels : 0; }
int32_t Frame::sample_fmt()  const noexcept { return av_ ? av_->format : -1; }
int32_t Frame::nb_samples()  const noexcept { return av_ ? av_->nb_samples : 0; }

}  // namespace syp::media
