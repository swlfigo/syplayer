// test_frame_digest.cpp — frame_digest：packet_digest 在解码维度的延伸。
//
// 用例列表：
//   1. video_hash_is_stride_aware —— 同一份像素、不同 stride，哈希必须相同；
//      顺带验证 pts_us/duration_us 真的按每帧记录（不是恒为某个常量）。
//   2. video_hash_covers_every_plane —— 亮度、两个色度平面各翻一个字节都
//      必须让哈希变化，堵死「只哈希了平面 0」这类退化实现。
//   3. reference_decode_is_deterministic —— 参照路径读两遍必须完全一致，
//      且顺带堵死「两边都没解出帧所以相等」这条自证恒真的逃生门
//      （REQUIRE 非空 frames）。
//   4. diff_frames_catches_one_flipped_pixel —— 真改一个像素，diff 必须
//      非空且差异落在 data_sha256 上。
//   5. diff_frames_rejects_when_reference_failed —— 沿用 diff_report 的
//      硬底线：参照失败即非空，包括「两边报的是同一个失败」以及「两边
//      都是零帧」这两种最容易被误判成"相等"的情形（前者是 packet
//      层踩过的真实缺陷，后者是同型漏网——之前只测了
//      "参照零帧、被测非零帧"，那种情形靠"帧数量不同"分支就能接住，
//      根本没测到"零帧硬底线"本身）。
//   6. digest_of_frame_reports_failure_for_unsupported_pix_fmt /
//      _for_degenerate_metadata —— 不支持的 pix_fmt（PAL8、将来
//      的 VideoToolbox 硬解格式）或非法宽高必须报失败，不能
//      sha256_of(nullptr,0,out) 静默退化——那样两条路径会在同一个不支持
//      的格式上产出同一个假摘要，diff_frames 永远判"相等"。
//   7. audio_hash_covers_every_channel / audio_hash_covers_packed_format ——
//      本项目素材曾经只有单声道音轨，导致 hash_audio_frame 里
//      「按声道拼接多个 planar 平面」的循环从未真正跑过 channels>1 的
//      迭代、packed 分支整体是死代码——一条把整个音频哈希函数替换成常量
//      填充的变异体因此在真实素材上 5/5 全绿地漏网。这两条用合成帧直接
//      覆盖 planar 多声道与 packed 两条路径。
//   8. pipeline_decode_matches_reference —— 真正的交付物：Pipeline
//      解出来的帧摘要序列，跟 FFmpeg 直接解码的参照摘要序列，在真实素材
//      （现在音轨是立体声）上逐帧全等。没有这条，前面几条只证明了"摘要
//      算法本身没写错"，没有证明"管线解码是对的"——而这正是后续全部
//      场景用例要站上去的地基。
#include "frame_digest.h"
#include "frame_digest_internal.h"
#include "media/pipeline.h"
#include "tiny_test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

using namespace syp::probe;
using namespace syp::media;

namespace {

std::string fixture(const char* name) {
    const char* d = std::getenv("SYP_FIXTURE_DIR");
    return std::string(d ? d : "") + "/" + name;
}

// 造一个 16x16 yuv420p 的合成帧：valid 区域按 (plane, y, x) 填一个确定性
// 图案（不是纯色！纯色填充测不出「按行剥」跟「整块拷贝」的区别——两者在
// 纯色输入上碰巧总是相等），align 决定 av_frame_get_buffer 的对齐、进而
// 决定 linesize 是否带 padding；padding 区域故意填一个跟 valid 区域图案
// 不同的字节，用来证明「两个 align 不同、padding 内容也不同」的帧，
// 摘要必须依然相等——如果实现漏剥了 padding，这条会先暴露出来。
// pts/duration 由调用方显式传入（不是内部常量）：video_hash_is_stride_aware
// 靠这两个参数验证 digest_of_frame 真的按每帧记录了 pts_us/duration_us。
AVFrame* make_synthetic_yuv420p(int align, uint8_t seed, uint8_t pad_fill,
                                 int64_t pts, int64_t duration) {
    AVFrame* f = av_frame_alloc();
    f->format   = AV_PIX_FMT_YUV420P;
    f->width    = 16;
    f->height   = 16;
    f->pts      = pts;
    f->duration = duration;
    av_frame_get_buffer(f, align);
    for (int p = 0; p < 3; ++p) {
        const int pw = (p == 0) ? f->width  : f->width  / 2;
        const int ph = (p == 0) ? f->height : f->height / 2;
        for (int y = 0; y < ph; ++y) {
            uint8_t* row = f->data[p] + static_cast<ptrdiff_t>(y) * f->linesize[p];
            // padding 先整行填成 pad_fill，再把 valid 区域覆盖成图案——
            // 这样 valid 区域之后的字节（stride 带来的那部分）保留 pad_fill。
            std::memset(row, pad_fill, static_cast<size_t>(f->linesize[p]));
            for (int x = 0; x < pw; ++x) {
                row[x] = static_cast<uint8_t>(seed + p * 7 + y * 3 + x);
            }
        }
    }
    return f;
}

// 造一个合成音频帧：nb_samples 个采样、channels 个声道、指定采样格式。
// planar（如 AV_SAMPLE_FMT_FLTP）与 packed（如 AV_SAMPLE_FMT_S16）都按
// 各自的内存布局填一个确定性图案（跟视频合成帧同一个理由：不能用纯色，
// 否则测不出「拼对了声道」跟「只拼了一部分」的区别）。
// AV_NUM_DATA_POINTERS（8）是 AVFrame::data[] 的固定长度；channels 超过
// 这个数就要走 AVFrame::extended_data，本 helper 没有实现那条路径（现有
// 用例最多用到 2 声道）。这里构造 9 声道帧直接验证（`channels >
// AV_NUM_DATA_POINTERS` 判负）时，第一次照抄这个 helper 就在 `f->data[ch]`
// 越界写上真的段错误了——不是纸面风险。用运行期硬检查挡住，不用
// assert()：Release 构建下 -DNDEBUG 会让 assert 被优化掉，那样这颗地雷在
// Release 下依然埋着，谁下一次想加 5.1/7.1 用例还是会先炸一次再来查。
AVFrame* make_synthetic_audio_frame(AVSampleFormat fmt, int channels, int nb_samples,
                                     uint8_t seed) {
    if (channels > AV_NUM_DATA_POINTERS) {
        std::fprintf(stderr,
                      "make_synthetic_audio_frame: channels=%d 超过 "
                      "AV_NUM_DATA_POINTERS(%d)，本 helper 未实现 extended_data 路径\n",
                      channels, AV_NUM_DATA_POINTERS);
        std::abort();
    }
    AVFrame* f     = av_frame_alloc();
    f->format      = fmt;
    f->sample_rate = 48000;
    f->nb_samples  = nb_samples;
    av_channel_layout_default(&f->ch_layout, channels);
    av_frame_get_buffer(f, 0);

    const int bytes = av_get_bytes_per_sample(fmt);
    if (av_sample_fmt_is_planar(fmt)) {
        for (int ch = 0; ch < channels; ++ch) {
            uint8_t* p = f->data[ch];
            const int n = nb_samples * bytes;
            for (int i = 0; i < n; ++i) {
                p[i] = static_cast<uint8_t>(seed + ch * 11 + i);
            }
        }
    } else {
        uint8_t* p = f->data[0];
        const int n = nb_samples * channels * bytes;
        for (int i = 0; i < n; ++i) {
            p[i] = static_cast<uint8_t>(seed + i);
        }
    }
    return f;
}

}  // namespace

TEST_CASE(video_hash_is_stride_aware) {
    // align=1 与 align=64：linesize 应该明显不同，否则这条用例没有测到
    // 它声称要测的东西。padding 内容也故意设成不同的常量（0xAA vs 0x55），
    // 双重确认「stride 不同 + padding 字节也不同」时哈希依然相等。
    // pts/duration 也故意设成不同的值——不是为了让哈希不同（哈希只覆盖
    // 像素本体，不含 pts/duration），是为了顺带验证 digest_of_frame 真的
    // 把每帧自己的 pts_us/duration_us 记进了 FrameDigest（变异体
    // 「digest_of_frame 里 pts_us/duration_us 恒写 0」在没有这条断言之前
    // 会被漏过）。
    AVFrame* fa = make_synthetic_yuv420p(/*align=*/1,  /*seed=*/7, /*pad_fill=*/0xAA,
                                          /*pts=*/12345, /*duration=*/100);
    AVFrame* fb = make_synthetic_yuv420p(/*align=*/64, /*seed=*/7, /*pad_fill=*/0x55,
                                          /*pts=*/99999, /*duration=*/250);

    REQUIRE(fa->linesize[0] != fb->linesize[0]);   // 判别力自检：确实测到了不同 stride

    Frame a = Frame::from_av(fa, AVRational{1, 1000}, /*is_video=*/true);
    Frame b = Frame::from_av(fb, AVRational{1, 1000}, /*is_video=*/true);

    FrameDigest da{};
    FrameDigest db{};
    std::string err;
    REQUIRE(syp::probe::detail::digest_of_frame(a, /*track_index=*/0, &da, &err));
    REQUIRE(syp::probe::detail::digest_of_frame(b, /*track_index=*/0, &db, &err));

    CHECK(std::memcmp(da.data_sha256, db.data_sha256, 32) == 0);
    // 元数据本来就该相等，顺手确认一下不是靠元数据碰巧盖过了像素差异。
    CHECK_EQ(da.width, db.width);
    CHECK_EQ(da.height, db.height);
    CHECK_EQ(da.fmt, db.fmt);
    // time_base 是 {1, 1000}：换算成微秒要乘 1000。
    CHECK_EQ(da.pts_us, int64_t{12345000});
    CHECK_EQ(db.pts_us, int64_t{99999000});
    CHECK_EQ(da.duration_us, int64_t{100000});
    CHECK_EQ(db.duration_us, int64_t{250000});
}

TEST_CASE(video_hash_covers_every_plane) {
    // diff_frames_catches_one_flipped_pixel 只改了亮度平面
    // （plane 0），一条「只哈希了 plane 0，色度平面整体跳过」的变异体能
    // 靠那一条独自存活。这里对 plane ∈ {0,1,2} 各自翻一个字节，逐个要求
    // 摘要变化。
    for (int p = 0; p < 3; ++p) {
        AVFrame* fa = make_synthetic_yuv420p(/*align=*/32, /*seed=*/5, /*pad_fill=*/0,
                                              /*pts=*/1, /*duration=*/1);
        AVFrame* fb = make_synthetic_yuv420p(/*align=*/32, /*seed=*/5, /*pad_fill=*/0,
                                              /*pts=*/1, /*duration=*/1);
        fb->data[p][0] = static_cast<uint8_t>(fb->data[p][0] ^ 0xFF);

        Frame a = Frame::from_av(fa, AVRational{1, 1000}, /*is_video=*/true);
        Frame b = Frame::from_av(fb, AVRational{1, 1000}, /*is_video=*/true);

        FrameDigest da{};
        FrameDigest db{};
        std::string err;
        REQUIRE(syp::probe::detail::digest_of_frame(a, 0, &da, &err));
        REQUIRE(syp::probe::detail::digest_of_frame(b, 0, &db, &err));
        CHECK(std::memcmp(da.data_sha256, db.data_sha256, 32) != 0);
    }
}

TEST_CASE(digest_of_frame_reports_failure_for_unsupported_pix_fmt) {
    // PAL8 这类格式在 align=1 下 av_image_alloc 会失败（
    // 实测 FFmpeg 8.1.2：rc=-22 "Formats with a palette require a
    // minimum alignment of 4"）；硬件专用格式（如将来会引入的
    // VideoToolbox 像素格式）也会在同一条路径上失败。digest_of_frame
    // 必须把这种情况报成失败，不能 sha256_of(nullptr,0,out) 静默退化成
    // 一个「摘要为空」的假结果——那样两条路径会在同一个不支持的格式上
    // 都产出同一个假摘要，diff_frames 因此永远判「相等」（这正是本项目
    // 原先的实现，用真实探针实测出来的洞）。
    //
    // 不需要真正分配 PAL8 buffer（那需要额外处理调色板数据）：
    // av_image_alloc 在 align=1 那一步就已经失败，根本不会走到需要读
    // data[] 的 av_image_copy，所以 data 留空也能测到这条失败路径。
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_PAL8;
    f->width  = 16;
    f->height = 16;

    Frame frame = Frame::from_av(f, AVRational{1, 1000}, /*is_video=*/true);

    FrameDigest d{};
    std::string err;
    CHECK(!syp::probe::detail::digest_of_frame(frame, 0, &d, &err));
    CHECK(!err.empty());
}

TEST_CASE(digest_of_frame_reports_failure_for_degenerate_metadata) {
    // width<=0 同样必须报失败，不能悄悄退化——覆盖 hash_video_frame 里
    // 另一处不依赖具体 pix_fmt 的早退分支。
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P;
    f->width  = 0;
    f->height = 16;

    Frame frame = Frame::from_av(f, AVRational{1, 1000}, /*is_video=*/true);

    FrameDigest d{};
    std::string err;
    CHECK(!syp::probe::detail::digest_of_frame(frame, 0, &d, &err));
    CHECK(!err.empty());
}

TEST_CASE(audio_hash_covers_every_channel) {
    // 本项目素材曾经只有单声道音轨，planar（FLTP）音频哈希里
    // 「逐声道拼接」的循环因此从未真正跑过 channels>1 的迭代——一条把
    // hash_audio_frame 整体换成常量填充的变异体在那份素材上 5/5 全绿。
    // 这里用 2 声道 FLTP 合成帧，逐声道各翻一个 sample，要求每次都变化。
    constexpr int kChannels = 2;
    constexpr int kSamples  = 256;
    for (int ch = 0; ch < kChannels; ++ch) {
        AVFrame* fa = make_synthetic_audio_frame(AV_SAMPLE_FMT_FLTP, kChannels, kSamples, 11);
        AVFrame* fb = make_synthetic_audio_frame(AV_SAMPLE_FMT_FLTP, kChannels, kSamples, 11);
        fb->data[ch][0] = static_cast<uint8_t>(fb->data[ch][0] ^ 0xFF);

        Frame a = Frame::from_av(fa, AVRational{1, 48000}, /*is_video=*/false);
        Frame b = Frame::from_av(fb, AVRational{1, 48000}, /*is_video=*/false);

        FrameDigest da{};
        FrameDigest db{};
        std::string err;
        REQUIRE(syp::probe::detail::digest_of_frame(a, 1, &da, &err));
        REQUIRE(syp::probe::detail::digest_of_frame(b, 1, &db, &err));
        CHECK(std::memcmp(da.data_sha256, db.data_sha256, 32) != 0);
    }
}

TEST_CASE(audio_hash_covers_packed_format) {
    // packed（AV_SAMPLE_FMT_S16）：所有声道交织进 data[0]，覆盖
    // hash_audio_frame 里 `!av_sample_fmt_is_planar` 那条分支——真实素材
    // 走的是 FFmpeg AAC 解码器，输出恒为 planar FLTP，packed 分支在
    // pipeline_decode_matches_reference 这类端到端用例上永远不会被跑到，
    // 只能靠合成帧直接覆盖。翻的字节特意放在整段数据的最后一个字节，
    // 证明 nb_samples×channels×bytes_per_sample 这一整段都真的参与了
    // 哈希，不是只哈希了前面一部分。
    constexpr int kChannels = 2;
    constexpr int kSamples  = 256;
    AVFrame* fa = make_synthetic_audio_frame(AV_SAMPLE_FMT_S16, kChannels, kSamples, 21);
    AVFrame* fb = make_synthetic_audio_frame(AV_SAMPLE_FMT_S16, kChannels, kSamples, 21);
    const int bytes = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    const int last  = kSamples * kChannels * bytes - 1;
    fb->data[0][last] = static_cast<uint8_t>(fb->data[0][last] ^ 0xFF);

    Frame a = Frame::from_av(fa, AVRational{1, 48000}, /*is_video=*/false);
    Frame b = Frame::from_av(fb, AVRational{1, 48000}, /*is_video=*/false);

    FrameDigest da{};
    FrameDigest db{};
    std::string err;
    REQUIRE(syp::probe::detail::digest_of_frame(a, 1, &da, &err));
    REQUIRE(syp::probe::detail::digest_of_frame(b, 1, &db, &err));
    CHECK(std::memcmp(da.data_sha256, db.data_sha256, 32) != 0);
}

TEST_CASE(reference_decode_is_deterministic) {
    DecodeOptions opt;
    DecodeResult r1 = decode_reference(fixture("faststart.mp4"), opt);
    DecodeResult r2 = decode_reference(fixture("faststart.mp4"), opt);

    REQUIRE(r1.error_stage.empty());
    REQUIRE(r2.error_stage.empty());
    // 硬底线自证：不能是「两边都没解出帧所以相等」——那样下面的
    // r1.frames == r2.frames 会对着两个空 vector 恒真，整条差分比对就没
    // 有意义。必须先证明真的解出了东西。
    REQUIRE(!r1.frames.empty());
    REQUIRE(!r2.frames.empty());

    CHECK(r1.frames == r2.frames);
    CHECK_EQ(r1.skipped_packets, r2.skipped_packets);
    CHECK_EQ(r1.skipped_packets, int64_t{0});   // 素材未损坏，不应该有坏包
    CHECK_EQ(r1.averror, r2.averror);

    // 判别力自检：真的同时测到了视频轨与音频轨（不是只测到一种媒体类型），
    // 且音频轨真的是多声道（gen-fixtures.sh 现在产出立体声）——否则上面
    // 的相等比较可能只覆盖了退化的一半，且音频哈希的「逐声道拼接」分支
    // 根本没被这份素材跑到。
    bool saw_video = false, saw_audio_multi_channel = false;
    for (const FrameDigest& d : r1.frames) {
        if (d.width > 0) saw_video = true;
        if (d.nb_samples > 0 && d.channels > 1) saw_audio_multi_channel = true;
    }
    CHECK(saw_video);
    CHECK(saw_audio_multi_channel);
}

TEST_CASE(diff_frames_catches_one_flipped_pixel) {
    AVFrame* fa = make_synthetic_yuv420p(/*align=*/32, /*seed=*/3, /*pad_fill=*/0,
                                          /*pts=*/1, /*duration=*/1);
    AVFrame* fb = make_synthetic_yuv420p(/*align=*/32, /*seed=*/3, /*pad_fill=*/0,
                                          /*pts=*/1, /*duration=*/1);
    // 真改一个像素：亮度平面第 0 行第 0 列。
    fb->data[0][0] = static_cast<uint8_t>(fb->data[0][0] ^ 0xFF);

    Frame a = Frame::from_av(fa, AVRational{1, 1000}, /*is_video=*/true);
    Frame b = Frame::from_av(fb, AVRational{1, 1000}, /*is_video=*/true);

    DecodeResult expected;
    DecodeResult actual;
    FrameDigest de{};
    FrameDigest da{};
    std::string err;
    REQUIRE(syp::probe::detail::digest_of_frame(a, 0, &de, &err));
    REQUIRE(syp::probe::detail::digest_of_frame(b, 0, &da, &err));
    expected.frames.push_back(de);
    actual.frames.push_back(da);

    // 改动之前先确认「除了像素之外别的字段全一样」——不然下面 diff 非空
    // 说明不了问题到底出在哪个字段。
    REQUIRE(expected.frames[0].pts_us == actual.frames[0].pts_us);
    REQUIRE(expected.frames[0].width == actual.frames[0].width);
    REQUIRE(expected.frames[0].height == actual.frames[0].height);
    REQUIRE(expected.frames[0].fmt == actual.frames[0].fmt);
    REQUIRE(std::memcmp(expected.frames[0].data_sha256, actual.frames[0].data_sha256, 32) != 0);

    const std::string diff = diff_frames(expected, actual);
    CHECK(!diff.empty());
    // 差异必须落在 sha 上——pts/size 那些字段本来就相等，diff_report 的
    // 逐字段比较不会在它们身上报出差异。
    CHECK(diff.find("sha=") != std::string::npos);
}

TEST_CASE(diff_frames_rejects_when_reference_failed) {
    DecodeResult ok;
    ok.frames.push_back(FrameDigest{});

    DecodeResult failed_a;
    failed_a.error_stage = "模拟失败: open_input";
    failed_a.averror     = -5;

    // 参照失败、被测正常 —— 必须非空。
    CHECK(!diff_frames(failed_a, ok).empty());

    // 真正要堵的缺陷：参照失败、被测报的是*同一个*失败
    // （error_stage/averror 逐字段相等）。如果 diff_frames 图省事先判
    // 「两边都失败就当相等」，这条会悄悄放过——必须仍然非空，因为
    // 「比对没有意义」这件事跟"两边失败得是否一样"无关。
    DecodeResult failed_b = failed_a;
    CHECK(!diff_frames(failed_a, failed_b).empty());

    // 被测失败、参照正常 —— 必须非空。
    CHECK(!diff_frames(ok, failed_a).empty());

    // 参照路径「成功」但一帧都没解出来、被测有帧 —— 这次比对本身没测到
    // 任何东西，不能算通过（这条走的是"帧数量不同"分支也能接住，还不是
    // 真正的零帧硬底线自证）。
    DecodeResult empty_ok;   // error_stage 为空、frames 为空
    CHECK(!diff_frames(empty_ok, ok).empty());

    // 真正要堵的门是"两边都是零帧"——上面那条即便删掉
    // frame_digest.cpp 里 `if (e.frames.empty())` 那条硬底线，也会被
    // "帧数量不同"分支（0 vs 1）接住继续判非空，测不出硬底线本身是否
    // 存在。这条让两边帧数量也相等（都是 0），只有专门的零帧硬底线才能
    // 接住它——删掉那条硬底线，这条必须变红。
    CHECK(!diff_frames(empty_ok, DecodeResult{}).empty());
}

TEST_CASE(diff_frames_catches_skipped_packets_mismatch) {
    // frame_digest.cpp::diff_frames() 末尾
    // `if (e.skipped_packets != a.skipped_packets)` 这条分支删掉后
    // 完整 ctest 16/16 依然全绿——测试层对它的覆盖全部是间接的（场景 F
    // 直接断言 got.skipped_packets 本身，不经过 diff_frames() 的这条
    // 分支），而它是 `syp_probe --decode --diff`（这条命令行路径没有
    // ctest）唯一会检出"两边帧内容逐位相同、但跳过坏包的数量不一样"
    // 这类差异的地方——两边都成功、帧数相同、逐帧 sha 也相同，只是
    // 参照路径一个坏包都没跳、被测路径悄悄多跳了几个，这种差异只有
    // skipped_packets 这一个字段能反映出来。
    DecodeResult expected;
    expected.frames.push_back(FrameDigest{});
    expected.skipped_packets = 0;

    DecodeResult actual = expected;   // 帧内容逐位相同（拷贝，不是重新构造）
    actual.skipped_packets = 3;       // 唯一的差异

    // 改动之前先确认「除了 skipped_packets 之外别的字段全一样」——不然
    // 下面 diff 非空说明不了问题到底出在哪个字段，也验证了这条用例真的
    // 没有踩中前面几条分支（error_stage/帧数量不同）。
    REQUIRE(expected.error_stage.empty());
    REQUIRE(actual.error_stage.empty());
    REQUIRE(expected.frames.size() == actual.frames.size());
    REQUIRE(expected.frames == actual.frames);
    REQUIRE(expected.skipped_packets != actual.skipped_packets);

    const std::string diff = diff_frames(expected, actual);
    CHECK(!diff.empty());
    CHECK(diff.find("skipped_packets") != std::string::npos);
}

TEST_CASE(pipeline_decode_matches_reference) {
    // 本任务的真正交付物：Pipeline 解出来的帧，跟 FFmpeg 直接解码的参照
    // 帧，在真实素材（含视频 H.264 + 音频 AAC 立体声两条轨）上逐帧全等。
    DecodeOptions opt;
    DecodeResult ref = decode_reference(fixture("faststart.mp4"), opt);
    REQUIRE(ref.error_stage.empty());
    REQUIRE(!ref.frames.empty());   // 同一份自证：不能靠「参照也是空的」通过

    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    DecodeResult got = decode_pipeline(*p, opt);

    const std::string diff = diff_frames(ref, got);
    CHECK_EQ(diff, std::string());
    CHECK_EQ(got.skipped_packets, int64_t{0});
}

int main() { return tiny_test_main(); }
