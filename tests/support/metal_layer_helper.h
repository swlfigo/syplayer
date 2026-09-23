// metal_layer_helper.h — Metal 相关测试共用的造帧/造 layer 工具。
//
// 本头刻意不出现任何 ObjC 类型：test_metal_renderer.cpp 是普通 .cpp，
// ObjC 对象一律以 void* 过界（CFBridgingRetain 过来的 +1 引用），实现在
// metal_layer_helper.mm 里。
//
// 造出来的 VIDEOTOOLBOX Frame 形状与 FFmpeg videotoolbox hwaccel 输出一致：
// format = AV_PIX_FMT_VIDEOTOOLBOX，data[3] 是 CVPixelBufferRef，buf[0]
// 持有该 buffer 的一份引用（Frame 析构 → av_frame_free → CVPixelBufferRelease）。
#pragma once

#include "media/frame.h"

#include <cstdint>

namespace syp::test {

// 造一个 IOSurface 支撑、内容为常量 (y, cb, cr) 的 NV12 CVPixelBuffer，包成
// AV_PIX_FMT_VIDEOTOOLBOX 的 Frame（data[3] 持有 buf 引用，Frame 析构时释放）。
// full_range 决定 420f / 420v 以及 AVFrame::color_range；colorspace 恒标 BT.709。
syp::media::Frame make_nv12_frame(int32_t w, int32_t h, uint8_t y, uint8_t cb, uint8_t cr,
                                  bool full_range, int64_t pts_us);

// 声明宽高（AVFrame::width/height = frame_w×frame_h）与 CVPixelBuffer 实际尺寸
// （buf_w×buf_h）不一致的 NV12 VIDEOTOOLBOX Frame，测渲染器对"声明比 buffer 大"的拒绝。
syp::media::Frame make_nv12_frame_with_declared_size(int32_t buf_w, int32_t buf_h,
                                                     int32_t frame_w, int32_t frame_h);

// 非 NV12（32BGRA）的 VIDEOTOOLBOX Frame，测渲染器对未知 CVPixelBuffer 格式的拒绝。
syp::media::Frame make_bgra_frame(int32_t w, int32_t h);

// 常量 (y, cb, cr) 的软件 yuv420p Frame，color_range/colorspace 与 make_nv12_frame 同规则，
// 用来跟 NV12 路径逐像素对照。
syp::media::Frame make_yuv420p_frame(int32_t w, int32_t h, uint8_t y, uint8_t cb, uint8_t cr,
                                     bool full_range, int64_t pts_us);

// 分块着色的帧，用来测 blit 的几何变换（旋转/gravity）。画面切成 cols×rows 个
// 等大的块，cells 行主序给出每块的 (y, cb, cr)（cells 至少 cols*rows 个）。limited
// range、BT.709。w、h 应是 2*cols、2*rows 的倍数，块边界才与 4:2:0 色度块对齐。
// 素材必须**非对称**：对称图案在旋转前后逐像素相同，旋转用例会假绿。
struct YuvColor {
    uint8_t y, cb, cr;
};
syp::media::Frame make_yuv420p_grid_frame(int32_t w, int32_t h, int32_t cols, int32_t rows,
                                          const YuvColor* cells);
// 同上，NV12（VIDEOTOOLBOX，IOSurface 支撑）版——硬解路径。
syp::media::Frame make_nv12_grid_frame(int32_t w, int32_t h, int32_t cols, int32_t rows,
                                       const YuvColor* cells);

// 四象限帧：左上红、右上绿、左下蓝、右下白（BT.709 limited 的近似值）。四色在 BGRA
// 回读后两两相差很大，且都不是黑色（与 letterbox 黑边可区分）。
inline constexpr YuvColor kQuadrantColors[4] = {
    {63, 102, 240},    // 左上：红
    {173, 42, 26},     // 右上：绿
    {32, 240, 118},    // 左下：蓝
    {235, 128, 128},   // 右下：白
};
inline syp::media::Frame make_yuv420p_quadrant_frame(int32_t w, int32_t h) {
    return make_yuv420p_grid_frame(w, h, 2, 2, kQuadrantColors);
}
inline syp::media::Frame make_nv12_quadrant_frame(int32_t w, int32_t h) {
    return make_nv12_grid_frame(w, h, 2, 2, kQuadrantColors);
}

// 离屏 CAMetalLayer（不挂任何视图），drawableSize = w×h。返回 CFBridgingRetain
// 的 CAMetalLayer*，调用方必须用 release_metal_layer() 释放。
void* make_offscreen_metal_layer(int32_t w, int32_t h);
// 同上，但已按 MetalRenderer::set_output_layer() 的线程契约预先配好
// device=系统默认设备、pixelFormat=BGRA8Unorm、framebufferOnly=YES（调用方线程上配置，
// 离屏 layer 不挂视图树，不受主线程约束）。
void* make_configured_offscreen_metal_layer(int32_t w, int32_t h);
void  release_metal_layer(void* layer);

// CACurrentMediaTime()（秒）——与 MetalRenderer::debug_last_present_host_time() 同一时基。
double host_time_now();

}  // namespace syp::test
