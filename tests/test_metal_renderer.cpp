// test_metal_renderer.cpp — MetalRenderer 测试。
//
// 三段，跟 test_audio_unit_sink.cpp 同一个组织方式（见该文件顶部注释）：
//   1. 纯函数（零 GPU 依赖）：is_pix_fmt_supported() / select_color_
//      matrix() / pack_plane_rows()。这三个是发起方追加要求点名的对象
//      ——前两个是明写的最低要求（格式判定、色彩矩阵选择），第三个
//      （pack_plane_rows）是本实现为了让"按 stride 逐行拷"这条硬要求
//      本身可测而额外抽出来的（present() 本可以直接把 f.plane(i)/
//      f.stride(i) 交给 -[MTLTexture replaceRegion:...bytesPerRow:] 一次
//      性搞定，Metal 的这个 API 本身就支持 bytesPerRow != 区域宽度——
//      但那样"有没有正确处理 stride"这件事就整个锁死在 GPU 调用里，没
//      有独立验证的缝。手写 pack_plane_rows() 多一次 CPU 拷贝，换来的
//      是这三个字符：可测。）。
//   2. 不需要 GPU 的对象级用例：present() 的检查顺序（pix_fmt 校验必须
//      先于设备可用性校验，Frame 无效性校验）——这两条即使在没有 GPU
//      的机器上也必须能通过，不应该被"设备不可用"这条分支抢答成一个
//      语义不同的错误码。
//   3. 需要真实 GPU、但本机（目标设备是 macOS，这台开发机有真实
//      Apple Silicon GPU）打得开的接线用例：present() 有没有真的把
//      Frame 的宽高/平面/stride 接给 pack_plane_rows()、色彩矩阵选择
//      有没有真的传给着色器、"推定"有没有真的打进日志——渲染器
//      ready() 为 false 就 skip（不算失败），不会让 CI 在没有 GPU 的
//      机器上变得不确定，跟 test_audio_unit_sink.cpp「open() 失败就
//      skip」是同一条纪律。
//
// 第 3 段追加了两条用例：
// object_level_present_uses_real_colorspace_over_resolution_presumption /
// object_level_present_uses_real_full_range_from_avframe —— present()
// 此前恒以 AVCOL_SPC_UNSPECIFIED/AVCOL_RANGE_UNSPECIFIED 调用
// select_color_matrix()（Frame 当时没有暴露 colorspace()/color_range()
// 访问器），"AVFrame 真的带 colorspace/color_range 时选对矩阵/range"
// 这两条路径在那时结构性地测不到。Frame::colorspace()/color_range()
// 落地、present() 接上真值之后补齐。
//
// 真正没有自动化覆盖、且明确不在本文件待办里的：
//   - MetalRenderer 在真正没有 GPU 的环境下的失败路径（构造时
//     MTLCreateSystemDefaultDevice() 返回 nil 的分支）——这台开发机有
//     真实 GPU，没有安全的方法诱导它返回 nil（伪造一个非法指针给
//     Impl::device 是未定义行为，不是测试）。
#include "media/frame.h"
#include "platform/apple/metal_renderer.h"
#include "support/metal_layer_helper.h"

#include <syplayer/syp_types.h>

#include "tiny_test.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using syp::media::Frame;
using syp::platform::MetalRenderer;

namespace {

// ---------------------------------------------------------------------
// 日志捕获——验证"presumed 打进日志""不支持的格式名打进日志"这两条
// 硬要求的第三部分（不只是纯函数返回了 presumed=true，产线代码有没有
// 真的把它打出来）。
// ---------------------------------------------------------------------

std::vector<std::string> g_captured_logs;

extern "C" void capture_log_cb(void* ctx, syp_log_level lvl, const char* tag, const char* msg) {
    (void)ctx;
    (void)lvl;
    (void)tag;
    g_captured_logs.emplace_back(msg != nullptr ? msg : "");
}

struct LogCapture {
    LogCapture() {
        g_captured_logs.clear();
        syp_set_log_callback(&capture_log_cb, nullptr, SYP_LOG_DEBUG);
    }
    ~LogCapture() { syp_set_log_callback(nullptr, nullptr, SYP_LOG_ERROR); }
};

bool any_log_contains(const std::string& needle) {
    for (const auto& s : g_captured_logs) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// 造带 padding 的真实 yuv420p AVFrame。align 传给 av_frame_get_buffer——
// 传一个明显大于 width 的对齐值（比如 64），linesize 会被撑到远大于
// width/chroma_width，制造出跟真实素材同性质的 stride padding（
// frame_digest.cpp 同款手法）。pad_sentinel 填进 padding 区域：如果
// pack_plane_rows() 没有正确按 stride 逐行拷（比如退化成整块
// memcpy(dst, src, width*height)，会把 padding 里的 sentinel 当成下一行
// 的有效数据往前"吃"进去），下游的颜色断言会失败——这是本文件唯一一处
// 真正区分"逐行拷"与"整块拷"的用例。
// ---------------------------------------------------------------------

// colorspace/color_range 两个尾参默认 UNSPECIFIED（不设置真值，跟
// present() 原本恒传的哨兵一致）——追加在既有可选参数
// 之后，不改动任何既有调用点。
Frame make_yuv420p_frame(int32_t width, int32_t height, uint8_t y_val, uint8_t u_val,
                          uint8_t v_val, int32_t align, uint8_t pad_sentinel = 0xEE,
                          AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED,
                          AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED) {
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P;
    f->width  = width;
    f->height = height;
    f->colorspace  = colorspace;
    f->color_range = color_range;
    av_frame_get_buffer(f, align);

    const int32_t cw = (width + 1) / 2;
    const int32_t ch = (height + 1) / 2;

    for (int32_t r = 0; r < height; ++r) {
        uint8_t* row = f->data[0] + r * f->linesize[0];
        std::memset(row, y_val, static_cast<size_t>(width));
        if (f->linesize[0] > width) {
            std::memset(row + width, pad_sentinel,
                        static_cast<size_t>(f->linesize[0] - width));
        }
    }
    for (int32_t r = 0; r < ch; ++r) {
        uint8_t* row_u = f->data[1] + r * f->linesize[1];
        uint8_t* row_v = f->data[2] + r * f->linesize[2];
        std::memset(row_u, u_val, static_cast<size_t>(cw));
        std::memset(row_v, v_val, static_cast<size_t>(cw));
        if (f->linesize[1] > cw) {
            std::memset(row_u + cw, pad_sentinel, static_cast<size_t>(f->linesize[1] - cw));
        }
        if (f->linesize[2] > cw) {
            std::memset(row_v + cw, pad_sentinel, static_cast<size_t>(f->linesize[2] - cw));
        }
    }
    f->pts = 0;
    return Frame::from_av(f, AVRational{1, 1000000}, /*is_video=*/true);
}

Frame make_frame_with_fmt(AVPixelFormat fmt, int32_t width, int32_t height) {
    AVFrame* f = av_frame_alloc();
    f->format = fmt;
    f->width  = width;
    f->height = height;
    av_frame_get_buffer(f, 32);
    f->pts = 0;
    return Frame::from_av(f, AVRational{1, 1000000}, /*is_video=*/true);
}

// 独立于 video_shaders.metal 里 kernel 的参考实现（手写标量公式，不是从着色器
// 常量复制粘贴）——两条路径共用一套系数是标准值，重复本身不是问题，
// 但表达方式故意不同（矩阵乘法 vs. 直接标量式），降低"复制粘贴同一个
// typo 两遍"的相关性。
struct RefRgb {
    double r, g, b;
};

RefRgb reference_yuv_to_rgb(double y_norm, double u_norm, double v_norm, bool bt709,
                             bool full_range) {
    double y = y_norm, u = u_norm, v = v_norm;
    if (!full_range) {
        y = (y * 255.0 - 16.0) / 219.0;
        u = (u * 255.0 - 128.0) / 224.0;
        v = (v * 255.0 - 128.0) / 224.0;
    } else {
        u -= 0.5;
        v -= 0.5;
    }
    double r, g, b;
    if (bt709) {
        r = y + 1.5748 * v;
        g = y - 0.187324 * u - 0.468124 * v;
        b = y + 1.8556 * u;
    } else {
        r = y + 1.402 * v;
        g = y - 0.344136 * u - 0.714136 * v;
        b = y + 1.772 * u;
    }
    auto clamp01 = [](double x) { return std::min(1.0, std::max(0.0, x)); };
    return {clamp01(r), clamp01(g), clamp01(b)};
}

// GPU 浮点/Unorm 量化的误差裕度——不要求逐字节相等，只要求足够接近。
constexpr int kByteTolerance = 3;

bool byte_close(uint8_t actual, double expected_0_1, int tolerance = kByteTolerance) {
    const double expected_byte = expected_0_1 * 255.0;
    return std::fabs(static_cast<double>(actual) - expected_byte) <= tolerance;
}

// 断言 rgba（w*h*4，RGBA8）里每一个像素都跟同一个预期颜色接近——两处
// 用例共用（纯色输入帧，预期整张画面是同一个颜色）。
bool all_pixels_close(const std::vector<uint8_t>& rgba, int32_t w, int32_t h,
                      const RefRgb& expect) {
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            const size_t idx =
                (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            if (!byte_close(rgba[idx + 0], expect.r) || !byte_close(rgba[idx + 1], expect.g) ||
                !byte_close(rgba[idx + 2], expect.b)) {
                return false;
            }
        }
    }
    return true;
}

// 第 3 段全部 GPU 用例共用的守卫。
// 这是本次修复的核心：以前四条 GPU 用例各自写 `if (!renderer.ready())
// { skip; return; }`——ready() 把"没有 Metal 设备"（环境限制，该 skip）
// 与"设备存在但着色器编译/管线创建失败"（真实缺陷，该 FAIL）合并成
// 一个布尔，用一次故意破坏（video_shaders.metal 删掉一个分号，让着色器整个
// 编译不过）证明了合并的代价：23 条用例在着色器彻底坏掉的情况下仍然
// 全绿。
//
// 正确的判定必须分两层：
//   1. device_available()==false → 这台机器真的没有 Metal 设备，跳过
//      是唯一合理的选择（CI 机器没有 GPU 时不该让 ctest 变得不确定）。
//   2. device_available()==true 但 ready()==false → 设备找到了，但我们
//      自己的着色器/管线代码有问题，这是本任务范围内的真实缺陷，必须
//      FAIL，不能被静默放过。这台开发机有真实 Apple Silicon GPU，
//      device_available() 预期恒为 true，因此这条分支在正常（非注入
//      缺陷）情况下预期恒不触发——一旦触发（比如 video_shaders.metal 语法
//      坏掉），调用方必须能看到红，而不是一条容易被误读成"环境问题"
//      的 SKIP。
//
// 返回值：true 表示可以继续往下做真正的 present()/回读断言；false 表示
// 调用方应该直接 return（要么是合理的 skip，要么失败已经被下面的
// CHECK 记录，没有必要再往下跑更多断言让失败信息变得更难读）。
bool gpu_ready_or_fail(const MetalRenderer& renderer) {
    if (!renderer.device_available()) {
        std::printf("  SKIP：本机没有可用的 Metal 设备（device_available()==false）\n");
        return false;
    }
    // 设备存在：着色器/管线必须建起来，这不是可以跳过的环境限制。
    CHECK(renderer.ready());
    return renderer.ready();
}

}  // namespace

// =======================================================================
// 第 1 段：纯函数，零 GPU 依赖
// =======================================================================

TEST_CASE(is_pix_fmt_supported_accepts_yuv420p_and_videotoolbox) {
    CHECK(MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_YUV420P));
    CHECK(MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_VIDEOTOOLBOX));
}

TEST_CASE(is_pix_fmt_supported_rejects_common_unsupported_formats) {
    CHECK(!MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_YUV420P10LE));
    CHECK(!MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_NV12));
    CHECK(!MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_YUVA420P));
    CHECK(!MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_RGB24));
    CHECK(!MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_YUV422P));
    CHECK(!MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_YUV444P));
}

TEST_CASE(is_pix_fmt_supported_rejects_none_and_negative) {
    CHECK(!MetalRenderer::is_pix_fmt_supported(AV_PIX_FMT_NONE));
    CHECK(!MetalRenderer::is_pix_fmt_supported(-999));
}

TEST_CASE(select_color_matrix_uses_real_colorspace_when_available) {
    const auto c = MetalRenderer::select_color_matrix(AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1920, 1080);
    CHECK(c.matrix == MetalRenderer::ColorMatrixKind::BT709);
    CHECK(!c.full_range);
    CHECK(!c.presumed);
}

TEST_CASE(select_color_matrix_recognizes_bt601_aliases) {
    // BT470BG 与 SMPTE170M 都是"BT.601"这套系数的不同命名（见
    // libavutil/pixfmt.h 注释：functionally identical），两个都要落进
    // BT601 分支、且都不是推定。
    const auto c1 =
        MetalRenderer::select_color_matrix(AVCOL_SPC_BT470BG, AVCOL_RANGE_MPEG, 720, 480);
    CHECK(c1.matrix == MetalRenderer::ColorMatrixKind::BT601);
    CHECK(!c1.presumed);

    const auto c2 =
        MetalRenderer::select_color_matrix(AVCOL_SPC_SMPTE170M, AVCOL_RANGE_JPEG, 720, 480);
    CHECK(c2.matrix == MetalRenderer::ColorMatrixKind::BT601);
    CHECK(c2.full_range);
    CHECK(!c2.presumed);
}

TEST_CASE(select_color_matrix_presumes_bt709_at_or_above_720p) {
    // 边界：height == 720 恰好落在"≥720p"这条规则里，必须选 BT.709。
    const auto at_boundary =
        MetalRenderer::select_color_matrix(AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_MPEG, 1280, 720);
    CHECK(at_boundary.matrix == MetalRenderer::ColorMatrixKind::BT709);
    CHECK(at_boundary.presumed);

    const auto above =
        MetalRenderer::select_color_matrix(AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_MPEG, 1920, 1080);
    CHECK(above.matrix == MetalRenderer::ColorMatrixKind::BT709);
    CHECK(above.presumed);
}

TEST_CASE(select_color_matrix_presumes_bt601_below_720p) {
    const auto just_below =
        MetalRenderer::select_color_matrix(AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_MPEG, 1280, 719);
    CHECK(just_below.matrix == MetalRenderer::ColorMatrixKind::BT601);
    CHECK(just_below.presumed);

    const auto small =
        MetalRenderer::select_color_matrix(AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_MPEG, 640, 360);
    CHECK(small.matrix == MetalRenderer::ColorMatrixKind::BT601);
    CHECK(small.presumed);
}

TEST_CASE(select_color_matrix_presumes_limited_range_when_unspecified) {
    // colorspace 已知、color_range 未知：matrix 不推定，range 推定成
    // limited；presumed 合取标志仍然为 true（"任一项被推定"）。
    const auto c = MetalRenderer::select_color_matrix(AVCOL_SPC_BT709, AVCOL_RANGE_UNSPECIFIED,
                                                       1920, 1080);
    CHECK(c.matrix == MetalRenderer::ColorMatrixKind::BT709);
    CHECK(!c.full_range);   // 推定成 limited，不是 full
    CHECK(c.presumed);
}

TEST_CASE(select_color_matrix_both_unspecified_presumes_both) {
    const auto c = MetalRenderer::select_color_matrix(AVCOL_SPC_UNSPECIFIED,
                                                       AVCOL_RANGE_UNSPECIFIED, 1920, 1080);
    CHECK(c.matrix == MetalRenderer::ColorMatrixKind::BT709);
    CHECK(!c.full_range);
    CHECK(c.presumed);
}

TEST_CASE(select_color_matrix_choice_equality_operator) {
    const MetalRenderer::ColorMatrixChoice a{MetalRenderer::ColorMatrixKind::BT601, false, true};
    const MetalRenderer::ColorMatrixChoice b{MetalRenderer::ColorMatrixKind::BT601, false, true};
    const MetalRenderer::ColorMatrixChoice c{MetalRenderer::ColorMatrixKind::BT709, false, true};
    CHECK(a == b);
    CHECK(!(a == c));
}

TEST_CASE(pack_plane_rows_strips_stride_padding) {
    // src：3 行，每行 4 字节有效数据 + 2 字节 padding（stride=6）。
    // 对照组先确认"整块拷 6*3=18 字节"会把 padding 也带进去——证明下面
    // pack_plane_rows() 的行为不是恰好跟整块拷一样。
    const std::vector<uint8_t> src = {
        1, 2, 3, 4, 0xEE, 0xEE,
        5, 6, 7, 8, 0xEE, 0xEE,
        9, 10, 11, 12, 0xEE, 0xEE,
    };
    std::vector<uint8_t> whole_block(18, 0);
    std::memcpy(whole_block.data(), src.data(), 18);
    // 整块拷出来的第 5、6 字节是 padding（0xEE），不是第二行的开头——
    // 这条断言只是把"折算前"钉在这里。
    CHECK_EQ(whole_block[4], uint8_t{0xEE});

    std::vector<uint8_t> dst(12, 0);
    CHECK(MetalRenderer::pack_plane_rows(dst.data(), src.data(), /*src_stride=*/6,
                                          /*row_bytes=*/4, /*rows=*/3));
    const std::vector<uint8_t> expect = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(dst == expect);
}

TEST_CASE(pack_plane_rows_no_padding_is_a_straight_copy) {
    const std::vector<uint8_t> src = {1, 2, 3, 4, 5, 6};
    std::vector<uint8_t> dst(6, 0);
    CHECK(MetalRenderer::pack_plane_rows(dst.data(), src.data(), /*src_stride=*/3,
                                          /*row_bytes=*/3, /*rows=*/2));
    CHECK(dst == src);
}

TEST_CASE(pack_plane_rows_rejects_stride_smaller_than_row_bytes) {
    std::vector<uint8_t> src(8, 0xAB);
    std::vector<uint8_t> dst(8, 0);
    // stride(3) < row_bytes(4)：一行都不够读，必须整体拒绝，不能读到
    // 下一行头上。
    CHECK(!MetalRenderer::pack_plane_rows(dst.data(), src.data(), /*src_stride=*/3,
                                           /*row_bytes=*/4, /*rows=*/2));
}

TEST_CASE(pack_plane_rows_rejects_null_pointers) {
    std::vector<uint8_t> buf(4, 0);
    CHECK(!MetalRenderer::pack_plane_rows(nullptr, buf.data(), 4, 4, 1));
    CHECK(!MetalRenderer::pack_plane_rows(buf.data(), nullptr, 4, 4, 1));
}

TEST_CASE(pack_plane_rows_rejects_non_positive_row_bytes_or_rows) {
    std::vector<uint8_t> buf(8, 0);
    CHECK(!MetalRenderer::pack_plane_rows(buf.data(), buf.data(), 4, 0, 1));
    CHECK(!MetalRenderer::pack_plane_rows(buf.data(), buf.data(), 4, -1, 1));
    CHECK(!MetalRenderer::pack_plane_rows(buf.data(), buf.data(), 4, 4, 0));
    CHECK(!MetalRenderer::pack_plane_rows(buf.data(), buf.data(), 4, 4, -1));
}

// =======================================================================
// 第 2 段：不需要 GPU 的对象级用例——present() 的检查顺序
// =======================================================================

TEST_CASE(present_rejects_invalid_frame_without_touching_gpu) {
    MetalRenderer renderer;
    Frame invalid;   // 默认构造：valid() == false
    CHECK_EQ(renderer.present(invalid, 0), syp_status{SYP_ERR_INVALID_ARG});
}

TEST_CASE(present_rejects_unsupported_pix_fmt_before_checking_device_readiness) {
    // 关键断言：不管这台机器有没有真实 GPU（renderer.ready() 是 true
    // 还是 false），不支持的格式必须恒定返回 SYP_ERR_NOT_IMPLEMENTED——
    // 格式校验必须先于设备可用性校验，不能被"设备不可用"这条分支抢答
    // 成一个语义不同的错误码（metal_renderer.mm::present() 顶部注释）。
    MetalRenderer renderer;
    Frame nv12 = make_frame_with_fmt(AV_PIX_FMT_NV12, 64, 64);
    CHECK_EQ(renderer.present(nv12, 0), syp_status{SYP_ERR_NOT_IMPLEMENTED});
}

TEST_CASE(present_unsupported_pix_fmt_logs_format_name) {
    LogCapture capture;
    MetalRenderer renderer;
    Frame nv12 = make_frame_with_fmt(AV_PIX_FMT_NV12, 64, 64);
    renderer.present(nv12, 0);
    CHECK(any_log_contains("nv12"));
}

TEST_CASE(debug_copy_output_rgba_false_before_any_present) {
    MetalRenderer renderer;
    std::vector<uint8_t> out;
    int32_t w = 0, h = 0;
    CHECK(!renderer.debug_copy_output_rgba(out, w, h));
}

// =======================================================================
// 第 3 段：接线用例，真的碰 GPU——renderer.ready() 为 false 就 skip
// =======================================================================

TEST_CASE(object_level_present_and_readback_on_real_gpu) {
    MetalRenderer renderer;
    if (!gpu_ready_or_fail(renderer)) return;

    // 17x11：奇数宽高，色度平面走 (w+1)/2=9, (h+1)/2=6 的向上取整——跟
    // frame_digest.cpp 里奇数宽验证的用意一样，顺带覆盖"宽高不是 2 的
    // 整数倍"这个边界。align=64 制造明显的 stride padding（Y/U/V 三个
    // 平面的 linesize 都会被撑到远大于各自的有效宽度）。
    constexpr uint8_t kY = 150, kU = 100, kV = 160;
    Frame frame = make_yuv420p_frame(17, 11, kY, kU, kV, /*align=*/64);

    const syp_status st = renderer.present(frame, 0);
    CHECK_EQ(st, syp_status{SYP_OK});

    std::vector<uint8_t> rgba;
    int32_t w = 0, h = 0;
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));
    CHECK_EQ(w, int32_t{17});
    CHECK_EQ(h, int32_t{11});
    CHECK_EQ(rgba.size(), size_t{17 * 11 * 4});

    // 17x11 < 720p，且 present() 恒以 UNSPECIFIED 调用 select_color_
    // matrix()：矩阵推定成 BT.601，range 推定成 limited——跟生产接线的
    // 真实路径一致，不是测试自己另外选的参数。
    const RefRgb expect = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                /*bt709=*/false, /*full_range=*/false);

    // 全部 17*11 个像素都要跟同一个预期颜色接近——如果 pack_plane_rows()
    // 在生产接线里退化成整块拷（没有正确处理 stride），padding 里的
    // 0xEE sentinel 会被当成后续行的有效数据往前"吃"，读出来的颜色会
    // 明显偏离这里算出的预期值（尤其是后面几行，偏差会越滚越大），这条
    // 断言会抓到（反向自检：临时让 pack_plane_
    // rows() 退化成整块 memcpy，本用例真的红了）。
    CHECK(all_pixels_close(rgba, w, h, expect));
}

TEST_CASE(object_level_present_actually_uses_selected_matrix_for_720p_frame) {
    // object_level_color_matrix_choice_is_logged_and_cached_per_size 只
    // 验证了"日志里报了 BT.709"；这条用例额外验证"送给着色器的 uniform
    // 真的是 BT.709"这件事本身——两者理论上应该永远一致（都读同一个
    // impl_->cached_choice），但"日志说了 A、实际送进 GPU 的是 B"这种
    // 分叉是完全可能被看漏的那类 bug（打日志的那行和填
    // ColorParamsGpu 的那行是两处独立的赋值），只有真的读回渲染结果
    // 才能确认它们没有分叉。
    MetalRenderer renderer;
    if (!gpu_ready_or_fail(renderer)) return;

    // y/u/v 选得让 BT.601 与 BT.709 两套矩阵算出的 RGB 差距远大于
    // kByteTolerance（实测 R 差 14、G 差 29、B 差 5），
    // 不会因为容差碰巧掩盖矩阵选错的问题。
    constexpr uint8_t kY = 100, kU = 180, kV = 200;
    Frame frame = make_yuv420p_frame(1280, 720, kY, kU, kV, /*align=*/32);
    CHECK_EQ(renderer.present(frame, 0), syp_status{SYP_OK});

    std::vector<uint8_t> rgba;
    int32_t w = 0, h = 0;
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));

    const RefRgb expect_bt709 = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                      /*bt709=*/true, /*full_range=*/false);
    CHECK(all_pixels_close(rgba, w, h, expect_bt709));

    // 对照：如果实现偷偷用了 BT.601（uniform 没接对），结果应该明显偏离
    // ——这条断言本身不是"要求它失败"，是确认我们选的测试数据真的有
    // 区分力（BT.601 的预期颜色不应该也满足 all_pixels_close）。
    const RefRgb expect_bt601 = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                      /*bt709=*/false, /*full_range=*/false);
    CHECK(!all_pixels_close(rgba, w, h, expect_bt601));
}

// present() 此前恒以 AVCOL_SPC_
// UNSPECIFIED/AVCOL_RANGE_UNSPECIFIED 调用 select_color_matrix()，"取
// 得到真值就用真值"这条分支结构性地永远走不到。Frame::colorspace()/
// color_range() 落地之后，下面两条用例直接验证"真的从 AVFrame 读出了
// 真值、真的传下去、真的影响了 GPU 输出"——不是再测一遍
// select_color_matrix() 本身（第 1 段纯函数用例已经测过判定逻辑）。
TEST_CASE(object_level_present_uses_real_colorspace_over_resolution_presumption) {
    // 640x480 是 SD 尺寸：如果 present() 仍然恒传 UNSPECIFIED，分辨率
    // 推定规则（<720p）会选 BT.601。这里显式给 AVFrame 设
    // colorspace=AVCOL_SPC_BT709——一旦 present() 真的读出了这个真值，
    // 结果必须是 BT.709，而不是分辨率推定出来的 BT.601。这是本文件
    // 唯一一条"真实 colorspace 压过分辨率推定"的用例；反向自检时把
    // present() 改回硬编码 UNSPECIFIED，本用例确实会红。
    MetalRenderer renderer;
    if (!gpu_ready_or_fail(renderer)) return;

    // 复用 object_level_present_actually_uses_selected_matrix_for_720p_frame
    // 同一组 y/u/v（BT.601 与 BT.709 算出的 RGB 差距远大于
    // kByteTolerance：R 差 14、G 差 29、B 差 5），保证判据有区分力。
    constexpr uint8_t kY = 100, kU = 180, kV = 200;
    Frame frame = make_yuv420p_frame(640, 480, kY, kU, kV, /*align=*/32, /*pad_sentinel=*/0xEE,
                                      /*colorspace=*/AVCOL_SPC_BT709,
                                      /*color_range=*/AVCOL_RANGE_UNSPECIFIED);
    CHECK_EQ(renderer.present(frame, 0), syp_status{SYP_OK});

    std::vector<uint8_t> rgba;
    int32_t w = 0, h = 0;
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));

    // color_range 仍未标注 → 推定 limited，跟生产接线一致。
    const RefRgb expect_bt709 = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                      /*bt709=*/true, /*full_range=*/false);
    CHECK(all_pixels_close(rgba, w, h, expect_bt709));

    // 对照：分辨率推定（640x480 < 720p）会选 BT.601——如果 present()
    // 没有真的读出 colorspace、仍然按分辨率推定，输出会是这一套颜色。
    const RefRgb expect_presumed_bt601 = reference_yuv_to_rgb(
        kY / 255.0, kU / 255.0, kV / 255.0, /*bt709=*/false, /*full_range=*/false);
    CHECK(!all_pixels_close(rgba, w, h, expect_presumed_bt601));
}

TEST_CASE(object_level_present_uses_real_full_range_from_avframe) {
    // AVCOL_RANGE_JPEG（full range）——这是唯一能杀掉"full_range
    // uniform 被硬编码成 0u、完全忽略 cached_choice.full_range"这类回归
    // 的用例：24 条旧用例全部只用过恒为 AVCOL_RANGE_UNSPECIFIED 的路径，
    // 推定出的 full_range 恒为 false，跟"压根没看 cached_choice、写死 0u"
    // 这个错误实现产出完全相同的值，区分不出来。这里显式设置
    // AVCOL_RANGE_JPEG，这个错误实现会让这条用例真的红。
    MetalRenderer renderer;
    if (!gpu_ready_or_fail(renderer)) return;

    // colorspace 留 UNSPECIFIED：640x480 < 720p，矩阵推定成 BT.601——
    // 跟下面的期望值矩阵参数（bt709=false）对应，本用例只关心 range 这
    // 一半有没有真的接到 GPU，不需要 colorspace 也是真值。
    constexpr uint8_t kY = 100, kU = 180, kV = 200;
    Frame frame = make_yuv420p_frame(640, 480, kY, kU, kV, /*align=*/32, /*pad_sentinel=*/0xEE,
                                      /*colorspace=*/AVCOL_SPC_UNSPECIFIED,
                                      /*color_range=*/AVCOL_RANGE_JPEG);
    CHECK_EQ(renderer.present(frame, 0), syp_status{SYP_OK});

    std::vector<uint8_t> rgba;
    int32_t w = 0, h = 0;
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));

    const RefRgb expect_full = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                     /*bt709=*/false, /*full_range=*/true);
    CHECK(all_pixels_close(rgba, w, h, expect_full));

    // 对照：limited-range 的预期颜色（差距 R~11/G~11/B~10，远大于
    // kByteTolerance）不应该也满足——确认这组测试数据真的能区分"full_range
    // 真的接到了 GPU"与"恒被当成 limited 处理"这两种实现。
    const RefRgb expect_limited = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                        /*bt709=*/false, /*full_range=*/false);
    CHECK(!all_pixels_close(rgba, w, h, expect_limited));
}

// 上面两条用例各自只让一个轴非默认（colorspace 真值 + range 默认；
// colorspace 默认 + range 真值），没有一条同时让两个轴都非默认（比如
// BT.709 + full range 一起生效）。这是有意的，不是漏了：
//   - select_color_matrix() 作为纯函数早就覆盖了"两个输入都是真值"的
//     组合（第 1 段 select_color_matrix_uses_real_colorspace_when_
//     available 等用例），判定逻辑本身不缺这一格。
//   - 这两条用例要证明的不是判定逻辑，是接线——真实 AVFrame 字段 →
//     Frame::colorspace()/color_range() → present() → ColorParamsGpu
//     → 着色器——而接线本身是按轴独立的：colorspace 那条线（f.
//     colorspace() 有没有真的读出来、有没有真的传给 select_color_
//     matrix() 的第一个参数）跟 range 那条线（f.color_range()/
//     full_range uniform）是两处独立赋值，互不经过对方。两条各证一条
//     轴，接线层面没有第三条"组合"路径需要单独证明。

// 色彩矩阵缓存键此前只有 (width,height)。这里把
// f.colorspace()/f.color_range() 真接上之后，缓存键没跟着改——同一个
// renderer、同一个分辨率下，第 2 帧起无论 colorspace/color_range 怎么变
// 都会沿用第 1 帧算出来的矩阵。
//
// 这条缺陷躲过了全部 26 条既有用例，原因是结构性的：**每条用例都各建一个
// 新 renderer**，缓存里永远是空的；已有用例只测了"尺寸变了要
// 失效"，反方向（尺寸不变、色彩参数变）零覆盖。所以这条用例的形状本身就
// 是判据的一部分——**全程只用一个 renderer、全程同一个 640x480 尺寸**。
//
// 实测（同一个 renderer，全部 640x480）：
//   #1 cs=UNSPEC           → rgba 213 19 203（推定 BT.601 limited）
//   #2 cs=BT709            → rgba 213 19 203  ← 沿用第 1 帧的矩阵
//   #3 cs=BT709 range=JPEG → rgba 213 19 203  ← 同上
//   对照：全新 renderer 同参数 → 227 48 208 / 214 56 197
// 下面三段断言就是把这三步各自钉死：每一步都断言"这一帧该有的颜色"，并
// 断言"上一帧那套颜色不成立"——后半句才是真正杀掉沿用缓存的那一半。
TEST_CASE(object_level_same_size_different_colorspace_invalidates_matrix_cache) {
    MetalRenderer renderer;
    if (!gpu_ready_or_fail(renderer)) return;

    // 跟上面几条 GPU 用例同一组 y/u/v：BT.601 与 BT.709 的结果差
    // R 14 / G 29 / B 5，limited 与 full 的结果差约 R 11 / G 11 / B 10，
    // 都远大于 kByteTolerance=3，判据有区分力。
    constexpr uint8_t kY = 100, kU = 180, kV = 200;
    constexpr int32_t kW = 640, kH = 480;   // 三帧同尺寸，全程不变

    const RefRgb bt601_limited = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                       /*bt709=*/false, /*full_range=*/false);
    const RefRgb bt709_limited = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                       /*bt709=*/true, /*full_range=*/false);
    const RefRgb bt709_full    = reference_yuv_to_rgb(kY / 255.0, kU / 255.0, kV / 255.0,
                                                       /*bt709=*/true, /*full_range=*/true);

    std::vector<uint8_t> rgba;
    int32_t              w = 0, h = 0;

    // #1 两个轴都未标注：640x480 < 720p → 推定 BT.601 + limited。
    Frame f1 = make_yuv420p_frame(kW, kH, kY, kU, kV, /*align=*/32, /*pad_sentinel=*/0xEE,
                                   /*colorspace=*/AVCOL_SPC_UNSPECIFIED,
                                   /*color_range=*/AVCOL_RANGE_UNSPECIFIED);
    CHECK_EQ(renderer.present(f1, 0), syp_status{SYP_OK});
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));
    CHECK(all_pixels_close(rgba, w, h, bt601_limited));

    // #2 同一个 renderer、同一个尺寸，只换 colorspace → 必须重新选矩阵。
    Frame f2 = make_yuv420p_frame(kW, kH, kY, kU, kV, /*align=*/32, /*pad_sentinel=*/0xEE,
                                   /*colorspace=*/AVCOL_SPC_BT709,
                                   /*color_range=*/AVCOL_RANGE_UNSPECIFIED);
    CHECK_EQ(renderer.present(f2, 0), syp_status{SYP_OK});
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));
    CHECK(all_pixels_close(rgba, w, h, bt709_limited));
    CHECK(!all_pixels_close(rgba, w, h, bt601_limited));   // 沿用第 1 帧的矩阵 → 红

    // #3 再只换 color_range → 同样必须重新选。
    Frame f3 = make_yuv420p_frame(kW, kH, kY, kU, kV, /*align=*/32, /*pad_sentinel=*/0xEE,
                                   /*colorspace=*/AVCOL_SPC_BT709,
                                   /*color_range=*/AVCOL_RANGE_JPEG);
    CHECK_EQ(renderer.present(f3, 0), syp_status{SYP_OK});
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));
    CHECK(all_pixels_close(rgba, w, h, bt709_full));
    CHECK(!all_pixels_close(rgba, w, h, bt709_limited));   // 沿用第 2 帧的矩阵 → 红

    // #4 回到两个轴都未标注：缓存键是"相等就复用"，不是"变过就永远重算"
    //    ——回到第 1 帧的参数必须回到第 1 帧的颜色。
    Frame f4 = make_yuv420p_frame(kW, kH, kY, kU, kV, /*align=*/32, /*pad_sentinel=*/0xEE,
                                   /*colorspace=*/AVCOL_SPC_UNSPECIFIED,
                                   /*color_range=*/AVCOL_RANGE_UNSPECIFIED);
    CHECK_EQ(renderer.present(f4, 0), syp_status{SYP_OK});
    CHECK(renderer.debug_copy_output_rgba(rgba, w, h));
    CHECK(all_pixels_close(rgba, w, h, bt601_limited));
}

TEST_CASE(object_level_color_matrix_choice_is_logged_and_cached_per_size) {
    MetalRenderer renderer;
    if (!gpu_ready_or_fail(renderer)) return;

    {
        LogCapture capture;
        // 1280x720：恰好落在"≥720p"边界，presumed 一定为 true，矩阵
        // 一定是 BT.709——日志里两者都要出现。
        Frame frame720 = make_yuv420p_frame(1280, 720, 128, 128, 128, /*align=*/32);
        CHECK_EQ(renderer.present(frame720, 0), syp_status{SYP_OK});
        CHECK(any_log_contains("BT.709"));
        CHECK(any_log_contains("presumed"));
        const size_t logs_after_first = g_captured_logs.size();

        // 同一个渲染器、同一个尺寸再 present() 一次：color matrix 选择
        // 按 (width,height) 缓存（metal_renderer.mm 的 Impl::has_cached_
        // choice），不应该再打第二条"color matrix"日志——真实播放里
        // 30/60fps 每帧都打一行会是纯噪音。用日志总条数不变来判定，而
        // 不是精确数着"color matrix"这几个字出现几次，避免跟其它无关
        // 日志的具体条数耦合。
        CHECK_EQ(renderer.present(frame720, 0), syp_status{SYP_OK});
        CHECK_EQ(g_captured_logs.size(), logs_after_first);
    }

    {
        LogCapture capture;
        // 换一个 <720p 的尺寸：缓存必须失效，重新算、重新打日志，选出
        // BT.601。
        Frame frame_small = make_yuv420p_frame(64, 48, 128, 128, 128, /*align=*/32);
        CHECK_EQ(renderer.present(frame_small, 0), syp_status{SYP_OK});
        CHECK(any_log_contains("BT.601"));
        CHECK(any_log_contains("presumed"));
    }
}

TEST_CASE(object_level_device_available_but_pipeline_build_failure_is_reported) {
    // 取代原来的
    // object_level_ready_reflects_construction_success——那条用例是
    // "if (!ready()) skip; CHECK(ready());" 的同义反复，任何情况下都
    // 不可能变红，零信息量。
    //
    // 这条用例做的是 gpu_ready_or_fail() 同一层判定的显式版本，外加一条
    // 它没有做的事：设备存在但管线/着色器没建起来时，构造期必须打过一
    // 行能定位到"着色器"或"管线"这个层面的错误日志（metal_renderer.mm
    // 构造函数里三处失败分支的日志文案都含"着色器"或"管线"二字），不能
    // 是"无声失败、只留一个布尔"。这台开发机有真实 GPU，
    // device_available() 预期恒为 true，因此 ready() 也预期恒为
    // true——下面 "设备存在但管线未建成" 这个分支在正常运行下预期永远
    // 不触发；它的价值在反向自检里：把
    // video_shaders.metal 改坏、重编之后，这条分支必须真的被走到、且日志断言
    // 必须真的通过。
    LogCapture capture;
    MetalRenderer renderer;
    if (!renderer.device_available()) {
        std::printf("  SKIP：本机没有可用的 Metal 设备（device_available()==false）\n");
        return;
    }
    CHECK(renderer.ready());
    if (!renderer.ready()) {
        CHECK(any_log_contains("着色器") || any_log_contains("管线"));
    }
}

TEST_CASE(present_rejects_non_positive_width_or_height) {
    // metal_renderer.h 明文承诺
    // "SYP_ERR_INVALID_ARG — Frame 本身无效，或宽高非正"，但"宽高非正"
    // 这一半此前零覆盖。present() 里这条检查排在 ready() 判定之后（见
    // metal_renderer.mm::present()），所以需要设备真的可用才能走到——
    // 用跟第 3 段一致的 gpu_ready_or_fail() 守卫，而不是当成"不需要
    // GPU"的用例，避免把它错放进第 2 段。
    //
    // 两个帧都不调用 av_frame_get_buffer()——present() 应该在碰任何
    // 平面数据之前就因为宽高非正而返回，构造一个"格式合法但没有真实
    // buffer"的 Frame 足够触发这条检查，不需要真的分配像素数据。
    MetalRenderer renderer;
    if (!gpu_ready_or_fail(renderer)) return;

    Frame zero_width = make_frame_with_fmt(AV_PIX_FMT_YUV420P, 0, 64);
    CHECK_EQ(renderer.present(zero_width, 0), syp_status{SYP_ERR_INVALID_ARG});

    Frame negative_height = make_frame_with_fmt(AV_PIX_FMT_YUV420P, 64, -1);
    CHECK_EQ(renderer.present(negative_height, 0), syp_status{SYP_ERR_INVALID_ARG});
}

// =======================================================================
// 第 4 段：VideoToolbox NV12 帧经 CVMetalTextureCache 零拷贝
// =======================================================================

namespace {
bool rgba_close(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(int(a[i]) - int(b[i])) > tol) return false;
    return true;
}
}  // namespace

// 同一组 (y, cb, cr)、同一 range：NV12 零拷贝路径与 yuv420p 上传路径的输出逐像素一致（容差 1）。
TEST_CASE(nv12_pixel_buffer_renders_same_as_yuv420p) {
    for (bool full : {false, true}) {
        MetalRenderer r;
        if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
        REQUIRE(r.ready());
        std::vector<uint8_t> a, b;
        int32_t w = 0, h = 0;
        REQUIRE(r.present(syp::test::make_yuv420p_frame(64, 32, 120, 90, 180, full, 0), 0) == SYP_OK);
        REQUIRE(r.debug_copy_output_rgba(a, w, h));
        REQUIRE(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, full, 40000), 0) == SYP_OK);
        REQUIRE(r.debug_copy_output_rgba(b, w, h));
        CHECK(rgba_close(a, b, 1));
    }
}

// 结构性验收 2：硬解帧零 CPU 上传；软解帧每帧一次。
TEST_CASE(nv12_path_never_uploads_through_cpu) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    // present 不阻塞，逐帧先等空闲，保证每帧都真的提交（否则会合法地 BUSY）。
    for (int i = 0; i < 5; ++i) {
        r.wait_until_idle();
        REQUIRE(r.present(syp::test::make_nv12_frame(64, 32, 100, 128, 128, false, i * 40000), 0) == SYP_OK);
    }
    CHECK_EQ(r.debug_cpu_upload_count(), int64_t{0});
    for (int i = 0; i < 3; ++i) {
        r.wait_until_idle();
        REQUIRE(r.present(syp::test::make_yuv420p_frame(64, 32, 100, 128, 128, false, i * 40000), 0) == SYP_OK);
    }
    CHECK_EQ(r.debug_cpu_upload_count(), int64_t{3});
}

TEST_CASE(videotoolbox_frame_with_non_nv12_buffer_is_not_implemented) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    CHECK_EQ(r.present(syp::test::make_bgra_frame(64, 32), 0), syp_status{SYP_ERR_NOT_IMPLEMENTED});
}

// CVPixelBuffer 亮度平面比 Frame 声明的宽高小时必须拒绝——
// 否则 nv12_to_rgba 按 out_tex（Frame 宽高）铺线程，会越界读 GPU 纹理。
TEST_CASE(videotoolbox_frame_larger_than_pixel_buffer_is_invalid_arg) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    CHECK_EQ(r.present(syp::test::make_nv12_frame_with_declared_size(64, 32, 128, 32), 0),
             syp_status{SYP_ERR_INVALID_ARG});
    CHECK_EQ(r.present(syp::test::make_nv12_frame_with_declared_size(64, 32, 64, 64), 0),
             syp_status{SYP_ERR_INVALID_ARG});
    CHECK_EQ(r.debug_cpu_upload_count(), int64_t{0});
}

// 生产上屏入口。离屏 CAMetalLayer 上 present 的 drawable 计数、回读不受影响、
// 摘掉 layer 后不再 present drawable。
TEST_CASE(present_to_offscreen_metal_layer_succeeds_and_readback_still_works) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_offscreen_metal_layer(320, 180);
    r.set_output_layer(layer);
    // drawable 计数在完成回调里（异步），逐帧等空闲后再断言。
    for (int i = 0; i < 3; ++i) {
        r.wait_until_idle();
        CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, i * 40000), 0),
                 syp_status{SYP_OK});
    }
    r.wait_until_idle();
    CHECK_EQ(r.debug_presented_drawable_count(), int64_t{3});
    std::vector<uint8_t> rgba;
    int32_t w = 0, h = 0;
    CHECK(r.debug_copy_output_rgba(rgba, w, h));   // 读 out_tex，与是否有 layer 无关
    r.set_output_layer(nullptr);
    CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 200000), 0),
             syp_status{SYP_OK});
    r.wait_until_idle();
    CHECK_EQ(r.debug_presented_drawable_count(), int64_t{3});   // 离屏后不再 present drawable
    syp::test::release_metal_layer(layer);
}

// set_output_layer() 不跨线程改 CALayer。tiny_test 在 main() 里跑用例，
// 上一条用例的"未配置 layer + 主线程调用 → 就地补配置"走的是主线程分支；这里用
// std::thread 构造"不在主线程"的两种情形。
TEST_CASE(set_output_layer_off_main_thread_refuses_unconfigured_layer) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_offscreen_metal_layer(320, 180);   // device=nil，未配置
    std::thread([&] { r.set_output_layer(layer); }).join();
    CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 0),
             syp_status{SYP_OK});                                      // 仍可离屏呈现
    r.wait_until_idle();
    CHECK_EQ(r.debug_presented_drawable_count(), int64_t{0});          // 但没挂上 layer
    syp::test::release_metal_layer(layer);
}

TEST_CASE(set_output_layer_off_main_thread_accepts_preconfigured_layer) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
    std::thread([&] { r.set_output_layer(layer); }).join();
    CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 0),
             syp_status{SYP_OK});
    r.wait_until_idle();
    CHECK_EQ(r.debug_presented_drawable_count(), int64_t{1});
    r.set_output_layer(nullptr);
    syp::test::release_metal_layer(layer);
}

// #52：present 不阻塞——挂离屏 layer 连续提交，不等待 GPU，每次调用 < 50ms，
// 结果只可能是 OK 或 BUSY；等空闲后必定能再次成功。
TEST_CASE(present_never_blocks_and_returns_ok_or_busy) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(1920, 1080);
    r.set_output_layer(layer);
    // 预热一帧、不计时：首帧要建 kMaxInflightFrames 组 1080p 槽纹理 + CVMetalTextureCache
    // 映射，是一次性建设开销而非"等 GPU"（TSan 下实测约 200ms，稳态约 35ms）。
    REQUIRE(r.present(syp::test::make_nv12_frame(1920, 1080, 120, 90, 180, false, 0), 0) == SYP_OK);
    r.wait_until_idle();
    int ok = 0, busy = 0;
    for (int i = 0; i < 30; ++i) {
        // 造帧不计时：helper 逐像素填色度，TSan 下 1080p 一帧约 30ms，会淹没被测的 present。
        const auto frame = syp::test::make_nv12_frame(1920, 1080, 120, 90, 180, false, (i + 1) * 40000);
        const auto t0 = std::chrono::steady_clock::now();
        const syp_status st = r.present(frame, 0);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 50);
        CHECK(st == SYP_OK || st == SYP_ERR_BUSY);
        if (st == SYP_OK) ++ok; else ++busy;
    }
    CHECK(ok > 0);
    std::printf("  [inflight] ok=%d busy=%d\n", ok, busy);
    r.wait_until_idle();
    CHECK_EQ(r.present(syp::test::make_nv12_frame(1920, 1080, 120, 90, 180, false, 2000000), 0), SYP_OK);
    r.wait_until_idle();
    syp::test::release_metal_layer(layer);
}

// due_in_us 转成 presentDrawable:atTime: 的目标时刻。
TEST_CASE(present_schedules_drawable_at_now_plus_due) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
    r.set_output_layer(layer);
    const double before = syp::test::host_time_now();
    REQUIRE(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 30000) == SYP_OK);
    const double after = syp::test::host_time_now();
    const double target = r.debug_last_present_host_time();
    CHECK(target >= before + 0.030 - 0.005);
    CHECK(target <= after + 0.030 + 0.005);
    r.wait_until_idle();
    syp::test::release_metal_layer(layer);
}

// 槽轮换后连续 yuv420p 帧的回读仍是最后一帧的内容（CPU 不改写 GPU 正在读的纹理）。
TEST_CASE(yuv420p_slots_keep_readback_consistent_without_waiting) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    std::vector<uint8_t> expect, got;
    int32_t w = 0, h = 0;
    REQUIRE(r.present(syp::test::make_yuv420p_frame(64, 32, 200, 60, 60, false, 0), 0) == SYP_OK);
    REQUIRE(r.debug_copy_output_rgba(expect, w, h));
    for (int i = 0; i < 6; ++i) {
        const syp_status st = r.present(syp::test::make_yuv420p_frame(64, 32, (i % 2) ? 30 : 220, 128, 128, false, i), 0);
        CHECK(st == SYP_OK || st == SYP_ERR_BUSY);
    }
    r.wait_until_idle();
    REQUIRE(r.present(syp::test::make_yuv420p_frame(64, 32, 200, 60, 60, false, 99), 0) == SYP_OK);
    REQUIRE(r.debug_copy_output_rgba(got, w, h));
    CHECK(got == expect);
}

// ---- 在途门、槽轮换、layer 路径上限的确定性用例 ----

namespace {
int64_t elapsed_us(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
        .count();
}
}  // namespace

// (a) 离屏：扣住名额归还（debug 钩子），第 kMaxInflightFrames+1 次必定 BUSY，且立即返回。
TEST_CASE(offscreen_gate_full_returns_busy_immediately_deterministic) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    const auto warm = syp::test::make_yuv420p_frame(3840, 2160, 120, 90, 180, false, 0);
    REQUIRE(r.present(warm, 0) == SYP_OK);   // 预热：建槽纹理
    r.wait_until_idle();
    std::vector<syp::media::Frame> fs;
    for (int i = 0; i < 6; ++i) fs.push_back(syp::test::make_yuv420p_frame(3840, 2160, 120, 90, 180, false, i + 1));
    r.debug_hold_gate_releases(true);
    int ok = 0, busy = 0;
    for (int i = 0; i < 6; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const syp_status st = r.present(fs[static_cast<size_t>(i)], 0);
        const int64_t us = elapsed_us(t0);
        if (st == SYP_OK) {
            ++ok;
        } else {
            CHECK_EQ(st, SYP_ERR_BUSY);
            ++busy;
            CHECK(us < 5000);   // 满了不碰 GPU：只是一次原子比较
        }
    }
    CHECK_EQ(ok, 3);   // kMaxInflightFrames
    CHECK_EQ(busy, 3);
    CHECK_EQ(r.debug_inflight_count(), 3);
    r.debug_hold_gate_releases(false);
    r.wait_until_idle();
    CHECK_EQ(r.debug_inflight_count(), 0);
    CHECK_EQ(r.present(fs[0], 0), SYP_OK);
    r.wait_until_idle();
    CHECK_EQ(r.debug_gpu_error_count(), int64_t{0});
}

// (b) 槽轮换：连续成功的 present 依次使用不同的槽（即便中间等到空闲）。
TEST_CASE(consecutive_presents_rotate_slots) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    CHECK_EQ(r.debug_last_slot(), -1);
    std::vector<int32_t> used;
    for (int i = 0; i < 6; ++i) {
        REQUIRE(r.present(syp::test::make_yuv420p_frame(64, 32, 120, 90, 180, false, i), 0) == SYP_OK);
        used.push_back(r.debug_last_slot());
        r.wait_until_idle();
    }
    for (size_t i = 0; i < used.size(); ++i) {
        CHECK(used[i] >= 0 && used[i] < 3);
        if (i >= 1) CHECK(used[i] != used[i - 1]);
        if (i >= 2) CHECK(used[i] != used[i - 2]);
    }
    // 在途时也不复用：扣住归还，三帧三个槽。
    r.debug_hold_gate_releases(true);
    std::vector<int32_t> held;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, i), 0) == SYP_OK);
        held.push_back(r.debug_last_slot());
    }
    CHECK(held[0] != held[1] && held[1] != held[2] && held[0] != held[2]);
    r.debug_hold_gate_releases(false);
    r.wait_until_idle();
}

// (c1) 挂 layer：上限 = min(kMaxInflightFrames, maximumDrawableCount-1)（默认 3 → 2）。
// 扣住归还后第 3 次必定 BUSY、立即返回、且没去取 drawable（drawable 计数不变）。
TEST_CASE(layer_gate_caps_at_drawable_count_minus_one_and_skips_next_drawable) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(1920, 1080);
    r.set_output_layer(layer);
    REQUIRE(r.present(syp::test::make_nv12_frame(1920, 1080, 120, 90, 180, false, 0), 0) == SYP_OK);
    r.wait_until_idle();
    const int64_t presented_before = r.debug_presented_drawable_count();
    std::vector<syp::media::Frame> fs;
    for (int i = 0; i < 4; ++i) fs.push_back(syp::test::make_nv12_frame(1920, 1080, 120, 90, 180, false, i + 1));
    r.debug_hold_gate_releases(true);
    CHECK_EQ(r.present(fs[0], 30000), SYP_OK);
    CHECK_EQ(r.present(fs[1], 30000), SYP_OK);
    for (int i = 2; i < 4; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_EQ(r.present(fs[static_cast<size_t>(i)], 30000), SYP_ERR_BUSY);
        CHECK(elapsed_us(t0) < 5000);
    }
    CHECK_EQ(r.debug_inflight_count(), 2);
    r.debug_hold_gate_releases(false);
    r.wait_until_idle();
    CHECK_EQ(r.debug_inflight_count(), 0);
    CHECK_EQ(r.debug_presented_drawable_count(), presented_before + 2);   // BUSY 的两帧没取 drawable
    CHECK_EQ(r.debug_gpu_error_count(), int64_t{0});
    // 名额必须经 presentedHandler 归还，而不是靠丢失兜底（兜底会掩盖主路径失效）。
    CHECK_EQ(r.debug_reclaimed_permit_count(), int64_t{0});
    syp::test::release_metal_layer(layer);
}

// (c2) 挂 layer 快速连发（due=30ms，不扣归还）：名额在 presentedHandler 归还，
// 调用不会等到 nextDrawable 的 1 秒超时。无窗口 CAMetalLayer 的 nextDrawable
// 按显示刷新节拍放行（实测每次约 8ms，与空闲 drawable 数无关），所以这里只能
// 断言"不超过数个刷新周期"；上屏 layer 上实测 <5ms。
TEST_CASE(layer_rapid_presents_with_due_never_stall) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(1920, 1080);
    r.set_output_layer(layer);
    REQUIRE(r.present(syp::test::make_nv12_frame(1920, 1080, 120, 90, 180, false, 0), 0) == SYP_OK);
    r.wait_until_idle();
    std::vector<syp::media::Frame> fs;
    for (int i = 0; i < 30; ++i) fs.push_back(syp::test::make_nv12_frame(1920, 1080, 120, 90, 180, false, i + 1));
    int ok = 0, busy = 0;
    int64_t max_us = 0;
    for (auto& f : fs) {
        const auto t0 = std::chrono::steady_clock::now();
        const syp_status st = r.present(f, 30000);
        const int64_t us = elapsed_us(t0);
        max_us = std::max(max_us, us);
        CHECK(st == SYP_OK || st == SYP_ERR_BUSY);
        CHECK(us < 50000);
        if (st == SYP_OK) ++ok; else ++busy;
        CHECK(r.debug_inflight_count() <= 2);
    }
    CHECK(ok > 0);
    std::printf("  [layer rapid] ok=%d busy=%d max_us=%lld\n", ok, busy, static_cast<long long>(max_us));
    r.wait_until_idle();
    CHECK_EQ(r.debug_inflight_count(), 0);
    CHECK_EQ(r.debug_reclaimed_permit_count(), int64_t{0});   // 全部经 presentedHandler 归还
    syp::test::release_metal_layer(layer);
}

// 析构时仍扣着归还：析构必须先放掉扣住的名额再等空闲，不能挂死。
TEST_CASE(destructor_releases_held_gate_and_does_not_hang) {
    {
        MetalRenderer r;
        if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
        REQUIRE(r.ready());
        void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
        r.set_output_layer(layer);
        r.debug_hold_gate_releases(true);
        CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 30000), SYP_OK);
        syp::test::release_metal_layer(layer);   // 渲染器仍强引用 layer
    }
    CHECK(true);
}

// 超范围的 due 在渲染器内夹到 [0, 1s]。
TEST_CASE(present_clamps_due_to_one_second) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
    r.set_output_layer(layer);
    const double before = syp::test::host_time_now();
    REQUIRE(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 60'000'000) == SYP_OK);
    const double after = syp::test::host_time_now();
    CHECK(r.debug_last_present_host_time() >= before + 1.0 - 0.005);
    CHECK(r.debug_last_present_host_time() <= after + 1.0 + 0.005);
    r.wait_until_idle();
    syp::test::release_metal_layer(layer);
}

// 分辨率变化重建槽后，回读不会读到旧/未初始化的槽：重建即作废 has_output。
TEST_CASE(readback_false_after_resize_until_new_frame_committed) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    std::vector<uint8_t> px;
    int32_t w = 0, h = 0;
    REQUIRE(r.present(syp::test::make_yuv420p_frame(64, 32, 200, 60, 60, false, 0), 0) == SYP_OK);
    REQUIRE(r.debug_copy_output_rgba(px, w, h));
    // 换分辨率但本帧在 pack 阶段失败（stride 太窄）→ 已重建、未提交。
    AVFrame* av = av_frame_alloc();
    av->format = AV_PIX_FMT_YUV420P;
    av->width  = 128;
    av->height = 64;
    REQUIRE(av_frame_get_buffer(av, 0) >= 0);
    av->linesize[0] = 1;   // stride < 宽：pack_plane_rows 拒绝
    const auto bad = syp::media::Frame::from_av(av, AVRational{1, 1000000}, true);
    CHECK_EQ(r.present(bad, 0), SYP_ERR_INVALID_ARG);
    CHECK(!r.debug_copy_output_rgba(px, w, h));
    r.wait_until_idle();
}

// ---- 名额兜底回收 ----

// drawable 正常完成却始终等不到 presentedHandler（用钩子让回调不归还来模拟）。
// 名额不能永久泄漏：命令缓冲完成且超过 提交时刻 + kMaxPresentDueUs + 余量 后，调用方线程
// 强制认领归还——此前满额 BUSY，此后恢复 OK。
TEST_CASE(layer_missing_presented_handler_permit_reclaimed_by_fallback) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
    r.set_output_layer(layer);
    r.debug_set_permit_reclaim_after_ms(500);   // 缩短兜底期限（默认 1.2s）以省测试时间
    r.debug_suppress_presented_release(true);
    CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 0), SYP_OK);
    CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 1), 0), SYP_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // 完成回调早已跑完
    CHECK_EQ(r.debug_inflight_count(), 2);                          // 名额仍被占着
    const auto f3 = syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 2);
    auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(r.present(f3, 0), SYP_ERR_BUSY);
    CHECK(elapsed_us(t0) < 5000);
    // 过了兜底期限后，present 先回收过期名额——轮询到 OK，上限 3s。
    r.debug_suppress_presented_release(false);
    const auto t_poll = std::chrono::steady_clock::now();
    syp_status st3 = SYP_ERR_BUSY;
    while (st3 != SYP_OK && std::chrono::steady_clock::now() - t_poll < std::chrono::seconds(3)) {
        st3 = r.present(f3, 0);
        if (st3 != SYP_OK) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(st3, SYP_OK);
    // 两帧期限相差几毫秒：轮询可能在只回收了第一个时就拿到 OK。
    CHECK(r.debug_reclaimed_permit_count() >= 1);
    r.wait_until_idle();
    CHECK_EQ(r.debug_inflight_count(), 0);
    CHECK_EQ(r.debug_reclaimed_permit_count(), int64_t{2});   // f3 正常经 presentedHandler 归还
    syp::test::release_metal_layer(layer);
}

// 同样的漏回调下，析构（经 wait_until_idle）有界返回，不挂死。
TEST_CASE(destructor_is_bounded_when_presented_handler_never_releases) {
    void* layer = nullptr;
    std::chrono::steady_clock::time_point t0;
    {
        MetalRenderer r;
        if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
        REQUIRE(r.ready());
        layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
        r.set_output_layer(layer);
        r.debug_set_permit_reclaim_after_ms(200);
        r.debug_suppress_presented_release(true);
        CHECK_EQ(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 30000), SYP_OK);
        t0 = std::chrono::steady_clock::now();
    }
    const int64_t us = elapsed_us(t0);
    std::printf("  [dtor] %lld us\n", static_cast<long long>(us));
    CHECK(us < 3'000'000);
    syp::test::release_metal_layer(layer);
}

// 区分"layer 路径在 presentedHandler 归还"与"在完成时归还"。
// 不能靠 due 做无钩子判别：无窗口 CAMetalLayer 上 presentedHandler 与完成回调几乎同时到达
// （实测相差 <10µs，与 due=0/30ms/500ms、64px/1080p 无关）。
// 改用 suppress 钩子（只关 presentedHandler 的归还，不碰完成回调）：完成之后名额必须仍被占着；
// 若归还点退回完成回调，这里读到 0。
TEST_CASE(layer_permit_not_released_by_completion_alone) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
    r.set_output_layer(layer);
    r.debug_set_permit_reclaim_after_ms(300);
    r.debug_suppress_presented_release(true);
    REQUIRE(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, 0), 0) == SYP_OK);
    std::vector<uint8_t> px;
    int32_t w = 0, h = 0;
    REQUIRE(r.debug_copy_output_rgba(px, w, h));   // 等命令缓冲完成
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 完成回调跑完
    CHECK_EQ(r.debug_inflight_count(), 1);
    CHECK_EQ(r.debug_gpu_error_count(), int64_t{0});
    r.debug_suppress_presented_release(false);
    r.wait_until_idle();   // 兜底回收（期限 300ms）
    CHECK_EQ(r.debug_inflight_count(), 0);
    CHECK_EQ(r.debug_reclaimed_permit_count(), int64_t{1});
    syp::test::release_metal_layer(layer);
}

// 主路径——不借助任何钩子，挂 layer 的 due=0 帧，名额约一个刷新周期内经
// presentedHandler 归还（远早于兜底期限），且兜底计数为 0。删掉 presentedHandler 里的
// 归还，这里会超时。
TEST_CASE(layer_permit_returns_via_presented_handler_promptly) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
    r.set_output_layer(layer);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(r.present(syp::test::make_nv12_frame(64, 32, 120, 90, 180, false, i), 0) == SYP_OK);
        const auto t0 = std::chrono::steady_clock::now();
        while (r.debug_inflight_count() > 0 && elapsed_us(t0) < 100'000)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        CHECK_EQ(r.debug_inflight_count(), 0);
    }
    CHECK_EQ(r.debug_reclaimed_permit_count(), int64_t{0});
    r.wait_until_idle();
    syp::test::release_metal_layer(layer);
}

// =====================================================================
// blit 按显示几何出图（旋转 / gravity / SAR）。
//
// 旋转与 gravity 做在 blit pass 里；blit 在生产路径上只画进 framebufferOnly 的
// drawable，不可回读。所以几何用例一律经 debug_blit_to_bgra()——它与生产 present()
// 调用同一个 encode_blit()，blit 参数同由 blit_transform() 算。回读是 BGRA8。
// =====================================================================

namespace {

struct Bgra {
    int b, g, r, a;
};

// 归一化坐标 (fx, fy) 处的像素，(0,0) = 左上。
Bgra bgra_at(const std::vector<uint8_t>& buf, int32_t w, int32_t h, double fx, double fy) {
    const auto x = std::clamp(static_cast<int32_t>(fx * w), int32_t{0}, w - 1);
    const auto y = std::clamp(static_cast<int32_t>(fy * h), int32_t{0}, h - 1);
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    return Bgra{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};
}

int color_dist(const Bgra& a, const Bgra& b) {
    return std::max({std::abs(a.b - b.b), std::abs(a.g - b.g), std::abs(a.r - b.r)});
}

// YUV→RGB 与线性采样有舍入：同色容差 12；"不同色"要求差得远（四象限色两两差 >200）。
bool same_color(const Bgra& a, const Bgra& b) { return color_dist(a, b) <= 12; }
bool diff_color(const Bgra& a, const Bgra& b) { return color_dist(a, b) >= 100; }
bool is_black(const Bgra& p) { return std::max({p.b, p.g, p.r}) <= 8; }

// 一次 present + 一次 debug_blit_to_bgra（dst = n×n）。
std::vector<uint8_t> blit_square(MetalRenderer& r, const Frame& f, int32_t n) {
    std::vector<uint8_t> out;
    if (r.present(f, 0) != SYP_OK) return {};
    if (!r.debug_blit_to_bgra(n, n, out)) return {};
    return out;
}

// 正立四象限（左上、右上、左下、右下）。
struct Quads {
    Bgra tl, tr, bl, br;
};
Quads quads(const std::vector<uint8_t>& buf, int32_t n) {
    return Quads{bgra_at(buf, n, n, 0.25, 0.25), bgra_at(buf, n, n, 0.75, 0.25),
                 bgra_at(buf, n, n, 0.25, 0.75), bgra_at(buf, n, n, 0.75, 0.75)};
}

// 素材自检：四个象限两两可区分、且都不是黑色——否则后面的旋转断言可能假绿。
void check_quadrants_distinct(const Quads& q) {
    const Bgra all[4] = {q.tl, q.tr, q.bl, q.br};
    for (int i = 0; i < 4; ++i) {
        CHECK(!is_black(all[i]));
        for (int j = i + 1; j < 4; ++j) CHECK(diff_color(all[i], all[j]));
    }
}

// 用例 a / c 共用：Resize + 方形源 + 方形目标，rotation 0 与 90。
void check_quarter_turn_clockwise(const Frame& f) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    constexpr int32_t kN = 64;
    r.set_gravity(syp::media::Gravity::Resize);   // 排除 letterbox 的干扰
    r.set_source_geometry(1, 1, 0);
    const auto up_buf = blit_square(r, f, kN);
    REQUIRE(up_buf.size() == static_cast<size_t>(kN * kN * 4));
    r.set_source_geometry(1, 1, 90);
    const auto rot_buf = blit_square(r, f, kN);
    REQUIRE(rot_buf.size() == static_cast<size_t>(kN * kN * 4));

    const Quads up  = quads(up_buf, kN);
    const Quads rot = quads(rot_buf, kN);
    check_quadrants_distinct(up);
    // 顺时针 90°：左上 → 右上，右上 → 右下，右下 → 左下，左下 → 左上。
    CHECK(same_color(up.tl, rot.tr));
    CHECK(same_color(up.tr, rot.br));
    CHECK(same_color(up.br, rot.bl));
    CHECK(same_color(up.bl, rot.tl));
    // 反向守护：旋转前后左上确实变了（恒等实现下上面四条不可能全成立，这条再加一道）。
    CHECK(diff_color(up.tl, rot.tl));
}

}  // namespace

// MetalRenderer 必须真的覆盖两个新接口——IVideoRenderer 给了默认空实现，
// 编译器不会替我们发现"忘了覆盖"。未覆盖时 &MetalRenderer::f 的类型是
// void (IVideoRenderer::*)(...)，与基类成员指针同型。
TEST_CASE(metal_renderer_overrides_the_geometry_seams) {
    static_assert(!std::is_same_v<decltype(&syp::media::IVideoRenderer::set_source_geometry),
                                  decltype(&MetalRenderer::set_source_geometry)>,
                  "MetalRenderer 没有覆盖 set_source_geometry");
    static_assert(!std::is_same_v<decltype(&syp::media::IVideoRenderer::set_gravity),
                                  decltype(&MetalRenderer::set_gravity)>,
                  "MetalRenderer 没有覆盖 set_gravity");
    CHECK(true);   // 判据全在上面的 static_assert（编译期）；这一行只为让用例计数
}

// a. 软解 yuv420p：顺时针 90°。
TEST_CASE(blit_rotates_quarter_turn_clockwise_yuv420p) {
    check_quarter_turn_clockwise(syp::test::make_yuv420p_quadrant_frame(64, 64));
}

// c. 硬解 NV12：同一组旋转断言。软解与硬解写的是同一张 out_tex，这条钉的是
// "硬解帧也走到了几何变换"。
TEST_CASE(blit_rotates_quarter_turn_clockwise_nv12) {
    check_quarter_turn_clockwise(syp::test::make_nv12_quadrant_frame(64, 64));
}

// d. 180° 与 270°：着色器 rot_uv 的四个 case 每个都要被某条用例钉住。
TEST_CASE(blit_rotates_half_and_three_quarter_turns) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    constexpr int32_t kN = 64;
    const Frame f = syp::test::make_yuv420p_quadrant_frame(64, 64);
    r.set_gravity(syp::media::Gravity::Resize);
    r.set_source_geometry(1, 1, 0);
    const auto up_buf = blit_square(r, f, kN);
    REQUIRE(!up_buf.empty());
    r.set_source_geometry(1, 1, 180);
    const auto half_buf = blit_square(r, f, kN);
    REQUIRE(!half_buf.empty());
    r.set_source_geometry(1, 1, 270);
    const auto tq_buf = blit_square(r, f, kN);
    REQUIRE(!tq_buf.empty());

    const Quads up = quads(up_buf, kN);
    const Quads h  = quads(half_buf, kN);
    const Quads t  = quads(tq_buf, kN);
    check_quadrants_distinct(up);
    // 180°：左上 ↔ 右下，右上 ↔ 左下。
    CHECK(same_color(up.tl, h.br));
    CHECK(same_color(up.tr, h.bl));
    CHECK(same_color(up.br, h.tl));
    // 顺时针 270°（= 逆时针 90°）：左上 → 左下，右上 → 左上，右下 → 右上。
    CHECK(same_color(up.tl, t.bl));
    CHECK(same_color(up.tr, t.tl));
    CHECK(same_color(up.br, t.tr));
    // 270° 与 90° 方向相反：若 case 1/3 写成同一个方向，下面这条会红。
    CHECK(diff_color(up.tl, t.tr));
}

// b. 2:1 源 + 方形目标：Fit 上下黑边；Fill 无黑边且裁掉左右；Resize 无黑边、不裁。
// 素材是四条竖条（红/绿/蓝/白）：四象限帧关于中线对称缩放后每半边颜色不变，
// 分辨不出"裁掉左右"。
TEST_CASE(blit_gravity_fit_fill_resize_on_wide_source) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    constexpr int32_t kN = 64;
    const syp::test::YuvColor stripes[4] = {syp::test::kQuadrantColors[0], syp::test::kQuadrantColors[1],
                                            syp::test::kQuadrantColors[2], syp::test::kQuadrantColors[3]};
    const Frame f = syp::test::make_yuv420p_grid_frame(128, 64, 4, 1, stripes);
    r.set_source_geometry(1, 1, 0);

    r.set_gravity(syp::media::Gravity::AspectFit);
    const auto fit = blit_square(r, f, kN);
    REQUIRE(!fit.empty());
    r.set_gravity(syp::media::Gravity::AspectFill);
    const auto fill = blit_square(r, f, kN);
    REQUIRE(!fill.empty());
    r.set_gravity(syp::media::Gravity::Resize);
    const auto resize = blit_square(r, f, kN);
    REQUIRE(!resize.empty());

    // 顶边中点：Fit 是黑边（画面只占中间一半高），Fill / Resize 都铺满了。
    CHECK(is_black(bgra_at(fit, kN, kN, 0.5, 0.02)));
    CHECK(!is_black(bgra_at(fill, kN, kN, 0.5, 0.02)));
    CHECK(!is_black(bgra_at(resize, kN, kN, 0.5, 0.02)));
    // 画面中间一行：Fit 也有画面（排除"全黑"实现）。
    CHECK(!is_black(bgra_at(fit, kN, kN, 0.1, 0.5)));
    // 左边缘：Resize 不裁 ⇒ 第 1 条（红）；Fill 裁掉左右各 1/4 ⇒ 第 2 条（绿）。
    const Bgra resize_left = bgra_at(resize, kN, kN, 0.1, 0.5);
    const Bgra fill_left   = bgra_at(fill, kN, kN, 0.1, 0.5);
    CHECK(diff_color(resize_left, fill_left));
    CHECK(same_color(fill_left, bgra_at(resize, kN, kN, 0.375, 0.5)));   // Fill 左缘 = Resize 的第 2 条
}

// f. 钉住"先缩放再旋转"的顺序。blit_transform 在**旋转之后**的显示空间
// 里算 uv_scale，所以顺序只在"奇数象限 + uv_scale≠1（AspectFill）"同时成立时才有差别；
// 上面的用例从不同时满足（90/270 全是 Resize，Fill 全在 0°，180° 取负与缩放可交换）。
//
// 素材：128×64 四竖条 C0..C3（从左到右，各占纹理 x 的 1/4）。目标 64×64。
// 90°：显示尺寸 64×128（竖），src_aspect 0.5 < dst_aspect 1 ⇒ Fill 给 uv_scale = (1, 0.5)
// （裁屏幕的纵向）。记屏幕中心化坐标 t = s - 0.5（y 向下）。
//   正确（先缩放再旋转）：p = rot_uv((tx, 0.5·ty), 1) = (0.5·ty, -tx) ⇒ 纹理 x = 0.5 + 0.5·ty
//     ⇒ 屏幕自上而下只看到 C1、C2（竖条转成横带 C0..C3，Fill 裁掉上下各一带）。
//   错误顺序（先旋转再缩放）：p = rot_uv(t, 1) * (1, 0.5) = (ty, -0.5·tx) ⇒ 纹理 x = 0.5 + ty
//     ⇒ 自上而下 C0..C3 全在（裁到了错的轴——纹理 y，而横带与纹理 y 无关）。
//   采样点 s = (0.5, 0.1)：ty = -0.4 ⇒ 正确 x = 0.30（C1），错误顺序 x = 0.10（C0）；
//           s = (0.5, 0.9)：ty = +0.4 ⇒ 正确 x = 0.70（C2），错误顺序 x = 0.90（C3）。
// 270°：rot_uv(·, 3) = (-t.y, t.x)，uv_scale 同上。
//   正确：纹理 x = 0.5 - 0.5·ty ⇒ (0.5,0.1) → 0.70（C2），(0.5,0.9) → 0.30（C1）；
//   错误顺序：纹理 x = 0.5 - ty      ⇒ (0.5,0.1) → 0.90（C3），(0.5,0.9) → 0.10（C0）。
// 各采样点离条带边界 ≥ 0.05（≥6 个纹素），线性采样不会混色。
TEST_CASE(blit_scales_before_rotating_on_odd_quadrants_with_fill) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    constexpr int32_t kN = 64;
    const syp::test::YuvColor stripes[4] = {syp::test::kQuadrantColors[0], syp::test::kQuadrantColors[1],
                                            syp::test::kQuadrantColors[2], syp::test::kQuadrantColors[3]};
    const Frame f = syp::test::make_yuv420p_grid_frame(128, 64, 4, 1, stripes);

    // 参照：0°、Resize，四条竖条原样铺满 ⇒ 从各条中心取参照色。
    r.set_gravity(syp::media::Gravity::Resize);
    r.set_source_geometry(1, 1, 0);
    const auto ref = blit_square(r, f, kN);
    REQUIRE(!ref.empty());
    Bgra c[4];
    for (int i = 0; i < 4; ++i) c[i] = bgra_at(ref, kN, kN, 0.125 + 0.25 * i, 0.5);
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) CHECK(diff_color(c[i], c[j]));

    r.set_gravity(syp::media::Gravity::AspectFill);
    r.set_source_geometry(1, 1, 90);
    const auto q1 = blit_square(r, f, kN);
    REQUIRE(!q1.empty());
    CHECK(same_color(bgra_at(q1, kN, kN, 0.5, 0.1), c[1]));   // 正确：C1
    CHECK(diff_color(bgra_at(q1, kN, kN, 0.5, 0.1), c[0]));   // 错误顺序：C0
    CHECK(same_color(bgra_at(q1, kN, kN, 0.5, 0.9), c[2]));   // 正确：C2
    CHECK(diff_color(bgra_at(q1, kN, kN, 0.5, 0.9), c[3]));   // 错误顺序：C3

    r.set_source_geometry(1, 1, 270);
    const auto q3 = blit_square(r, f, kN);
    REQUIRE(!q3.empty());
    CHECK(same_color(bgra_at(q3, kN, kN, 0.5, 0.1), c[2]));   // 正确：C2
    CHECK(diff_color(bgra_at(q3, kN, kN, 0.5, 0.1), c[3]));   // 错误顺序：C3
    CHECK(same_color(bgra_at(q3, kN, kN, 0.5, 0.9), c[1]));   // 正确：C1
    CHECK(diff_color(bgra_at(q3, kN, kN, 0.5, 0.9), c[0]));   // 错误顺序：C0
}

// e. SAR 真的接进了 blit：64×64 编码尺寸、SAR 2:1 ⇒ 显示 2:1，方形目标 AspectFit 有上下黑边；
// SAR 1:1 时没有。
TEST_CASE(blit_applies_sample_aspect_ratio) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    constexpr int32_t kN = 64;
    const Frame f = syp::test::make_yuv420p_quadrant_frame(64, 64);
    r.set_gravity(syp::media::Gravity::AspectFit);
    r.set_source_geometry(1, 1, 0);
    const auto square = blit_square(r, f, kN);
    REQUIRE(!square.empty());
    r.set_source_geometry(2, 1, 0);
    const auto wide = blit_square(r, f, kN);
    REQUIRE(!wide.empty());
    CHECK(!is_black(bgra_at(square, kN, kN, 0.25, 0.1)));
    CHECK(is_black(bgra_at(wide, kN, kN, 0.25, 0.1)));
}

// 默认几何（从未调用 set_*）= 1:1、不旋转、AspectFit：与此前的 aspect_fit_scale 一致。
TEST_CASE(blit_default_geometry_is_aspect_fit_upright) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    constexpr int32_t kN = 64;
    const auto buf = blit_square(r, syp::test::make_yuv420p_quadrant_frame(128, 64), kN);
    REQUIRE(!buf.empty());
    CHECK(is_black(bgra_at(buf, kN, kN, 0.5, 0.1)));   // 2:1 源进方形目标 ⇒ letterbox
    CHECK(diff_color(bgra_at(buf, kN, kN, 0.25, 0.4), bgra_at(buf, kN, kN, 0.75, 0.4)));
}

// debug_blit_to_bgra 的边界：无帧 / 目标尺寸非正返回 false。
TEST_CASE(debug_blit_to_bgra_rejects_no_frame_and_bad_size) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    std::vector<uint8_t> out;
    CHECK(!r.debug_blit_to_bgra(16, 16, out));   // 从未 present
    REQUIRE(r.present(syp::test::make_yuv420p_quadrant_frame(16, 16), 0) == SYP_OK);
    CHECK(!r.debug_blit_to_bgra(0, 16, out));
    CHECK(!r.debug_blit_to_bgra(16, -1, out));
    CHECK(out.empty());
    CHECK(r.debug_blit_to_bgra(16, 16, out));
    CHECK_EQ(out.size(), size_t{16 * 16 * 4});
}

// 生产路径冒烟：挂离屏 layer、非默认几何与 gravity，present() 仍 SYP_OK、drawable
// 计数前进、GPU 无执行错误——证明生产 blit 走新的 encode_blit 不崩、uniform 大小对得上。
TEST_CASE(layer_present_with_rotation_and_fill_smoke) {
    MetalRenderer r;
    if (!r.device_available()) { std::printf("  SKIP：无 Metal 设备\n"); return; }
    REQUIRE(r.ready());
    void* layer = syp::test::make_configured_offscreen_metal_layer(320, 180);
    r.set_output_layer(layer);
    r.set_source_geometry(4, 3, 90);
    r.set_gravity(syp::media::Gravity::AspectFill);
    REQUIRE(r.present(syp::test::make_nv12_quadrant_frame(64, 32), 0) == SYP_OK);
    r.wait_until_idle();
    r.set_gravity(syp::media::Gravity::Resize);
    r.set_source_geometry(1, 1, 270);
    REQUIRE(r.present(syp::test::make_yuv420p_quadrant_frame(64, 32), 0) == SYP_OK);
    r.wait_until_idle();
    CHECK_EQ(r.debug_presented_drawable_count(), int64_t{2});
    CHECK_EQ(r.debug_gpu_error_count(), int64_t{0});
    r.set_output_layer(nullptr);
    syp::test::release_metal_layer(layer);
}

int main() { return tiny_test_main(); }
