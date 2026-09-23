// test_ffmpeg_coexist.cpp — 接入方自带一份 FFmpeg 时，与我们的 SYFFmpeg 共存。
//
// 场景：接入方的 App 里静态链了自己的 FFmpeg（这里用 fake_integrator_ffmpeg
// 代替，导出真实的 FFmpeg 函数名、返回可辨认的假值并记录调用），同时链了
// 我们的 syp_media + SYFFmpeg。SYFFmpeg 只导出 syp_ 前缀的名字、我们的代码
// 经映射头只引用 syp_ 名字，所以两边应当各用各的：
//   ① 链接成功，没有 duplicate symbol（构建这一步本身就是断言）；
//   ② 接入方直接调 avformat_version() 等，拿到的是它自己那份的假值；
//   ③ 我们经 Demuxer 打开样片成功，尺寸 640x360，还能读出包；
//   ④ 假库记到的调用全部来自接入方自己，我们这一侧一次都没调进去。
//
// 同一份目标文件按五种方式各链一次（静态假库：SYFFmpeg 在前、假库在前、
// -Wl,-all_load；动态假库：SYFFmpeg 在前、假库在前），见 tests/CMakeLists.txt。
//
// 本文件代表接入方的代码：**不得**带 syp_ffmpeg_prefix.h，也不包含任何
// FFmpeg 头——它编进单独的 OBJECT 库，不链 syp_media。若它被误加上映射头，
// 下面的 avformat_version() 会被改写成 syp_avformat_version，② 立刻变红。
#include "support/coexist_syp_side.h"
#include "support/fake_integrator_ffmpeg.h"
#include "tiny_test.h"

#include <cstdio>

#ifndef SYP_SAMPLE_MP4_PATH
#error "SYP_SAMPLE_MP4_PATH 未定义"
#endif

TEST_CASE(integrator_calls_bind_to_its_own_ffmpeg) {
    fake_ffmpeg_reset_counts();

    CHECK_EQ(avformat_version(), kFakeAvformatVersion);
    CHECK_EQ(avcodec_version(), kFakeAvcodecVersion);

    av_log_set_level(7);
    CHECK_EQ(av_log_get_level(), 7);

    void* p = av_malloc(16);
    CHECK(p != nullptr);
    av_free(p);

    // ctx 从 nullptr 出发：万一串用到真 FFmpeg，它会正常开文件而不是崩溃，
    // 失败落在下面的断言上，能看清是哪一条。
    void* ctx = nullptr;
    CHECK_EQ(avformat_open_input(&ctx, SYP_SAMPLE_MP4_PATH, nullptr, nullptr),
             kFakeOpenInputResult);
    CHECK(ctx == nullptr);

    CHECK_EQ(fake_ffmpeg_call_count("avformat_version"), 1);
    CHECK_EQ(fake_ffmpeg_call_count("avcodec_version"), 1);
    CHECK_EQ(fake_ffmpeg_call_count("av_log_set_level"), 1);
    CHECK_EQ(fake_ffmpeg_call_count("av_log_get_level"), 1);
    CHECK_EQ(fake_ffmpeg_call_count("av_malloc"), 1);
    CHECK_EQ(fake_ffmpeg_call_count("av_free"), 1);
    CHECK_EQ(fake_ffmpeg_call_count("avformat_open_input"), 1);
    CHECK_EQ(fake_ffmpeg_total_calls(), 7);
}

TEST_CASE(syp_side_uses_syffmpeg_and_never_reaches_integrator_ffmpeg) {
    fake_ffmpeg_reset_counts();

    const SypSideOpenResult r = syp_side_open_and_read(SYP_SAMPLE_MP4_PATH, 32);
    REQUIRE(r.ok);
    CHECK_EQ(r.video_tracks, 1);
    CHECK_EQ(r.width, 640);
    CHECK_EQ(r.height, 360);
    CHECK_EQ(r.packets_read, 32);

    // 我们这一侧的 avformat_version() 落到 SYFFmpeg：等于编译期头文件的版本，
    // 不是假库的值。
    CHECK_EQ(syp_side_avformat_version(), syp_side_expected_avformat_version());
    CHECK_EQ(syp_side_avcodec_version(), syp_side_expected_avcodec_version());
    CHECK(syp_side_avformat_version() != kFakeAvformatVersion);
    CHECK(syp_side_malloc_roundtrip());

    CHECK_EQ(fake_ffmpeg_call_count("avformat_open_input"), 0);
    CHECK_EQ(fake_ffmpeg_call_count("avformat_find_stream_info"), 0);
    CHECK_EQ(fake_ffmpeg_call_count("av_read_frame"), 0);
    CHECK_EQ(fake_ffmpeg_call_count("avcodec_find_decoder"), 0);
    CHECK_EQ(fake_ffmpeg_call_count("av_malloc"), 0);
    CHECK_EQ(fake_ffmpeg_call_count("av_free"), 0);
    CHECK_EQ(fake_ffmpeg_call_count("avformat_version"), 0);
    CHECK_EQ(fake_ffmpeg_total_calls(), 0);
}

// 两边交替使用：我们打开过样片之后，接入方的调用仍然落在它自己那份上。
TEST_CASE(interleaved_use_keeps_both_sides_apart) {
    fake_ffmpeg_reset_counts();

    CHECK_EQ(avformat_version(), kFakeAvformatVersion);
    const SypSideOpenResult r = syp_side_open_and_read(SYP_SAMPLE_MP4_PATH, 4);
    CHECK(r.ok);
    CHECK_EQ(avformat_version(), kFakeAvformatVersion);
    CHECK_EQ(syp_side_avformat_version(), syp_side_expected_avformat_version());

    CHECK_EQ(fake_ffmpeg_call_count("avformat_version"), 2);
    CHECK_EQ(fake_ffmpeg_total_calls(), 2);
}

int main() {
    std::printf("integrator avformat_version=0x%X  syp avformat_version=0x%X\n",
                avformat_version(), syp_side_avformat_version());
    return tiny_test_main();
}
