#include "media/ffmpeg_audio_decoder.h"

#include "media/frame.h"

extern "C" {
#include <libavutil/error.h>
}

namespace syp::media {

FFmpegAudioDecoder::~FFmpegAudioDecoder() { close(); }

void FFmpegAudioDecoder::close() noexcept {
    if (ctx_ != nullptr) avcodec_free_context(&ctx_);
}

syp_status FFmpegAudioDecoder::open(const AVCodecParameters* par, AVRational time_base) {
    close();
    skipped_packets_ = 0;

    if (par == nullptr) return SYP_ERR_INVALID_ARG;
    // 接口契约（与 IVideoDecoder::open() 相同，见 video_decoder.h）：
    // codec_type 必须匹配本解码器的角色，防止 Pipeline 传错轨。
    if (par->codec_type != AVMEDIA_TYPE_AUDIO) return SYP_ERR_INVALID_ARG;

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (codec == nullptr) return SYP_ERR_NOT_IMPLEMENTED;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) return SYP_ERR_OOM;

    if (avcodec_parameters_to_context(ctx, par) < 0) {
        avcodec_free_context(&ctx);
        return SYP_ERR_INVALID_ARG;
    }
    ctx->pkt_timebase = time_base;

    // 与 ffmpeg_video_decoder.cpp 对称地钉死单线程，理由见那边的长注释。
    // 这里不依赖「AAC 解码器本来就不做帧级多线程」这个判断——那需要读
    // FFmpeg 的 AAC 解码器源码才能确认，而本仓库没有 vendored 源码可查。
    // 一行显式设置的代价为零，比一个未经核实的假设便宜。
    ctx->thread_count = 1;

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return SYP_ERR_IO;
    }

    ctx_       = ctx;
    time_base_ = time_base;
    return SYP_OK;
}

syp_status FFmpegAudioDecoder::send(const AVPacket* pkt) {
    if (ctx_ == nullptr) return SYP_ERR_INVALID_ARG;

    const int rc = avcodec_send_packet(ctx_, pkt);
    if (rc == 0) return SYP_OK;

    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
        // 契约违反：见 video_decoder.h 顶部「send()/receive() 的契约」，
        // 音频与视频解码器遵守同一份契约。
        return SYP_ERR_INVALID_ARG;
    }

    ++skipped_packets_;
    return SYP_OK;
}

FFmpegAudioDecoder::Receive FFmpegAudioDecoder::receive(Frame* out) {
    if (ctx_ == nullptr || out == nullptr) return Receive::Error;

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) return Receive::Error;

    const int rc = avcodec_receive_frame(ctx_, frame);
    if (rc == 0) {
        *out = Frame::from_av(frame, time_base_, /*is_video=*/false);
        return Receive::Frame;
    }

    av_frame_free(&frame);
    if (rc == AVERROR(EAGAIN)) return Receive::NeedInput;
    if (rc == AVERROR_EOF) return Receive::Eof;
    return Receive::Error;
}

void FFmpegAudioDecoder::flush() {
    if (ctx_ != nullptr) avcodec_flush_buffers(ctx_);
}

}  // namespace syp::media
