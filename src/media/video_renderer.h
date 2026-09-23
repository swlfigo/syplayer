// video_renderer.h — 缝 ②：视频呈现抽象。
//
// 平台实现只做「上传纹理 + 一个着色器」，薄到没地方藏逻辑——因为它没有
// 自动化覆盖。
#pragma once

#include "media/frame.h"
#include "media/video_geometry.h"

#include <syplayer/syp_types.h>

namespace syp::media {

class IVideoRenderer {
public:
    virtual ~IVideoRenderer() = default;

    // 呈现一帧。返回非 SYP_OK 表示该帧未被呈现——计入统计，不终止播放
    // （错误分级里最轻的一级）。
    //
    // due_in_us：这一帧应在"现在"之后多少微秒出现在屏幕上（已按倍速换算为真实时间，≥0）。
    // 支持按时刻上屏的实现据此安排上屏；不支持的实现忽略它。
    // 返回 SYP_ERR_BUSY 表示这次没有画（资源暂满，瞬态）：调用方不应视为故障。
    // 调用方应**保留这一帧稍后重试**，而不是丢弃（挂 layer 的实现名额在
    // 上屏之后才归还，按时刻提前提交时 BUSY 是常态，丢弃会成批丢帧）；帧在重试期间
    // 变得过期由调用方自己的迟到判定丢弃。实现在 BUSY 时
    // 不得留下妨碍重试的副作用：同一帧可原样反复提交。
    virtual syp_status present(const Frame& f, int64_t due_in_us) = 0;

    // 设置源的显示几何。打开媒体时调用一次；未调用时按 1:1、不旋转处理。
    // rotation_deg 的语义同 TrackInfo::rotation_deg：画面需要**顺时针**旋转多少度才正立。
    //
    // 默认空实现（不是纯虚）：与 IAudioSink::underrun_count() 同一先例，
    // 现有假实现不必全改。代价是"实现忘了覆盖"不会编译报错——
    // test_metal_renderer 有一条结构性用例钉住 MetalRenderer 确实覆盖了。
    virtual void set_source_geometry(int32_t /*sar_num*/, int32_t /*sar_den*/,
                                     int32_t /*rotation_deg*/) noexcept {}

    // 设置填充方式。可在播放中任意时刻调用；下一次 present() 生效，
    // 已经上屏的那一帧不会重画。
    virtual void set_gravity(Gravity /*g*/) noexcept {}
};

}  // namespace syp::media
