// audio_sink.h — 缝 ②：音频输出抽象。
//
// 为什么抽象音频*输出*而不抽象音频*解码*：各平台的音频输出路径确实不同
// （AudioUnit / AAudio / WASAPI），第二个实现在可见未来会出现；而音频解码
// 在可见未来只有 FFmpeg 软解一种。
#pragma once

#include "media/frame.h"

#include <syplayer/syp_types.h>

#include <cstdint>

namespace syp::media {

class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    // sample_fmt 取 AVSampleFormat。失败表示该 sink 不可用，
    // 调用方应退回系统时钟（不是终止播放）。
    virtual syp_status open(int32_t sample_rate, int32_t channels, int32_t sample_fmt) = 0;

    // 返回 false 表示环形缓冲没有空位（背压）。帧未被消费，调用方保留所有权。
    virtual bool write(const Frame& f) = 0;

    // 真正播出去的位置，媒体时间轴，微秒。已扣除设备输出延迟。
    //
    // 单一契约：
    //   - 已 failed() 时必须返回 AV_NOPTS_VALUE —— 不要返回 0，0 是一个
    //     合法的播放位置。
    //   - 尚未 open() 时返回上一次 flush() 的基准 base_us_（从未 flush
    //     过则为 0）—— 不是 AV_NOPTS_VALUE。
    // 两个实现（FakeAudioSink、AudioUnitSink）必须遵守同一份语义，不允许
    // 按实现各自选择：否则就是"FakeAudioSink
    // 隐藏了真实实现必须自己处理的落差"——如果替身和真身在同一接口方法
    // 的同一状态下返回语义相反的值，替身作为"跟真身行为一致"的判据就
    // 失去了价值。
    //
    // 为什么是这一侧而不是"未 open 也返回 AV_NOPTS_VALUE"：
    // AV_NOPTS_VALUE 等于 INT64_MIN。调用方（如 TrackPlayer::step()）
    // 里常见 `pts - clock_->now_us()` 这类减法，若 sink 在被当作 Audio
    // 主时钟使用的同时尚未真正 open 成功，返回 INT64_MIN 会让这类减法
    // 发生有符号整数溢出（UB）。返回 base_us_ 不依赖"调用方是否真的会
    // 撞上这个窗口"这条实现细节——即便当前 TrackPlayer::create() 的判据
    // （`audio_ready = open() 是否成功`）让这个窗口事实上不存在，接口
    // 契约仍然按"可能存在"设防。
    virtual int64_t played_us() const noexcept = 0;

    virtual void pause()  = 0;
    virtual void resume() = 0;

    // 丢弃缓冲内容，并把时钟基准重设到 base_us。
    virtual void flush(int64_t base_us) = 0;

    // 更新重采样比例。三条约定，不写下来
    // 会被写反：
    //   1. 调用点固定在 flush(base) 之后——TrackPlayer::set_speed() 按
    //      固定顺序：记基准 → flush(base) → 这里 → 更新
    //      TrackPlayer 自己的 speed_ 镜像，顺序不可换。
    //   2. 这里改的是"喂给设备多少样本对应多少媒体时长"（重采样让写入
    //      的样本数和媒体时长不再是 1:1）；played_us() 公式里另外乘的
    //      那个 × speed 是把"设备已经消费了多少真实时间"换算回"对应
    //      多少媒体时间"——两者用的是同一个 speed 数值，但分别作用在
    //      写入路径与读出路径上，不是重复计入。FakeAudioSink 今天不做
    //      真正的重采样，只在 played_us() 里乘 speed_，数值上跟"只有
    //      读出路径生效"巧合一致，语义上不是一回事，接线
    //      AudioUnitSink 时不要照抄这个巧合。
    //   3. 范围校验（[0.5, 2.0]）的责任在调用方 TrackPlayer::set_speed()
    //      ——跟 time_source.h 里 SystemClock::set_speed() 同一个立场，
    //      本类刻意不做防御性检查。
    virtual void set_speed(double speed) = 0;

    // 中途失败（设备被抢占、路由变化等）后为 true，调用方退回系统时钟。
    virtual bool failed() const noexcept = 0;

    // 自上次 flush()/open() 以来 write() 接受的每一个样本都已被设备取走，sink
    // 内部不再缓着任何待播样本时为 true。
    //   - 为 true 时 played_us() 已冻结，不会再前进；设备输出延迟那一截尾巴
    //     此刻可能仍在出声——这正是它与"played_us() ≥ 最后写入结束时刻"判据
    //     的区别：后者被扣除的设备延迟卡住（延迟大于容差就永远不成立），本判据
    //     与延迟无关。
    //   - 尚未 open()：true（从来没有接受过样本，没有东西要播）。
    //   - 已 failed()：true（不会再有样本被播出去；调用方本就应先按 failed() 降级）。
    // TrackPlayer 用它判定"音频已播完"，据此把主时钟切到系统时钟播完视频尾部。
    virtual bool output_drained() const noexcept = 0;

    // 自 open() 以来的音频欠载段数（设备要数据时环里不够、补了静音；同一段持续
    // 断粮只计一次）。只用于观测（TrackPlayer::audio_underruns()），不参与任何判定。
    // 默认 0：不建模欠载的实现不必覆写。
    virtual int64_t underrun_count() const noexcept { return 0; }

    // 线性增益，实现应把入参夹在 [0,1]。默认空实现（与 underrun_count() 同例）。
    //
    // 【不是暂停】增益为 0 时时钟照常前进、played_us() 照常推进、
    // output_drained() 语义不变。静音只是把样本乘 0。
    virtual void set_gain(double /*gain*/) noexcept {}
};

}  // namespace syp::media
