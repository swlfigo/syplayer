// test_vt_decode.mm —— VideoToolbox 硬解集成测。macOS 宿主原生跑（macos-arm64 slice
// 编进了 VT hwaccel）。Catalyst 形状（hwaccel 未编入）靠 vt_supports() 纯函数穷举覆盖。
#include "media/demuxer.h"
#include "media/ffmpeg_video_decoder.h"
#include "media/pipeline.h"
#include "platform/apple/metal_renderer.h"
#include "platform/apple/vt_decode_backend.h"
#include "scenarios.h"                  // ensure_apple_backend / TempCacheDir
#include "support/loopback_server.h"
#include "tiny_test.h"

#include <CoreVideo/CoreVideo.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <syplayer/syp_config.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

using namespace syp::media;
using syp::platform::videotoolbox_decode_backend;
using syp::platform::vt_supports;

namespace {
std::string fixture(const char* name) {
    const char* d = std::getenv("SYP_FIXTURE_DIR");
    return std::string(d ? d : "") + "/" + name;
}

struct DecodeRun {
    bool                 created = false;
    int64_t              frames  = 0;
    int64_t              non_hw_frames = 0;
    int64_t              non_iosurface = 0;
    std::vector<int64_t> pts;
    syp_status           err = SYP_OK;
    bool                 reached_eof  = false;   // 以 StepOutcome::Eof 干净结束（不是 Error、不是耗尽步数）
    bool                 video_failed = false;   // 结束时视频轨 track_failed()
};

// 用 Pipeline 跑到 Eof，只收视频帧。
DecodeRun run(const char* name, VideoDecodeMode mode) {
    DecodeRun r;
    PipelineConfig cfg;
    cfg.video_decode = mode;
    cfg.hw_backend   = &videotoolbox_decode_backend();
    auto p = Pipeline::create_file(fixture(name), cfg, &r.err);
    if (p == nullptr) return r;
    r.created = true;
    for (int i = 0; i < 2000000; ++i) {
        const StepOutcome o = p->step();
        if (o.kind == StepOutcome::Kind::Eof) { r.reached_eof = true; break; }
        if (o.kind == StepOutcome::Kind::Error) break;
        for (const TrackInfo& t : p->tracks()) {
            while (auto f = p->pop_frame(t.index)) {
                if (!t.is_video) continue;
                ++r.frames;
                r.pts.push_back(f->pts_us());
                if (f->pix_fmt() != AV_PIX_FMT_VIDEOTOOLBOX) ++r.non_hw_frames;
                if (mode == VideoDecodeMode::Hardware) {
                    auto* pb = static_cast<CVPixelBufferRef>(f->hw_handle());
                    if (pb == nullptr || CVPixelBufferGetIOSurface(pb) == nullptr) ++r.non_iosurface;
                }
            }
        }
    }
    for (const TrackInfo& t : p->tracks()) {
        if (t.is_video && !t.attached_pic && p->track_failed(t.index)) r.video_failed = true;
    }
    return r;
}

// 逐帧收集视频平面：软解帧 yuv420p 直接拷；硬解帧从 CVPixelBuffer 读 NV12，
// 把 UV 交织拆成 U、V 两个平面——两者统一成 (Y, U, V) 字节序列。
struct Planes { std::vector<uint8_t> y, u, v; int64_t pts = 0; };

// `Frame` 不暴露底层 `AVFrame`，这里不为测试加公开访问器：硬解帧用
// `hw_handle()` 拿 `CVPixelBufferRef`、`CVPixelBufferLockBaseAddress` 读
// 两个平面。Y 平面按 visible width×height 裁，不带 CVPixelBuffer 的行
// 内 stride 填充。
bool planes_from_frame(const Frame& f, Planes* out) {
    const int32_t w = f.width(), h = f.height();
    out->pts = f.pts_us();
    out->y.clear(); out->u.clear(); out->v.clear();
    if (f.pix_fmt() == AV_PIX_FMT_YUV420P) {
        for (int32_t r = 0; r < h; ++r)
            out->y.insert(out->y.end(), f.plane(0) + r * f.stride(0), f.plane(0) + r * f.stride(0) + w);
        for (int32_t r = 0; r < (h + 1) / 2; ++r) {
            out->u.insert(out->u.end(), f.plane(1) + r * f.stride(1), f.plane(1) + r * f.stride(1) + (w + 1) / 2);
            out->v.insert(out->v.end(), f.plane(2) + r * f.stride(2), f.plane(2) + r * f.stride(2) + (w + 1) / 2);
        }
        return true;
    }
    auto* pb = static_cast<CVPixelBufferRef>(f.hw_handle());
    if (pb == nullptr || CVPixelBufferGetPlaneCount(pb) != 2) return false;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const auto* yp = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    const auto* cp = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const size_t cs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (int32_t r = 0; r < h; ++r) {
        const size_t row = static_cast<size_t>(r) * ys;
        out->y.insert(out->y.end(), yp + row, yp + row + static_cast<size_t>(w));
    }
    for (int32_t r = 0; r < (h + 1) / 2; ++r) {
        const size_t row = static_cast<size_t>(r) * cs;
        for (int32_t c = 0; c < (w + 1) / 2; ++c) {
            const size_t col = static_cast<size_t>(2 * c);
            out->u.push_back(cp[row + col]);
            out->v.push_back(cp[row + col + 1]);
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return true;
}

// decode_planes() 的结果：区分「干净读到文件 Eof、解码器全程没有报错」
// 与「半路出错、帧集合被悄悄截断」——Demuxer::ReadResult::Error
// 与 IVideoDecoder::Receive::Error 之前跟正常的 Eof 走同一条 break，调用方
// 拿到的是一个看起来正常、实际不完整的帧集合。reached_eof 只在读到干净的
// Demuxer::ReadResult::Eof、且 send()/receive() 全程没有返回错误时才置真。
struct PlaneRun {
    std::vector<Planes> frames;
    bool                reached_eof = false;
};

// 用 Demuxer + FFmpegVideoDecoder 直接跑一遍解码循环，逐帧收集视频平面
// （不经 Pipeline，避免额外的队列/同步逻辑干扰逐比特比对）。
PlaneRun decode_planes(const char* name, VideoDecodeMode mode) {
    PlaneRun run;
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture(name), &err);
    if (d == nullptr) return run;
    int32_t vidx = -1;
    for (unsigned i = 0; i < d->raw()->nb_streams; ++i)
        if (d->raw()->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vidx = static_cast<int32_t>(i); break; }
    if (vidx < 0) return run;
    FFmpegVideoDecoder dec(mode, &videotoolbox_decode_backend());
    if (dec.open(d->raw()->streams[vidx]->codecpar, d->raw()->streams[vidx]->time_base) != SYP_OK) return run;
    bool decode_error = false;
    auto drain = [&] {
        Frame f;
        for (;;) {
            const auto rc = dec.receive(&f);
            if (rc == IVideoDecoder::Receive::Frame) {
                Planes p;
                if (planes_from_frame(f, &p)) run.frames.push_back(std::move(p));
                continue;
            }
            if (rc == IVideoDecoder::Receive::Error) decode_error = true;
            break;
        }
    };
    bool clean_eof = false;
    for (;;) {
        AVPacket* pkt = nullptr;
        int32_t ti = -1;
        const auto r = d->read(&pkt, &ti);
        if (r == Demuxer::ReadResult::Eof) { clean_eof = true; break; }
        if (r != Demuxer::ReadResult::Packet) break;  // Demuxer::ReadResult::Error：非干净结束
        if (ti == vidx && dec.send(pkt) != SYP_OK) decode_error = true;
        av_packet_free(&pkt);
        drain();
        if (decode_error) break;
    }
    if (clean_eof && !decode_error) {
        dec.send(nullptr);
        drain();
    }
    run.reached_eof = clean_eof && !decode_error;
    return run;
}
}  // namespace

using syp::dl::test::LoopbackServer;

TEST_CASE(vt_supports_requires_hwaccel_compiled_in) {
    // Catalyst 形状：系统说支持也没用。
    CHECK(!vt_supports(false, AV_CODEC_ID_H264, AV_PIX_FMT_YUV420P, 100, true));
    CHECK(!vt_supports(false, AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV420P, 1, true));
}

TEST_CASE(vt_supports_codec_and_bit_depth_matrix) {
    CHECK(vt_supports(true, AV_CODEC_ID_H264, AV_PIX_FMT_YUV420P, 100, true));
    CHECK(vt_supports(true, AV_CODEC_ID_H264, AV_PIX_FMT_YUVJ420P, 100, true));
    CHECK(vt_supports(true, AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV420P, 1, true));
    CHECK(!vt_supports(true, AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV420P10LE, 2, true));   // Main10
    CHECK(!vt_supports(true, AV_CODEC_ID_HEVC, AV_PIX_FMT_NONE, 2, true));          // 未知格式按 profile 排除
    CHECK(!vt_supports(true, AV_CODEC_ID_MPEG4, AV_PIX_FMT_YUV420P, 0, true));
    CHECK(!vt_supports(true, AV_CODEC_ID_H264, AV_PIX_FMT_YUV420P, 100, false));    // 设备不支持
}

TEST_CASE(videotoolbox_backend_supports_h264_and_hevc_on_this_mac) {
    syp_status err = SYP_OK;
    for (const char* name : {"faststart.mp4", "hevc.mp4"}) {
        auto d = Demuxer::open_file(fixture(name), &err);
        REQUIRE(d != nullptr);
        bool saw_video = false;
        for (unsigned i = 0; i < d->raw()->nb_streams; ++i) {
            const AVCodecParameters* par = d->raw()->streams[i]->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            saw_video = true;
            CHECK(videotoolbox_decode_backend().supports(*par));
        }
        CHECK(saw_video);
    }
}

// 结构性验收 1：选了硬解，每一帧都是 IOSurface 支撑的 VideoToolbox 帧——没有静默软解。
TEST_CASE(hardware_decode_never_produces_software_frames) {
    for (const char* name : {"faststart.mp4", "bframes.mp4", "hevc.mp4"}) {
        const DecodeRun r  = run(name, VideoDecodeMode::Hardware);
        const DecodeRun sw = run(name, VideoDecodeMode::Software);
        REQUIRE(r.created);
        REQUIRE(sw.created);
        // 干净 Eof、视频轨没失败、帧数与软解相同——否则"零软件帧"可能只是
        // 视频轨半路终止后的空洞胜利（C1 修复后硬解被拒表现为 track_failed）。
        CHECK(r.reached_eof);
        CHECK(sw.reached_eof);
        CHECK(!r.video_failed);
        CHECK(r.frames > 0);
        CHECK_EQ(r.frames, sw.frames);
        CHECK_EQ(r.non_hw_frames, int64_t{0});
        CHECK_EQ(r.non_iosurface, int64_t{0});
    }
}

TEST_CASE(pipeline_reports_hardware_decoding_with_videotoolbox) {
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    cfg.hw_backend   = &videotoolbox_decode_backend();
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("hevc.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    CHECK(p->video_hardware_decoding());
}

// H.264/HEVC 重建是规范性的：硬解与软解预期逐比特一致。若不一致，先查根因（输出 range
// 转换、裁剪边界、奇数尺寸），记录问题，再退到 PSNR ≥ 50 dB——
// 不允许不查就放宽。
TEST_CASE(hardware_and_software_decode_are_bit_exact) {
    for (const char* name : {"faststart.mp4", "bframes.mp4", "hevc.mp4"}) {
        const auto sw = decode_planes(name, VideoDecodeMode::Software);
        const auto hw = decode_planes(name, VideoDecodeMode::Hardware);
        // 先确认两路都干净跑到 Eof——半路解码错误不该被悄悄当成「帧数对不上
        // 就少几帧」，必须显式炸出来。
        REQUIRE(sw.reached_eof);
        REQUIRE(hw.reached_eof);
        REQUIRE(!sw.frames.empty());
        REQUIRE(sw.frames.size() == hw.frames.size());
        int64_t mismatched = 0;
        for (size_t i = 0; i < sw.frames.size(); ++i) {
            CHECK_EQ(sw.frames[i].pts, hw.frames[i].pts);
            if (sw.frames[i].y != hw.frames[i].y || sw.frames[i].u != hw.frames[i].u ||
                sw.frames[i].v != hw.frames[i].v)
                ++mismatched;
        }
        std::printf("  [bit-exact] %s frames=%zu mismatched=%lld\n", name, sw.frames.size(),
                    static_cast<long long>(mismatched));
        CHECK_EQ(mismatched, int64_t{0});
    }
}

// seek → flush → 再解到 Eof，硬解模式下成立（带 B 帧素材，flush 丢掉 VT 里压着的重排序帧）。
TEST_CASE(hardware_decode_seek_then_reaches_eof_again) {
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    cfg.hw_backend   = &videotoolbox_decode_backend();
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("bframes.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    auto to_eof = [&](int64_t* video) {
        for (int i = 0; i < 2000000; ++i) {
            const StepOutcome o = p->step();
            if (o.kind == StepOutcome::Kind::Eof) return true;
            if (o.kind == StepOutcome::Kind::Error) return false;
            for (const TrackInfo& t : p->tracks())
                while (auto f = p->pop_frame(t.index)) if (t.is_video) ++*video;
        }
        return false;
    };
    int64_t first = 0, second = 0;
    REQUIRE(to_eof(&first));
    REQUIRE(p->seek(0) == SYP_OK);
    REQUIRE(to_eof(&second));
    CHECK_EQ(first, second);
}

TEST_CASE(hardware_decode_hls_vod_matches_software_frame_count) {
    REQUIRE(syp::probe::ensure_apple_backend());
    syp::dl::test::LoopbackServer srv;
    const std::string dir = fixture("hls/vod_single");
    for (const char* n : {"master.m3u8", "media.m3u8", "init.mp4", "seg0.m4s", "seg1.m4s", "seg2.m4s", "seg3.m4s"}) {
        std::ifstream in(dir + "/" + n, std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        srv.set_route(std::string("/hls/") + n, std::move(b),
                      std::string(n).find(".m3u8") != std::string::npos ? "application/vnd.apple.mpegurl" : "video/mp4");
    }
    auto count = [&](VideoDecodeMode mode, const char* tag) -> int64_t {
        syp_config dl{};
        syp_config_init(&dl);
        dl.struct_size = sizeof(syp_config);
        const syp::probe::TempCacheDir cache(tag);
        dl.cache_dir = cache.path.c_str();
        PipelineConfig cfg;
        cfg.video_decode = mode;
        cfg.hw_backend   = &videotoolbox_decode_backend();
        syp_status err = SYP_OK;
        auto p = Pipeline::create_hls(srv.url("/hls/master.m3u8"), cfg, dl, HlsOptions{}, &err);
        if (p == nullptr) return -1;
        int64_t v = 0;
        bool reached_eof = false;
        for (int i = 0; i < 2000000; ++i) {
            const StepOutcome o = p->step();
            if (o.kind == StepOutcome::Kind::Eof) { reached_eof = true; break; }
            if (o.kind == StepOutcome::Kind::Error) break;  // reached_eof 保持 false，下面按 -3 报错
            for (const TrackInfo& t : p->tracks())
                while (auto f = p->pop_frame(t.index))
                    if (t.is_video && !t.discard) {
                        if (mode == VideoDecodeMode::Hardware && f->pix_fmt() != AV_PIX_FMT_VIDEOTOOLBOX) return -2;
                        ++v;
                    }
        }
        // 走到 Error、或耗尽 step 预算都没见到 Eof——跟「干净播完」不是一回事，
        // 不能让调用方把这种半路截断误当成一个偏小但合法的帧数。
        return reached_eof ? v : int64_t{-3};
    };
    const int64_t sw = count(VideoDecodeMode::Software, "vt_hls_sw");
    const int64_t hw = count(VideoDecodeMode::Hardware, "vt_hls_hw");
    CHECK(sw > 0);
    CHECK_EQ(hw, sw);
}

// 真实 VideoToolbox 解码输出直接喂 MetalRenderer。渲染器自测用的是自建
// 带 kCVPixelBufferMetalCompatibilityKey 的 IOSurface buffer；FFmpeg 的 VT buffer 只挂
// IOSurface + OpenGL(ES) 兼容键（videotoolbox.c）。这里证明真实硬解帧能过 420v/420f 闸、
// 走零拷贝（debug_cpu_upload_count()==0），且画面与同一帧软解后呈现的结果一致（±2）。
namespace {
// 直接用 Demuxer + FFmpegVideoDecoder 解前 n 帧，逐帧回调；返回是否真的拿到 n 帧。
template <typename Fn>
bool for_first_frames(const char* name, VideoDecodeMode mode, int n, Fn&& fn) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture(name), &err);
    if (d == nullptr) return false;
    int32_t vidx = -1;
    for (unsigned i = 0; i < d->raw()->nb_streams; ++i)
        if (d->raw()->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vidx = static_cast<int32_t>(i); break; }
    if (vidx < 0) return false;
    FFmpegVideoDecoder dec(mode, &videotoolbox_decode_backend());
    if (dec.open(d->raw()->streams[vidx]->codecpar, d->raw()->streams[vidx]->time_base) != SYP_OK) return false;
    int got = 0;
    while (got < n) {
        AVPacket* pkt = nullptr;
        int32_t ti = -1;
        if (d->read(&pkt, &ti) != Demuxer::ReadResult::Packet) break;
        const bool send_ok = (ti != vidx) || dec.send(pkt) == SYP_OK;
        av_packet_free(&pkt);
        if (!send_ok) return false;
        for (;;) {
            if (got >= n) break;
            Frame f;
            const auto rc = dec.receive(&f);
            if (rc == IVideoDecoder::Receive::Error) return false;
            if (rc != IVideoDecoder::Receive::Frame) break;
            fn(f, got);
            ++got;
        }
    }
    return got == n;
}
}  // namespace

TEST_CASE(videotoolbox_frames_present_zero_copy_and_match_software) {
    const int kFrames  = 30;
    const int kCompare = 12;   // 比对第几帧（显示序）：避开首帧，落在 GOP 里
    for (const char* name : {"faststart.mp4", "hevc.mp4"}) {
        syp::platform::MetalRenderer hw_r;
        if (!hw_r.device_available()) {
            std::printf("  SKIP：本机没有可用的 Metal 设备\n");
            return;
        }
        REQUIRE(hw_r.ready());   // 有设备但管线没建起来是真实缺陷，不 skip
        syp::platform::MetalRenderer sw_r;
        REQUIRE(sw_r.ready());

        std::vector<uint8_t> hw_px, sw_px;
        int32_t hw_w = 0, hw_h = 0, sw_w = 0, sw_h = 0;
        int64_t hw_pts = -1, sw_pts = -1;
        int64_t hw_bad = 0, hw_non_vt = 0;
        const bool hw_ok = for_first_frames(name, VideoDecodeMode::Hardware, kFrames,
            [&](const Frame& f, int i) {
                if (f.pix_fmt() != AV_PIX_FMT_VIDEOTOOLBOX) ++hw_non_vt;
                // present 不阻塞、在途满了返回 BUSY——本用例断言每帧都呈现成功，
                // 所以逐帧先等空闲（喂帧速度远快于 GPU，不等就会合法地 BUSY）。
                hw_r.wait_until_idle();
                if (hw_r.present(f, 0) != SYP_OK) ++hw_bad;
                if (i == kCompare) {
                    hw_pts = f.pts_us();
                    if (!hw_r.debug_copy_output_rgba(hw_px, hw_w, hw_h)) hw_px.clear();
                }
            });
        const bool sw_ok = for_first_frames(name, VideoDecodeMode::Software, kFrames,
            [&](const Frame& f, int i) {
                if (i == kCompare) {
                    CHECK_EQ(sw_r.present(f, 0), SYP_OK);
                    sw_pts = f.pts_us();
                    if (!sw_r.debug_copy_output_rgba(sw_px, sw_w, sw_h)) sw_px.clear();
                }
            });
        REQUIRE(hw_ok);
        REQUIRE(sw_ok);
        CHECK_EQ(hw_non_vt, int64_t{0});
        CHECK_EQ(hw_bad, int64_t{0});
        CHECK_EQ(hw_r.debug_cpu_upload_count(), int64_t{0});   // 零拷贝：30 帧无一次 CPU 上传
        CHECK(sw_r.debug_cpu_upload_count() > 0);               // 区分力：软解路径确实计数
        REQUIRE(!hw_px.empty());
        REQUIRE(!sw_px.empty());
        CHECK_EQ(hw_pts, sw_pts);
        REQUIRE(hw_w == sw_w);
        REQUIRE(hw_h == sw_h);
        REQUIRE(hw_px.size() == sw_px.size());
        int max_diff = 0;
        int64_t over = 0;
        for (size_t k = 0; k < hw_px.size(); ++k) {
            const int dd = std::abs(static_cast<int>(hw_px[k]) - static_cast<int>(sw_px[k]));
            if (dd > max_diff) max_diff = dd;
            if (dd > 2) ++over;
        }
        std::printf("  [vt->metal] %s %dx%d upload=%lld max_diff=%d over2=%lld\n", name, hw_w, hw_h,
                    static_cast<long long>(hw_r.debug_cpu_upload_count()), max_diff,
                    static_cast<long long>(over));
        CHECK(max_diff <= 2);
    }
}

int main() { return tiny_test_main(); }
