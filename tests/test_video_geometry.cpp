// test_video_geometry.cpp — 显示几何纯函数的穷举用例。
#include "tiny_test.h"

#include <media/video_geometry.h>

#include <cmath>
#include <cstdint>
#include <limits>

using syp::media::BlitParams;
using syp::media::blit_transform;
using syp::media::DisplaySize;
using syp::media::display_size;
using syp::media::Gravity;
using syp::media::normalize_rotation;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

BlitParams fit(int32_t w, int32_t h, double dw, double dh) {
    return blit_transform(w, h, 1, 1, 0, Gravity::AspectFit, dw, dh);
}

}  // namespace

TEST_CASE(rotation_normalizes_to_four_quadrants) {
    CHECK_EQ(normalize_rotation(0.0), 0);
    CHECK_EQ(normalize_rotation(90.0), 90);
    CHECK_EQ(normalize_rotation(-90.0), 270);
    CHECK_EQ(normalize_rotation(450.0), 90);
    CHECK_EQ(normalize_rotation(-450.0), 270);
    // 就近取整：45 落在 90 那一侧（lround 的半值远离零）
    CHECK_EQ(normalize_rotation(44.0), 0);
    CHECK_EQ(normalize_rotation(46.0), 90);
    CHECK_EQ(normalize_rotation(std::nan("")), 0);
    CHECK_EQ(normalize_rotation(std::numeric_limits<double>::infinity()), 0);
}

TEST_CASE(display_size_applies_sar_then_swaps_on_quarter_turns) {
    CHECK_EQ(display_size(1920, 1080, 1, 1, 0).width, 1920);
    CHECK_EQ(display_size(1920, 1080, 1, 1, 0).height, 1080);
    // 竖屏：1920x1080 + 顺时针 90 ⇒ 显示 1080x1920
    CHECK_EQ(display_size(1920, 1080, 1, 1, 90).width, 1080);
    CHECK_EQ(display_size(1920, 1080, 1, 1, 90).height, 1920);
    CHECK_EQ(display_size(1920, 1080, 1, 1, 270).width, 1080);
    CHECK_EQ(display_size(1920, 1080, 1, 1, 180).width, 1920);
    // 非方像素：720x480 SAR 8:9 ⇒ 640x480
    CHECK_EQ(display_size(720, 480, 8, 9, 0).width, 640);
    CHECK_EQ(display_size(720, 480, 8, 9, 0).height, 480);
    // SAR 未知（FFmpeg 给 0/1）按 1:1
    CHECK_EQ(display_size(720, 480, 0, 1, 0).width, 720);
    // 退化输入
    CHECK_EQ(display_size(0, 480, 1, 1, 0).width, 0);
    CHECK_EQ(display_size(720, -1, 1, 1, 0).height, 0);
}

// 【畸形超大 SAR】640 × INT32_MAX 远超 int32 范围，旧实现
// static_cast<int32_t>(lround(w)) 回绕——640×(2^31−1) = 640×2^31 − 640，
// 模 2^32 恰为 −640，再被"< 1 ⇒ 1"兜成宽 1（一张 1 像素宽的竖条）。夹到
// [1, INT32_MAX]：宽 = INT32_MAX（= 2147483647），高不变；四分之一圈照常互换。
TEST_CASE(display_size_clamps_width_to_int32_range_for_absurd_sar) {
    const int32_t kMax = std::numeric_limits<int32_t>::max();
    CHECK_EQ(display_size(640, 360, kMax, 1, 0).width, 2147483647);
    CHECK_EQ(display_size(640, 360, kMax, 1, 0).height, 360);
    CHECK_EQ(display_size(640, 360, kMax, 1, 90).width, 360);
    CHECK_EQ(display_size(640, 360, kMax, 1, 90).height, 2147483647);
    // 刚好越界一点：2 × (2^31−1) = 4294967294 > INT32_MAX ⇒ 夹到 INT32_MAX
    CHECK_EQ(display_size(2, 1, kMax, 1, 0).width, 2147483647);
    // 另一端：极小 SAR 仍兜底到 1（既有行为）
    CHECK_EQ(display_size(640, 360, 1, kMax, 0).width, 1);
}

TEST_CASE(aspect_fit_letterboxes_on_the_short_axis_only) {
    // 源 16:9 画进 1:1 容器 ⇒ 上下留黑边（纵向缩），横向铺满
    const BlitParams p = fit(1920, 1080, 500.0, 500.0);
    CHECK(near(p.pos_scale[0], 1.0f));
    CHECK(near(p.pos_scale[1], 9.0f / 16.0f));
    CHECK(near(p.uv_scale[0], 1.0f));
    CHECK(near(p.uv_scale[1], 1.0f));
    CHECK_EQ(p.rot_quadrant, 0u);

    // 源 1:1 画进 16:9 容器 ⇒ 左右留黑边
    const BlitParams q = fit(500, 500, 1920.0, 1080.0);
    CHECK(near(q.pos_scale[0], 1080.0f / 1920.0f));
    CHECK(near(q.pos_scale[1], 1.0f));
}

TEST_CASE(aspect_fill_crops_in_uv_and_never_scales_the_quad) {
    // 源 16:9 铺满 1:1 容器 ⇒ 左右裁掉
    const BlitParams p = blit_transform(1920, 1080, 1, 1, 0, Gravity::AspectFill, 500.0, 500.0);
    CHECK(near(p.pos_scale[0], 1.0f));
    CHECK(near(p.pos_scale[1], 1.0f));
    CHECK(near(p.uv_scale[0], 9.0f / 16.0f));
    CHECK(near(p.uv_scale[1], 1.0f));
}

TEST_CASE(resize_is_identity_on_both) {
    const BlitParams p = blit_transform(1920, 1080, 1, 1, 0, Gravity::Resize, 500.0, 500.0);
    CHECK(near(p.pos_scale[0], 1.0f));
    CHECK(near(p.pos_scale[1], 1.0f));
    CHECK(near(p.uv_scale[0], 1.0f));
    CHECK(near(p.uv_scale[1], 1.0f));
}

TEST_CASE(rotation_is_accounted_before_the_aspect_decision) {
    // 1920x1080 转 90° ⇒ 显示 1080x1920（竖），画进 1:1 容器应当是
    // 左右留黑边，而不是像未旋转时那样上下留黑边。
    const BlitParams p = blit_transform(1920, 1080, 1, 1, 90, Gravity::AspectFit, 500.0, 500.0);
    CHECK_EQ(p.rot_quadrant, 1u);
    CHECK(near(p.pos_scale[0], 9.0f / 16.0f));
    CHECK(near(p.pos_scale[1], 1.0f));
}

TEST_CASE(degenerate_inputs_return_identity_without_dividing_by_zero) {
    for (const Gravity g : {Gravity::AspectFit, Gravity::AspectFill, Gravity::Resize}) {
        for (const BlitParams p : {blit_transform(0, 1080, 1, 1, 0, g, 500.0, 500.0),
                                   blit_transform(1920, 0, 1, 1, 0, g, 500.0, 500.0),
                                   blit_transform(1920, 1080, 1, 1, 0, g, 0.0, 500.0),
                                   blit_transform(1920, 1080, 1, 1, 0, g, 500.0, -1.0)}) {
            CHECK(near(p.pos_scale[0], 1.0f));
            CHECK(near(p.pos_scale[1], 1.0f));
            CHECK(near(p.uv_scale[0], 1.0f));
            CHECK(near(p.uv_scale[1], 1.0f));
            CHECK_EQ(p.rot_quadrant, 0u);
        }
    }
}

TEST_CASE(three_gravities_hold_their_structural_invariants_across_the_matrix) {
    const int32_t sars[3][2] = {{1, 1}, {4, 3}, {8, 9}};
    const double dsts[3][2]  = {{1920.0, 1080.0}, {1080.0, 1920.0}, {600.0, 600.0}};
    for (const auto& sar : sars) {
        for (int32_t rot = 0; rot < 360; rot += 90) {
            for (const auto& dst : dsts) {
                const BlitParams fitp = blit_transform(1280, 720, sar[0], sar[1], rot,
                                                       Gravity::AspectFit, dst[0], dst[1]);
                CHECK(near(fitp.uv_scale[0], 1.0f) && near(fitp.uv_scale[1], 1.0f));
                CHECK(near(fitp.pos_scale[0], 1.0f) || near(fitp.pos_scale[1], 1.0f));
                CHECK(fitp.pos_scale[0] <= 1.0f + 1e-5f && fitp.pos_scale[1] <= 1.0f + 1e-5f);

                const BlitParams fillp = blit_transform(1280, 720, sar[0], sar[1], rot,
                                                        Gravity::AspectFill, dst[0], dst[1]);
                CHECK(near(fillp.pos_scale[0], 1.0f) && near(fillp.pos_scale[1], 1.0f));
                CHECK(near(fillp.uv_scale[0], 1.0f) || near(fillp.uv_scale[1], 1.0f));
                CHECK(fillp.uv_scale[0] <= 1.0f + 1e-5f && fillp.uv_scale[1] <= 1.0f + 1e-5f);

                const BlitParams rsz = blit_transform(1280, 720, sar[0], sar[1], rot,
                                                      Gravity::Resize, dst[0], dst[1]);
                CHECK(near(rsz.pos_scale[0], 1.0f) && near(rsz.pos_scale[1], 1.0f));
                CHECK(near(rsz.uv_scale[0], 1.0f) && near(rsz.uv_scale[1], 1.0f));

                CHECK_EQ(fitp.rot_quadrant, static_cast<uint32_t>(rot / 90));
            }
        }
    }

    // 【上面只断言结构不变量】SAR 被整个忽略（blit_transform 里
    // display_size 传 1:1）也照样成立。补两组 SAR≠1 且 rotation≠0 的字面量，
    // 期望值手算（不调 display_size/blit_transform 算期望）：
    //
    // ① 1280x720、SAR 4:3、顺时针 90、目标 1920x1080：
    //    SAR 校正宽 1280×4/3 = 1706.67 → 1707；旋转后显示 720x1707，
    //    src_aspect = 720/1707 ≈ 0.421793，dst_aspect = 1920/1080 ≈ 1.777778；
    //    src < dst ⇒ Fit 缩横向：pos_scale[0] = (720/1707)/(1920/1080)
    //    = 777600/3277440 ≈ 0.237258；Fill 裁纵向：uv_scale[1] 同值。
    //    （忽略 SAR 时是 (720/1280)/(1920/1080) = 0.31640625，差得很远。）
    {
        const BlitParams f = blit_transform(1280, 720, 4, 3, 90, Gravity::AspectFit, 1920.0, 1080.0);
        CHECK(near(f.pos_scale[0], 0.237258f));
        CHECK(near(f.pos_scale[1], 1.0f));
        CHECK_EQ(f.rot_quadrant, 1u);
        const BlitParams l = blit_transform(1280, 720, 4, 3, 90, Gravity::AspectFill, 1920.0, 1080.0);
        CHECK(near(l.uv_scale[0], 1.0f));
        CHECK(near(l.uv_scale[1], 0.237258f));
    }
    // ② 1280x720、SAR 8:9、顺时针 270、目标 1080x1920：
    //    SAR 校正宽 1280×8/9 = 1137.78 → 1138；旋转后显示 720x1138，
    //    src_aspect = 720/1138 ≈ 0.632689，dst_aspect = 1080/1920 = 0.5625；
    //    src > dst ⇒ Fit 缩纵向：pos_scale[1] = 0.5625×1138/720 = 640.125/720
    //    = 0.8890625（精确）；Fill 裁横向：uv_scale[0] 同值。
    //    （忽略 SAR 时显示 720x1280，src_aspect = 0.5625 = dst ⇒ 全是 1。）
    {
        const BlitParams f = blit_transform(1280, 720, 8, 9, 270, Gravity::AspectFit, 1080.0, 1920.0);
        CHECK(near(f.pos_scale[0], 1.0f));
        CHECK(near(f.pos_scale[1], 0.8890625f));
        CHECK_EQ(f.rot_quadrant, 3u);
        const BlitParams l = blit_transform(1280, 720, 8, 9, 270, Gravity::AspectFill, 1080.0, 1920.0);
        CHECK(near(l.uv_scale[0], 0.8890625f));
        CHECK(near(l.uv_scale[1], 1.0f));
    }
}

int main() { return tiny_test_main(); }
