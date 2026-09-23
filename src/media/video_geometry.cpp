#include "media/video_geometry.h"

#include <cmath>
#include <limits>

namespace syp::media {

int32_t normalize_rotation(double deg) noexcept {
    if (!std::isfinite(deg)) return 0;
    long q = std::lround(deg / 90.0);
    q %= 4;
    if (q < 0) q += 4;
    return static_cast<int32_t>(q) * 90;
}

DisplaySize display_size(int32_t src_w, int32_t src_h,
                         int32_t sar_num, int32_t sar_den,
                         int32_t rotation_deg) noexcept {
    if (src_w <= 0 || src_h <= 0) return DisplaySize{0, 0};

    double w = static_cast<double>(src_w);
    if (sar_num > 0 && sar_den > 0) {
        w = w * static_cast<double>(sar_num) / static_cast<double>(sar_den);
    }
    // 夹到 [1, INT32_MAX] 再转 int32：畸形超大 SAR（例如
    // sar_num = INT32_MAX）下 w 远超 int32 范围，直接 static_cast 会回绕成
    // 负数/小数（640 × INT32_MAX 恰好回绕成 −640，再被下限兜成宽 1）。先比较
    // 再 lround，也避开"lround 的结果超出 long 范围"那条域错误。
    constexpr int32_t kMaxDim = std::numeric_limits<int32_t>::max();
    int32_t ow = (w >= static_cast<double>(kMaxDim)) ? kMaxDim
                                                     : static_cast<int32_t>(std::lround(w));
    int32_t oh = src_h;
    if (ow < 1) ow = 1;

    const int32_t rot = normalize_rotation(static_cast<double>(rotation_deg));
    if (rot == 90 || rot == 270) return DisplaySize{oh, ow};
    return DisplaySize{ow, oh};
}

BlitParams blit_transform(int32_t src_w, int32_t src_h,
                          int32_t sar_num, int32_t sar_den,
                          int32_t rotation_deg, Gravity gravity,
                          double dst_w, double dst_h) noexcept {
    BlitParams p{{1.0f, 1.0f}, {1.0f, 1.0f}, 0u};
    if (src_w <= 0 || src_h <= 0) return p;
    if (!(dst_w > 0.0) || !(dst_h > 0.0)) return p;   // NaN 也走这条

    const DisplaySize ds = display_size(src_w, src_h, sar_num, sar_den, rotation_deg);
    if (ds.width <= 0 || ds.height <= 0) return p;

    p.rot_quadrant = static_cast<uint32_t>(normalize_rotation(
                         static_cast<double>(rotation_deg)) / 90);

    const double src_aspect = static_cast<double>(ds.width) / static_cast<double>(ds.height);
    const double dst_aspect = dst_w / dst_h;

    switch (gravity) {
        case Gravity::Resize:
            break;
        case Gravity::AspectFit:
            if (src_aspect > dst_aspect) {
                p.pos_scale[1] = static_cast<float>(dst_aspect / src_aspect);
            } else {
                p.pos_scale[0] = static_cast<float>(src_aspect / dst_aspect);
            }
            break;
        case Gravity::AspectFill:
            if (src_aspect > dst_aspect) {
                p.uv_scale[0] = static_cast<float>(dst_aspect / src_aspect);
            } else {
                p.uv_scale[1] = static_cast<float>(src_aspect / dst_aspect);
            }
            break;
    }
    return p;
}

}  // namespace syp::media
