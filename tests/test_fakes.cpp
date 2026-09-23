// test_fakes.cpp — FakeAudioSink / FakeRenderer 本身的判据检验。
//
// 这两个替身是场景 A~I 全部数值断言的判据来源：这里没有
// FFmpeg 参照解码那种「逐帧比对」的外部真相，同步质量能不能被证伪，
// 全靠 played_us() 的算术（设备延迟扣除 + 倍速换算）和 FakeRenderer
// 记录的 (pts, 呈现时刻) 对不对。判据没人验，后面九条场景的绿灯就
// 建在一个未经检查的算式上——出错的表现是"场景莫名假红或假绿"，
// 极难归因回判据本身。所以这里的每一条用例都必须能在算式被搞错时
// 真的变红（反向自检过）。
#include "media/frame.h"
#include "support/fake_audio_sink.h"
#include "support/fake_clock.h"
#include "support/fake_renderer.h"
#include "tiny_test.h"

#include <limits>
#include <tuple>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

using syp::media::Frame;
using syp::test::FakeAudioSink;
using syp::test::FakeClock;
using syp::test::FakeRenderer;

namespace {

// 造一个只有 nb_samples 有意义的音频帧——FakeAudioSink::write() 只读
// f.nb_samples()，不碰实际样本数据，所以不必分配真实缓冲区。
Frame make_audio_frame(int32_t nb_samples) {
    AVFrame* f = av_frame_alloc();
    f->format      = AV_SAMPLE_FMT_S16;
    f->sample_rate  = 48000;
    f->nb_samples   = nb_samples;
    av_channel_layout_default(&f->ch_layout, 1);
    f->pts = 0;
    return Frame::from_av(f, AVRational{1, 1000000}, false);
}

// 造一个只有 pts 有意义的视频帧——time_base 直接取 1/1e6，pts_us 与传入
// 的 pts 数值相等，不需要再心算一次换算。
Frame make_video_frame(int64_t pts_us) {
    AVFrame* f = av_frame_alloc();
    f->pts = pts_us;
    return Frame::from_av(f, AVRational{1, 1000000}, true);
}

}  // namespace

// ---------------------------------------------------------------------------
// FakeAudioSink::played_us()
// ---------------------------------------------------------------------------

TEST_CASE(fake_sink_played_us_subtracts_device_latency) {
    // 48000Hz、延迟 10000us = 480 帧，整除得开，避免浮点凑整干扰判断。
    FakeAudioSink sink;
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);
    sink.set_device_latency_us(10000);

    // 顺带覆盖 write() 背压的「恰好等于容量」边界：默认容量是 48000 帧
    // （见 fake_audio_sink.h），这里写入正好 48000 帧必须成功（不能把
    // “等于”误判成“超过”）。fake_sink_write_applies_backpressure_when_full
    // 只测了“超过之后失败”，这条连同下面的 fake_sink_played_us_scales_
    // with_speed 一起，间接把“等于容量”这一侧钉住——写这句注释是为了让
    // 这层覆盖显式化，免得将来改默认容量时无声丢掉。
    Frame f = make_audio_frame(48000);   // 1 秒的样本，正好填满默认容量
    REQUIRE(sink.write(f));

    sink.advance(1'000'000);             // 模拟消费掉 1 秒
    REQUIRE(sink.consumed_frames() == 48000);

    // 扣掉 480 帧延迟后，有效消费 47520 帧 → 990000us。
    // 如果实现忘了扣延迟，这里会算出 1000000，与期望值不同，判据能抓到。
    CHECK_EQ(sink.played_us(), int64_t{990000});
}

TEST_CASE(fake_sink_played_us_scales_with_speed) {
    FakeAudioSink s1;
    REQUIRE(s1.open(48000, 2, 0) == SYP_OK);
    Frame f1 = make_audio_frame(48000);
    REQUIRE(s1.write(f1));
    s1.advance(500'000);
    const int64_t p1 = s1.played_us();

    FakeAudioSink s2;
    REQUIRE(s2.open(48000, 2, 0) == SYP_OK);
    s2.set_speed(2.0);
    Frame f2 = make_audio_frame(48000);
    REQUIRE(s2.write(f2));
    s2.advance(500'000);
    const int64_t p2 = s2.played_us();

    CHECK_EQ(p1, int64_t{500000});
    CHECK_EQ(p2, int64_t{1000000});
    CHECK_EQ(p2, p1 * 2);   // 同样的消费量，2 倍速下 played_us 增量应是 1 倍速的两倍
}

TEST_CASE(fake_sink_played_us_before_open_is_last_flush_base_not_nopts) {
    // 契约统一之后：played_us() 只在 failed() 时
    // 返回 AV_NOPTS_VALUE；未 open 时返回上一次 flush() 的基准（从未
    // flush 则为 0）——不再是"未 open 或已失败都返回 AV_NOPTS_VALUE"。
    FakeAudioSink sink;
    // 未 open、从未 flush：基准默认是 0。这里合法地是 0，不是
    // AV_NOPTS_VALUE——跟"open 之后还没消费任何样本时是 0"同一个理由
    // （落在起点），不是"这个状态没有播放位置的概念"。
    CHECK_EQ(sink.played_us(), int64_t{0});

    // 未 open 时 flush() 仍然要能设置基准——AudioUnitSink 的 flush() 在
    // sink 从未成功 open 过时也必须能记下 base_us（比如 TrackPlayer 的
    // seek 恰好落在 sink 打开失败之后）。
    sink.flush(5'000'000);
    CHECK_EQ(sink.played_us(), int64_t{5'000'000});

    // open() 成功后基准被保留（open() 本身不改 base_us_）。
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);
    CHECK_EQ(sink.played_us(), int64_t{5'000'000});
}

TEST_CASE(fake_sink_played_us_reports_nopts_only_after_failure) {
    // AV_NOPTS_VALUE 是契约里唯一保留给"已经 failed()"的返回值——open
    // 之后、还没消费任何样本时是 0（合法的播放位置起点，不是"没有这个
    // 概念"），失败之后才切换到 AV_NOPTS_VALUE，且不能是 0（0 会被
    // 调用方误读成"正常播放在起点"）。
    FakeAudioSink sink;
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);
    CHECK_EQ(sink.played_us(), int64_t{0});

    sink.inject_failure();
    CHECK_EQ(sink.played_us(), int64_t{AV_NOPTS_VALUE});
    CHECK(sink.played_us() != 0);
}

TEST_CASE(fake_sink_write_applies_backpressure_when_full) {
    FakeAudioSink sink;
    sink.set_capacity_frames(100);
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);

    Frame full = make_audio_frame(100);
    CHECK(sink.write(full));
    CHECK_EQ(sink.written_frames(), int64_t{100});

    Frame extra = make_audio_frame(1);
    CHECK(!sink.write(extra));                       // 写满之后必须失败
    CHECK_EQ(sink.written_frames(), int64_t{100});    // 且没有吞掉那一帧（计数不变）
}

TEST_CASE(fake_sink_advance_cannot_consume_more_than_written) {
    FakeAudioSink sink;
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);

    Frame small = make_audio_frame(100);   // 只写入 100 帧
    REQUIRE(sink.write(small));

    // 请求消费远超已写入量的时长；consumed_frames_ 只能封顶在 written_frames_。
    sink.advance(1'000'000);
    CHECK_EQ(sink.consumed_frames(), int64_t{100});
    CHECK_EQ(sink.written_frames(), int64_t{100});

    // 再 advance 一次：已经没有可消费的样本了，consumed_frames_ 不能继续增长
    // ——否则时钟会凭空前进，对应 AudioRing 欠载补静音不计入
    // consumed_bytes 的同款性质。
    sink.advance(1'000'000);
    CHECK_EQ(sink.consumed_frames(), int64_t{100});
}

// IAudioSink::output_drained() 契约：未 open → true；写入未被取完 → false；
// 取完 → true（与设备延迟无关：played_us() 此刻仍扣着延迟）；flush 后 → true；failed → true。
TEST_CASE(fake_sink_output_drained_follows_contract_independent_of_latency) {
    FakeAudioSink sink;
    CHECK(sink.output_drained());                    // 未 open
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);
    sink.set_device_latency_us(150'000);
    CHECK(sink.output_drained());                    // 打开但什么都没写

    Frame f = make_audio_frame(4800);                // 100ms
    REQUIRE(sink.write(f));
    CHECK(!sink.output_drained());
    sink.advance(50'000);
    CHECK(!sink.output_drained());                   // 只取走一半
    sink.advance(50'000);
    CHECK(sink.output_drained());                    // 全部取走
    CHECK_EQ(sink.played_us(), int64_t{0});          // 读数仍被 150ms 延迟压着——判据不能靠它

    Frame g = make_audio_frame(4800);
    REQUIRE(sink.write(g));
    CHECK(!sink.output_drained());
    sink.flush(1'000'000);
    CHECK(sink.output_drained());                    // flush 丢弃了缓冲

    REQUIRE(sink.write(g));
    sink.inject_failure();
    CHECK(sink.output_drained());                    // failed：不会再有样本被播出
}

TEST_CASE(fake_sink_flush_resets_base_and_counters) {
    // flush(base_us) 之后 played_us() 必须立即等于 base_us，且后续
    // advance() 相对新基准计数，不受 flush 前的历史影响。这条守的是
    // 「变速/seek 四步」里「flush 重设基准」那一步——
    // 变速/seek 场景全压在它上面，而 flush() 在其它用例里从没被调用过，
    // 变异体（flush() 不清 consumed_frames_）之前能全绿存活。
    FakeAudioSink sink;
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);

    // 先跑一段历史：写入并消费 0.5 秒（24000 帧），确认不是从头就冻结的。
    Frame f1 = make_audio_frame(24000);
    REQUIRE(sink.write(f1));
    sink.advance(500'000);
    CHECK_EQ(sink.played_us(), int64_t{500000});

    // flush 到一个新基准。
    sink.flush(7'000'000);
    CHECK_EQ(sink.played_us(), int64_t{7000000});     // 立即等于新基准
    CHECK_EQ(sink.consumed_frames(), int64_t{0});     // 历史消费量必须清零
    CHECK_EQ(sink.written_frames(), int64_t{0});      // 历史写入量也必须清零

    // flush 之后重新写入、消费：增量必须相对新基准计算，flush 前的历史
    // 消费量（24000 帧）完全不能掺进来——否则这里会算出
    // 7000000 + (24000+48000)/48000*1e6 = 8500000，而不是 8000000。
    Frame f2 = make_audio_frame(48000);
    REQUIRE(sink.write(f2));
    sink.advance(1'000'000);
    CHECK_EQ(sink.played_us(), int64_t{8000000});
}

TEST_CASE(fake_sink_played_us_speed_and_device_latency_interact_correctly) {
    // fake_sink_played_us_scales_with_speed 只测过 latency=0 时的缩放，
    // fake_sink_played_us_subtracts_device_latency 只测过 speed=1.0 时的
    // 延迟扣除——两个参数从未同时非零出现过。这条钉住「speed 乘在扣完
    // 延迟后的净差上」这个顺序，而不是「只缩放 consumed_frames_、不缩放
    // latency_frames」。
    //
    // 48000Hz，延迟 10000us=480 帧，消费 48000 帧，speed=2.0：
    //   latency_frames = 480
    //   effective      = 48000 - 480 = 47520
    //   played_us      = 47520 / 48000 * 1e6 * 2.0 = 1980000
    FakeAudioSink sink;
    REQUIRE(sink.open(48000, 2, 0) == SYP_OK);
    sink.set_device_latency_us(10000);
    sink.set_speed(2.0);

    Frame f = make_audio_frame(48000);
    REQUIRE(sink.write(f));
    sink.advance(1'000'000);
    REQUIRE(sink.consumed_frames() == 48000);

    CHECK_EQ(sink.played_us(), int64_t{1980000});
}

TEST_CASE(fake_sink_played_us_freezes_during_latency_catchup_without_jump) {
    // effective <= 0 时必须冻结在 base_us_ 不动，且越过临界点之后开始
    // 前进、不能有跳变——删掉 `if (effective <= 0) return base_us_;` 那行
    // 之前能全绿存活，因为没有用例真的测过「延迟还没被吃满」这段窗口。
    //
    // 用 50000Hz（20us/帧，整除得开）避免浮点凑整干扰判断：
    // 延迟 20000us = 1000 帧。
    FakeAudioSink sink;
    REQUIRE(sink.open(50000, 2, 0) == SYP_OK);
    sink.set_device_latency_us(20000);

    Frame f = make_audio_frame(2000);
    REQUIRE(sink.write(f));

    // 消费 900 帧（18000us）：900 < 1000，延迟还没被吃满，必须原地冻结
    // 在 base_us_（默认 0）——不能算出负数，也不能算出任何非零值。
    sink.advance(18'000);
    REQUIRE(sink.consumed_frames() == 900);
    CHECK_EQ(sink.played_us(), int64_t{0});

    // 再推进 101 帧（2020us），累计消费 1001 帧，刚好越过临界点
    // （effective 从 -100 变成 +1）：played_us() 应该开始前进到 20us，
    // 不能跳变到远大于「effective=1」对应的量。
    sink.advance(2'020);
    REQUIRE(sink.consumed_frames() == 1001);
    CHECK_EQ(sink.played_us(), int64_t{20});
}

TEST_CASE(fake_sink_played_us_latency_conversion_truncates_toward_zero) {
    // device_latency_us_ * sample_rate_ / 1'000'000 是整数除法：非整除
    // 的延迟/采样率组合会被截断，而不是四舍五入。这条钉死截断方向——
    // 之前唯一用到延迟的用例选的是 480 帧那种整除得开的组合，没人验证
    // 过取整方向，把截断改成四舍五入也能全绿存活。
    //
    // 44100Hz + 15000us：15000*44100/1e6 = 661.5，截断后是 661（不是
    // 四舍五入后的 662）。消费恰好 1 秒（44100 帧）：
    //   effective (截断) = 44100 - 661 = 43439 → played_us = 985011
    //   effective (四舍五入) = 44100 - 662 = 43438 → played_us = 984988
    // 两者相差 23us，足以让 CHECK_EQ 区分。
    //
    // 这不是在断言“截断就是唯一正确答案”——是把当前实现选择的方向明确
    // 钉住：误差量级 <1 帧（48kHz 下 <21us），方向恒定（截断永远让
    // played_us() 偏大，因为少扣了延迟），量级上不影响 40ms 判定窗口，
    // 已知可接受。将来若要改成四舍五入，必须连这条用例一起改，不能
    // 无声漂移。
    FakeAudioSink sink;
    REQUIRE(sink.open(44100, 2, 0) == SYP_OK);
    sink.set_device_latency_us(15000);

    Frame f = make_audio_frame(44100);
    REQUIRE(sink.write(f));
    sink.advance(1'000'000);
    REQUIRE(sink.consumed_frames() == 44100);

    CHECK_EQ(sink.played_us(), int64_t{985011});
}

// set_gain() 的夹取要跟真身 AudioUnitSink::
// set_gain() 逐条一致：NaN → 0、+inf → 1、-inf → 0、负数 → 0、> 1 → 1。
// gain() 返回的是夹取之后的值，不是原始入参（见头文件 set_gain() 注释）。
TEST_CASE(fake_sink_set_gain_clamps_out_of_range_values_including_nan) {
    FakeAudioSink sink;
    CHECK_EQ(sink.gain_calls(), 0);

    sink.set_gain(2.0);
    CHECK_EQ(sink.gain(), 1.0);
    sink.set_gain(-3.0);
    CHECK_EQ(sink.gain(), 0.0);
    sink.set_gain(std::numeric_limits<double>::quiet_NaN());
    CHECK_EQ(sink.gain(), 0.0);
    sink.set_gain(std::numeric_limits<double>::infinity());
    CHECK_EQ(sink.gain(), 1.0);
    sink.set_gain(-std::numeric_limits<double>::infinity());
    CHECK_EQ(sink.gain(), 0.0);
    sink.set_gain(0.5);
    CHECK_EQ(sink.gain(), 0.5);

    CHECK_EQ(sink.gain_calls(), 6);
}

// ---------------------------------------------------------------------------
// FakeRenderer
// ---------------------------------------------------------------------------

TEST_CASE(fake_renderer_records_pts_and_clock_at_presentation) {
    FakeClock clock;
    clock.set(12345);
    FakeRenderer r(&clock);

    Frame f = make_video_frame(6789);
    CHECK_EQ(r.present(f, 0), SYP_OK);

    REQUIRE(r.shown().size() == 1);
    CHECK_EQ(r.shown()[0].pts_us, int64_t{6789});
    CHECK_EQ(r.shown()[0].at_us, int64_t{12345});
}

TEST_CASE(fake_renderer_injected_failure_is_not_recorded_as_shown) {
    FakeClock clock;
    FakeRenderer r(&clock);
    r.inject_failure_every(1);   // 每次都失败

    Frame f = make_video_frame(0);
    const syp_status st = r.present(f, 0);

    CHECK(st != SYP_OK);
    CHECK(r.shown().empty());    // 失败的那次不能进 shown()
}

// FakeRenderer 记下最近一次几何 / 填充方式与几何调用次数。
TEST_CASE(fake_renderer_records_geometry_and_gravity) {
    FakeRenderer r(nullptr);
    CHECK_EQ(r.geometry_calls(), 0);
    CHECK(r.last_geometry() == std::make_tuple(0, 0, 0));
    CHECK(r.gravity() == syp::media::Gravity::AspectFit);

    r.set_source_geometry(4, 3, 90);
    r.set_source_geometry(16, 11, 270);
    r.set_gravity(syp::media::Gravity::AspectFill);

    CHECK_EQ(r.geometry_calls(), 2);
    CHECK(r.last_geometry() == std::make_tuple(16, 11, 270));
    CHECK(r.gravity() == syp::media::Gravity::AspectFill);
    CHECK(r.shown().empty());   // 几何调用不算呈现
}

int main() { return tiny_test_main(); }
