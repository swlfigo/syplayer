// metal_layer_helper.mm — 见 metal_layer_helper.h。ARC 由顶层 CMakeLists.txt
// 对 OBJCXX 全局开启（-fobjc-arc），本文件不再单独设编译选项。

#import "support/metal_layer_helper.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cstring>

namespace syp::test {

namespace {

// AVBufferRef 的 free 回调：归还 CVPixelBufferCreate 给的那一份 +1 引用。
void release_pixel_buffer(void* opaque, uint8_t* /*data*/) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(opaque));
}

// 按 FFmpeg videotoolbox hwaccel 的形状包一层 AVFrame：data[3] = CVPixelBufferRef，
// buf[0] 接管 pb 的所有权。
AVFrame* wrap_pixel_buffer(CVPixelBufferRef pb, int32_t w, int32_t h, int64_t pts) {
    AVFrame* av = av_frame_alloc();
    av->format  = AV_PIX_FMT_VIDEOTOOLBOX;
    av->width   = w;
    av->height  = h;
    av->pts     = pts;
    av->data[3] = reinterpret_cast<uint8_t*>(pb);
    av->buf[0]  = av_buffer_create(reinterpret_cast<uint8_t*>(pb), 1, &release_pixel_buffer, pb, 0);
    return av;
}

// IOSurface 支撑 + Metal 兼容——CVMetalTextureCache 零拷贝的前提，跟 VT 解码输出同一类 buffer。
CVPixelBufferRef create_iosurface_buffer(int32_t w, int32_t h, OSType fmt) {
    NSDictionary* attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                            (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferCreate(kCFAllocatorDefault, static_cast<size_t>(w), static_cast<size_t>(h), fmt,
                        (__bridge CFDictionaryRef)attrs, &pb);
    return pb;
}

}  // namespace

syp::media::Frame make_nv12_frame(int32_t w, int32_t h, uint8_t y, uint8_t cb, uint8_t cr,
                                  bool full_range, int64_t pts_us) {
    const OSType fmt = full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                  : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CVPixelBufferRef pb = create_iosurface_buffer(w, h, fmt);
    if (pb == nullptr) return {};
    CVPixelBufferLockBaseAddress(pb, 0);
    auto* yp = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    auto* cp = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const size_t c_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (size_t r = 0; r < CVPixelBufferGetHeightOfPlane(pb, 0); ++r)
        std::memset(yp + r * y_stride, y, CVPixelBufferGetWidthOfPlane(pb, 0));
    for (size_t r = 0; r < CVPixelBufferGetHeightOfPlane(pb, 1); ++r)
        for (size_t c = 0; c < CVPixelBufferGetWidthOfPlane(pb, 1); ++c) {
            cp[r * c_stride + 2 * c]     = cb;
            cp[r * c_stride + 2 * c + 1] = cr;
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    AVFrame* av = wrap_pixel_buffer(pb, w, h, pts_us);
    av->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    av->colorspace  = AVCOL_SPC_BT709;
    return syp::media::Frame::from_av(av, AVRational{1, 1000000}, true);
}

syp::media::Frame make_nv12_frame_with_declared_size(int32_t buf_w, int32_t buf_h,
                                                     int32_t frame_w, int32_t frame_h) {
    CVPixelBufferRef pb =
        create_iosurface_buffer(buf_w, buf_h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    if (pb == nullptr) return {};
    AVFrame* av = wrap_pixel_buffer(pb, frame_w, frame_h, 0);
    av->color_range = AVCOL_RANGE_MPEG;
    av->colorspace  = AVCOL_SPC_BT709;
    return syp::media::Frame::from_av(av, AVRational{1, 1000000}, true);
}

syp::media::Frame make_bgra_frame(int32_t w, int32_t h) {
    CVPixelBufferRef pb = create_iosurface_buffer(w, h, kCVPixelFormatType_32BGRA);
    if (pb == nullptr) return {};
    return syp::media::Frame::from_av(wrap_pixel_buffer(pb, w, h, 0), AVRational{1, 1000000}, true);
}

syp::media::Frame make_yuv420p_frame(int32_t w, int32_t h, uint8_t y, uint8_t cb, uint8_t cr,
                                     bool full_range, int64_t pts_us) {
    AVFrame* av = av_frame_alloc();
    av->format      = AV_PIX_FMT_YUV420P;
    av->width       = w;
    av->height      = h;
    av->pts         = pts_us;
    av->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    av->colorspace  = AVCOL_SPC_BT709;
    if (av_frame_get_buffer(av, 0) < 0) {
        av_frame_free(&av);
        return {};
    }
    for (int r = 0; r < h; ++r)
        std::memset(av->data[0] + r * av->linesize[0], y, static_cast<size_t>(w));
    for (int r = 0; r < (h + 1) / 2; ++r) {
        std::memset(av->data[1] + r * av->linesize[1], cb, static_cast<size_t>((w + 1) / 2));
        std::memset(av->data[2] + r * av->linesize[2], cr, static_cast<size_t>((w + 1) / 2));
    }
    return syp::media::Frame::from_av(av, AVRational{1, 1000000}, true);
}

syp::media::Frame make_yuv420p_grid_frame(int32_t w, int32_t h, int32_t cols, int32_t rows,
                                          const YuvColor* cells) {
    if (w <= 0 || h <= 0 || cols <= 0 || rows <= 0 || cells == nullptr) return {};
    AVFrame* av = av_frame_alloc();
    av->format      = AV_PIX_FMT_YUV420P;
    av->width       = w;
    av->height      = h;
    av->color_range = AVCOL_RANGE_MPEG;
    av->colorspace  = AVCOL_SPC_BT709;
    if (av_frame_get_buffer(av, 0) < 0) {
        av_frame_free(&av);
        return {};
    }
    // 亮度按像素坐标取块；色度按色度块中心对应的亮度坐标取块。
    auto cell = [&](int32_t x, int32_t y) -> const YuvColor& {
        return cells[(y * rows / h) * cols + (x * cols / w)];
    };
    for (int32_t r = 0; r < h; ++r)
        for (int32_t c = 0; c < w; ++c) av->data[0][r * av->linesize[0] + c] = cell(c, r).y;
    for (int32_t r = 0; r < (h + 1) / 2; ++r)
        for (int32_t c = 0; c < (w + 1) / 2; ++c) {
            const YuvColor& col = cell(c * 2, r * 2);
            av->data[1][r * av->linesize[1] + c] = col.cb;
            av->data[2][r * av->linesize[2] + c] = col.cr;
        }
    return syp::media::Frame::from_av(av, AVRational{1, 1000000}, true);
}

syp::media::Frame make_nv12_grid_frame(int32_t w, int32_t h, int32_t cols, int32_t rows,
                                       const YuvColor* cells) {
    if (w <= 0 || h <= 0 || cols <= 0 || rows <= 0 || cells == nullptr) return {};
    CVPixelBufferRef pb =
        create_iosurface_buffer(w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    if (pb == nullptr) return {};
    auto cell = [&](size_t x, size_t y) -> const YuvColor& {
        const size_t cy = y * static_cast<size_t>(rows) / static_cast<size_t>(h);
        const size_t cx = x * static_cast<size_t>(cols) / static_cast<size_t>(w);
        return cells[cy * static_cast<size_t>(cols) + cx];
    };
    CVPixelBufferLockBaseAddress(pb, 0);
    auto* yp = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    auto* cp = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const size_t c_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (size_t r = 0; r < CVPixelBufferGetHeightOfPlane(pb, 0); ++r)
        for (size_t c = 0; c < CVPixelBufferGetWidthOfPlane(pb, 0); ++c)
            yp[r * y_stride + c] = cell(c, r).y;
    for (size_t r = 0; r < CVPixelBufferGetHeightOfPlane(pb, 1); ++r)
        for (size_t c = 0; c < CVPixelBufferGetWidthOfPlane(pb, 1); ++c) {
            const YuvColor& col  = cell(c * 2, r * 2);
            cp[r * c_stride + 2 * c]     = col.cb;
            cp[r * c_stride + 2 * c + 1] = col.cr;
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    AVFrame* av = wrap_pixel_buffer(pb, w, h, 0);
    av->color_range = AVCOL_RANGE_MPEG;
    av->colorspace  = AVCOL_SPC_BT709;
    return syp::media::Frame::from_av(av, AVRational{1, 1000000}, true);
}

void* make_offscreen_metal_layer(int32_t w, int32_t h) {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.drawableSize  = CGSizeMake(w, h);
    return (void*)CFBridgingRetain(layer);
}

void* make_configured_offscreen_metal_layer(int32_t w, int32_t h) {
    CAMetalLayer* layer   = [CAMetalLayer layer];
    layer.drawableSize    = CGSizeMake(w, h);
    layer.device          = MTLCreateSystemDefaultDevice();
    layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    return (void*)CFBridgingRetain(layer);
}

void release_metal_layer(void* layer) {
    if (layer != nullptr) CFBridgingRelease(layer);
}

double host_time_now() { return CACurrentMediaTime(); }

}  // namespace syp::test
