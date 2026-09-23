// fake_renderer.h — 测试用的 IVideoRenderer。
//
// 记下每次呈现的 (帧 pts, 呈现时刻)。同步质量的全部数值断言都建在这上面：
// 每一帧 |pts − 呈现时刻| ≤ 阈值、无重复呈现、除策略性丢帧外无遗漏。
#pragma once

#include "media/time_source.h"
#include "media/video_renderer.h"

#include <cstdint>
#include <tuple>
#include <vector>

namespace syp::test {

class FakeRenderer final : public syp::media::IVideoRenderer {
public:
    explicit FakeRenderer(const syp::media::TimeSource* clock) noexcept : clock_(clock) {}

    syp_status present(const syp::media::Frame& f, int64_t due_in_us) override;

    // 记下最近一次几何 / 填充方式与几何调用次数（断言"打开时设了一次几何"）。
    void set_source_geometry(int32_t sar_num, int32_t sar_den, int32_t rotation_deg) noexcept override;
    void set_gravity(syp::media::Gravity g) noexcept override;
    // (sar_num, sar_den, rotation_deg)；从未调用时为 (0, 0, 0)。
    std::tuple<int32_t, int32_t, int32_t> last_geometry() const noexcept {
        return {sar_num_, sar_den_, rotation_deg_};
    }
    // 从未调用 set_gravity() 时为 AspectFit（与 MetalRenderer 的默认一致）。
    syp::media::Gravity gravity() const noexcept { return gravity_; }
    int geometry_calls() const noexcept { return geometry_calls_; }

    struct Shown {
        int64_t pts_us;
        int64_t at_us;      // 呈现那一刻的 clock_->now_us()
        int64_t due_in_us;
    };

    const std::vector<Shown>& shown() const noexcept { return shown_; }
    void inject_failure_every(int32_t n) noexcept { fail_every_ = n; }
    // 每第 n 次 present 调用返回 SYP_ERR_BUSY（不记入 shown，记入 busy_pts）。
    // 计数按"调用次数"，不按"帧"：TrackPlayer 对 BUSY 的帧会原样重试，
    // 重试也是一次调用。n=1 即持续 BUSY；随时改回 0 即恢复。
    void inject_busy_every(int32_t n) noexcept { busy_every_ = n; }
    // 每次返回 BUSY 时那一帧的 pts（按调用顺序）——用来钉住"重试的是同一帧"。
    const std::vector<int64_t>& busy_pts() const noexcept { return busy_pts_; }

private:
    const syp::media::TimeSource* clock_;
    std::vector<Shown>            shown_;
    std::vector<int64_t>          busy_pts_;
    int32_t                       fail_every_ = 0;
    int32_t                       busy_every_ = 0;
    int32_t                       calls_      = 0;
    int32_t                       sar_num_        = 0;
    int32_t                       sar_den_        = 0;
    int32_t                       rotation_deg_   = 0;
    int                           geometry_calls_ = 0;
    syp::media::Gravity           gravity_        = syp::media::Gravity::AspectFit;
};

}  // namespace syp::test
