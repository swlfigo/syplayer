// fake_audio_sink.h — 测试用的 IAudioSink。
//
// 它不是「空实现」：它按可注入的速率消费样本并据此推进 played_us()，
// 所以同步质量在测试里是**数值可断言**的，不是「看着还行」。
// 设备延迟也可注入 —— 那是真机上算错也不会有
// 断言变红的那个量，这里守住的是公式，常数靠真机漂移读数守。
//
// 【关于 fail_inside_next_write()】：真身 AudioUnitSink 有**四处**
// 在 write() 的调用路径内部把自己置成 failed_ 并返回 false ——
//   1. src/platform/apple/audio_unit_sink.mm:379  ensure_swr_for()：
//      in_rate/in_ch/in_fmt 非法，重采样器建不起来
//   2. src/platform/apple/audio_unit_sink.mm:404  ensure_swr_for()：
//      swr_alloc_set_opts2/swr_init 失败
//   3. src/platform/apple/audio_unit_sink.mm:431  swr_get_out_samples() < 0
//   4. src/platform/apple/audio_unit_sink.mm:464  swr_convert() < 0
// 这份替身此前**结构性地**做不到这件事：write() 只有"没 open / 已经
// failed_ / 环装不下"三条出口，没有任何一条会把 failed_ 从假翻成真，
// 失败只能靠 inject_failure() 在两次 step() 之间带外注入 —— 也就是恰好
// 只能覆盖 TrackPlayer::step() 开头那次守卫已经挡住的那一半。守卫检查
// 的时刻早于唯一能让它失效的动作，这个问题就活在替身够不着的另一
// 半里。fail_inside_next_write() 补的就是这个形状：**在 write() 内部
// 翻脸**。
#pragma once

#include "media/audio_sink.h"

#include <cstdint>
#include <limits>

namespace syp::test {

class FakeAudioSink final : public syp::media::IAudioSink {
public:
    syp_status open(int32_t sample_rate, int32_t channels, int32_t sample_fmt) override;
    bool       write(const syp::media::Frame& f) override;
    int64_t    played_us() const noexcept override;
    void       pause() override  { paused_ = true; }
    void       resume() override { paused_ = false; }
    bool       paused() const noexcept { return paused_; }   // 缓冲冻结时钟的可观测出口
    void       flush(int64_t base_us) override;
    // IAudioSink::set_speed()——这份实现不做
    // 真正的重采样，只把 speed_ 记下来，跟 played_us() 里乘的那个
    // × speed_ 数值上巧合一致（见 audio_sink.h 该接口的注释：语义上不是
    // 一回事，别把这份 Fake 的实现当成"× speed 就够了"的证据）。
    void       set_speed(double speed) noexcept override {
        speed_             = speed;
        last_set_speed_seq_ = ++op_seq_;   // 见下方 last_set_speed_seq()
    }
    bool       failed() const noexcept override { return failed_; }
    // 与 AudioUnitSink 同一契约：未 open / 已 failed → true；否则已写入的全被 advance() 消费完。
    bool       output_drained() const noexcept override {
        return failed_ || !opened_ || consumed_frames_ >= written_frames_;
    }
    // 欠载段数由用例注入（替身不建模实时回调）。
    int64_t    underrun_count() const noexcept override { return underrun_count_; }
    void       set_underrun_count(int64_t n) noexcept { underrun_count_ = n; }

    // set_gain()：跟真身
    // AudioUnitSink::set_gain() 逐条一致地把入参夹到 [0,1]：普通值按
    // [0,1] 钳位；非有限值不是笼统"一律夹到 0"——NaN → 0、+inf → 1、
    // -inf → 0。gain() 返回的是**夹取之后**
    // 的值，不是调用方传进来的原始值——这份替身的价值在于跟真身报告同一
    // 个数，而真身的可观测状态（gain_target_）本来就只存夹取后的结果，
    // 从没存过原始入参。
    void       set_gain(double gain) noexcept override;
    double     gain()       const noexcept { return gain_; }
    int        gain_calls() const noexcept { return gain_calls_; }
    // 最近一次**成功** open() 那一刻的增益（夹取后的 gain_）。
    // 对应真身 AudioUnitSink::open() 构造 render_ctx_ 时的快照 gain_current =
    // gain_target_（首次打开不淡入）：真身从这个值起播，所以"open
    // 那一刻 sink 手里是什么增益"就是开头那批样本实际乘上的起点。gain() 只能
    // 看到最终值，看不出"先 1.0 open、再改到 0"与"一开始就是 0"的区别——
    // 后者才是对的，前者会有 15ms 近满音量。
    // 从未成功 open() 过（含 fail_next_open() 那次、参数非法那次）返回 NaN
    // ——哨兵，用例用 std::isnan 判断；不用 -1 之类，免得与合法增益区间
    // [0,1] 之外的某个"碰巧写成 -1 的期望值"混淆。
    double     gain_at_open() const noexcept { return gain_at_open_; }

    // —— 测试注入面 ——
    void set_device_latency_us(int64_t us) noexcept { device_latency_us_ = us; }
    void set_capacity_frames(int64_t n)   noexcept { capacity_frames_ = n; }
    void inject_failure()                 noexcept { failed_ = true; }
    // 下一次 open() 直接失败（SYP_ERR_IO），不落 opened_/sample_rate_/
    // channels_。用于覆盖"sink 提供了但 open() 失败"这条 TrackPlayer
    // 此前零覆盖的分支。只生效一次，模拟设备被
    // 占用这类瞬时故障，不是"这个 sink 以后永远打不开"。
    void fail_next_open()                 noexcept { fail_next_open_ = true; }
    // 下一次 write() **在内部**翻脸：置 failed_ 并返回 false，跟真身
    // AudioUnitSink 的四处 `failed_ = true; return false;` 同形（四处的
    // 文件:行号见本文件顶部注释）。只生效一次——failed_ 一旦为真，之后
    // 的 write() 走既有的 `if (!opened_ || failed_) return false;` 那条
    // 出口，跟真身一致。
    //
    // 跟 inject_failure() 的区别不是"方便"，是**可达性**：inject_failure()
    // 只能在两次 step() 之间调用，构造出来的永远是"step() 开头那次守卫
    // 能看见的失败"；这个注入面构造的是"守卫看过之后才发生的失败"，也就
    // 是这条路径。
    void fail_inside_next_write()         noexcept { fail_inside_next_write_ = true; }

    // —— flush() → set_speed() 的调用顺序（audio_sink.h
    // 三条约定、track_player.h/.cpp 四处成文）此前在两个实现上都不可
    // 观测（flush() 不碰 speed，set_speed() 只记账），顺序对调的变异全绿
    // 存活。这里记一条极小的操作序号账，把那条契约变成可断言的：每次
    // flush()/set_speed() 各自记下自己发生时的全局序号，用例断言
    // set_speed 的序号紧跟在 flush 的序号之后。
    int64_t last_flush_seq()     const noexcept { return last_flush_seq_; }

    // 最近一次 flush() 收到的 base_us。用来钉死
    // TrackPlayer::sanitize_position() 的钳位真的生效了——set_speed() 与
    // seek() 都会把钳位后的值经由 flush() 递给 sink，这是它在外部唯一的
    // 可观测出口。
    int64_t last_flush_base_us() const noexcept { return base_us_; }
    int64_t last_set_speed_seq() const noexcept { return last_set_speed_seq_; }

    // 模拟实时回调消费了 us 微秒的音频（暂停时不消费）。
    void advance(int64_t us) noexcept;

    int64_t written_frames()  const noexcept { return written_frames_; }
    int64_t consumed_frames() const noexcept { return consumed_frames_; }
    int32_t flush_count()     const noexcept { return flush_count_; }

private:
    int32_t sample_rate_       = 0;
    int32_t channels_          = 0;
    int64_t capacity_frames_   = 48000;      // 默认 1 秒
    int64_t written_frames_    = 0;
    int64_t consumed_frames_   = 0;
    int64_t base_us_           = 0;
    int64_t device_latency_us_ = 0;
    double  speed_             = 1.0;
    bool    paused_            = false;
    bool    failed_            = false;
    bool    opened_            = false;
    int32_t flush_count_       = 0;
    bool    fail_next_open_    = false;
    bool    fail_inside_next_write_ = false;
    int64_t op_seq_            = 0;    // flush()/set_speed() 共用的全局操作序号
    int64_t last_flush_seq_    = 0;
    int64_t last_set_speed_seq_ = 0;
    int64_t underrun_count_    = 0;
    double  gain_              = 1.0;   // 与真身 gain_target_ 默认值一致
    int     gain_calls_        = 0;
    double  gain_at_open_      = std::numeric_limits<double>::quiet_NaN();   // 见 gain_at_open()
};

}  // namespace syp::test
