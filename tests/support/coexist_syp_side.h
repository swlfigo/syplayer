// coexist_syp_side.h — 冲突集成测试里"我们这一侧"的入口。
//
// 实现文件经 syp_media 编译、带 syp_ffmpeg_prefix.h，走的是 SYFFmpeg；本头
// 不暴露任何 FFmpeg 类型，接入方一侧的编译单元（不带映射头）可以放心包含。
#pragma once

#include <cstdint>

struct SypSideOpenResult {
    bool     ok     = false;
    int32_t  width  = 0;
    int32_t  height = 0;
    int32_t  video_tracks = 0;
    int32_t  packets_read = 0;
};

// 经 Demuxer 打开本地文件，取第一条视频轨尺寸，并读若干个包。
SypSideOpenResult syp_side_open_and_read(const char* path, int32_t max_packets);

// 我们这一侧调用 avformat_version() / avcodec_version()（经映射头落到
// SYFFmpeg）得到的值，以及编译期 LIBAVFORMAT_VERSION_INT / LIBAVCODEC_VERSION_INT。
unsigned syp_side_avformat_version();
unsigned syp_side_avcodec_version();
unsigned syp_side_expected_avformat_version();
unsigned syp_side_expected_avcodec_version();

// 我们这一侧做一次 av_malloc / av_free 往返，成功返回 true。
bool syp_side_malloc_roundtrip();
