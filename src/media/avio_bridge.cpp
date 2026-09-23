#include "media/avio_bridge.h"

#include <cerrno>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace syp::media {

std::string ffmpeg_version_string() {
    const unsigned v = avformat_version();
    return "libavformat " + std::to_string(v >> 16) + "." +
           std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}

int syp_status_to_averror(int32_t status) noexcept {
    switch (status) {
        case SYP_ERR_EOF:                return AVERROR_EOF;
        case SYP_ERR_CANCELED:           return AVERROR_EXIT;
        case SYP_ERR_TIMEOUT:            return AVERROR(ETIMEDOUT);
        case SYP_ERR_INVALID_ARG:        return AVERROR(EINVAL);
        case SYP_ERR_OOM:                return AVERROR(ENOMEM);
        case SYP_ERR_BUSY:               return AVERROR(EAGAIN);   // 瞬态：稍后再试，不是故障
        case SYP_ERR_IO:                 return AVERROR(EIO);
        case SYP_ERR_NO_SPACE:           return AVERROR(ENOSPC);
        case SYP_ERR_CACHE_CORRUPT:      return AVERROR_INVALIDDATA;
        case SYP_ERR_NETWORK:            return AVERROR(EIO);
        case SYP_ERR_HTTP_STATUS:        return AVERROR(EIO);
        case SYP_ERR_TOO_MANY_REDIRECTS: return AVERROR(ELOOP);
        case SYP_ERR_RANGE_UNSUPPORTED:  return AVERROR(ENOSYS);
        case SYP_ERR_CONTENT_CHANGED:    return AVERROR_INVALIDDATA;
        case SYP_ERR_NOT_IMPLEMENTED:    return AVERROR(ENOSYS);
        default:                         return AVERROR_UNKNOWN;
    }
}

bool avio_whence_to_syp(int whence, int32_t* out) noexcept {
    if (out == nullptr) return false;
    // AVSEEK_FORCE 是标志位不是 whence，必须先摘掉；
    // 不摘的话 SEEK_END|AVSEEK_FORCE 会掉进 default 被当成未知 whence。
    const int w = whence & ~AVSEEK_FORCE;
    switch (w) {
        case SEEK_SET: *out = SYP_SEEK_SET; return true;
        case SEEK_CUR: *out = SYP_SEEK_CUR; return true;
        case SEEK_END: *out = SYP_SEEK_END; return true;
        default:       return false;   // 含 AVSEEK_SIZE：由调用方先行处理
    }
}

int AvioBridge::on_read(void* opaque, uint8_t* buf, int buf_size) {
    auto* self = static_cast<AvioBridge*>(opaque);
    self->reads_.fetch_add(1, std::memory_order_relaxed);
    const int32_t n = syp_source_read(self->src_, buf, buf_size);
    if (n > 0) {
        const int64_t end = self->pos_.fetch_add(n, std::memory_order_relaxed) + n;
        int64_t hi = self->max_read_end_.load(std::memory_order_relaxed);
        while (end > hi
               && !self->max_read_end_.compare_exchange_weak(
                      hi, end, std::memory_order_relaxed)) {
        }
        return n;
    }
    const int e = syp_status_to_averror(n);
    self->last_averror_.store(e, std::memory_order_relaxed);
    return e;
}

int64_t AvioBridge::on_seek(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<AvioBridge*>(opaque);
    self->seeks_.fetch_add(1, std::memory_order_relaxed);

    // AVSEEK_SIZE：不 seek，只报总长。
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) {
        const int64_t len = syp_source_length(self->src_);
        if (len < 0) return AVERROR(ENOSYS);
        return len;
    }

    int32_t w = 0;
    if (!avio_whence_to_syp(whence, &w)) return AVERROR(EINVAL);

    const int64_t pos = syp_source_seek(self->src_, offset, w);
    if (pos >= 0) self->pos_.store(pos, std::memory_order_relaxed);
    if (pos < 0) {
        const int e = syp_status_to_averror(static_cast<int32_t>(pos));
        self->last_averror_.store(e, std::memory_order_relaxed);
        return e;
    }
    return pos;
}

int AvioBridge::on_interrupt(void* opaque) {
    auto* self = static_cast<AvioBridge*>(opaque);
    return self->abort_.load(std::memory_order_acquire) ? 1 : 0;
}

std::unique_ptr<AvioBridge> AvioBridge::create(syp_source* src, int buffer_size) {
    if (src == nullptr) return nullptr;
    const int sz = buffer_size > 0 ? buffer_size : 64 * 1024;

    // 缓冲区必须用 av_malloc：FFmpeg 内部可能对它做 av_realloc。
    auto* buf = static_cast<unsigned char*>(av_malloc(static_cast<size_t>(sz)));
    if (buf == nullptr) return nullptr;

    std::unique_ptr<AvioBridge> self(new AvioBridge());
    self->src_ = src;

    AVIOContext* ctx = avio_alloc_context(buf, sz, /*write_flag=*/0, self.get(),
                                          &AvioBridge::on_read,
                                          /*write_packet=*/nullptr,
                                          &AvioBridge::on_seek);
    if (ctx == nullptr) {
        av_free(buf);
        return nullptr;
    }

    // 总长已知才声明可 seek；否则 mov demuxer 会按不可 seek 退化。
    ctx->seekable = (syp_source_length(src) >= 0) ? AVIO_SEEKABLE_NORMAL : 0;

    self->ctx_ = ctx;
    return self;
}

AvioBridge::~AvioBridge() {
    if (ctx_ != nullptr) {
        // avio_context_free 不释放 buffer，且 FFmpeg 可能中途 av_realloc 过它。
        // 必须读**当前的** ctx_->buffer 再放——存旧指针会 double free 或漏。
        unsigned char* buf = ctx_->buffer;
        avio_context_free(&ctx_);
        av_freep(&buf);
    }
}

AVIOInterruptCB AvioBridge::interrupt_cb() noexcept {
    return AVIOInterruptCB{&AvioBridge::on_interrupt, this};
}

void AvioBridge::request_abort() noexcept {
    abort_.store(true, std::memory_order_release);
    // 只置标志不够：read 可能正阻塞在等数据上，interrupt_callback 不会被轮询到。
    if (src_ != nullptr) syp_source_interrupt(src_);
}

AvioBridge::Diag AvioBridge::diag() const noexcept {
    return Diag{reads_.load(std::memory_order_relaxed),
                seeks_.load(std::memory_order_relaxed),
                last_averror_.load(std::memory_order_relaxed),
                max_read_end_.load(std::memory_order_relaxed)};
}

}  // namespace syp::media
