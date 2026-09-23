// test_demuxer.cpp — Demuxer 与 Track 元信息。
// 素材来自 SYP_FIXTURE_DIR（gen-fixtures.sh 产出），与 probe_e2e 共用。
#include "media/demuxer.h"
#include "media/video_geometry.h"
#include "support/synth_media.h"
#include "tiny_test.h"

extern "C" {
#include <libavutil/display.h>
}

#include <cmath>
#include <cstdlib>
#include <string>

using namespace syp::media;

namespace {
std::string fixture(const char* name) {
    const char* d = std::getenv("SYP_FIXTURE_DIR");
    return std::string(d ? d : "") + "/" + name;
}

// 读 TrackInfo 的一行 helper：拿第一条视频轨，没有就返回 nullptr。
const TrackInfo* first_video_track(const Demuxer& d) {
    for (const TrackInfo& t : d.tracks()) {
        if (t.is_video) return &t;
    }
    return nullptr;
}

// 独立于 build_tracks() 内部的符号换算，直接从已打开的 Demuxer 里再读一遍
// displaymatrix 的原始 av_display_rotation_get() 值——用来在用例里做模 360
// 的关系断言。
//
// 【为什么必须独立读一遍】真实发生过"生产代码的符号换算和
// 用例的期望值一起写反、于是巧合地绿了"：build_tracks() 曾经错误地不取负，
// 用例的 CHECK_EQ 也写成同一个（错误推导出的）数字，两处同时错，测试照样
// 全绿。如果这里的关系断言也去调用生产代码路径（比如再跑一遍
// Demuxer::open_file 然后比较两次 rotation_deg），完全测不出这类"两处一起
// 错"的问题——所以这个 helper 绕开 build_tracks()，直接从
// AVFormatContext::streams[i]->codecpar 拿原始 side data 再算一遍。
// 没有侧数据（调用方传错 track_index，或素材本身没打旋转标签）时返回
// NaN，调用方用 std::isnan 判断。
double raw_display_rotation(const Demuxer& d, int32_t track_index) {
    const auto&              streams = d.raw()->streams;
    const AVCodecParameters* p       = streams[static_cast<unsigned>(track_index)]->codecpar;
    const AVPacketSideData*  sd      = av_packet_side_data_get(
        p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd == nullptr) return std::nan("");
    return av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
}

// raw（av_display_rotation_get() 的原始返回值，逆时针角）与 rotation_deg
// （顺时针校正角）的关系必须是 raw + rotation_deg ≡ 0 (mod 360)——两者互为
// 相反数（模 360 意义下）。四舍五入到最近的整数角再取模，容许浮点误差。
bool rotation_relation_holds(double raw, int32_t rotation_deg) {
    const auto raw_rounded = static_cast<int32_t>(std::lround(raw));
    const int32_t sum_mod  = ((raw_rounded % 360) + (rotation_deg % 360) + 360) % 360;
    return sum_mod == 0;
}
}  // namespace

// normalize_duration 是纯函数，脱离素材测——AV_NOPTS_VALUE 这条分支的真实
// 触发场景（直播源、chunked 传输、长度未知的流）做不成 SYP_FIXTURE_DIR 里的
// fixture，此前尝试构造过三次，都没能成功搭出可用素材。
TEST_CASE(normalize_duration_handles_nopts) {
    CHECK_EQ(normalize_duration(AV_NOPTS_VALUE), int64_t{0});
    CHECK_EQ(normalize_duration(-1), int64_t{0});
    CHECK_EQ(normalize_duration(0), int64_t{0});
    CHECK_EQ(normalize_duration(123456), int64_t{123456});
}

TEST_CASE(demuxer_opens_and_lists_tracks) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    CHECK_EQ(err, SYP_OK);
    // gen-fixtures.sh 产出的素材恒为 h264 + aac 双轨
    REQUIRE(d->tracks().size() == 2);
    int video = 0, audio = 0;
    for (const TrackInfo& t : d->tracks()) {
        if (t.is_video) { ++video; CHECK(t.width > 0); CHECK(t.height > 0); }
        else            { ++audio; CHECK(t.sample_rate > 0); CHECK(t.channels > 0); }
        CHECK(t.time_base.den > 0);
        // 没有任何用例读过 t.duration_us：av_rescale_q 的两个 AVRational
        // 参数（st->time_base / {1,1000000}）写反也会四个用例全绿。下界
        // 加量级交叉核对（与格式级 duration_us() 应同一量级）一起钉住——
        // 参数写反会把结果甩出几个数量级，不会只是差个零头。
        CHECK(t.duration_us > 0);
        CHECK(t.duration_us > d->duration_us() / 2);
        CHECK(t.duration_us < d->duration_us() * 2);
    }
    CHECK_EQ(video, 1);
    CHECK_EQ(audio, 1);
    CHECK(d->duration_us() > 0);
}

TEST_CASE(demuxer_reads_packets_to_eof) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);

    int64_t n = 0;
    for (;;) {
        AVPacket* p = nullptr;
        int32_t   ti = -1;
        auto r = d->read(&p, &ti);
        if (r == Demuxer::ReadResult::Eof) break;
        REQUIRE(r == Demuxer::ReadResult::Packet);
        REQUIRE(p != nullptr);
        CHECK(ti >= 0);
        // track_index 必须就是 pkt->stream_index——ti 恒为 0 这种错误不会
        // 被上面那条 CHECK(ti >= 0) 发现。
        CHECK_EQ(ti, p->stream_index);
        av_packet_free(&p);
        ++n;
    }
    CHECK(n > 100);
}

TEST_CASE(demuxer_open_missing_file_reports_error) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file("/nonexistent/nope.mp4", &err);
    CHECK(d == nullptr);
    CHECK(err != SYP_OK);
}

TEST_CASE(demuxer_seek_lands_at_or_before_target) {
    // AVSEEK_FLAG_BACKWARD：落在最近的前一个关键帧，所以首帧 pts <= 目标。
    // 这条语义要被断言钉住 —— 后续会依赖它决定丢弃到目标还是从关键帧起播。
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);

    // 在还没调过 seek() 的实例上先读一次，记下起始帧 pts
    // 当基线。如果 seek() 是空操作（例如直接 return SYP_OK、根本不调
    // av_seek_frame），读指针仍停在文件开头——mp4 首个视频包几乎必然是
    // IDR 帧、pts 接近 0，下面 "pts_us <= target" 与 "首个视频包带
    // AV_PKT_FLAG_KEY" 两条断言会双双恒真，测不出 seek() 完全不生效。
    AVPacket* first    = nullptr;
    int32_t   first_ti = -1;
    REQUIRE(d->read(&first, &first_ti) == Demuxer::ReadResult::Packet);
    REQUIRE(first != nullptr);
    const AVRational first_tb = d->tracks()[static_cast<std::size_t>(first_ti)].time_base;
    const int64_t baseline_pts_us = av_rescale_q(first->pts, first_tb, AVRational{1, 1000000});
    av_packet_free(&first);

    const int64_t target = 5000000;   // 5 秒
    REQUIRE(d->seek(target) == SYP_OK);

    AVPacket* p = nullptr;
    int32_t   ti = -1;
    REQUIRE(d->read(&p, &ti) == Demuxer::ReadResult::Packet);
    REQUIRE(p != nullptr);
    const AVRational tb = d->tracks()[static_cast<std::size_t>(ti)].time_base;
    const int64_t pts_us = av_rescale_q(p->pts, tb, AVRational{1, 1000000});
    CHECK(pts_us <= target);
    // seek() 若不生效，pts_us 会停在 baseline_pts_us 附近（接近 0）；真正
    // seek 到 5s 落在关键帧后必然比起始帧明显靠后——直接戳穿「seek() 完全
    // 不生效」这种比 BACKWARD/ANY 语义差异更基础的错误。
    CHECK(pts_us > baseline_pts_us + 1000000);

    // 反向自检发现：faststart.mp4 恰好在 5s 处有一个非关键帧 pts 精确等于
    // target，`pts_us <= target` 在 AVSEEK_FLAG_ANY 下也会碰巧通过
    // （5000000 <= 5000000）——单靠它钉不住 BACKWARD 的语义。补一条更强的：
    // BACKWARD 保证视频轨落在最近的前一个关键帧，往后找到的第一个视频
    // packet 必须带 AV_PKT_FLAG_KEY。
    bool      found_video = (ti >= 0 && d->tracks()[static_cast<std::size_t>(ti)].is_video);
    AVPacket* video_pkt   = found_video ? p : nullptr;
    if (!found_video) {
        av_packet_free(&p);
        for (int i = 0; i < 8 && !found_video; ++i) {
            AVPacket* q  = nullptr;
            int32_t   qi = -1;
            REQUIRE(d->read(&q, &qi) == Demuxer::ReadResult::Packet);
            if (qi >= 0 && d->tracks()[static_cast<std::size_t>(qi)].is_video) {
                found_video = true;
                video_pkt   = q;
            } else {
                av_packet_free(&q);
            }
        }
    }
    REQUIRE(found_video);
    REQUIRE(video_pkt != nullptr);
    CHECK((video_pkt->flags & AV_PKT_FLAG_KEY) != 0);
    av_packet_free(&video_pkt);
}

// 旋转与 SAR 的采集。素材现场用 ffmpeg CLI 合成，不进
// gen-fixtures 的验证素材矩阵——那套素材的尺寸参与多条用例的判据，
// 已知有两个会让用例确定性失败的反例种子需要避开。
//
// 旋转钉双向 + 关系断言：真实发生过
// "生产代码的符号换算没取负、用例的绝对期望值也手滑写成同一个（错误推导
// 出的）数字"——两处一起错，CHECK_EQ 照样绿。单纯加第二个方向本身堵不住
// 这个洞（第二个方向依然可以被同一个错误直觉一起带偏）；真正堵住它的是
// rotation_relation_holds() 的原始值来自 raw_display_rotation()，那个
// helper 直接读 AVFormatContext 的 side data、不调用 build_tracks() 里
// 那段符号换算代码——生产代码的符号只要错，不管测试作者的绝对期望值有没
// 有跟着错，独立算出来的 raw 和 rotation_deg 之和都不会 ≡ 0 (mod 360)，
// 关系断言必红。双向覆盖是另一件事：确认符号换算在两个方向上都自洽，
// 不是"凑巧对一个方向、另一个方向没测过"。
//
// 语义依据（FFmpeg 8.1.2 源码，见 demuxer.cpp 对应注释同一处）：
// `-display_rotation R` 是"逆时针 R 度"（doc/ffmpeg.texi:1546,1550）；
// FFmpeg 自己的播放器用 `theta = -round(av_display_rotation_get())`
// （fftools/cmdutils.c:1557）作为需要顺时针施加的角度
// （fftools/ffplay.c:2023-2033：theta==90 ⇒ transpose=clock 顺时针）。
TEST_CASE(track_info_reports_display_rotation_ccw90_needs_cw270) {
    syp::test::TempDir dir;
    // -display_rotation 90：素材声明"逆时针转 90° 后显示"。
    const std::string path = syp::test::synth_video_with_display_rotation(dir, 90);
    REQUIRE(!path.empty());

    syp_status err = SYP_OK;
    auto       d   = Demuxer::open_file(path, &err);
    REQUIRE(d != nullptr);
    REQUIRE(err == SYP_OK);
    const auto* v = first_video_track(*d);
    REQUIRE(v != nullptr);
    CHECK_EQ(v->width, 640);
    CHECK_EQ(v->height, 360);
    // 契约：rotation_deg 是"为得到正立画面需要顺时针施加的角度"。逆时针
    // 90° 的素材，正立需要顺时针 270°（实测 av_display_rotation_get()
    // 原始返回值为 +90.0）。
    CHECK_EQ(v->rotation_deg, 270);

    const double raw = raw_display_rotation(*d, v->index);
    REQUIRE(!std::isnan(raw));
    CHECK(rotation_relation_holds(raw, v->rotation_deg));
}

// iPhone 竖屏录像的典型形态：逆时针 270°（≡ 顺时针 90°）。
TEST_CASE(track_info_reports_display_rotation_ccw270_needs_cw90) {
    syp::test::TempDir dir;
    const std::string path = syp::test::synth_video_with_display_rotation(dir, 270);
    REQUIRE(!path.empty());

    syp_status err = SYP_OK;
    auto       d   = Demuxer::open_file(path, &err);
    REQUIRE(d != nullptr);
    REQUIRE(err == SYP_OK);
    const auto* v = first_video_track(*d);
    REQUIRE(v != nullptr);
    CHECK_EQ(v->width, 640);
    CHECK_EQ(v->height, 360);
    // 逆时针 270° ≡ 顺时针 90°（实测 av_display_rotation_get() 原始返回值
    // 为 -90.0）。
    CHECK_EQ(v->rotation_deg, 90);

    const double raw = raw_display_rotation(*d, v->index);
    REQUIRE(!std::isnan(raw));
    CHECK(rotation_relation_holds(raw, v->rotation_deg));
}

TEST_CASE(track_info_reports_sample_aspect_ratio) {
    syp::test::TempDir dir;
    const std::string  path = syp::test::synth_video_with_sar(dir, 8, 9);
    REQUIRE(!path.empty());

    syp_status err = SYP_OK;
    auto       d   = Demuxer::open_file(path, &err);
    REQUIRE(d != nullptr);
    REQUIRE(err == SYP_OK);
    const auto* v = first_video_track(*d);
    REQUIRE(v != nullptr);
    CHECK_EQ(v->sar_num, 8);
    CHECK_EQ(v->sar_den, 9);
    CHECK_EQ(syp::media::display_size(v->width, v->height, v->sar_num, v->sar_den,
                                      v->rotation_deg).width, 640);
}

// SAR 只写在容器层（mp4 pasp）的素材：TrackInfo 必须报容器层
// 的 3:4，不能只读 codecpar 的 1:1。期望值手算：DAR 4:3 ÷ (640/360) = 3/4；
// 显示宽 640 × 3/4 = 480。前提（codecpar 那层确是 1:1）直接从 AVFormatContext
// 读，不经 build_tracks()——前提不成立时这份素材就失去判别力，先红在这里。
TEST_CASE(track_info_reports_container_level_sample_aspect_ratio) {
    syp::test::TempDir dir;
    const std::string  path = syp::test::synth_video_with_container_sar(dir);
    REQUIRE(!path.empty());

    syp_status err = SYP_OK;
    auto       d   = Demuxer::open_file(path, &err);
    REQUIRE(d != nullptr);
    REQUIRE(err == SYP_OK);
    const auto* v = first_video_track(*d);
    REQUIRE(v != nullptr);

    const AVStream* st = d->raw()->streams[static_cast<unsigned>(v->index)];
    // 前提：码流层 1:1、容器层 3:4——两层确实不一致
    CHECK_EQ(st->codecpar->sample_aspect_ratio.num, 1);
    CHECK_EQ(st->codecpar->sample_aspect_ratio.den, 1);
    CHECK_EQ(st->sample_aspect_ratio.num, 3);
    CHECK_EQ(st->sample_aspect_ratio.den, 4);

    CHECK_EQ(v->width, 640);
    CHECK_EQ(v->height, 360);
    CHECK_EQ(v->sar_num, 3);
    CHECK_EQ(v->sar_den, 4);
    CHECK_EQ(syp::media::display_size(v->width, v->height, v->sar_num, v->sar_den,
                                      v->rotation_deg).width, 480);
}

TEST_CASE(track_info_reports_no_rotation_and_square_pixels_for_plain_source) {
    syp::test::TempDir dir;
    // rotation=0 ⇒ 合成函数不加 -display_rotation，产出一份普通素材
    const std::string path = syp::test::synth_video_with_display_rotation(dir, 0);
    REQUIRE(!path.empty());

    syp_status err = SYP_OK;
    auto       d   = Demuxer::open_file(path, &err);
    REQUIRE(d != nullptr);
    REQUIRE(err == SYP_OK);
    const auto* v = first_video_track(*d);
    REQUIRE(v != nullptr);
    CHECK_EQ(v->rotation_deg, 0);
    // FFmpeg 对未知 SAR 给 0/1；display_size() 按 1:1 处理
    CHECK_EQ(syp::media::display_size(v->width, v->height, v->sar_num, v->sar_den, 0).width, 640);
}

int main() { return tiny_test_main(); }
