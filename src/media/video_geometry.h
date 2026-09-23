// video_geometry.h — 显示几何的唯一真相。
//
// 为什么是独立的平台无关文件：MetalRenderer 没有自动化覆盖，
// 几何算错了在那一侧看不出来。把计算放在
// 纯函数里，就能在平台无关用例里穷举——与 vt_supports() 同一形状。
//
// 坐标约定（改这里之前先读完）：
//   - pos_scale 作用在 NDC 顶点上，(1,1) = 铺满 drawable。
//   - uv_scale 作用在**屏幕空间**的 uv 上、以 (0.5,0.5) 为中心，
//     (1,1) = 不裁剪。着色器先缩放再旋转，所以这里的两个分量对应的是
//     屏幕的横/纵，不是源纹理的横/纵。
//   - rot_quadrant = k 表示"画面需要**顺时针**旋转 k*90° 才正立"。
//     着色器对纹理坐标施加的是它的逆。
#pragma once

#include <cstdint>

namespace syp::media {

enum class Gravity { AspectFit, AspectFill, Resize };

struct BlitParams {
    float    pos_scale[2];
    float    uv_scale[2];
    uint32_t rot_quadrant;
};

struct DisplaySize {
    int32_t width;
    int32_t height;
};

// 规整到 {0, 90, 180, 270}。非有限值 → 0；负角与 >360 都映射进 [0,360)；
// 非 90° 倍数就近取整（本轮不支持任意角）。
int32_t normalize_rotation(double deg) noexcept;

// SAR 拉伸后按旋转交换宽高。src 非正 → {0,0}。sar 任一分量非正 → 按 1:1。
DisplaySize display_size(int32_t src_w, int32_t src_h,
                         int32_t sar_num, int32_t sar_den,
                         int32_t rotation_deg) noexcept;

// dst 非正或 src 非正时返回恒等参数（铺满、不裁剪、不旋转），不除零。
BlitParams blit_transform(int32_t src_w, int32_t src_h,
                          int32_t sar_num, int32_t sar_den,
                          int32_t rotation_deg, Gravity gravity,
                          double dst_w, double dst_h) noexcept;

}  // namespace syp::media
