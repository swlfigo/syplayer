// test_audio_unit_sink.cpp — AudioUnitSink 里可以脱离硬件独立验证的两段
// 算术：
//   1. commit_frame()：AudioRing::write() 按字节、允许部分写入的语义，
//      折算成 IAudioSink::write() 要求的"整帧成败"语义。
//   2. compute_played_us()：AudioRing::consumed_bytes() 单调递增、
//      reset() 不清它，played_us() 必须用"flush() 时的快照 − 当前值"
//      算出"自上次 flush 以来"的净消费量，不能直接拿当前值当净消费量用。
//
// 这两段是 FakeAudioSink 结构性测不到的：
// FakeAudioSink::consumed_frames_ 的"自上次 flush 以来"语义只在替身内部
// 自洽，flush() 直接清零；FakeAudioSink::write() 按帧数计、没有"部分写入"
// 这个概念，从没跨越过"字节 vs 帧"、"部分写入 vs 整帧"这两条边界。
//
// 真正碰 AudioComponentInstance 的路径（open() 的硬件初始化、render
// callback 在真实设备上跑）结构性地没有自动化覆盖——本文件原版只测了
// compute_played_us()/commit_frame() 两个**纯函数**，没有测"AudioUnitSink
// 真正把这两段接到 played_us()/flush()/write() 成员函数上"这件事本身——
// 纯函数测的是"函数内部有没有做减法"，接线测的是"flush() 有没有真的把
// 快照传给它、played_us() 有没有真的把参数传对"。
//
// 所以本文件分三组：
//   1. 不需要硬件的对象级用例：played_us() 未 open 时的行为、
//      open() 的参数校验——这些是"连 open() 都不用调"的纯状态查询，
//      没有理由跟硬件路径捆在一起才测。
//   2. 需要真实设备、但本机打得开的对象级用例：open() 失败就 skip
//      （不算失败），不会让 CI 变得不确定。反向自检确认过它能杀掉两个
//      存活的接线变异体：
//      "played_us() 里传字面量 0 而不是 base_consumed_bytes_"、
//      "flush() 里删掉快照赋值 base_consumed_bytes_ = ring_->
//      consumed_bytes()"。
//   3. write() 里"swr_get_out_samples() 预检必须在 swr_convert() 之前"
//      这条顺序（第三层折算）的回归用例——最初判断"构造不出确定性用例"，
//      后来发现那个判断错了：那一版用"同速率填充帧把环灌满"的自然写法
//      去触发背压，而这条路恰好会在触发目标效应之前先把它抹掉（下面
//      swr_precheck_ordering_same_frame_retry_exposes_phantom_
//      consumption 用例头部有完整的根因说明与实测数据）。真正能复现的
//      构造需要用**不同速率**的填充帧制造背压，绕开这个自毁。
#include "media/audio_ring.h"
#include "media/frame.h"
#include "platform/apple/audio_unit_sink.h"
#include "tiny_test.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using syp::media::AudioRing;
using syp::media::Frame;
using syp::platform::AudioUnitSink;

namespace {

// 造一个真正带样本数据的音频帧（不像 test_fakes.cpp 的 make_audio_frame
// 那样只有 nb_samples 有意义）——AudioUnitSink::write() 会真的把这些
// 样本喂给 swresample，需要合法的 data 指针，不能是空的 AVFrame。
Frame make_real_audio_frame(int32_t nb_samples, int32_t sample_rate, int32_t channels) {
    AVFrame* f = av_frame_alloc();
    f->format = AV_SAMPLE_FMT_S16;
    f->sample_rate = sample_rate;
    f->nb_samples  = nb_samples;
    av_channel_layout_default(&f->ch_layout, channels);
    f->pts = 0;
    av_frame_get_buffer(f, 0);
    if (f->data[0] != nullptr && f->linesize[0] > 0) {
        std::memset(f->data[0], 0x11, static_cast<size_t>(f->linesize[0]));
    }
    return Frame::from_av(f, AVRational{1, 1000000}, false);
}

// 用 1 个样本的帧反复 write()，直到背压（write() 返回 false）为止，
// 返回成功次数——用来把"环里还剩多少空间"翻译成一个可比较的整数计数，
// 而不用去猜具体字节数（不同 speed_ 下输出字节数不是常数）。
int saturate_with_one_sample_frames(AudioUnitSink& sink, int32_t sample_rate, int32_t channels) {
    int count = 0;
    while (sink.write(make_real_audio_frame(1, sample_rate, channels))) ++count;
    return count;
}

}  // namespace

// ---------------------------------------------------------------------
// commit_frame()：write() 的"部分写入 → 整帧成败"折算
// ---------------------------------------------------------------------

TEST_CASE(commit_frame_writes_fully_when_capacity_allows) {
    AudioRing ring(64);
    std::vector<uint8_t> data(32, 0xAB);
    CHECK(AudioUnitSink::commit_frame(ring, data.data(), 32));
    CHECK_EQ(ring.readable(), int32_t{32});
    CHECK_EQ(ring.writable(), int32_t{32});
}

TEST_CASE(commit_frame_rejects_whole_frame_when_it_does_not_fit) {
    // 这是"整帧成败折算"的核心场景：裸 AudioRing::write() 在
    // 空间不够时会"能写多少写多少"（部分写入，见
    // tests/test_audio_ring.cpp 的 ring_write_returns_partial_when_full），
    // 但 IAudioSink::write() 的契约是"整帧成败"——commit_frame() 必须把
    // 前者折算成后者：空间不够整帧，一个字节都不能进环。
    AudioRing ring(16);
    std::vector<uint8_t> data(32, 0xCD);

    // 对照组：直接调裸 AudioRing::write() 会"部分写入"——这条断言只是
    // 把"折算前"的行为钉在这里，证明下面 commit_frame() 的行为不是
    // AudioRing 本来就有的，是这一层专门做的折算。
    AudioRing bare(16);
    const int32_t bare_written = bare.write(data.data(), 32);
    CHECK_EQ(bare_written, int32_t{16});   // 部分写入：只写进去 16 字节
    CHECK_EQ(bare.readable(), int32_t{16});

    // 折算之后：整帧不写，环一个字节都不能动。
    CHECK(!AudioUnitSink::commit_frame(ring, data.data(), 32));
    CHECK_EQ(ring.readable(), int32_t{0});
    CHECK_EQ(ring.writable(), int32_t{16});
    CHECK_EQ(ring.consumed_bytes(), int64_t{0});
}

TEST_CASE(commit_frame_rejects_zero_or_negative_bytes) {
    AudioRing ring(64);
    std::vector<uint8_t> data(4, 0);
    CHECK(!AudioUnitSink::commit_frame(ring, data.data(), 0));
    CHECK(!AudioUnitSink::commit_frame(ring, data.data(), -1));
    CHECK_EQ(ring.readable(), int32_t{0});
}

TEST_CASE(commit_frame_rejects_null_data) {
    AudioRing ring(64);
    CHECK(!AudioUnitSink::commit_frame(ring, nullptr, 8));
}

TEST_CASE(commit_frame_exact_capacity_succeeds_one_byte_over_fails) {
    AudioRing ring(16);
    std::vector<uint8_t> exact(16, 0x11);
    CHECK(AudioUnitSink::commit_frame(ring, exact.data(), 16));
    CHECK_EQ(ring.writable(), int32_t{0});

    // 环已经写满：哪怕只多 1 字节，也必须整帧拒绝，不能因为"反正也没多
    // 少"就允许再塞一点进去。
    std::vector<uint8_t> one(1, 0x22);
    CHECK(!AudioUnitSink::commit_frame(ring, one.data(), 1));
    CHECK_EQ(ring.readable(), int32_t{16});   // 没被那次失败的调用动过
}

TEST_CASE(commit_frame_sequence_never_partially_writes_a_rejected_frame) {
    // 连续多次 commit：前几次刚好填满，最后一次超量的必须整帧拒绝，且
    // 不影响之前已经成功写入、尚未被消费的内容。
    AudioRing ring(32);
    std::vector<uint8_t> a(20, 1);
    std::vector<uint8_t> b(20, 2);   // 剩余空间只有 12，装不下 20

    CHECK(AudioUnitSink::commit_frame(ring, a.data(), 20));
    CHECK_EQ(ring.readable(), int32_t{20});

    CHECK(!AudioUnitSink::commit_frame(ring, b.data(), 20));
    CHECK_EQ(ring.readable(), int32_t{20});   // 还是 20，b 一个字节都没进去

    // 剩余 12 字节的空间可以整帧写入一个更小的帧。
    std::vector<uint8_t> c(12, 3);
    CHECK(AudioUnitSink::commit_frame(ring, c.data(), 12));
    CHECK_EQ(ring.readable(), int32_t{32});
    CHECK_EQ(ring.writable(), int32_t{0});
}

// ---------------------------------------------------------------------
// compute_played_us()：快照相减 + 延迟扣除 + 倍速换算
// ---------------------------------------------------------------------

TEST_CASE(played_us_uses_snapshot_delta_not_raw_consumed_bytes) {
    // 具体症状复现："刚读完 AudioRing 头
    // 文件那句『永不回退』的人，很容易把『reset 不清 consumed_bytes』
    // 误读成『played_us() 直接拿当前 consumed_bytes() 用就行』"。
    //
    // 场景：flush 之前已经播放了 100 秒（historical，48000Hz、
    // bytes_per_frame=4，100 秒 = 19200000 字节）。flush 把基准快照到
    // 这个历史值。flush 之后只又播放了 1 秒（48000 帧 = 192000 字节）。
    // 正确实现必须只把这 1 秒算进 played_us 的增量，不能把 flush 之前
    // 的 100 秒也算进来。
    constexpr int32_t kSampleRate     = 48000;
    constexpr int32_t kBytesPerFrame  = 4;
    const int64_t historical_bytes = static_cast<int64_t>(kSampleRate) * kBytesPerFrame * 100;
    const int64_t base_consumed_bytes = historical_bytes;              // flush 时的快照
    const int64_t consumed_bytes = historical_bytes + static_cast<int64_t>(kSampleRate) * kBytesPerFrame; // 又消费了 1 秒
    const int64_t base_us = 7'000'000;

    const int64_t got = AudioUnitSink::compute_played_us(
        consumed_bytes, base_consumed_bytes, /*device_latency_us=*/0, kSampleRate,
        kBytesPerFrame, /*speed=*/1.0, base_us);

    // 正确答案：base_us + 1 秒 = 8000000。
    // 如果实现忘了减快照（直接用 consumed_bytes 换算），会算出
    // base_us + 101 秒 = 108000000——这条断言直接把两者分开。
    // 对照：如果实现忘了减快照（直接用 consumed_bytes 换算 100+1=101 秒），
    // 会算出 base_us + 101000000 = 108000000，跟正确答案差一个数量级。
    const int64_t buggy_without_snapshot =
        base_us + consumed_bytes / kBytesPerFrame * 1'000'000 / kSampleRate;
    CHECK_EQ(got, int64_t{8'000'000});
    CHECK(got != buggy_without_snapshot);
}

TEST_CASE(played_us_immediately_after_flush_equals_base) {
    // flush 之后还没有新的消费量：净差为 0，必须原地等于 base_us，不能
    // 因为历史 consumed_bytes 很大就报出一个远大于 base_us 的值。
    const int64_t got = AudioUnitSink::compute_played_us(
        /*consumed_bytes=*/999'999'999, /*base_consumed_bytes=*/999'999'999,
        /*device_latency_us=*/0, 48000, 4, 1.0, /*base_us=*/5'000'000);
    CHECK_EQ(got, int64_t{5'000'000});
}

TEST_CASE(played_us_subtracts_device_latency) {
    // 48000Hz、延迟 10000us = 480 帧，整除得开。消费 1 秒（48000 帧）：
    // 扣掉 480 帧延迟后，有效消费 47520 帧 → 990000us。
    constexpr int32_t kSampleRate    = 48000;
    constexpr int32_t kBytesPerFrame = 4;
    const int64_t consumed_bytes = static_cast<int64_t>(kSampleRate) * kBytesPerFrame;

    const int64_t got = AudioUnitSink::compute_played_us(consumed_bytes, /*base_consumed_bytes=*/0,
                                                          /*device_latency_us=*/10'000, kSampleRate,
                                                          kBytesPerFrame, 1.0, /*base_us=*/0);
    CHECK_EQ(got, int64_t{990'000});
}

TEST_CASE(played_us_scales_with_speed) {
    constexpr int32_t kSampleRate    = 48000;
    constexpr int32_t kBytesPerFrame = 4;
    // 消费 0.5 秒（24000 帧）。
    const int64_t consumed_bytes = static_cast<int64_t>(24000) * kBytesPerFrame;

    const int64_t p1 = AudioUnitSink::compute_played_us(consumed_bytes, 0, 0, kSampleRate,
                                                          kBytesPerFrame, 1.0, 0);
    const int64_t p2 = AudioUnitSink::compute_played_us(consumed_bytes, 0, 0, kSampleRate,
                                                          kBytesPerFrame, 2.0, 0);
    CHECK_EQ(p1, int64_t{500'000});
    CHECK_EQ(p2, int64_t{1'000'000});
    CHECK_EQ(p2, p1 * 2);
}

TEST_CASE(played_us_speed_and_device_latency_interact_correctly) {
    // speed 必须乘在"扣完延迟之后的净差"上，不能只缩放 consumed_frames
    // 却不缩放 latency_frames。48000Hz，延迟 10000us=480 帧，消费 48000
    // 帧，speed=2.0：
    //   latency_frames = 480
    //   effective      = 48000 - 480 = 47520
    //   played_us      = 47520 / 48000 * 1e6 * 2.0 = 1980000
    constexpr int32_t kSampleRate    = 48000;
    constexpr int32_t kBytesPerFrame = 4;
    const int64_t consumed_bytes = static_cast<int64_t>(kSampleRate) * kBytesPerFrame;

    const int64_t got = AudioUnitSink::compute_played_us(consumed_bytes, 0, 10'000, kSampleRate,
                                                          kBytesPerFrame, 2.0, 0);
    CHECK_EQ(got, int64_t{1'980'000});
}

TEST_CASE(played_us_freezes_during_latency_catchup_without_jump) {
    // effective <= 0 时必须冻结在 base_us 不动，越过临界点之后才开始
    // 前进，且不能有跳变。50000Hz（20us/帧，整除得开），延迟 20000us =
    // 1000 帧。
    constexpr int32_t kSampleRate    = 50000;
    constexpr int32_t kBytesPerFrame = 4;
    constexpr int64_t kLatencyUs     = 20'000;
    constexpr int64_t kBaseUs        = 0;

    // 消费 900 帧（18000us）：900 < 1000，必须原地冻结在 base_us。
    const int64_t before = AudioUnitSink::compute_played_us(
        static_cast<int64_t>(900) * kBytesPerFrame, 0, kLatencyUs, kSampleRate, kBytesPerFrame,
        1.0, kBaseUs);
    CHECK_EQ(before, kBaseUs);

    // 消费 1001 帧：effective 从 -100 变成 +1，应该开始前进到 20us，
    // 不能跳变到远大于「effective=1」对应的量。
    const int64_t after = AudioUnitSink::compute_played_us(
        static_cast<int64_t>(1001) * kBytesPerFrame, 0, kLatencyUs, kSampleRate, kBytesPerFrame,
        1.0, kBaseUs);
    CHECK_EQ(after, int64_t{20});
}

TEST_CASE(played_us_latency_conversion_truncates_toward_zero) {
    // device_latency_us * sample_rate / 1'000'000 是整数除法：非整除的
    // 延迟/采样率组合被截断，不是四舍五入。44100Hz + 15000us：
    // 15000*44100/1e6 = 661.5，截断后是 661。消费恰好 1 秒（44100 帧）：
    //   effective = 44100 - 661 = 43439 → played_us = 985011
    constexpr int32_t kSampleRate    = 44100;
    constexpr int32_t kBytesPerFrame = 4;
    const int64_t consumed_bytes = static_cast<int64_t>(kSampleRate) * kBytesPerFrame;

    const int64_t got = AudioUnitSink::compute_played_us(consumed_bytes, 0, 15'000, kSampleRate,
                                                          kBytesPerFrame, 1.0, 0);
    CHECK_EQ(got, int64_t{985'011});
}

TEST_CASE(played_us_guards_against_nonpositive_sample_rate_or_frame_size) {
    // 防御性分支：sample_rate/bytes_per_frame 任一项 <= 0（比如从未真正
    // open 成功就被拿来算）直接回退到 base_us，不做除零。
    CHECK_EQ(AudioUnitSink::compute_played_us(100, 0, 0, 0, 4, 1.0, 42), int64_t{42});
    CHECK_EQ(AudioUnitSink::compute_played_us(100, 0, 0, 48000, 0, 1.0, 42), int64_t{42});
}

// ---------------------------------------------------------------------
// played_us()/open() 在对象级、不需要硬件的行为
// ---------------------------------------------------------------------
//
// 这两条连 open() 成功与否都不依赖——played_us() 是纯状态查询，open()
// 的参数校验在真正碰硬件之前就返回。变异注入"未 open 时改回返回
// AV_NOPTS_VALUE"能让这条 22/22 全绿地判断出来，补上就是
// 为了不再让这类变异存活。

TEST_CASE(played_us_before_open_is_zero_then_flush_sets_base) {
    AudioUnitSink sink;
    // 从未 open、从未 flush：契约默认基准是 0，不是 AV_NOPTS_VALUE
    // （audio_sink.h 统一后的契约）。
    CHECK_EQ(sink.played_us(), int64_t{0});

    // flush() 在 sink 从未 open 成功时也必须能设置基准——TrackPlayer 的
    // seek 有可能恰好落在"sink 提供了但 open() 失败"之后。
    sink.flush(3'000'000);
    CHECK_EQ(sink.played_us(), int64_t{3'000'000});
}

// IAudioSink::output_drained() 契约：未 open（ring_ 为空）时为 true。
// 打开后"环里有无可读字节"的分支依赖真实硬件回调取走数据，不在无硬件用例里覆盖。
TEST_CASE(output_drained_before_open_is_true) {
    AudioUnitSink sink;
    CHECK(sink.output_drained());
    sink.flush(3'000'000);
    CHECK(sink.output_drained());
}

TEST_CASE(open_rejects_invalid_sample_rate_or_channels) {
    AudioUnitSink sink;
    CHECK_EQ(sink.open(0, 2, 0), SYP_ERR_INVALID_ARG);
    CHECK_EQ(sink.open(-1, 2, 0), SYP_ERR_INVALID_ARG);
    CHECK_EQ(sink.open(48000, 0, 0), SYP_ERR_INVALID_ARG);
    CHECK_EQ(sink.open(48000, -1, 0), SYP_ERR_INVALID_ARG);
    // 参数校验在任何硬件调用之前就返回：failed()/opened_ 都不应该因为
    // 这几次被拒绝的 open() 调用而改变可观察状态。
    CHECK(!sink.failed());
    CHECK_EQ(sink.played_us(), int64_t{0});
}

// ---------------------------------------------------------------------
// 真实硬件上的对象级"接线"用例
// ---------------------------------------------------------------------
//
// 本机打得开真实 AudioUnit 就用；open() 失败就 skip（不算
// 失败），不会让 CI 在没有音频设备的机器上变得不确定。

TEST_CASE(object_level_flush_snapshot_and_underrun_signal_on_real_hardware) {
    // 默认 200ms 环：下面写入的 0.1 秒样本要能整帧塞进
    // 去，不能自己先撞上背压——背压路径留到最后单独用一个明显更大的
    // 帧测。
    AudioUnitSink sink;
    const syp_status st = sink.open(48000, 2, 0);
    if (st != SYP_OK) {
        std::printf("  [SKIP] AudioUnitSink::open() 在本机失败（status=%d），"
                    "跳过需要真实硬件的用例，不算失败\n",
                    static_cast<int>(st));
        return;
    }

    // 写入 0.1 秒的样本（4800 帧 @ 48kHz，远小于 200ms 环容量），让硬件
    // 回调有真实内容可消费一段时间——制造"flush 之前已经有非零历史
    // consumed_bytes"这个前提条件，这正是"played_us() 里传字面量 0 而
    // 不是 base_consumed_bytes_"、"flush() 里删掉快照赋值"这两个变异体
    // 会露出马脚的地方：如果基准没有被正确快照，flush 之后 played_us()
    // 会把这段历史消费量也算进净差，立即跳到远大于 base_us 的值。
    Frame f = make_real_audio_frame(4800, 48000, 2);
    REQUIRE(sink.write(f));

    // 让真实硬件的 render_cb 有机会跑几次、真正消费掉一些样本
    // （不要求消费多少，只要非零，下面的断言不依赖具体消费量）。
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    sink.flush(9'000'000);
    // flush 之后立即查：ring 已经清空，且基准已经快照——不管 flush
    // 之前消费了多少历史样本，这里必须精确等于 base_us，不能有任何
    // 偏移。这条断言直接对应实测到的真实故障复现
    // （"played_us right after flush(9000000) = 9489687 ← 应为
    // 9000000，跳了 490ms"）。
    CHECK_EQ(sink.played_us(), int64_t{9'000'000});

    // 再等一段时间：flush 之后没有写入新样本，ring 是空的，真实硬件
    // 的欠载补静音不能推进 consumed_bytes（
    // ring_underrun_does_not_advance_consumed_bytes 在真实设备上的
    // 对应物）——played_us() 必须原地不动，不能随时间线性跳变
    // （实测："200ms 之后 = 9692354，跳变随历史线性增长"）。
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(sink.played_us(), int64_t{9'000'000});

    // 顺带覆盖 consume_underrun()：ring 在上面已经空了
    // 一段时间，真实回调应该已经报过欠载；test-and-clear 之后立即再问
    // 一次必须是 false（清零生效，不是"一直卡在 true"）。
    const bool had_underrun = sink.consume_underrun();
    CHECK(had_underrun);
    CHECK(!sink.consume_underrun());

    // 背压路径：200ms 环装不下 3 秒的样本，write() 必须整帧拒绝——顺带
    // 用真实 swr 路径覆盖（write() 的第一段样本必须真的被喂给 swr，
    // 不能因为暂时没有输出就静默声称已消费；这里用一个明显超容量的帧
    // 直接走背压分支，不依赖 swr 内部延迟缓冲的具体行为）。
    sink.flush(0);
    // 钉住 flush() 里 last_played_us_ = base_us 那句
    // 单调护栏重置——删掉它这里也不会红（上一次读数 9000000 > 这次 base_us=0，
    // 若护栏没跟着 flush 重置到新基准，下面这条会读到卡住的 9000000 而不是 0）。
    CHECK_EQ(sink.played_us(), int64_t{0});
    Frame big = make_real_audio_frame(144000, 48000, 2);   // 3 秒，远超 200ms 环
    CHECK(!sink.write(big));
}

// ---------------------------------------------------------------------
// 第三层折算：swr_get_out_samples() 预检必须在
// swr_convert() 之前——同一帧背压被拒后重试，检验 swr 状态没有被偷偷
// 前进过一次
// ---------------------------------------------------------------------
//
// 背景（最初判断"构造不出确定性用例"，后来发现这个判断是错的，
// 过程本身值得记下来）：
//
// 最初尝试的构造是"开一个小环、用同速率的填充帧把它灌满、再让目标帧 A
// 背压被拒、flush 腾空、重试 A、跟一次干净的 A 比较提交字节数"——这条
// 路径在本机反复跑都得到 delta=0，看起来"折算顺序对不对不影响结果"，
// 但真正的原因是**这条自然的构造路径会在触发目标效应之前先把它自己
// 抹掉**：`swr_convert` 因为背压被拒后"消费输入但丢弃输出"这个效应只
// 在 swr 内部滤波器状态**冷启动**的头几十个样本内存在，一旦同一个
// `SwrContext` 已经真正转换过大约 100 个样本，重复投喂同一份数据会
// 产出逐字节相同的输出——而"用同速率填充帧灌满环"这一步，恰好会用掉
// 目标帧 A 将要用的那个 `SwrContext`（`ensure_swr_for()` 按"输入格式 +
// speed_"整体重建，同速率的填充帧和 A 共用同一个上下文），等真正轮到
// A 被拒绝时，上下文早就不是冷的了。两个裸 `libswresample` 探针实测的
// 数据（同一份数据反复喂给同一个热身程度不同的 SwrContext，比较真正
// 转换 attempt1 与丢弃后重试 retry 的输出量）：
//
//   warm_calls=  0   ref=3184  attempt1(discarded)=3184  retry=3200  delta=16
//   warm_calls= 10   ref=3190  attempt1(discarded)=3190  retry=3200  delta=10
//   warm_calls=100   ref=3200  attempt1(discarded)=3200  retry=3200  delta=0
//
// 也就是说：想复现这个效应，目标帧 A 的第一次 write() 必须是它自己
// 专属的 `SwrContext` 的**真·第一次**转换——不能被任何填充帧预热过。
// 构造方式：**用跟 A 不同的 speed（因而不同的 out_sample_rate、不同的
// SwrContext 缓存 key）填充帧制造背压**，这样填充阶段建的是另一个
// SwrContext，A 自己那个直到第一次 write(A) 才第一次被用到——那一次
// 就是冷启动条件。`flush()` 不碰 `swr_`（flush 只重置 `ring_`，
// 不冲刷/重建 SwrContext），所以它能腾空
// 环、但不会抹掉"A 已经真正转换过一次"这个既成事实。
//
// 数值裕度的代价（如实标注，不是文档化契约）：这条用例的判据依赖
// libswresample 内部滤波器预热瞬态的具体样本数量级——这不是 FFmpeg
// 公开文档承诺的行为，纯粹是当前版本（本仓库 vendored、钉死在 8.1.2）
// 的实现细节，升级 FFmpeg 后需要重新验证。但判别式本身的**方向**
// 是稳健的：只要重采样存在非零群延迟，
// 有缺陷的实现（先转换后判背压）提交的样本数必然 ≥ 正确实现，不会
// 反过来——用 speed=1.5（非 1:1 直通）就是为了避开可能没有群延迟的
// 直通路径。裕度选 <=2（正确应为 0，观测到的缺陷量级约 10~24）远离
// 两侧，不是卡着某次具体读数调出来的。
TEST_CASE(swr_precheck_ordering_same_frame_retry_exposes_phantom_consumption) {
    constexpr int32_t kSampleRate = 48000;
    constexpr int32_t kChannels   = 2;
    constexpr int32_t kRingMs     = 500;

    // REF：一次干净的冷启动——sink 一开 speed 就是 1.5，第一次
    // write(A) 就是 A 专属 SwrContext 的第一次转换，不经历任何背压/
    // 重试，作为"正确情况下应该提交多少"的参照。
    AudioUnitSink ref(kRingMs);
    if (ref.open(kSampleRate, kChannels, 0) != SYP_OK) {
        std::printf("  [SKIP] AudioUnitSink::open() 在本机失败，跳过需要真实硬件的用例，"
                    "不算失败\n");
        return;
    }
    ref.pause();   // 全程 pause()：不依赖硬件消费节奏，只看 write() 的接受/拒绝
    ref.set_speed(1.5);
    Frame a_ref = make_real_audio_frame(4800, kSampleRate, kChannels);
    REQUIRE(ref.write(a_ref));
    const int k_ref = saturate_with_one_sample_frames(ref, kSampleRate, kChannels);

    // MAIN：先用 speed_ 仍为 1.0（不同于后面 A 用的 1.5）的填充帧把环
    // 灌满——这一步建的是"speed=1.0"这个 SwrContext，不会预热 A 要用
    // 的"speed=1.5"那个。再切到 1.5、尝试写 A：此时环还是满的（填充帧
    // 从未被消费，sink 全程 pause()），A 必然背压被拒——而这次"被拒的
    // 转换"正好是 A 专属 SwrContext 的第一次调用，也就是复现条件本身。
    AudioUnitSink main_sink(kRingMs);
    REQUIRE(main_sink.open(kSampleRate, kChannels, 0) == SYP_OK);
    main_sink.pause();
    while (main_sink.write(make_real_audio_frame(1, kSampleRate, kChannels))) {
    }   // 灌满，speed_ 仍是构造默认值 1.0
    main_sink.set_speed(1.5);
    Frame a_main = make_real_audio_frame(4800, kSampleRate, kChannels);
    REQUIRE(!main_sink.write(a_main));   // 冷上下文，背压被拒——缺陷代码在这一步偷偷消费了 A

    main_sink.flush(0);   // 腾空 ring_，但不碰 swr_（m9）
    REQUIRE(main_sink.write(a_main));    // 重试，同一个 Frame 对象
    const int k_main = saturate_with_one_sample_frames(main_sink, kSampleRate, kChannels);

    // 正确实现：k_ref == k_main（A 只被真正转换过一次，无论是否经历过
    // 背压-拒绝-重试，提交到环里的字节数完全一致）。有缺陷的实现（预检
    // 挪到 swr_convert 之后）：第一次被拒的调用已经把 A 喂给 swr 一次，
    // 重试时 swr 状态相对"真·第一次"已经推进过，产出的样本数变多，
    // main_sink 会比 ref 提前触顶——k_ref - k_main 应为正且明显大于 0。
    std::printf("  [DEBUG] k_ref=%d k_main=%d delta=%d\n", k_ref, k_main, k_ref - k_main);
    CHECK(k_ref - k_main <= 2);
}

// ---------------------------------------------------------------------
// compose_device_latency_us()：iOS/Catalyst 设备延迟纳入
// AVAudioSession outputLatency + IOBufferDuration，负值按 0、饱和相加、
// 钳位 [0, 500000]，全局约束。
// ---------------------------------------------------------------------

TEST_CASE(compose_device_latency_sums_all_components) {
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(1000, 2000, 3000, 4000, 5000), int64_t{15000});
}

TEST_CASE(compose_device_latency_treats_negative_components_as_zero) {
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(-1, -2, 3000, -4, 5000), int64_t{8000});
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(0, 0, 0, 0, 0), int64_t{0});
}

TEST_CASE(compose_device_latency_clamps_to_half_second) {
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(400000, 0, 0, 200000, 0), int64_t{500000});
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(INT64_MAX / 2, INT64_MAX / 2, INT64_MAX / 2, 0, 0),
             int64_t{500000});
}

// 钉住钳位边界本身——499999 是最后一个不
// 触发钳位的和，多 1 就必须钳到 500000；一个分量恰好等于钳位值
// （500000）必须单独就触发钳位（不需要跟别的分量凑）；INT64_MIN 这种
// 极端负值必须走"负值按 0"分支而不是参与运算（若被误当正常值参与运算，
// 混合正负分量的加法容易在没打好防线的实现里产出错误结果甚至溢出）。
// 原来的"iOS 典型量级落在 10~40ms"用例跟
// compose_device_latency_sums_all_components 实质重复（两者都只是验证
// "各分量之和"这条最基本的性质），删掉换成这些更能钉住边界行为的用例。
TEST_CASE(compose_device_latency_boundary_values) {
    // 499999 + 1 = 500000：恰好到钳位值，属于"正常相加"还是"触发钳位"
    // 在数值上无法区分，但这条边界本身必须钉住——差 1 都不能被漏掉。
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(499999, 1, 0, 0, 0), int64_t{500000});
    // 499999 + 0 = 499999：钳位值以内的和必须原样返回，不能被过度钳位。
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(499999, 0, 0, 0, 0), int64_t{499999});
    // 单个分量恰好等于钳位值：不需要跟其它分量相加就必须触发钳位。
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(500000, 0, 0, 0, 0), int64_t{500000});
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(0, 0, 0, 500000, 0), int64_t{500000});
    // INT64_MIN：五个分量全是这个极端负值，必须全部落进"负值按 0"分支，
    // 结果是 0，不能在参与算术时触发溢出/UB。
    CHECK_EQ(AudioUnitSink::compose_device_latency_us(INT64_MIN, INT64_MIN, INT64_MIN, INT64_MIN,
                                                        INT64_MIN),
             int64_t{0});
}

// ---------------------------------------------------------------------
// seconds_to_us_clamped()：
// AVAudioSession/AudioUnit 的秒数是系统 API 返回的 double，直接
// static_cast<int64_t>(seconds * 1e6) 对 NaN/±inf/超范围值是未定义行为
// ——这个函数必须先钳位、再转换，任何输入都不能触发 UB。
// ---------------------------------------------------------------------

TEST_CASE(seconds_to_us_clamped_handles_nan_and_infinities) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double pos_inf = std::numeric_limits<double>::infinity();
    const double neg_inf = -std::numeric_limits<double>::infinity();
    // NaN 参与的任何比较都是 false——`!(nan > 0)` 恒真，落进"视为 0"分支。
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(nan), int64_t{0});
    // +inf >= 0.5，钳到上限。
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(pos_inf), int64_t{500000});
    // -inf 不满足 > 0，视为 0。
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(neg_inf), int64_t{0});
}

TEST_CASE(seconds_to_us_clamped_handles_ordinary_and_extreme_finite_values) {
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(-1.0), int64_t{0});
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(0.0), int64_t{0});
    // 恰好 0.5s：钳位边界本身，必须是 500000，不能因为浮点乘法的舍入误差
    // 变成 499999 或触发下面的换算分支产出别的值。
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(0.5), int64_t{500000});
    // 远超 int64_t us 能表示范围的有限值：必须在钳位分支被拦下，不能走到
    // 乘法+static_cast 那一步（那一步对这么大的数是 UB）。
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(1e300), int64_t{500000});
    // 正常范围内的值：换算准确，不多不少。
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(0.005), int64_t{5000});
    CHECK_EQ(AudioUnitSink::seconds_to_us_clamped(0.02322), int64_t{23220});
}

// ---------------------------------------------------------------------
// 路由变化延迟斜坡与欠载段计数
// ---------------------------------------------------------------------

TEST_CASE(slew_latency_moves_toward_target_at_most_rate_times_elapsed) {
    CHECK_EQ(AudioUnitSink::slew_latency_us(20000, 120000, 100000), int64_t{30000});    // 100ms 最多追 10ms
    CHECK_EQ(AudioUnitSink::slew_latency_us(120000, 20000, 100000), int64_t{110000});   // 反方向同样限速
}

TEST_CASE(slew_latency_stops_exactly_at_target) {
    CHECK_EQ(AudioUnitSink::slew_latency_us(115000, 120000, 100000), int64_t{120000});
    CHECK_EQ(AudioUnitSink::slew_latency_us(25000, 20000, 100000), int64_t{20000});
    CHECK_EQ(AudioUnitSink::slew_latency_us(120000, 120000, 5000000), int64_t{120000});
}

TEST_CASE(slew_latency_ignores_nonpositive_elapsed) {
    CHECK_EQ(AudioUnitSink::slew_latency_us(20000, 120000, 0), int64_t{20000});
    CHECK_EQ(AudioUnitSink::slew_latency_us(20000, 120000, -5), int64_t{20000});
}

// 依据：rate=0.1 ⇒ max_step = int64_t(elapsed_us * 0.1)，
// elapsed_us < 10 时截断成 0，单次调用不产生任何位移；恰好 10 才够 1us。
// played_us() 的调用点必须据此只把"真正吃掉的时间"往前推，不能无条件把
// latency_updated_at_ 推到 now——否则高频轮询（间隔 < 10us）下 elapsed 每次
// 都被清零重算，永远凑不够 10us，斜坡永久停滞（见该函数注释）。
TEST_CASE(slew_latency_truncates_step_below_ten_microseconds) {
    CHECK_EQ(AudioUnitSink::slew_latency_us(20000, 30000, 9), int64_t{20000});
    CHECK_EQ(AudioUnitSink::slew_latency_us(20000, 30000, 10), int64_t{20001});
}

// 斜率 < 1 ⇒ 设备按实时消费时读数单调：模拟 48kHz 实时消费，1.5 秒处目标延迟跳 +200ms
// （蓝牙量级），每 10ms 取一次读数。
TEST_CASE(slew_latency_keeps_played_us_monotonic_across_route_change) {
    constexpr int32_t kRate = 48000;
    constexpr int32_t kBpf  = 8;
    int64_t applied = 20000;
    int64_t prev    = std::numeric_limits<int64_t>::min();
    bool    monotonic = true;
    for (int64_t t = 1000000; t <= 4000000; t += 10000) {
        const int64_t target = (t < 1500000) ? 20000 : 220000;
        applied = AudioUnitSink::slew_latency_us(applied, target, 10000);
        const int64_t consumed = t * kRate / 1000000 * kBpf;
        const int64_t v = AudioUnitSink::compute_played_us(consumed, 0, applied, kRate, kBpf, 1.0, 0);
        if (v < prev) monotonic = false;
        prev = v;
    }
    CHECK(monotonic);
    CHECK_EQ(applied, int64_t{220000});   // 2.5 秒足够追完 200ms
}

TEST_CASE(underrun_edge_counts_each_starvation_episode_once) {
    bool in = false;
    CHECK(!AudioUnitSink::underrun_edge(true, /*armed=*/false, &in));   // 从未写入：不是欠载
    CHECK(!in);
    CHECK(AudioUnitSink::underrun_edge(true, true, &in));     // 断粮开始：+1
    CHECK(!AudioUnitSink::underrun_edge(true, true, &in));    // 同一段持续：不重复计
    CHECK(!AudioUnitSink::underrun_edge(false, true, &in));   // 恢复供给
    CHECK(AudioUnitSink::underrun_edge(true, true, &in));     // 新的一段：+1
    CHECK(!AudioUnitSink::underrun_edge(true, false, &in));   // flush 解除武装：不计且复位
    CHECK(AudioUnitSink::underrun_edge(true, true, &in));     // 重新写入后断粮：+1
}

TEST_CASE(underrun_count_is_zero_before_open) {
    AudioUnitSink sink;
    CHECK_EQ(sink.underrun_count(), int64_t{0});
}

// 真实硬件：写 100ms 样本后等它播完，环持续断粮只计一段。open 失败就 skip。
TEST_CASE(underrun_count_on_real_hardware_counts_one_episode) {
    AudioUnitSink sink;
    const syp_status st = sink.open(48000, 2, 0);
    if (st != SYP_OK) {
        std::printf("  [SKIP] AudioUnitSink::open() 在本机失败（status=%d），跳过需要真实硬件的用例\n",
                    static_cast<int>(st));
        return;
    }
    CHECK_EQ(sink.underrun_count(), int64_t{0});   // 打开后还没写：空转不算欠载
    Frame f = make_real_audio_frame(4800, 48000, 2);
    REQUIRE(sink.write(f));
    // 固定 300ms 睡眠对真实设备调度（尤其是蓝牙输出，
    // 缓冲/唤醒延迟可能远超 300ms）不稳，改成轮询等到第一次真正欠载，最多
    // 等 2 秒；等到之后再做"持续断粮只计一段"的稳定性检查不变。
    int64_t n = 0;
    for (int i = 0; i < 20 && n < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        n = sink.underrun_count();
    }
    CHECK(n >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(sink.underrun_count(), n);
}

// ---------------------------------------------------------------------
// 音量与斜坡：advance_gain()/apply_gain() 是纯算术，与
// commit_frame()/compute_played_us() 同一形状——脱离硬件单测。
// ---------------------------------------------------------------------

// set_gain() 的夹取逻辑不需要硬件——它只写一个原子量，跟 open() 有没有
// 成功、有没有真实设备都无关。用 gain_target_for_test()（
// 测试专用只读口子，见头文件注释）直接读回，不依赖真实硬件把它播出来
// 才能验证："> 1.0 夹到 1.0" 这条变异如果不加这条用例会无声存活——
// 已用 mutation testing 实测确认。
// 非有限值的口径是 NaN → 0、+inf → 1、-inf → 0，
// 不是"非有限值一律夹到 0"——早先版本把
// std::isfinite() 当闸门，会把 +inf 也错误地夹成 0（应为 1）。
TEST_CASE(set_gain_clamps_out_of_range_values_including_nan) {
    AudioUnitSink sink;
    sink.set_gain(2.0);
    CHECK_EQ(sink.gain_target_for_test(), 1.0f);
    sink.set_gain(-3.0);
    CHECK_EQ(sink.gain_target_for_test(), 0.0f);
    sink.set_gain(std::numeric_limits<double>::quiet_NaN());
    CHECK_EQ(sink.gain_target_for_test(), 0.0f);
    sink.set_gain(std::numeric_limits<double>::infinity());
    CHECK_EQ(sink.gain_target_for_test(), 1.0f);
    sink.set_gain(-std::numeric_limits<double>::infinity());
    CHECK_EQ(sink.gain_target_for_test(), 0.0f);
    sink.set_gain(0.5);
    CHECK_EQ(sink.gain_target_for_test(), 0.5f);
}

// 【open() 不淡入】open() 之前设的增益就是渲染线程的
// 起点：gain_at_open_for_test() 读的是 open() 构造好的 render_ctx_.gain_current，
// 不是 gain_target_。期望值是手写字面量（0.25f / 0.0f），不经 advance_gain 等
// 生产函数推算。gain_at_open_ 在 AudioComponent 相关调用之前记下，有没有真实
// 设备、后面的硬件初始化成不成功都不影响这条断言；open() 返回值因此不断言。
// 未 open / 参数非法的 open 都是 NaN 哨兵。
TEST_CASE(open_starts_from_the_gain_set_before_it_without_fading_in) {
    {
        AudioUnitSink sink;
        CHECK(std::isnan(sink.gain_at_open_for_test()));
        sink.set_gain(0.25);
        (void)sink.open(48000, 2, -1);
        CHECK_EQ(sink.gain_at_open_for_test(), 0.25f);
    }
    {
        AudioUnitSink sink;
        sink.set_gain(0.0);
        (void)sink.open(48000, 2, -1);
        CHECK_EQ(sink.gain_at_open_for_test(), 0.0f);
        // 参数非法的重复 open 走不到快照那一步：回到哨兵，不残留上一次的值
        CHECK(sink.open(0, 2, -1) == SYP_ERR_INVALID_ARG);
        CHECK(std::isnan(sink.gain_at_open_for_test()));
    }
}

TEST_CASE(gain_ramp_approaches_target_monotonically_without_overshoot) {
    const int32_t rate = 48000;
    // 15ms @48k = 720 帧走完全程
    float g = 0.0f;
    float prev = -1.0f;
    for (int i = 0; i < 40; ++i) {
        g = AudioUnitSink::advance_gain(g, 1.0f, 64, rate);
        CHECK(g >= prev);          // 单调
        CHECK(g <= 1.0f + 1e-6f);  // 不过冲
        prev = g;
    }
    CHECK(g > 0.9f);               // 40*64 = 2560 帧 > 720，早该到顶
    CHECK_EQ(AudioUnitSink::advance_gain(1.0f, 1.0f, 64, rate), 1.0f);
}

TEST_CASE(gain_ramp_descends_to_zero_and_clamps) {
    const int32_t rate = 48000;
    float g = 1.0f;
    for (int i = 0; i < 40; ++i) {
        g = AudioUnitSink::advance_gain(g, 0.0f, 64, rate);
        CHECK(g >= -1e-6f);
    }
    CHECK(g < 0.1f);
    CHECK_EQ(AudioUnitSink::advance_gain(0.0f, 0.0f, 64, rate), 0.0f);
    // 退化输入不除零
    CHECK_EQ(AudioUnitSink::advance_gain(0.0f, 1.0f, 64, 0), 1.0f);
}

// 【Ruling T4-a】apply_gain()：真正对样本做乘法的那段逻辑，render_cb 本身
// 没有自动化覆盖，必须靠这个静态纯函数的用例钉住"乘法真的发生了"。

TEST_CASE(apply_gain_identity_when_current_and_target_are_one) {
    float samples[6] = {1.0f, -2.0f, 3.5f, 0.0f, -0.25f, 42.0f};
    const float orig[6] = {1.0f, -2.0f, 3.5f, 0.0f, -0.25f, 42.0f};
    const float g1 = AudioUnitSink::apply_gain(samples, 6, /*channels=*/2, 1.0f, 1.0f, 48000);
    CHECK_EQ(g1, 1.0f);
    for (int i = 0; i < 6; ++i) CHECK_EQ(samples[i], orig[i]);
}

TEST_CASE(apply_gain_zero_target_and_current_silences_everything) {
    float samples[4] = {1.0f, -2.0f, 3.5f, -4.0f};
    const float g1 = AudioUnitSink::apply_gain(samples, 4, /*channels=*/2, 0.0f, 0.0f, 48000);
    CHECK_EQ(g1, 0.0f);
    for (float s : samples) CHECK_EQ(s, 0.0f);
}

// 0 → 1 的一块：首帧 ≈ 0（current=0）、末帧 ≈ 返回值 × 原值（块内在
// current → 返回值之间线性插值、块间连续，见 apply_gain 头文件注释），
// 整块单调不减（原始样本恒为正值）。用少量帧（远小于 720 帧的斜坡全程）
// 确保 g1 < 1，插值区间非退化。
TEST_CASE(apply_gain_ramps_up_first_frame_near_zero_last_frame_matches_return_value) {
    constexpr int32_t kChannels = 1;
    constexpr int32_t kFrames   = 8;
    const float orig = 2.0f;
    float samples[kFrames];
    for (float& s : samples) s = orig;

    const float g1 = AudioUnitSink::apply_gain(samples, kFrames, kChannels, 0.0f, 1.0f, 48000);
    CHECK(g1 > 0.0f);
    CHECK(g1 < 1.0f);   // 8 帧远小于 720 帧的斜坡全程，不该冲到顶

    CHECK(std::fabs(samples[0] - 0.0f) < 1e-5f);            // 首帧 current=0
    CHECK(std::fabs(samples[kFrames - 1] - g1 * orig) < 1e-4f);  // 末帧落在返回值上

    for (int i = 1; i < kFrames; ++i) {
        CHECK(samples[i] >= samples[i - 1] - 1e-6f);   // 单调不减（原值为正）
    }
}

// 【Ruling T4-a ⚠️】立体声：同一帧的左右两个样本必须乘同一个增益值——
// 插值按帧走，不按样本走。若误按样本插值（nfloats - 1 做分母），左声道
// （偶数下标）与右声道（奇数下标）会分到两个相邻但不同的插值系数，
// 这条用例就是要抓住那个差异。用不同的原始幅度（左 1.0f / 右 3.0f）让
// "同一帧内左右所受增益是否相等"可以直接从输出比值读出来，不需要事先
// 知道具体的斜坡数值。
TEST_CASE(apply_gain_interpolates_per_frame_not_per_sample_for_stereo) {
    constexpr int32_t kChannels = 2;
    constexpr int32_t kFrames   = 6;
    float samples[kChannels * kFrames];
    for (int32_t f = 0; f < kFrames; ++f) {
        samples[f * kChannels + 0] = 1.0f;
        samples[f * kChannels + 1] = 3.0f;
    }
    AudioUnitSink::apply_gain(samples, kChannels * kFrames, kChannels, 0.2f, 0.9f, 48000);
    for (int32_t f = 0; f < kFrames; ++f) {
        const float left_gain  = samples[f * kChannels + 0] / 1.0f;
        const float right_gain = samples[f * kChannels + 1] / 3.0f;
        CHECK(std::fabs(left_gain - right_gain) < 1e-6f);
    }
}

// 返回值必须真的是 advance_gain() 算出来的那个数，不能恒等于 current
// （斜坡不前进）——用跟 apply_gain 内部完全一样的参数直接调 advance_gain
// 比对，独立于"样本有没有被乘"这件事本身。
TEST_CASE(apply_gain_return_value_matches_advance_gain_for_same_frame_count) {
    constexpr int32_t kChannels = 2;
    constexpr int32_t kFrames   = 10;
    float samples[kChannels * kFrames] = {};
    const float expected = AudioUnitSink::advance_gain(0.2f, 0.9f, kFrames, 48000);
    const float g1 =
        AudioUnitSink::apply_gain(samples, kChannels * kFrames, kChannels, 0.2f, 0.9f, 48000);
    CHECK_EQ(g1, expected);
    CHECK(g1 > 0.2f);   // 顺带确认真的前进了，不是巧合等于 current
}

// 0 帧（或 channels<=0，同样"没有真实输出"）不推进
// 斜坡——返回 current，不是 target。早先版本这里断言的是 target，那是
// 需要修的 bug 本身（0 帧凭空跳到目标值）。
TEST_CASE(apply_gain_nonpositive_nfloats_or_channels_leaves_buffer_untouched) {
    constexpr float kSentinel = 12345.0f;
    float samples[4] = {kSentinel, kSentinel, kSentinel, kSentinel};

    const float g1 = AudioUnitSink::apply_gain(samples, 0, 2, 0.3f, 0.7f, 48000);
    CHECK_EQ(g1, 0.3f);   // advance_gain(current, target, 0, rate) == current
    for (float s : samples) CHECK_EQ(s, kSentinel);

    const float g2 = AudioUnitSink::apply_gain(samples, 4, 0, 0.3f, 0.7f, 48000);
    CHECK_EQ(g2, 0.3f);
    for (float s : samples) CHECK_EQ(s, kSentinel);
}

// advance_gain() 自身的契约：frames<=0 时不推进（返回
// current），跟 sample_rate<=0（真的有输出但算不出步长，落点到 target）
// 是两条不同的规则，不能合并成同一句 return target。
TEST_CASE(advance_gain_zero_or_negative_frames_does_not_advance_the_ramp) {
    CHECK_EQ(AudioUnitSink::advance_gain(0.3f, 0.7f, 0, 48000), 0.3f);
    CHECK_EQ(AudioUnitSink::advance_gain(0.3f, 0.7f, -5, 48000), 0.3f);
    // frames > 0 但 sample_rate <= 0：算不出步长，直接落点，跟此前
    // 就有的 gain_ramp_descends_to_zero_and_clamps 最后一句同一约定。
    CHECK_EQ(AudioUnitSink::advance_gain(0.3f, 0.7f, 64, 0), 0.7f);
}

// nfloats 不是 channels 整数倍时，末尾凑不成一帧的零头
// 样本必须按返回值 g1 处理，不能维持原样——静音（current=target=0）下
// 原样不动就是"全音量漏音"。channels=2、nfloats=5：2 帧 + 1 个"半帧"尾巴。
TEST_CASE(apply_gain_tail_samples_not_forming_a_full_frame_are_still_silenced) {
    constexpr int32_t kChannels = 2;
    constexpr int32_t kNfloats  = 5;
    float samples[kNfloats] = {9.0f, 9.0f, 9.0f, 9.0f, 9.0f};
    const float g1 = AudioUnitSink::apply_gain(samples, kNfloats, kChannels, 0.0f, 0.0f, 48000);
    CHECK_EQ(g1, 0.0f);
    for (float s : samples) CHECK_EQ(s, 0.0f);
}

// 【静音不是暂停】这条是头号语义保证，点名的用例是：
// 「isMuted == true 时……played_us() 照常推进」「consumed_bytes
// 记账与增益无关」。真实硬件：open 失败就 skip（跟本文件其它硬件用例同
// 一体例）。
//
// 原版只断言了 t1 > t0（时钟没停），从没断言"记账"这半
// 句——名字里的 "byte_accounting" 名不副实。这里用 output_drained() 补
// 上：它的契约是"自上次 flush()/open() 以来 write() 接受的每一个样本都
// 已被设备取走"（audio_sink.h），也就是 AudioRing 里的字节被 render_cb
// 真正 read() 走了。不需要新增任何生产接口——output_drained() 本来就是
// 公开接口。如果某个错误实现把"静音"错当成"暂停消费"（target==0 时
// render_cb 跳过 ring->read()），下面这条断言会因为环里的字节永远读不
// 完而卡在 false，超时后变红；对应变异见 render_cb 里的变异记录。
TEST_CASE(muting_does_not_stop_the_clock_or_change_byte_accounting) {
    AudioUnitSink sink;
    const syp_status st = sink.open(48000, 2, 0);
    if (st != SYP_OK) {
        std::printf("  [SKIP] AudioUnitSink::open() 在本机失败（status=%d），跳过需要真实硬件的用例\n",
                    static_cast<int>(st));
        return;
    }
    Frame f = make_real_audio_frame(4800, 48000, 2);   // 0.1 秒
    REQUIRE(sink.write(f));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    sink.set_gain(0.0);
    const int64_t t0 = sink.played_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t t1 = sink.played_us();
    CHECK(t1 > t0);   // 时钟照走——静音不是暂停

    // 再等到明显超过这 0.1 秒样本本该被完全消费的时长，确认字节记账真的
    // 没有被"静音"打断——环里的样本必须被 render_cb 全部读走，output_
    // drained() 才会变 true。
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(sink.output_drained());   // 字节记账没有被静音打断
}

int main() { return tiny_test_main(); }
