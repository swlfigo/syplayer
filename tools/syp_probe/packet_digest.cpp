#include "packet_digest.h"

#include "packet_digest_internal.h"

#include <CommonCrypto/CommonDigest.h>

#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace syp::probe {

namespace detail {

// 从匿名 namespace 提出来、放进 packet_digest_internal.h 声明：好让单测能直接
// 灌已知向量进来验证「这真的是 SHA-256，且真的读了 data」，而不是只能通过
// 「素材恰好逐字节相同」这种间接方式绕着验。
void sha256_of(const uint8_t* data, int32_t len, uint8_t out[32]) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    if (len > 0 && data != nullptr) {
        CC_SHA256_Update(&c, data, static_cast<CC_LONG>(len));
    }
    CC_SHA256_Final(out, &c);
}

std::string hex32(const uint8_t (&v)[32]) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (int i = 0; i < 32; ++i) {
        s.push_back(kHex[v[i] >> 4]);
        s.push_back(kHex[v[i] & 0x0F]);
    }
    return s;
}

}  // namespace detail

namespace {

using detail::hex32;
using detail::sha256_of;

std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

// 两条路径共用的主体：fmt 已 open_input 成功，从这里开始找流、读包。
void run_demux(AVFormatContext* fmt, const DemuxOptions& opt,
               const std::vector<SeekPlan>& seeks, DemuxResult& out) {
    int rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        out.averror = rc;
        out.error_stage = "find_stream_info: " + av_err(rc);
        return;
    }

    out.duration_us = fmt->duration;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* p = fmt->streams[i]->codecpar;
        out.streams.push_back(StreamSummary{
            static_cast<int32_t>(i), static_cast<int32_t>(p->codec_id),
            p->width, p->height});
    }

    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) {
        out.averror = AVERROR(ENOMEM);
        out.error_stage = "av_packet_alloc";
        return;
    }

    // 返回 false 表示遇到真实读错误（非 EOF）：调用方必须立刻停止整个 demux，
    // 不能再跑后续 seek——否则第一现场的 error_stage 会被后面的操作覆盖，
    // 且错误发生后继续追加的 packet 会让 diff_report 报出的位置失去意义。
    auto read_n = [&](int64_t limit) -> bool {
        int64_t n = 0;
        while (limit < 0 || n < limit) {
            rc = av_read_frame(fmt, pkt);
            if (rc < 0) {
                if (rc != AVERROR_EOF) {
                    out.averror = rc;
                    out.error_stage = "av_read_frame: " + av_err(rc);
                    return false;
                }
                break;
            }
            PacketDigest d;
            d.stream_index = pkt->stream_index;
            d.pts = pkt->pts;
            d.dts = pkt->dts;
            d.pos = pkt->pos;
            d.size = pkt->size;
            d.flags = pkt->flags;
            d.duration = pkt->duration;
            sha256_of(pkt->data, pkt->size, d.payload_sha256);
            out.packets.push_back(d);
            av_packet_unref(pkt);
            ++n;
        }
        return true;
    };

    if (seeks.empty()) {
        read_n(opt.max_packets);
    } else {
        for (const SeekPlan& s : seeks) {
            // stream_index = -1 → ts 用 AV_TIME_BASE（微秒）
            rc = av_seek_frame(fmt, -1, s.ts_us, 0);
            if (rc < 0) {
                out.averror = rc;
                out.error_stage = "av_seek_frame(" + std::to_string(s.ts_us) +
                                  "): " + av_err(rc);
                break;
            }
            if (!read_n(s.packets_after)) break;
        }
    }

    av_packet_free(&pkt);
}

}  // namespace

DemuxResult demux_file(const std::string& path, const DemuxOptions& opt,
                       const std::vector<SeekPlan>& seeks) {
    DemuxResult out;
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        out.averror = AVERROR(ENOMEM);
        out.error_stage = "avformat_alloc_context";
        return out;
    }
    fmt->probesize = opt.probesize;
    fmt->max_analyze_duration = opt.analyzeduration;

    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        // 失败时 avformat_open_input 已经把 fmt 释放并置空，不要再 free。
        out.averror = rc;
        out.error_stage = "open_input: " + av_err(rc);
        return out;
    }
    run_demux(fmt, opt, seeks, out);
    avformat_close_input(&fmt);
    return out;
}

DemuxResult demux_avio(AVIOContext* ctx, const DemuxOptions& opt,
                       const std::vector<SeekPlan>& seeks,
                       AVIOInterruptCB interrupt) {
    DemuxResult out;
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        out.averror = AVERROR(ENOMEM);
        out.error_stage = "avformat_alloc_context";
        return out;
    }
    fmt->pb = ctx;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt->probesize = opt.probesize;
    fmt->max_analyze_duration = opt.analyzeduration;
    fmt->interrupt_callback = interrupt;

    int rc = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (rc < 0) {
        out.averror = rc;
        out.error_stage = "open_input: " + av_err(rc);
        return out;
    }
    run_demux(fmt, opt, seeks, out);
    // 不要在这里把 fmt->pb 置空：读过 FFmpeg 8.1.2 的 libavformat/demux.c 确认，
    // avformat_close_input 自己已经用 AVFMT_FLAG_CUSTOM_IO 判断——命中时只会把
    // 它*局部*的 pb 变量置空，既不 close 也不 free 调用方持有的 AVIOContext；
    // 而 read_close(s) 在这之前执行，某些 demuxer 的 read_close 依赖 s->pb 仍然
    // 有效。提前置空反而会在这些 demuxer 上出问题，纯属画蛇添足。
    avformat_close_input(&fmt);
    return out;
}

std::string diff_report(const DemuxResult& e, const DemuxResult& a) {
    // 「谁先失败」这段前置检查跟 frame_digest.cpp::diff_frames 共用，见
    // packet_digest_internal.h::diff_failure_hardlines 顶部注释。
    if (auto hardline = detail::diff_failure_hardlines(
            e.error_stage, e.averror, a.error_stage, a.averror)) {
        return *hardline;
    }
    // 硬底线：参照侧空就说明这次比对本身没测到任何东西，不能算通过。
    if (e.streams.empty()) {
        return "参照路径没有解析出任何流，比对无意义";
    }
    if (e.packets.empty()) {
        return "参照路径没有读到任何 packet，比对无意义";
    }

    std::ostringstream o;
    if (e.streams != a.streams) {
        for (size_t i = 0; i < e.streams.size() && i < a.streams.size(); ++i) {
            if (e.streams[i] == a.streams[i]) continue;
            const StreamSummary& x = e.streams[i];
            const StreamSummary& y = a.streams[i];
            o << "流信息不同: stream #" << i
              << " 参照 index=" << x.index << " codec_id=" << x.codec_id
              << " width=" << x.width << " height=" << x.height
              << " / 实际 index=" << y.index << " codec_id=" << y.codec_id
              << " width=" << y.width << " height=" << y.height;
            return o.str();
        }
        o << "流信息不同: 参照 " << e.streams.size() << " 条, 实际 " << a.streams.size() << " 条";
        return o.str();
    }
    if (e.duration_us != a.duration_us) {
        o << "时长不同: 参照 " << e.duration_us << "us, 实际 " << a.duration_us << "us";
        return o.str();
    }
    const size_t n = e.packets.size() < a.packets.size() ? e.packets.size() : a.packets.size();
    for (size_t i = 0; i < n; ++i) {
        if (e.packets[i] == a.packets[i]) continue;
        const PacketDigest& x = e.packets[i];
        const PacketDigest& y = a.packets[i];
        o << "packet #" << i << " 不同:\n"
          << "  参照 stream=" << x.stream_index << " pts=" << x.pts << " dts=" << x.dts
          << " pos=" << x.pos << " size=" << x.size << " flags=" << x.flags
          << " duration=" << x.duration
          << " sha=" << hex32(x.payload_sha256) << "\n"
          << "  实际 stream=" << y.stream_index << " pts=" << y.pts << " dts=" << y.dts
          << " pos=" << y.pos << " size=" << y.size << " flags=" << y.flags
          << " duration=" << y.duration
          << " sha=" << hex32(y.payload_sha256);
        return o.str();
    }
    if (e.packets.size() != a.packets.size()) {
        o << "packet 数量不同: 参照 " << e.packets.size() << ", 实际 " << a.packets.size()
          << "（前 " << n << " 个相同）";
        return o.str();
    }
    return {};
}

}  // namespace syp::probe
