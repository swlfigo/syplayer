// video_shaders.metal — 视频帧 → RGBA 转换核（原名 yuv420p.metal；现在还承载 NV12 核）。
//
// src/platform/apple/metal_renderer.mm 用
// -[MTLDevice newLibraryWithSource:options:error:] 在运行期把本文件的
// 源码编译成 id<MTLLibrary>——不走 `xcrun metal`/`metallib` 的离线编译
// 流水线（本机 Xcode 装的是标准工具链，没有单独的 "Metal Toolchain" 组
// 件；跑 `xcrun metal` 会报 "missing Metal Toolchain: use `xcodebuild
// -downloadComponent MetalToolchain`"，四套构建 Debug/Release/
// ASan+UBSan/TSan 都不能依赖这个开发机没装、且需要联网才能装的东西）。
// 运行期编译走的是 Metal.framework 自带的编译器，Xcode 标准工具链自带，
// 不需要额外组件。本文件的源码由 tools/gen-embedded-header.sh 在构建期
// 内嵌进一个生成头（顶层 CMakeLists.txt 与 tools/check-deploy-target.sh
// 都调用同一份脚本，避免两处各写一份嵌入逻辑然后漂移）。
//
// 两个核：yuv420p_to_rgba（软解 yuv420p，三平面 CPU 上传）与文件末尾的
// nv12_to_rgba（VideoToolbox 硬解 NV12，经 CVMetalTextureCache 零
// 拷贝取纹理）。别的格式 metal_renderer.mm 在调用本核之前已经用
// MetalRenderer::is_pix_fmt_supported() / CVPixelBuffer 格式检查挡掉（见该
// 文件顶部注释：静默画错比报错更难查，这是需要特别小心的风险点）。下面
// "平面几何"一段描述的是 yuv420p 核。
//
// 平面几何：
//   Y 平面：MTLPixelFormatR8Unorm，宽 × 高。
//   U/V 平面：MTLPixelFormatR8Unorm 各一张，(宽+1)/2 × (高+1)/2（4:2:0
//   降采样，向上取整——与 FFmpeg yuv420p 色度平面尺寸约定一致，
//   metal_renderer.mm 按同一公式建纹理，见 frame_digest.cpp 对这条
//   约定的说明）。色度用最近邻放大（坐标 gid/2 整除），不做双线性
//   插值——目标是"颜色正确"，不是"视觉最优"。
//
// 色彩矩阵：两套 YCbCr → RGB 转换矩阵都编成常量，由 ColorParams::matrix
// 这个 uniform 在运行时选择用哪一套——BT.601（matrix==0）与 BT.709
// （matrix==1）的转换系数不同，用错一套颜色会整体偏移。full_range 控制
// Y/Cb/Cr 是按 limited（Y: 16-235，
// Cb/Cr: 16-240）还是 full（0-255）反归一化。两个 uniform 都由
// MetalRenderer::select_color_matrix()（可脱离 GPU 单测的纯函数，见
// metal_renderer.h）算出，本核只管拿着用，不做任何推定逻辑——推定逻辑
// 全部留在 CPU 侧的纯函数里，方便单测，也让本核保持"薄到没地方藏逻辑"。

#include <metal_stdlib>
using namespace metal;

struct ColorParams {
    uint matrix;       // 0 = BT.601, 1 = BT.709
    uint full_range;   // 0 = limited/tv range, 1 = full/pc range
};

// BT.601（ITU-R BT.601 / SMPTE 170M）YCbCr → RGB 反变换矩阵，按 Metal
// float3x3 的列主序构造：第一个参数是「Y 分量」那一列，第二个是「Cb」，
// 第三个是「Cr」。系数是这套色彩空间标准反矩阵里公开可查的值。
constant float3x3 kMatBT601 = float3x3(
    float3(1.0, 1.0,       1.0),
    float3(0.0, -0.344136, 1.772),
    float3(1.402, -0.714136, 0.0)
);

// BT.709（ITU-R BT.709，HD 内容默认色彩空间）同一套推导，系数不同。
constant float3x3 kMatBT709 = float3x3(
    float3(1.0, 1.0,        1.0),
    float3(0.0, -0.187324,  1.8556),
    float3(1.5748, -0.468124, 0.0)
);

kernel void yuv420p_to_rgba(texture2d<float, access::read>  yTex   [[texture(0)]],
                             texture2d<float, access::read>  uTex   [[texture(1)]],
                             texture2d<float, access::read>  vTex   [[texture(2)]],
                             texture2d<float, access::write> outTex [[texture(3)]],
                             constant ColorParams&           params [[buffer(0)]],
                             uint2                           gid    [[thread_position_in_grid]]) {
    if (gid.x >= outTex.get_width() || gid.y >= outTex.get_height()) return;

    float y = yTex.read(gid).r;
    const uint2 cgid = gid / 2;
    float u = uTex.read(cgid).r;
    float v = vTex.read(cgid).r;

    if (params.full_range == 0u) {
        // limited/tv range 反归一化：R8Unorm 读出来已经是 [0,1]，这里
        // 先还原成 8-bit 整数尺度，再按 ITU-R BT.601/709 共用的 limited
        // range 公式换算（Y: 16-235 → [0,1]；Cb/Cr: 16-240，中心 128 →
        // [-0.5, 0.5]）。
        y = (y * 255.0 - 16.0)  / 219.0;
        u = (u * 255.0 - 128.0) / 224.0;
        v = (v * 255.0 - 128.0) / 224.0;
    } else {
        // full/pc range：Y 本身已经是 [0,1]，Cb/Cr 只需要把中心从 0.5
        // 移到 0。
        u = u - 0.5;
        v = v - 0.5;
    }

    const float3x3 mat = (params.matrix == 1u) ? kMatBT709 : kMatBT601;
    float3 rgb = mat * float3(y, u, v);
    rgb = clamp(rgb, 0.0, 1.0);

    outTex.write(float4(rgb, 1.0), gid);
}

// NV12（VideoToolbox 输出的 420v/420f）：plane0 = Y（R8），plane1 = CbCr 交织（RG8）。
// 与 yuv420p_to_rgba 同一套矩阵与 range 处理，输出同一张 out_tex。
kernel void nv12_to_rgba(texture2d<float, access::read>  yTex    [[texture(0)]],
                         texture2d<float, access::read>  cbcrTex [[texture(1)]],
                         texture2d<float, access::write> outTex  [[texture(3)]],
                         constant ColorParams&           params  [[buffer(0)]],
                         uint2                           gid     [[thread_position_in_grid]]) {
    if (gid.x >= outTex.get_width() || gid.y >= outTex.get_height()) return;
    float y = yTex.read(gid).r;
    const float2 cbcr = cbcrTex.read(gid / 2).rg;
    float u = cbcr.r;
    float v = cbcr.g;
    if (params.full_range == 0u) {
        y = (y * 255.0 - 16.0)  / 219.0;
        u = (u * 255.0 - 128.0) / 224.0;
        v = (v * 255.0 - 128.0) / 224.0;
    } else {
        u = u - 0.5;
        v = v - 0.5;
    }
    const float3x3 mat = (params.matrix == 1u) ? kMatBT709 : kMatBT601;
    outTex.write(float4(clamp(mat * float3(y, u, v), 0.0, 1.0), 1.0), gid);
}

// blit：把 out_tex 按显示几何画进 CAMetalLayer 的 drawable。
// BlitParams 布局必须与 src/media/video_geometry.h 的 syp::media::BlitParams 逐字段一致
// （metal_renderer.mm 里有 static_assert 钉 sizeof == 20 与各字段偏移）。用 packed_float2
// 而不是 float2：float2 按 8 字节对齐，结构体会被补到 24 字节，与 C++ 侧的 20 字节
// （float[2], float[2], uint32_t）对不上。几何的计算全部在 blit_transform()，这里只执行。
struct BlitVertexOut { float4 pos [[position]]; float2 uv; };
struct BlitParams { packed_float2 pos_scale; packed_float2 uv_scale; uint rot_quadrant; };

// k = 画面需要顺时针旋转 k*90° 才正立 ⇒ 对纹理坐标施加它的逆。t 是以 (0.5,0.5) 为中心的
// 屏幕空间 uv（x 向右、y 向下——uvs 表里 corner(-1,-1) → uv(0,1)，v 是翻转的）。
// ⚠️ case 1 与 case 3 哪个是"顺时针"只由 test_metal_renderer 的象限用例
// （blit_rotates_quarter_turn_clockwise_*、blit_rotates_half_and_three_quarter_turns）判定；
// 若它们变红，**只在这里对调 case 1 与 case 3**，不要去改 video_geometry.cpp 或 demuxer.cpp。
// 最初的写法（case 1 = (-t.y, t.x)）让 90°/270° 两组象限断言
// 全红、180° 全绿——转对了轴、转反了方向；对调后全绿。推导：y 向下的屏幕坐标里视觉
// 顺时针是 (x,y)→(-y,x)，屏幕点 q 要采的纹理点是它的逆 (q.y, -q.x)。
static inline float2 rot_uv(float2 t, uint k) {
    switch (k) {
        case 1u: return float2( t.y, -t.x);
        case 2u: return float2(-t.x, -t.y);
        case 3u: return float2(-t.y,  t.x);
        default: return t;
    }
}

vertex BlitVertexOut blit_vertex(uint vid [[vertex_id]], constant BlitParams& bp [[buffer(0)]]) {
    const float2 corners[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
    const float2 uvs[4]     = { float2(0, 1),   float2(1, 1),  float2(0, 0),  float2(1, 0) };
    BlitVertexOut o;
    o.pos = float4(corners[vid] * float2(bp.pos_scale), 0.0, 1.0);
    // 先缩放（屏幕空间裁剪，AspectFill）再旋转——与 video_geometry.h 的坐标约定一致。
    o.uv  = 0.5 + rot_uv((uvs[vid] - 0.5) * float2(bp.uv_scale), bp.rot_quadrant);
    return o;
}

fragment float4 blit_fragment(BlitVertexOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return src.sample(s, in.uv);
}
