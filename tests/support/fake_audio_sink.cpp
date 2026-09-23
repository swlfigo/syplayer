#include "support/fake_audio_sink.h"

extern "C" {
#include <libavutil/avutil.h>
}

namespace syp::test {

syp_status FakeAudioSink::open(int32_t sample_rate, int32_t channels, int32_t /*sample_fmt*/) {
    if (fail_next_open_) {
        fail_next_open_ = false;   // 只生效一次，见头文件 fail_next_open() 注释
        return SYP_ERR_IO;
    }
    if (sample_rate <= 0 || channels <= 0) return SYP_ERR_INVALID_ARG;
    sample_rate_ = sample_rate;
    channels_    = channels;
    opened_      = true;
    gain_at_open_ = gain_;   // 真身在 open() 里快照 gain_current = gain_target_
    return SYP_OK;
}

bool FakeAudioSink::write(const syp::media::Frame& f) {
    if (!opened_ || failed_) return false;
    // 在 write() **内部**翻脸，跟真身 AudioUnitSink 的四处
    // `failed_ = true; return false;` 同形（见头文件顶部注释里的四个
    // 文件:行号）。位置刻意放在上面那条入口检查之后：真身的四处也都是
    // "先正常进入 write()，走到一半才发现干不下去"，不是入口就拒。
    if (fail_inside_next_write_) {
        fail_inside_next_write_ = false;   // 只生效一次，之后走上面那条既有出口
        failed_                 = true;
        return false;
    }
    const int64_t n = f.nb_samples();
    if (written_frames_ - consumed_frames_ + n > capacity_frames_) return false;  // 背压
    written_frames_ += n;
    return true;
}

int64_t FakeAudioSink::played_us() const noexcept {
    // 契约（audio_sink.h 统一）：只在 failed() 时
    // 返回 AV_NOPTS_VALUE；未 open 时返回上一次 flush() 的基准
    // base_us_（从未 flush 则为 0）——不再是"未 open 或已失败都返回
    // AV_NOPTS_VALUE"。这条分支跟 AudioUnitSink 保持同一语义，不允许
    // 替身和真身在这里分道扬镳（见接口注释里"替身的价值全部来自它和
    // 真身行为一致"那段）。
    if (failed_) return AV_NOPTS_VALUE;
    if (!opened_) return base_us_;
    const int64_t latency_frames =
        device_latency_us_ * static_cast<int64_t>(sample_rate_) / 1'000'000;
    const int64_t effective = consumed_frames_ - latency_frames;
    if (effective <= 0) return base_us_;
    return base_us_ +
           static_cast<int64_t>(static_cast<double>(effective) * 1'000'000.0 /
                                static_cast<double>(sample_rate_) * speed_);
}

void FakeAudioSink::flush(int64_t base_us) {
    base_us_         = base_us;
    consumed_frames_ = 0;
    written_frames_  = 0;
    ++flush_count_;
    last_flush_seq_  = ++op_seq_;   // 见头文件 last_flush_seq() 注释
}

void FakeAudioSink::set_gain(double gain) noexcept {
    ++gain_calls_;
    double g = gain;
    // 跟真身 AudioUnitSink::set_gain() 逐条一致：NaN → 0、
    // +inf → 1、-inf → 0、负数 → 0、> 1 → 1。这里没有对 NaN/+inf/-inf 分
    // 别写三条 if，是因为 `!(g >= 0.0)` 这一条比较已经同时吃掉了 NaN（NaN
    // 参与的任何比较恒为 false，`!false` = true）与 -inf（-inf >= 0.0 为
    // false）；+inf 会在 `g >= 0.0` 里判 true（跳过第一条），落到下面
    // `g > 1.0` 那条钳位到 1.0——跟真身注释里"±inf 靠既有钳位比较自然
    // 夹到边界"是同一个道理，两份实现殊途同归。
    if (!(g >= 0.0)) g = 0.0;
    if (g > 1.0) g = 1.0;
    gain_ = g;
}

void FakeAudioSink::advance(int64_t us) noexcept {
    if (paused_ || failed_ || !opened_) return;
    const int64_t want = us * static_cast<int64_t>(sample_rate_) / 1'000'000;
    const int64_t have = written_frames_ - consumed_frames_;
    consumed_frames_ += (want < have ? want : have);   // 只能消费已写入的
}

}  // namespace syp::test
