// packet_digest.h — 把一次 demux 过程压成可比对的摘要序列。
//
// 差分比对的参照物是 FFmpeg 自己：同一份 mp4，一条走 file: 协议，
// 一条走 syp_source，逐 packet 比对。payload 也比——只比元数据的话，
// 「字节错位但长度正确」这类错误会漏掉，而这恰恰是 dl 层最可能出的锅。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/demuxer.h"

extern "C" {
#include <libavformat/avio.h>
}

namespace syp::probe {

struct PacketDigest {
    int32_t stream_index = 0;
    int64_t pts = 0;          // AV_NOPTS_VALUE 原样保留
    int64_t dts = 0;
    int64_t pos = 0;          // packet 在文件中的字节偏移。下载层把字节送错位时，
                               // 这是几乎免费、信噪比很高的指纹（同一文件两条路径应当相同）。
    int32_t size = 0;
    int32_t flags = 0;
    int64_t duration = 0;     // AVPacket::duration，同 pts/dts 一样是解封装出的容器
                               // 层元数据，不受走哪条 I/O 路径影响，纳入摘要零风险。
                               // 注意：pkt->side_data 有意不纳入摘要。
    uint8_t payload_sha256[32] = {};

    bool operator==(const PacketDigest&) const = default;
};

struct StreamSummary {
    int32_t index = 0;
    int32_t codec_id = 0;     // AVCodecID 的整数值
    int32_t width = 0;        // 音频为 0
    int32_t height = 0;

    bool operator==(const StreamSummary&) const = default;
};

struct DemuxResult {
    std::vector<StreamSummary> streams;
    std::vector<PacketDigest>  packets;
    int64_t     duration_us = 0;
    int         averror = 0;      // 0 或 AVERROR_EOF 视为正常结束
    std::string error_stage;      // 非空表示在哪一步失败了，便于定位
};

// probesize / analyzeduration 必须显式给定，且两条路径用同一组值。
// 不钉死的话 avformat_find_stream_info 的探测深度可能不同，比对会假红。
// 默认值定义在 syp::media::kDefaultProbeSize / kDefaultMaxAnalyzeDurationUs
// （src/media/demuxer.h）——单一来源，不在这里另写一份，防止两处漂移。
struct DemuxOptions {
    int64_t probesize       = syp::media::kDefaultProbeSize;
    int64_t analyzeduration = syp::media::kDefaultMaxAnalyzeDurationUs;   // 微秒
    // -1 = 读到 EOF。**契约：调用方传入非空 seeks 时，max_packets 被忽略**——
    // run_demux()（packet_digest.cpp）里两条路径互斥：seeks 为空走
    // read_n(max_packets) 的顺序限量读；seeks 非空则改成按 seek 计划逐点
    // 读（每个 SeekPlan::packets_after 决定该点读多少个包），max_packets
    // 本身不再参与判定。
    int64_t max_packets     = -1;
};

// seek 到 ts_us（AV_TIME_BASE 即微秒），再读 packets_after 个包。
struct SeekPlan {
    int64_t ts_us = 0;
    int32_t packets_after = 0;
};

// 参照路径：走 file: 协议读本地文件。
DemuxResult demux_file(const std::string& path,
                       const DemuxOptions& opt,
                       const std::vector<SeekPlan>& seeks);

// 被测路径：走调用方给的 AVIOContext。ctx 的生命周期由调用方持有。
// interrupt 挂到 AVFormatContext::interrupt_callback，看门狗靠它打断阻塞的 read。
DemuxResult demux_avio(AVIOContext* ctx,
                       const DemuxOptions& opt,
                       const std::vector<SeekPlan>& seeks,
                       AVIOInterruptCB interrupt);

// 返回空串表示全等；否则是人可读的第一处差异描述。
std::string diff_report(const DemuxResult& expected, const DemuxResult& actual);

}  // namespace syp::probe
