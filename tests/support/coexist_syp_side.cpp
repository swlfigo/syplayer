// coexist_syp_side.cpp — 冲突集成测试"我们这一侧"的实现，见同名头文件。
//
// 本文件经 syp_media 链到 syp_ffmpeg，编译时强制包含 syp_ffmpeg_prefix.h：
// 下面照写的 avformat_version / av_malloc / av_packet_free 等原名全部被改写
// 成 syp_ 版本，与 src/media 里的业务代码同一待遇。
#include "support/coexist_syp_side.h"

#include "media/demuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

SypSideOpenResult syp_side_open_and_read(const char* path, int32_t max_packets) {
    SypSideOpenResult r;
    syp_status err{};
    auto d = syp::media::Demuxer::open_file(path, &err);
    if (!d) return r;
    for (const auto& t : d->tracks()) {
        if (!t.is_video) continue;
        if (r.video_tracks == 0) {
            r.width  = t.width;
            r.height = t.height;
        }
        ++r.video_tracks;
    }
    for (int32_t i = 0; i < max_packets; ++i) {
        AVPacket* pkt   = nullptr;
        int32_t   track = -1;
        if (d->read(&pkt, &track) != syp::media::Demuxer::ReadResult::Packet) break;
        av_packet_free(&pkt);
        ++r.packets_read;
    }
    r.ok = true;
    return r;
}

unsigned syp_side_avformat_version() { return avformat_version(); }
unsigned syp_side_avcodec_version() { return avcodec_version(); }
unsigned syp_side_expected_avformat_version() { return LIBAVFORMAT_VERSION_INT; }
unsigned syp_side_expected_avcodec_version() { return LIBAVCODEC_VERSION_INT; }

bool syp_side_malloc_roundtrip() {
    void* p = av_malloc(64);
    if (!p) return false;
    av_free(p);
    return true;
}
