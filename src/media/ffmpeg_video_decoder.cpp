#include "media/ffmpeg_video_decoder.h"

#include "media/frame.h"

extern "C" {
#include <libavutil/error.h>
}

namespace syp::media {

AVPixelFormat choose_hw_format(const AVPixelFormat* candidates, AVPixelFormat want) noexcept {
    if (candidates == nullptr) return AV_PIX_FMT_NONE;
    for (const AVPixelFormat* p = candidates; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == want) return want;
    }
    return AV_PIX_FMT_NONE;
}

AVPixelFormat FFmpegVideoDecoder::get_format_cb(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    // opaque 指向解码器本身（不是 backend）：拒绝要闩在实例上。FFmpeg 在 hwaccel
    // 初始化失败时也会回到这里再问一次（候选里去掉了失败的格式），同样闩住。
    auto* self = static_cast<FFmpegVideoDecoder*>(ctx->opaque);
    if (self == nullptr || self->backend_ == nullptr) return AV_PIX_FMT_NONE;
    const AVPixelFormat chosen = choose_hw_format(fmts, self->backend_->hw_pix_fmt());
    if (chosen == AV_PIX_FMT_NONE) self->hw_rejected_ = true;
    return chosen;
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() { close(); }

void FFmpegVideoDecoder::close() noexcept {
    if (ctx_ != nullptr) avcodec_free_context(&ctx_);
    hw_rejected_ = false;
    skip_nonref_ = false;
}

syp_status FFmpegVideoDecoder::open(const AVCodecParameters* par, AVRational time_base) {
    // 先关掉可能存在的旧状态，避免重复 open 半开——本类允许 open() 被
    // 重复调用（该轨终止后 Pipeline 若要重试会用到）。
    close();
    skipped_packets_ = 0;

    if (par == nullptr) return SYP_ERR_INVALID_ARG;
    // 接口契约（video_decoder.h）：codec_type 必须匹配本解码器的角色。
    // 不检查的话，Pipeline 传错轨（音频轨的 codecpar 给到这里）也能
    // open/send/receive 成功——底层解码器不关心「你把我当成视频解码器
    // 用」，只有 Frame::is_video() 硬编码为 true 会跟音频数据自相矛盾，
    // 且不会在这里报错，故障会推迟到更难定位的下游。
    if (par->codec_type != AVMEDIA_TYPE_VIDEO) return SYP_ERR_INVALID_ARG;

    const bool hw_mode = (mode_ == VideoDecodeMode::Hardware);
    if (hw_mode && (backend_ == nullptr || !backend_->supports(*par))) {
        return SYP_ERR_NOT_IMPLEMENTED;
    }

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (codec == nullptr) return SYP_ERR_NOT_IMPLEMENTED;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) return SYP_ERR_OOM;

    if (avcodec_parameters_to_context(ctx, par) < 0) {
        avcodec_free_context(&ctx);
        return SYP_ERR_INVALID_ARG;
    }
    // pkt_timebase 而不是 ctx->time_base：后者是编码器用的，解码方向靠
    // pkt_timebase 告诉解码器「送进来的包 pts 是什么单位」，输出帧的 pts
    // 才会落在同一单位上，Frame::from_av 才能用同一个 time_base 换算。
    ctx->pkt_timebase = time_base;

    if (hw_mode) {
        if (backend_->prepare(ctx, &codec) != SYP_OK) {
            avcodec_free_context(&ctx);
            return SYP_ERR_NOT_IMPLEMENTED;
        }
        ctx->opaque     = this;
        ctx->get_format = &FFmpegVideoDecoder::get_format_cb;
        // parameters_to_context 把 codecpar 的 yuv420p 抄进了
        // ctx->pix_fmt。H.264 首个 slice 上 hwaccel 初始化失败后，下一个 slice
        // 见 avctx->pix_fmt 仍在候选里就不再调 get_format（h264_slice.c
        // must_reinit），直接软解下去。置 NONE 迫使每次重新协商都经过回调。
        ctx->pix_fmt = AV_PIX_FMT_NONE;
    }

    // 显式钉死单线程解码，不靠 FFmpeg 的默认值裸奔。
    //
    // Pipeline 的整条确定性承诺（同一份输入 + 同一串 step() 调用 = 同一
    // 串输出，逐项比对验证过 6397/6397 全等）目前成立，是
    // 因为 avcodec_options_table.h 把 threads 的默认值定死成 1（不是
    // auto），导致 pthread.c 的 ff_thread_get_buffer/decode_init 一类
    // 路径里 active_thread_type 落到 0——单线程、无帧级流水线。这条
    // 「默认恰好是 1」的事实此前只是**隐式**依赖：一旦以后有人为性能
    // 把 thread_count 改成 0（auto）或某个 >1 的值（硬解切软解回退、
    // 或任何调优都可能踩），多线程解码器内部的帧完成顺序会变得跟调度器
    // 有关、跟机器有关，Kind/pts 序列就不再是「同一份输入必然同一串
    // 输出」——而整套差分比对全押在这条确定性上。这里把
    // 隐式依赖变成显式设置，且不管 FFmpeg 未来会不会改默认值，这条
    // 设置本身都不会失效。
    ctx->thread_count = 1;

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return SYP_ERR_IO;
    }

    ctx_       = ctx;
    time_base_ = time_base;
    return SYP_OK;
}

syp_status FFmpegVideoDecoder::send(const AVPacket* pkt) {
    if (ctx_ == nullptr) return SYP_ERR_INVALID_ARG;
    // 硬解被拒（闩）：不是"单包可跳过"，是该轨终止——否则 HEVC 会把每个包都
    // 计成 skipped、一路静默到 Eof。
    if (hw_rejected_) return SYP_ERR_NOT_IMPLEMENTED;

    const int rc = avcodec_send_packet(ctx_, pkt);
    // get_format 可能在 send 内部被调用并拒绝；FFmpeg 会把随之而来的 slice 错误
    // 吞掉（非 AV_EF_EXPLODE），所以以闩为准，不看 rc。
    if (hw_rejected_) return SYP_ERR_NOT_IMPLEMENTED;
    if (rc == 0) return SYP_OK;

    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
        // 契约违反，不是可跳过的坏包：调用方（Pipeline）本应保证每次
        // send() 前已经把 receive() 排空到 NeedInput/Eof。见
        // video_decoder.h 顶部「send()/receive() 的契约」。
        return SYP_ERR_INVALID_ARG;
    }

    // 单包可跳过：真实流里损坏/畸形包常见，不应终止整条解码——这是三级
    // 错误分类里最轻的一级，另两级（该轨终止/整体终止）由 Pipeline 处理。
    ++skipped_packets_;
    return SYP_OK;
}

IVideoDecoder::Receive FFmpegVideoDecoder::receive(Frame* out) {
    if (ctx_ == nullptr || out == nullptr) return Receive::Error;
    if (hw_rejected_) return Receive::Error;

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) return Receive::Error;

    const int rc = avcodec_receive_frame(ctx_, frame);
    if (hw_rejected_) {
        av_frame_free(&frame);
        return Receive::Error;
    }
    if (rc == 0) {
        // 输出闸（C1）：硬解模式只放行 backend 的硬件格式。独立于 FFmpeg 内部
        // 何时/是否调用 get_format——任何软件帧漏出来都当硬解失败处理。
        if (mode_ == VideoDecodeMode::Hardware && frame->format != backend_->hw_pix_fmt()) {
            av_frame_free(&frame);
            hw_rejected_ = true;
            return Receive::Error;
        }
        *out = Frame::from_av(frame, time_base_, /*is_video=*/true);
        return Receive::Frame;
    }

    av_frame_free(&frame);
    if (rc == AVERROR(EAGAIN)) return Receive::NeedInput;   // 需要更多输入
    if (rc == AVERROR_EOF) return Receive::Eof;              // 已排空
    return Receive::Error;
}

void FFmpegVideoDecoder::flush() {
    if (ctx_ != nullptr) avcodec_flush_buffers(ctx_);
}

void FFmpegVideoDecoder::set_skip_nonref(bool on) noexcept {
    skip_nonref_ = on;
    if (ctx_ != nullptr) ctx_->skip_frame = on ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

}  // namespace syp::media
