// test_sync_e2e.cpp — 九条端到端同步场景 A~I。
//
// 这一层是验收核心：解码那一层能拿 FFmpeg 直接解码当参照做逐帧差分——
// 判据来自外部、自己不会说谎。这一层没有这种参照，同步质量能不能被证伪，
// 全靠假时钟 + FakeAudioSink + FakeRenderer 把它变成数值断言，而不是
// "看着还行"。analyze() 是九条共用的判据本身——它若写松了，九条全绿也
// 不构成证据。
//
// 场景字母 → TEST_CASE 对照：
//   A → a_local_file_sync_to_eof                 本地文件稳态播放到尾
//   B → b_dl_layer_matches_a                      经 dl 层播放，且与 A 全等
//   C → c_pause_resume_freezes_position           暂停 / 恢复
//   D → d_speed_scales_presentation_rate          倍速 0.5/1.0/2.0
//   E → e_multi_point_seek_including_rollback     多点 seek（含回退）
//   F → f_early_and_late_arrival_respects_drop_cap 早到 / 晚到
//   G → g_no_audio_track_falls_back_to_system_clock 无音频轨
//   H → h_audio_sink_failure_degrades_and_keeps_playing 音频 sink 中途失败
//   I → i_played_us_reflects_device_latency_and_speed   played_us() 换算
//
// 九次反向自检的实际输出、每条场景的实际数值（呈现帧数/最大漂移/丢帧数
// 等）不在这里列出，这里的注释只放"为什么这样设计"。
//
// 看门狗：照 tests/test_decode_e2e.cpp 的用法，每条场景各包一层
// （soft=60s/hard=180s）——不是形式主义：Pipeline 真的会
// 活锁两次，实测又发现 TrackPlayer 的联合背压硬卡死（双音轨
// 素材 3000 次 step() 只出 2 帧）。
//
// 素材：A/B/C/D/E/H/I 用 gen-fixtures.sh 产出的 bframes_faststart.mp4
// （已知带音频轨、时长 40~100 秒随机、fps 24/25/30 之一，见顶层
// CMakeLists.txt 对 gen-fixtures.sh 的调用与该脚本自身注释）。F/G 需要
// video-only（F 需要精确控制"队列里堆了多少帧待丢"，G 需要"确实没有
// 音频轨"）的素材，标准验证素材矩阵里没有这两种，现场用 ffmpeg CLI 合成
// ——跟 test_track_player.cpp 的 synth_video_only()/TempDir 同一份手法，
// 各自本地一份而不是共享头（那份注释里写得很清楚：两个 TU 各自已经有
// 一套本地脚手架，为几行函数新增共享头不值得）。
#include "frame_digest.h"
#include "media/avio_bridge.h"
#include "media/pipeline.h"
#include "media/track_player.h"
#include "scenarios.h"
#include "support/fake_audio_sink.h"
#include "support/fake_clock.h"
#include "support/fake_renderer.h"
#include "support/loopback_server.h"
#include "support/watchdog.h"
#include "tiny_test.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_source.h>

extern "C" {
#include <libavutil/avutil.h>   // AV_NOPTS_VALUE
#include <unistd.h>             // mkdtemp
}

#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using syp::media::AudioClock;
using syp::media::ClockKind;
using syp::media::Pipeline;
using syp::media::PipelineConfig;
using syp::media::PlayOutcome;
using syp::media::TrackPlayer;
using syp::media::BufferPolicy;
using syp::dl::test::LoopbackConfig;
using syp::dl::test::LoopbackServer;
using syp::test::FakeAudioSink;
using syp::test::FakeClock;
using syp::test::FakeRenderer;

namespace {

// ---------------------------------------------------------------------
// 素材路径 / 现场合成脚手架
// ---------------------------------------------------------------------

std::string fixture(const char* name) {
    const char* dir = std::getenv("SYP_FIXTURE_DIR");
    if (dir == nullptr || *dir == '\0')
        tiny_test::fail(__FILE__, __LINE__, "SYP_FIXTURE_DIR 未设置",
                        "素材路径没配，不是同步本身失败");
    return std::string(dir) + "/" + name;
}

std::string ffmpeg_cli_path() { return SYP_FFMPEG_CLI_PATH; }

bool ffmpeg_available() {
    return std::system(("\"" + ffmpeg_cli_path() + "\" -version > /dev/null 2>&1").c_str()) == 0;
}

// RAII 临时目录：素材现场生成、用完即删，跟 test_track_player.cpp 同一
// 份手法，各自一份（见文件顶部注释）。
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/syp_sync_e2e_XXXXXX";
        char* p     = mkdtemp(tmpl);
        if (p != nullptr) path = p;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// video-only、CFR、无 B 帧的合成素材，给场景 F/G 用——它们需要"确实没
// 有音频轨"（G）或"能精确控制堆积帧数"（F），标准验证素材矩阵里的
// bframes_faststart.mp4 两条都不满足（它有音频轨，且时长/fps 随种子变，
// 没法预先算好堆积量）。
std::string synth_video_only(const std::string& dir, int fps, int frame_count) {
    const std::string path     = dir + "/synth_video.mp4";
    const double       dur_sec = static_cast<double>(frame_count) / static_cast<double>(fps) + 1.0;
    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-f lavfi -i \"testsrc2=size=64x64:rate=" << fps << ":duration=" << dur_sec << "\" "
        << "-frames:v " << frame_count << " "
        << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 1 "
        << "-video_track_timescale " << fps << " "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(cmd.str().c_str()) == 0) ? path : std::string();
}

void prime_pipeline(Pipeline& p, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        const syp::media::StepOutcome so = p.step();
        if (so.kind == syp::media::StepOutcome::Kind::Eof) break;
        if (so.kind == syp::media::StepOutcome::Kind::Error) break;
    }
}

// 场景 A/B 的"无遗漏"判据需要一个跟被测路径完全
// 独立的真实帧数——不能拿 TrackPlayer/Pipeline 自己的产出跟自己比。
// 直接复用已经验证过的参照路径（decode_reference()，纯 libavformat
// + libavcodec，不经过本项目任何组件，跟 test_decode_e2e.cpp 场景 A~D
// 同一份参照来源），按 FrameDigest::width>0 挑出视频帧计数——
// width>0/==0 是这个结构体自己的既有约定（音频为 0、视频为 0 的对偶，
// frame_digest.h 顶部注释），不是本文件新发明的判据。
int64_t count_reference_video_frames(const std::string& path) {
    syp::probe::DecodeOptions opt;
    const syp::probe::DecodeResult ref = syp::probe::decode_reference(path, opt);
    if (!ref.error_stage.empty()) {
        tiny_test::fail(__FILE__, __LINE__, "decode_reference 失败", ref.error_stage);
        return -1;
    }
    int64_t n = 0;
    for (const auto& d : ref.frames) {
        if (d.width > 0) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------
// 共用判据：九条场景的判据本身。写松了，九条全绿也不构成证据——见
// 文件顶部注释。analyze() 内部对 AV_NOPTS_VALUE 的显式拒绝（见该函数
// 内部注释——堵一个当前不会触发、但迟早会被踩到的 UB）。
// ---------------------------------------------------------------------

struct SyncReport {
    int64_t presented       = 0;
    int64_t dropped         = 0;
    int64_t max_abs_drift_us = 0;
    int64_t duplicates      = 0;   // 同一个 pts 被呈现两次
    bool    monotonic       = true;
};

SyncReport analyze(const std::vector<FakeRenderer::Shown>& shown) {
    SyncReport r;
    r.presented = static_cast<int64_t>(shown.size());
    // last_pts 初值用 INT64_MIN（数值上恰好等于 AV_NOPTS_VALUE）只是为了
    // 让"第一帧不会被误判成重复/乱序"这件事成立——第一次比较时 last_pts
    // 不可能等于任何真实 pts。这是巧合的数值重合，不是拿它当"哨兵值"用
    // （下面对 s.at_us 的哨兵检查是显式判断，不依赖这个巧合）。
    int64_t last_pts = INT64_MIN;
    for (const auto& s : shown) {
        // s.at_us 若是 AV_NOPTS_VALUE（场景 H 降级后，
        // ext_clock 转发的是已经 failed() 的 sink，读数恒为这个哨兵值），
        // 下面的 `at_us - pts_us` 是 INT64_MIN 减一个正数——有符号整数
        // 溢出，UB。当前九条场景里只有 H 会产出这种 Shown，且 H 从不调
        // analyze()（只读 .size()），所以这条 UB 今天不会被触发；但
        // analyze() 是九条共用的判据本身，谁哪天给 H 加一句 analyze()
        // 就会实际触发。显式拒绝，不做这个减法。
        if (s.at_us == AV_NOPTS_VALUE) {
            tiny_test::fail(__FILE__, __LINE__, "analyze(): s.at_us == AV_NOPTS_VALUE",
                            "clock 已经 failed() 之后仍被记进 shown——调用方场景需要先排除"
                            "降级后呈现的帧，或该场景根本不该用 analyze() 这个判据");
            continue;
        }
        const int64_t drift = s.at_us - s.pts_us;
        const int64_t a     = drift < 0 ? -drift : drift;
        if (a > r.max_abs_drift_us) r.max_abs_drift_us = a;
        if (s.pts_us == last_pts) ++r.duplicates;
        if (s.pts_us < last_pts) r.monotonic = false;
        last_pts = s.pts_us;
    }
    return r;
}

// 把一条播放跑到尾。fake sink 由 step() 的节奏驱动"消费"，所以整条链是
// 确定性的：不 sleep、不碰真实时间。
struct RunResult {
    std::vector<FakeRenderer::Shown> shown;
    int64_t   dropped     = 0;
    ClockKind clock       = ClockKind::System;
    bool      reached_eof = false;
};

RunResult run_to_eof(TrackPlayer& p, FakeAudioSink& sink, int32_t max_steps = 2'000'000) {
    RunResult rr;
    for (int32_t i = 0; i < max_steps; ++i) {
        const PlayOutcome o = p.step();
        if (o.kind == PlayOutcome::Kind::Eof) { rr.reached_eof = true; break; }
        if (o.kind == PlayOutcome::Kind::Error) break;
        // Waiting 表示"帧还没到时刻"——推进 fake sink 的消费即推进音频时钟。
        if (o.kind == PlayOutcome::Kind::Waiting) sink.advance(5000);
    }
    rr.dropped = p.dropped_frames();
    rr.clock   = p.clock_kind();
    return rr;
}

// run_to_eof 的 FakeClock 变体——给没有音频轨（场景 G）的播放用：没有
// sink 可推进，直接拨假时钟本身。
RunResult run_to_eof_clock(TrackPlayer& p, FakeClock& clock, int64_t chunk_us,
                            int32_t max_steps = 2'000'000) {
    RunResult rr;
    for (int32_t i = 0; i < max_steps; ++i) {
        const PlayOutcome o = p.step();
        if (o.kind == PlayOutcome::Kind::Eof) { rr.reached_eof = true; break; }
        if (o.kind == PlayOutcome::Kind::Error) break;
        if (o.kind == PlayOutcome::Kind::Waiting) clock.advance(chunk_us);
    }
    rr.dropped = p.dropped_frames();
    rr.clock   = p.clock_kind();
    return rr;
}

// 按"设备时间预算"或"固定 step() 调用次数"跑一段（不追到 EOF）：预算
// 耗尽、调用次数耗尽、或 Eof/Error 提前收工就停。presented_delta 数的是
// 这一段内真正 Kind::Presented 的次数（不经 renderer 转手——present() 在
// 这些用例里从不注入失败，直接数 outcome 更直接，少一层"renderer 是否
// 如实记录"的假设）。给场景 C/D/H/I 的"播一段、量一段"共用——D/I 传一个
// 大到不会先撞上的 device_us_budget、靠 max_steps 卡住调用次数（见场景
// D 顶部长注释：固定预算/播到 EOF 两种写法都实测撞过真实的驱动死角）。
struct BudgetRunResult {
    int64_t device_us_spent = 0;
    int64_t presented_delta = 0;
    bool    reached_eof     = false;
    bool    errored         = false;
    int64_t dropped_delta   = 0;
    int64_t queued_delta    = 0;
    int64_t waiting_delta   = 0;
    int64_t blocked_delta   = 0;
};

BudgetRunResult run_budget(TrackPlayer& p, FakeAudioSink& sink, int64_t device_us_budget,
                            int64_t chunk_us = 5000, int32_t max_steps = 5'000'000) {
    BudgetRunResult r;
    for (int32_t i = 0; i < max_steps && r.device_us_spent < device_us_budget; ++i) {
        const PlayOutcome o = p.step();
        if (o.kind == PlayOutcome::Kind::Eof) { r.reached_eof = true; break; }
        if (o.kind == PlayOutcome::Kind::Error) { r.errored = true; break; }
        if (o.kind == PlayOutcome::Kind::Waiting) {
            sink.advance(chunk_us);
            r.device_us_spent += chunk_us;
            ++r.waiting_delta;
        }
        if (o.kind == PlayOutcome::Kind::Presented) ++r.presented_delta;
        if (o.kind == PlayOutcome::Kind::Dropped) ++r.dropped_delta;
        if (o.kind == PlayOutcome::Kind::Queued) ++r.queued_delta;
        if (o.kind == PlayOutcome::Kind::Blocked) ++r.blocked_delta;
    }
    return r;
}

// seek 之后跑到第一帧呈现为止（至多再多呈现 target_presented 帧），
// 记录"呈现之前有没有任何一帧被判丢"——just_sought_ 的契约是"seek 后
// 第一帧不经三分支判定、立即呈现"，若这条契约被破坏，落地的可观测后果
// 就是这一帧反而先被正常窗口判定命中"太迟"分支、以 Dropped 收场。
struct SeekSegment {
    bool    ok                     = false;   // 确实呈现过至少一帧
    bool    dropped_before_present = false;
    // seek 后立即呈现的那一帧，及呈现那一刻的时钟
    // 读数——"随后把时钟基准设为该帧的实际 pts"这半句，此前
    // 只验过"呈现的是 Presented 不是 Dropped"，没验过基准有没有被真实
    // pts（而不是请求的 ts_us）纠偏。
    int64_t first_presented_pts       = -1;
    int64_t first_presented_clock_us  = -1;
};

SeekSegment run_post_seek_segment(TrackPlayer& p, FakeAudioSink& sink, int32_t target_presented,
                                   int32_t max_steps) {
    SeekSegment r;
    bool    seen_present    = false;
    int32_t presented_count = 0;
    for (int32_t i = 0; i < max_steps && presented_count < target_presented; ++i) {
        const PlayOutcome o = p.step();
        if (o.kind == PlayOutcome::Kind::Error) break;
        if (o.kind == PlayOutcome::Kind::Eof) break;
        if (o.kind == PlayOutcome::Kind::Waiting) sink.advance(5000);
        if (o.kind == PlayOutcome::Kind::Dropped && !seen_present) r.dropped_before_present = true;
        if (o.kind == PlayOutcome::Kind::Presented) {
            if (!seen_present) {
                // 必须在**这一次** step() 调用之后立即读 position_us()
                // ——晚一步（比如等整段跑完再读）时钟已经被后续的
                // Presented/Waiting 推进过，读到的就不是"呈现那一刻"的
                // 基准了。
                r.first_presented_pts      = o.pts_us;
                r.first_presented_clock_us = p.position_us();
            }
            seen_present = true;
            ++presented_count;
        }
    }
    r.ok = seen_present;
    return r;
}

// ---------------------------------------------------------------------
// TrackPlayer fixture：真实素材 + FakeAudioSink + FakeRenderer。
// ---------------------------------------------------------------------

// ext_clock：给 FakeRenderer 记"呈现时刻"用的一份独立 AudioClock，指向
// 跟 TrackPlayer 内部那份完全相同的 sink——AudioClock::now_us() 只是纯
// 转发 sink_->played_us()（time_source.cpp），没有任何内部状态，两份
// 各自持有裸指针指向同一个 sink，读数必然逐比特一致。这样 FakeRenderer
// 记录的 at_us 就是 TrackPlayer 自己那一刻会读到的同一个时钟读数，不是
// 另一条脱钩的计时轴。
//
// 声明顺序：ext_clock 先声明、后析构——它只是指向 sink 的裸指针包装，
// 本身不持有任何东西，谁先谁后并不会引发 UAF，这里仍然按"跟生命周期
// 有关的东西尽量晚析构"的既有约定来（test_track_player.cpp
// SeekFailMaterial 同一个理由）。
struct FileFixture {
    std::unique_ptr<AudioClock>  ext_clock;
    std::unique_ptr<TrackPlayer> player;
    FakeAudioSink*               sink     = nullptr;
    FakeRenderer*                renderer = nullptr;
};

#define SE_REQUIRE(expr)                                                \
    do {                                                                \
        if (!(expr)) {                                                 \
            ::tiny_test::fail(__FILE__, __LINE__, #expr);               \
            return fx;                                                 \
        }                                                               \
    } while (0)

FileFixture make_player_from_pipeline(std::unique_ptr<Pipeline> pipeline) {
    FileFixture fx;
    SE_REQUIRE(pipeline != nullptr);

    auto            sink     = std::make_unique<FakeAudioSink>();
    FakeAudioSink*  sink_ptr = sink.get();
    fx.ext_clock             = std::make_unique<AudioClock>(sink_ptr);

    auto           renderer     = std::make_unique<FakeRenderer>(fx.ext_clock.get());
    FakeRenderer*  renderer_ptr = renderer.get();

    syp_status err = SYP_OK;
    fx.player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                     nullptr, &err, BufferPolicy::disabled());
    SE_REQUIRE(fx.player != nullptr);
    fx.sink     = sink_ptr;
    fx.renderer = renderer_ptr;
    return fx;
}

FileFixture make_player_from_file(const std::string& path, const PipelineConfig& cfg = {}) {
    FileFixture fx;
    syp_status  err      = SYP_OK;
    auto        pipeline = Pipeline::create_file(path, cfg, &err);
    SE_REQUIRE(pipeline != nullptr);
    return make_player_from_pipeline(std::move(pipeline));
}

#undef SE_REQUIRE

// run_budget()（场景 C/D/H/I 用）会把一条播放拆成多段"跑一阵、停下来做
// 点别的事（pause/set_speed/inject_failure/查 position_us）、再跑一阵"。
// 这里给这几个场景一份比默认（max_frames_per_track=8）更深的 FrameQueue。
//
// 【这段注释此前描述的是修复前的行为，已重写】
// 旧注释的结论是"这是测试驱动方式本身的死角"，两句原文如下，逐字保留在
// 这里是因为它们**错得有代表性**：
//
//   「而 `TrackPlayer::step()` 的视频早到分支在这种时候不会反过来驱动
//     Pipeline（那是刻意的调度顺序，见 track_player.cpp step() 第 2 步
//     注释）」
//   「（不是 TrackPlayer 的缺陷：真实设备的环形缓冲以秒为单位……）」
//
// 后来发现这两句判断都不成立：那不是刻意的调度顺序，
// 是一条真实的永久停摆（音频 FrameQueue 空 ∧ 视频早到 → step() 成为纯
// 空操作，两条队列都不会再补货，闭环）。后来修了实现——**今天视频早到分支跳出循环而不是 return，
// 仍然会走到第 3 步驱动一次 Pipeline::step()**（见 track_player.cpp 该
// 分支内的长注释，以及回归用例 test_track_player.cpp::
// step_does_not_livelock_when_audio_queue_empty_and_video_early）。旧注释
// 却把已被推翻的说法当结论、还把 track_player.cpp 引为权威，是本仓库栽
// 过两次的"文档说反话"同一形状。
//
// 那么这份加宽配置今天还剩什么理由？**实测：没有硬理由。** 把
// max_frames_per_track 改回默认 8 复跑，九条场景 A~I 全绿——这个问题修掉
// 之后，加宽不再是任何断言成立的前提。保留 64 是取"更深的解码领先队列"
// 这一侧：它让 C/D/H/I 这几条"跑一阵停一阵"的场景在每段预算窗口里少花
// 步数等待补货、多花步数验证同步本身，跟 A/B 的连续播放形成不同的队列
// 深度组合。**它不再是任何缺陷的绕行**——这行数字如果哪天碍事，直接删
// 掉即可，不需要先解决什么。
PipelineConfig wide_frame_config() {
    PipelineConfig cfg;
    cfg.max_frames_per_track = 64;
    return cfg;
}

}  // namespace

// =======================================================================
// 场景 A：本地文件稳态播放到尾。
// =======================================================================
TEST_CASE(a_local_file_sync_to_eof) {
    syp::test::Watchdog wd("a_local_file_sync_to_eof", /*soft_ms=*/60000, /*hard_ms=*/180000,
                           "疑似卡在 Pipeline::step() 内部解码器，或 TrackPlayer 的音视频"
                           "联合背压——见 track_player.h 顶部注释。");

    const std::string path = fixture("bframes_faststart.mp4");
    // 「除策略性丢帧外无遗漏」这条判据原来只打印不断言，`presented` 也
    // 从未跟素材真实帧数比过，静默丢 10% 视频帧这种确定性缺陷全仓库
    // 21 条测试没有一条能抓住。这里补上：跟同一份独立参照
    // （decode_reference()，见 count_reference_video_frames()）比对。
    const int64_t ref_video_frames = count_reference_video_frames(path);
    REQUIRE(ref_video_frames > 500);

    auto fx = make_player_from_file(path);
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    RunResult rr = run_to_eof(*fx.player, *fx.sink);
    rr.shown          = fx.renderer->shown();
    const SyncReport rep = analyze(rr.shown);

    std::printf("  [A] presented=%lld dropped=%lld max_drift=%lldus dup=%lld eof=%d clock=%d "
                "ref_video_frames=%lld\n",
                static_cast<long long>(rep.presented), static_cast<long long>(rr.dropped),
                static_cast<long long>(rep.max_abs_drift_us),
                static_cast<long long>(rep.duplicates), static_cast<int>(rr.reached_eof),
                static_cast<int>(rr.clock), static_cast<long long>(ref_video_frames));

    REQUIRE(rr.reached_eof);
    REQUIRE(rep.presented > 500);   // 空转防护：真的播了东西
    // 无遗漏：稳态播放（不涉及 seek/晚到丢帧）不应该丢任何一帧，呈现数
    // 精确等于独立参照数出的真实视频帧数——不是"presented 够大就算过"。
    CHECK_EQ(rr.dropped, int64_t{0});
    CHECK_EQ(rep.presented, ref_video_frames);
    // 补一条独立于实现常数的绝对上限——`analyze()` 比
    // 较用的 kPresentWindowUs/kDropThresholdUs 若被同步改松，
    // 符号常数本身就是判据的一部分，判据会跟着实现一起变松、
    // 九条全绿却测不出真实的同步质量退化。字面量 80000（等于当前
    // kDropThresholdUs 的真实值）不随符号常数变化。同时把符号版判据从
    // kPresentWindowUs 改成 kDropThresholdUs：稳态播放理论上允许
    // 呈现"迟到 40~80ms"的帧（滞回带），用 kPresentWindowUs
    // 卡的话会跟这条契约矛盾，即使九条至今都没能真正踩进这条带子
    // （见 F 场景）。
    CHECK(rep.max_abs_drift_us <= syp::media::kDropThresholdUs);
    CHECK(rep.max_abs_drift_us <= 80000);
    CHECK_EQ(rep.duplicates, int64_t{0});
    CHECK(rep.monotonic);
    CHECK(rr.clock == ClockKind::Audio);
}

// =======================================================================
// 场景 B：经 dl 层（LoopbackServer + AvioBridge）播放，同 A 的判据，
// 且呈现序列与 A 逐项全等——单独断言，不靠"两边各自合格"推传递性。
// =======================================================================
TEST_CASE(b_dl_layer_matches_a) {
    syp::test::Watchdog wd("b_dl_layer_matches_a", /*soft_ms=*/60000, /*hard_ms=*/180000,
                           "疑似卡在 dl 层的阻塞读，或 Pipeline::step() 内部解码器——"
                           "AvioBridge 之下的 syp_source 同理。");

    const std::string path = fixture("bframes_faststart.mp4");
    // 同场景 A：独立参照数出的真实视频帧数，
    // A 侧、B 侧都要跟它精确比对，不能只是"presented 够大"。
    const int64_t ref_video_frames = count_reference_video_frames(path);
    REQUIRE(ref_video_frames > 500);

    // A 侧：独立跑一遍（不复用场景 A 用例的结果——用例之间不该有隐藏
    // 依赖）。
    auto fx_a = make_player_from_file(path);
    REQUIRE(fx_a.player != nullptr);
    RunResult rr_a = run_to_eof(*fx_a.player, *fx_a.sink);
    rr_a.shown         = fx_a.renderer->shown();
    const SyncReport rep_a = analyze(rr_a.shown);
    REQUIRE(rr_a.reached_eof);
    REQUIRE(rep_a.presented > 500);
    CHECK_EQ(rr_a.dropped, int64_t{0});
    CHECK_EQ(rep_a.presented, ref_video_frames);

    // B 侧：经 LoopbackServer + AvioBridge。跟 test_decode_e2e.cpp 场景 B
    // 同一套脚手架（ensure_apple_backend + syp_source_open + AvioBridge）。
    REQUIRE(syp::probe::ensure_apple_backend());
    LoopbackConfig cfg;
    cfg.body_file = path;
    cfg.etag      = "\"sync-e2e-b\"";
    LoopbackServer srv(cfg);

    const syp::probe::TempCacheDir cache_guard("sync_e2e_b");
    syp_config sc;
    syp_config_init(&sc);
    sc.struct_size = sizeof(syp_config);
    sc.cache_dir   = cache_guard.path.c_str();

    syp_source* src = nullptr;
    const syp_status open_st =
        syp_source_open(&src, srv.url("/media.mp4").c_str(), nullptr, &sc, nullptr);
    REQUIRE(open_st == SYP_OK);
    REQUIRE(src != nullptr);

    auto bridge = syp::media::AvioBridge::create(src, 64 * 1024);
    REQUIRE(bridge != nullptr);

    syp_status perr      = SYP_OK;
    auto       pipeline_b = Pipeline::create_avio(bridge->ctx(), PipelineConfig{}, &perr);
    REQUIRE(pipeline_b != nullptr);

    RunResult  rr_b;
    SyncReport rep_b;
    {
        auto fx_b = make_player_from_pipeline(std::move(pipeline_b));
        REQUIRE(fx_b.player != nullptr);
        rr_b = run_to_eof(*fx_b.player, *fx_b.sink);
        rr_b.shown = fx_b.renderer->shown();
        rep_b      = analyze(rr_b.shown);
    }   // fx_b（及它内部的 Pipeline/Demuxer）必须先于 bridge/src 析构——
        // AvioBridge 不接管 ctx 的所有权，Demuxer 在整个生命周期内都会
        // 用它读数据（avio_bridge.h 顶部注释）。

    const int last_averror = bridge->diag().last_averror;
    bridge.reset();
    syp_source_close(src);

    std::printf("  [B] presented=%lld dropped=%lld max_drift=%lldus dup=%lld eof=%d clock=%d "
                "last_averror=%d ref_video_frames=%lld\n",
                static_cast<long long>(rep_b.presented), static_cast<long long>(rr_b.dropped),
                static_cast<long long>(rep_b.max_abs_drift_us),
                static_cast<long long>(rep_b.duplicates), static_cast<int>(rr_b.reached_eof),
                static_cast<int>(rr_b.clock), last_averror,
                static_cast<long long>(ref_video_frames));

    REQUIRE(rr_b.reached_eof);
    REQUIRE(rep_b.presented > 500);
    // 无遗漏（同场景 A）：dl 层这条路径同样不该丢帧。
    CHECK_EQ(rr_b.dropped, int64_t{0});
    CHECK_EQ(rep_b.presented, ref_video_frames);
    // I3/I4（同场景 A）：字面量绝对上限 + 符号版判据改用 kDropThresholdUs。
    CHECK(rep_b.max_abs_drift_us <= syp::media::kDropThresholdUs);
    CHECK(rep_b.max_abs_drift_us <= 80000);
    CHECK_EQ(rep_b.duplicates, int64_t{0});
    CHECK(rep_b.monotonic);
    CHECK(rr_b.clock == ClockKind::Audio);

    // 单独断言：呈现序列与 A 逐项全等（pts 与呈现时刻都要对上）。
    REQUIRE(rr_a.shown.size() == rr_b.shown.size());
    bool   all_equal   = true;
    size_t mismatch_at = 0;
    for (size_t i = 0; i < rr_a.shown.size(); ++i) {
        if (rr_a.shown[i].pts_us != rr_b.shown[i].pts_us ||
            rr_a.shown[i].at_us != rr_b.shown[i].at_us) {
            all_equal   = false;
            mismatch_at = i;
            break;
        }
    }
    if (!all_equal) {
        std::printf("  [B] 首个不匹配 idx=%zu a=(pts=%lld,at=%lld) b=(pts=%lld,at=%lld)\n",
                    mismatch_at, static_cast<long long>(rr_a.shown[mismatch_at].pts_us),
                    static_cast<long long>(rr_a.shown[mismatch_at].at_us),
                    static_cast<long long>(rr_b.shown[mismatch_at].pts_us),
                    static_cast<long long>(rr_b.shown[mismatch_at].at_us));
    }
    CHECK(all_equal);
}

// =======================================================================
// 场景 C：暂停 / 恢复。暂停期间 position_us() 冻结；恢复后位置不跳变。
//
// 【行为调整】此前这里断言"暂停期间 seek() 后仍然零呈现、全程
// Waiting，只有 play() 之后才会兑现 just_sought_"——后来改成了
// 反过来的行为：暂停不再压住"seek 后第一帧立即呈现"这条契约。下面的
// 断言已经改成"这段暂停期间恰好出现一次 Presented（seek 落点那一帧），
// 其余全是 Waiting"，其余守卫（暂停期间不写音频、位置精确复位到落点
// pts、恢复瞬间不跳变）保持不变，见各断言点的注释。
// =======================================================================
TEST_CASE(c_pause_resume_freezes_position) {
    syp::test::Watchdog wd("c_pause_resume_freezes_position", /*soft_ms=*/60000,
                           /*hard_ms=*/180000, nullptr);

    auto fx = make_player_from_file(fixture("bframes_faststart.mp4"), wide_frame_config());
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    // 先播一段，确认真的在推进（呈现过至少几十帧），不是构造完就暂停这
    // 种零延迟场景——零延迟会巧合掩盖"忘记结算"这类
    // 缺陷。
    const BudgetRunResult warm = run_budget(*fx.player, *fx.sink, /*device_us_budget=*/500'000);
    REQUIRE(!warm.errored);
    REQUIRE(warm.presented_delta > 5);

    const size_t  shown_before_pause = fx.renderer->shown().size();

    fx.player->pause();
    REQUIRE(fx.player->paused());

    // 暂停期间 seek()——刻意制造一个"如果暂停分支被短路，会立刻露馅"的
    // 局面：seek() 会置位 just_sought_，其契约是"下一帧视频不经三分支
    // 判定、立即呈现"。若暂停分支正常工作，这条契约在暂停期间根本没有
    // 机会被消费（video_early 之前的整段判定都被暂停分支挡在外面），
    // 恢复后才会兑现——这正是 test_track_player.cpp 组合 2
    // （pause_then_seek_then_play_presents_first_frame_immediately）已经
    // 验证过的契约。但若暂停分支被短路、退回正常判定，第一次 step() 就
    // 会撞上 just_sought_，立刻呈现——不需要等 play()。这比"position_us()
    // 没变"更有区分力：本文件早先一版只查 position_us()/written_frames()
    // 时，在这份素材的某些时序下两者都被 sink 自己的 pause() 冻结顺带
    // 掩盖了（clock 冻结 ⇒ 视频恰好一直判"早了"，跟真正暂停的外部表现
    // 巧合一致）——这是本文件早先一版踩过的坑。
    REQUIRE(fx.player->seek(0) == SYP_OK);
    // written_frames_ 的基线必须在 seek() **之后**取——seek() 本身就会
    // flush() sink（正常契约的第 2 步），把 written_frames_/consumed_
    // frames_ 都清零；如果在 seek() 之前取基线，seek() 自己造成的清零会
    // 被误判成"暂停期间写了音频"，那是测试自己的账算错了，不是真的
    // 缺陷。
    const int64_t written_before_pause = fx.sink->written_frames();

    // 暂停期间反复 step()，且故意试图推进 sink——advance() 在 sink 已被
    // pause() 冻结时应当是空操作。暂停不再压住"seek 后第
    // 一帧立即呈现"——上面这次 seek() 留下的 just_sought_ 会在暂停期间
    // 的某一次 step() 就被消费掉（present_first_frame_after_seek()），
    // 不必等 play()；这段循环里因此**恰好该出现一次** Presented（seek
    // 落点那一帧），不能是别的次数、也不能一次都没有。
    bool    saw_seek_frame = false;
    int64_t seek_frame_pts = AV_NOPTS_VALUE;
    for (int i = 0; i < 50; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Presented) {
            REQUIRE(!saw_seek_frame);   // 只应该出现这一次
            saw_seek_frame = true;
            seek_frame_pts = o.pts_us;
        } else {
            CHECK(o.kind == PlayOutcome::Kind::Waiting);
        }
        fx.sink->advance(5000);
    }
    REQUIRE(saw_seek_frame);
    CHECK_EQ(fx.renderer->shown().size(), shown_before_pause + 1);
    // "不写音频"单独断言——seek() 之后 pending_audio_ 已清空，若暂停
    // 分支的音频判定被误开了口子，音频分支会重新 pop 并 write() 新位置
    // 的音频帧；暂停分支只处理视频（预览/seek 首帧），不碰音频。
    CHECK_EQ(fx.sink->written_frames(), written_before_pause);
    // seek(0) 落点那一帧的实际 pts 应该就是 AudioClock 的新基准
    // （present_first_frame_after_seek() 用它 flush sink），不应该在
    // 暂停期间又跑走。
    CHECK_EQ(fx.player->position_us(), seek_frame_pts);

    // 恢复前再多趁着暂停攒一段解码存量——seek() 清空了 Pipeline 的队列，
    // 从头起播要先有解码存量才能稳定出画，见 wide_frame_config() 顶部
    // 注释与场景 D 顶部长注释（暂停分支无条件驱动 Pipeline，天然适合
    // 干这件事，不会撞上"视频早到分支不驱动 Pipeline"那条死角）。
    for (int i = 0; i < 3000; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
    }

    fx.player->play();
    REQUIRE(!fx.player->paused());
    const int64_t pos_after_resume = fx.player->position_us();
    // 恢复瞬间不应跳变：sink_->resume() 不改基准，AudioClock 直接转发
    // sink_->played_us()，没有多余的墙钟结算逻辑可以跳——暂停期间位置
    // 钉在 seek(0) 落地那一帧的 pts，恢复瞬间也还应该是同一个值。
    CHECK_EQ(pos_after_resume, seek_frame_pts);

    // 恢复后继续跑，确认真的没卡住——呈现数应继续增长（正向量断言，不
    // 是只看"没报错"）。
    const BudgetRunResult after = run_budget(*fx.player, *fx.sink, 500'000);
    REQUIRE(!after.errored);
    CHECK(after.presented_delta > 5);

    std::printf("  [C] shown_before_pause=%zu presented_after_resume=%lld\n", shown_before_pause,
                static_cast<long long>(after.presented_delta));
}

// =======================================================================
// 场景 D：倍速 0.5/1.0/2.0。呈现速率与 speed 成比例；每次变速
// flush_count() +1。
//
// 每档速度各自独立构造一份全新的 fixture，set_speed() 在还没跑过一次
// step() 之前立即调用（flush() 清空的是本来就空的缓冲，无可损失），然后
// 用 run_budget() 驱动**固定的 step() 调用次数**（不是固定的设备时间
// 预算，靠 max_steps 卡住，device_us_budget 传一个大到不会先撞上的
// 值）——这是跟场景 A/B 同一套驱动节奏（只在 Waiting 上推进 fake sink），
// 只是运行长度换成"固定次数的 step() 调用"而不是"固定的设备时间预算"或
// "播到 EOF"。
//
// 为什么不是后两种写法：早先做过完整的排查；修掉"视频早到分支不再跳过
// 驱动 Pipeline"这个 bug 之后，原先那条"固定设备时间预算会撞上早到停摆
// 死角"的结论已经不成立——这里只保留仍然有效的部分，过时的部分删掉，
// 别误导后来者去找一个已经不存在的死角：
//   · 播到 EOF 再比较总耗时：连续跑满全场（近 90 秒媒体时长）仍然会撞上
//     一个独立的死角——sink 侧的写入容量（capacity_frames_，默认约
//     1 秒）如果被"提前写入但迟迟不被消费"的音频占满，write() 持续
//     失败，TrackPlayer::step() 判定 audio_blocked；这条测试驱动方式只
//     在 Waiting 上推进设备时间，遇到 Blocked 不推进，长时间维持
//     Blocked 会让呈现速率的比较失真（见下面第二条）。这条死角仍然
//     存在，跟上面那个 bug 不是同一类。
// 固定次数的 step() 调用（3000 次）不需要真的播到尾，规避了上面这条。
//
// 【反向自检挖出的新死角，替换掉旧注释里"早到分支不驱动
// Pipeline"那条】——这条死角发生在把上面那处修复接上、且仍用早期版本
// 配置（暂停预热到 wide_frame_config() 的 64 深、sink 用默认约 1 秒容量）
// 跑这条用例的时候：
//   1. 三档呈现数从 {19, 36, 70}（早期版本配置）变成
//      {144, 273, 253}——0.5x、1.0x 两档呈现数暴涨（对，暴涨，不是
//      暴跌），2x 反而比 1x 还低，`p_two > p_one` 直接翻红。
//   2. 反向核对暴露了早期版本这条用例真正在测什么：三档打印的
//      `queued`（音频真正写进 sink 的帧数）在早期版本下恒为 64，
//      跟 speed 完全无关——因为"视频早到不驱动 Pipeline"这条老缺陷
//      让 Pipeline 在暂停预热之后再也没有机会解出新内容，`run_budget()`
//      量的其实只是"把暂停预热阶段攒好的固定 64 帧音频、固定深度的
//      视频存量，用不同的时钟速率消费完要花几次 step()"——一个纯粹由
//      时钟倍率决定的量，从未真正测过"持续解码 + 呈现"这条链路。这也
//      是这条用例早期一直被判定"绿"却仍然放过了那个 bug 的原因：
//      它自己就是被这个 bug"喂养"出来的测量方式。
//   3. 后来修复之后，`pipeline_->step()` 真的会在视频早到时被驱动，
//      `queued` 因此随 speed 变化，呈现数因此第一次真实反映"视频早到
//      分支不再是空操作"这件事——但 wide_frame_config() 的 64 深
//      FrameQueue、sink 默认约 1 秒的写入容量，对着一条固定 3000 次的
//      step() 预算，在 2x 下不够用：音频要么写满 sink 触发大量 Blocked
//      （实测 blocked=1461/3000），要么（去掉暂停预热之后）视频来不及
//      解码、大量迟到被丢（实测 dropped=117~152）——2x 呈现数因此反而
//      低于 1x，不是同步逻辑错了，是这条用例给的深度/容量预算太紧。
//
// 【订正上一版结论】上一版在这里把"预热、
// FrameQueue 深度、sink 容量"三个旋钮一起调大（64→512、3000→20000、
// 默认→480000 帧），并写下"呈现数第一次真实反映持续解码吞吐"——
// 用变异隔离逐个旋钮验证，**这两句判断都不成立**，已订正：
//   · 三个旋钮里只有 FrameQueue 深度是必需的——调回 64（其它两个保持
//     调大后的值）会让 2x 档重新撞回上面第 3 点描述的失真（dropped 回
//     升、p_two≈p_one），调回 512 之后失真消失，是唯一有判别力的旋钮。
//   · sink 容量、暂停预热步数调回原值（默认约 1 秒 / 3000 次）
//     之后场景仍然绿，数值不降反升（三档 144/273/499，2x/1x≈1.83×）。
//     调大这两个不是"没用"，是"过犹不及"：480000 帧（约 10.9 秒）会
//     把 sink 写入容量撑到永远填不满，audio_blocked 这条真实反压路径
//     从此在 D/I 两个场景里再也测不到——而它是
//     `FakeAudioSink` 与真实设备之间的一处真实架构落差，不该被自己的
//     测试参数悄悄摘掉覆盖。10.9 秒量级的音频缓冲也不对应任何真实
//     设备（`AudioUnitSink` 典型环深约 1 秒）。
//   · "呈现数第一次真实反映持续解码吞吐"这句同样不成立：三档实测呈现
//     数（144/273/499）全部远小于 512 帧的预解码存量，说明解码从头到
//     尾都不在这条链路的关键路径上——D 现在测的准确说法是"在解码被
//     暂停预热刻意移出关键路径的前提下，呈现速率 ∝ speed"，这对 D 的
//     声明意图（量呈现速率跟 speed 的比例关系，不是量解码吞吐）而言
//     是正确的量法，但不是"持续解码吞吐"。
// 修法：只把 FrameQueue 深度从 64 调到 512，sink 容量、暂停预热步数都
// 保持原值不变。调整后 dropped/blocked 在三档下都是 0（见下面
// 循环体里的诊断 printf），呈现数干净地随 speed 单调、成比例变化
// （0.5x/1x/2x = 144/273/499，2x/1x ≈ 1.83×），既有比例判据
// （0.75×/1.5× 阈值）不需要放宽。
TEST_CASE(d_speed_scales_presentation_rate) {
    syp::test::Watchdog wd("d_speed_scales_presentation_rate", /*soft_ms=*/60000,
                           /*hard_ms=*/180000, nullptr);

    constexpr int32_t kSteps = 3000;

    int64_t presented_for_speed[3] = {0, 0, 0};
    double  speeds[3]              = {0.5, 1.0, 2.0};
    for (int i = 0; i < 3; ++i) {
        // FrameQueue 深度 512——见本 TEST_CASE 上方长注释"反向
        // 自检挖出的新死角"那一段：这是三个旋钮里唯一必需的一个（用
        // 变异隔离验证过，调回 wide_frame_config() 的 64 深会让 2x 档
        // 重新撞回"批量丢帧、呈现数追不上"）。sink 容量、暂停预热步数
        // 都保持默认/原值——同样验证过它们不是必需的，调大
        // 反而会把 audio_blocked 这条真实反压路径从这两条场景的覆盖里
        // 摘掉，且 10.9 秒量级的音频缓冲不对应任何真实设备（AudioUnitSink
        // 典型环深约 1 秒）。
        PipelineConfig hugecfg;
        hugecfg.max_frames_per_track = 512;
        auto fx = make_player_from_file(fixture("bframes_faststart.mp4"), hugecfg);
        REQUIRE(fx.player != nullptr);
        REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

        const int32_t flush_before = fx.sink->flush_count();
        REQUIRE(fx.player->set_speed(speeds[i]) == SYP_OK);
        CHECK_EQ(fx.sink->flush_count(), flush_before + 1);   // 每次变速 flush_count() +1

        // 暂停预热：track_player.cpp 第 4 步（暂停分支无条件驱动
        // Pipeline::step()）把两条 FrameQueue 灌到接近上限，恢复播放后
        // 音频分支能立刻、连续地把已经解码好的帧 pop-and-write 回
        // sink，攒出一段测量窗口（3000 次 step()）用得上的存量——这条
        // 预热本身在那处修复之后已经不是"绕开活锁"的必需品（视频早到
        // 分支现在自己会驱动 Pipeline），但仍然是这条用例量出干净的
        // "呈现速率 ∝ speed"这个结果所必需的，见上方长注释。步数维持
        // 原值 3000（用变异隔离验证过调大到 20000 不改变
        // 结果，属于可以但不必要的余量）。
        fx.player->pause();
        for (int32_t k = 0; k < 3000; ++k) {
            const PlayOutcome o = fx.player->step();
            REQUIRE(o.kind != PlayOutcome::Kind::Error);
        }
        fx.player->play();

        const BudgetRunResult r =
            run_budget(*fx.player, *fx.sink, /*device_us_budget=*/INT64_MAX,
                       /*chunk_us=*/5000, /*max_steps=*/kSteps);
        REQUIRE(!r.errored);
        presented_for_speed[i] = r.presented_delta;
        std::printf("  [D] speed=%.1f presented(%d steps)=%lld device_us_spent=%lld "
                    "dropped=%lld queued=%lld waiting=%lld blocked=%lld\n",
                    speeds[i], kSteps, static_cast<long long>(presented_for_speed[i]),
                    static_cast<long long>(r.device_us_spent), static_cast<long long>(r.dropped_delta),
                    static_cast<long long>(r.queued_delta), static_cast<long long>(r.waiting_delta),
                    static_cast<long long>(r.blocked_delta));
    }

    const int64_t p_half = presented_for_speed[0];
    const int64_t p_one  = presented_for_speed[1];
    const int64_t p_two  = presented_for_speed[2];

    REQUIRE(p_one > 20);   // 空转防护：1x 档同样次数的 step() 调用里确实呈现了不少帧
    CHECK(p_two > p_one);
    CHECK(p_one > p_half);
    // 比例宽松但有判别力：期望 2x≈2×p_one、0.5x≈0.5×p_one。
    CHECK(p_two > p_one + p_one / 2);    // > 1.5×
    CHECK(p_half < p_one - p_one / 4);   // < 0.75×

    // 补一条：同一个存活实例上连续多次变速，flush_count() 应该逐次 +1
    // ——上面三档各用独立 fixture 测的是"变速本身生效"，这里单独确认
    // "连续多次调用"这件事也如实累加，不是只在"第一次调用"这种特殊情形
    // 下才对。
    //
    // 光验 flush_count()+1 不够——那只证明"调用过
    // flush()"，不证明"传给 flush() 的基准是对的"。若实现恒传一个陈旧
    // 值（比如 0），flush_count() 照样 +1，但 position_us() 会跳变。
    // 这里先用暂停预热把位置推到一个有意义的非零值（构造完立即调用测
    // 不出这条——position 那时恒为 0，"没跳变"和"传了错基准恰好也是
    // 0"两种情况分不开，见 D 顶部长注释同一个陷阱），再连续变速，逐次
    // 断言 position_us() 在 set_speed() 前后连续（不跳变）。
    {
        auto fx2 = make_player_from_file(fixture("bframes_faststart.mp4"), wide_frame_config());
        REQUIRE(fx2.player != nullptr);
        fx2.player->pause();
        for (int32_t k = 0; k < 3000; ++k) {
            const PlayOutcome o = fx2.player->step();
            REQUIRE(o.kind != PlayOutcome::Kind::Error);
        }
        fx2.player->play();
        const BudgetRunResult warm2 = run_budget(*fx2.player, *fx2.sink, 300'000);
        REQUIRE(!warm2.errored);
        REQUIRE(fx2.player->position_us() > 100'000);   // 确认真的推进过，不是巧合的 0

        int32_t before = fx2.sink->flush_count();
        for (double s : {1.3, 0.6, 2.0, 0.9}) {
            const int64_t pos_before = fx2.player->position_us();
            REQUIRE(fx2.player->set_speed(s) == SYP_OK);
            CHECK_EQ(fx2.sink->flush_count(), before + 1);
            const int64_t pos_after = fx2.player->position_us();
            CHECK_EQ(pos_after, pos_before);
            before = fx2.sink->flush_count();
        }
    }
}

// =======================================================================
// 场景 E：多点 seek（含一次回退 seek）。每次 seek 后首帧立即呈现；此后
// 该 seek 段内仍满足 A 的断言。
// =======================================================================
TEST_CASE(e_multi_point_seek_including_rollback) {
    syp::test::Watchdog wd("e_multi_point_seek_including_rollback", /*soft_ms=*/60000,
                           /*hard_ms=*/180000, nullptr);

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(fixture("bframes_faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);
    int64_t duration_us = 0;
    for (const auto& t : pipeline->tracks()) {
        if (t.duration_us > duration_us) duration_us = t.duration_us;
    }
    // 素材已知时长 40~100 秒（gen-fixtures.sh），10 秒是留了大量余量的
    // 下限——不是巧合通过的值。
    REQUIRE(duration_us > 10'000'000);

    auto fx = make_player_from_pipeline(std::move(pipeline));
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    const BudgetRunResult warm = run_budget(*fx.player, *fx.sink, 300'000);
    REQUIRE(!warm.errored);

    // 四个 seek 点：前进、再前进、回退（比上一个点早——"含回退 seek"这
    // 条要求）、再前进到接近尾部。
    const int64_t seek_points[4] = {
        duration_us / 10,
        duration_us / 2,
        duration_us / 5,     // 回退：早于上一个点（duration_us/2）
        duration_us * 4 / 5,
    };

    for (size_t i = 0; i < 4; ++i) {
        const int64_t     target = seek_points[i];
        const syp_status   rc     = fx.player->seek(target);
        REQUIRE(rc == SYP_OK);

        const size_t before_size = fx.renderer->shown().size();
        const SeekSegment seg =
            run_post_seek_segment(*fx.player, *fx.sink, /*target_presented=*/30, /*max_steps=*/20000);
        REQUIRE(seg.ok);                       // 首帧确实呈现了，不是一直 Waiting/Dropped
        CHECK(!seg.dropped_before_present);     // 呈现之前没有任何一帧被判丢——证明走的是
                                                 // just_sought_ 立即呈现，不是退回正常窗口判定
                                                 // 后侥幸没丢。
        // 时钟基准精确等于首帧的**真实 pts**，不是请求
        // 的 target——seek 落点是最近的关键帧，通常早于 target（"随后把
        // 时钟基准设为该帧的实际 pts"）。
        CHECK_EQ(seg.first_presented_clock_us, seg.first_presented_pts);

        const std::vector<FakeRenderer::Shown>& all = fx.renderer->shown();
        const std::vector<FakeRenderer::Shown> segment(
            all.begin() + static_cast<std::ptrdiff_t>(before_size), all.end());
        const SyncReport rep = analyze(segment);
        std::printf("  [E] seek#%zu target=%lld first_pts=%lld first_clock=%lld presented=%lld "
                    "max_drift=%lldus dup=%lld dropped_before_present=%d\n",
                    i, static_cast<long long>(target), static_cast<long long>(seg.first_presented_pts),
                    static_cast<long long>(seg.first_presented_clock_us),
                    static_cast<long long>(rep.presented),
                    static_cast<long long>(rep.max_abs_drift_us),
                    static_cast<long long>(rep.duplicates),
                    static_cast<int>(seg.dropped_before_present));

        CHECK(rep.presented > 0);
        // I3/I4（同场景 A）：字面量绝对上限 + 符号版判据改用 kDropThresholdUs。
        CHECK(rep.max_abs_drift_us <= syp::media::kDropThresholdUs);
        CHECK(rep.max_abs_drift_us <= 80000);
        CHECK_EQ(rep.duplicates, int64_t{0});
        CHECK(rep.monotonic);
    }
}

// =======================================================================
// 场景 F：早到 / 晚到。早到 Waiting 且不呈现；晚到丢帧；一次 step() 丢帧
// 数 ≤ kMaxDropsPerStep。
//
// 用真实素材（不是合成 pts 序列——那是 test_track_player.cpp 的判定单测
// 已经做过的事，这里要的是"真实解码出来的帧在真实 Pipeline 背压下"这条
// 端到端路径）+ FakeClock（人为拨快/拨慢），配一份把 max_frames_per_track
// 调大的 PipelineConfig，在 TrackPlayer 接手之前先把 Pipeline 的
// FrameQueue 喂到远超过 kMaxDropsPerStep 的帧数——否则丢帧数不超过上限
// 可能只是因为"队列里本来就没几帧"，测不出真正的上限判定。
// =======================================================================
TEST_CASE(f_early_and_late_arrival_respects_drop_cap) {
    syp::test::Watchdog wd("f_early_and_late_arrival_respects_drop_cap", /*soft_ms=*/60000,
                           /*hard_ms=*/180000, nullptr);

    PipelineConfig cfg;
    cfg.max_frames_per_track = 24;   // 明显大于 kMaxDropsPerStep（8）
    syp_status err           = SYP_OK;
    auto       pipeline      = Pipeline::create_file(fixture("bframes_faststart.mp4"), cfg, &err);
    REQUIRE(pipeline != nullptr);
    // 先驱动原始 Pipeline（还没交给 TrackPlayer），把 FrameQueue 喂满——
    // 没有人 pop_frame()，视频/音频各自的 FrameQueue 会堆到
    // max_frames_per_track 的上限然后 Blocked，多余的迭代只是空转，无害。
    prime_pipeline(*pipeline, 20000);

    auto           clock        = std::make_unique<FakeClock>();
    FakeClock*     clock_ptr    = clock.get();
    auto           renderer     = std::make_unique<FakeRenderer>(clock_ptr);
    FakeRenderer*  renderer_ptr = renderer.get();
    auto player = TrackPlayer::create(std::move(pipeline), nullptr, std::move(renderer),
                                       std::move(clock), &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);

    // 早到：时钟远早于任何真实 pts。多次 step() 排空可能已经预先解码好
    // 的音频帧（没有 sink，这些音频帧会被 pop-and-discard，不算这条用例
    // 的"产出"），直到第一次触到视频判定——用 track_index>=0 &&
    // pts_us!=AV_NOPTS_VALUE 确认这确实是视频"早了"分支产生的 Waiting，
    // 不是 Pipeline 兜底驱动那种没有具体帧信息的泛化 Waiting。
    clock_ptr->set(-1'000'000'000LL);
    bool        saw_early_video = false;
    PlayOutcome early;
    for (int i = 0; i < 200 && !saw_early_video; ++i) {
        early = player->step();
        REQUIRE(early.kind != PlayOutcome::Kind::Error);
        if (early.kind == PlayOutcome::Kind::Waiting && early.track_index >= 0 &&
            early.pts_us != AV_NOPTS_VALUE) {
            saw_early_video = true;
        }
    }
    REQUIRE(saw_early_video);
    CHECK_EQ(renderer_ptr->shown().size(), size_t{0});

    // 晚到：时钟拨到远远超前所有剩余帧的 pts + kDropThresholdUs。
    clock_ptr->set(2'000'000'000LL);
    const int64_t dropped_before1 = player->dropped_frames();
    const PlayOutcome late1       = player->step();
    REQUIRE(late1.kind != PlayOutcome::Kind::Error);
    const int64_t dropped_delta1 = player->dropped_frames() - dropped_before1;
    CHECK(dropped_delta1 >= 1);
    CHECK(dropped_delta1 <= syp::media::kMaxDropsPerStep);

    const int64_t dropped_before2 = player->dropped_frames();
    const PlayOutcome late2       = player->step();
    REQUIRE(late2.kind != PlayOutcome::Kind::Error);
    const int64_t dropped_delta2 = player->dropped_frames() - dropped_before2;
    CHECK(dropped_delta2 <= syp::media::kMaxDropsPerStep);
    if (dropped_delta1 == syp::media::kMaxDropsPerStep) {
        // 第一次真的撞了上限——若队列里堆积的迟到帧确实超过上限，第二
        // 次调用还应该能再丢一些，证明上限真的是"限制因素"，不是"队列
        // 本来就没几帧、凑巧没撞上限"这种弱证据。
        CHECK(dropped_delta2 >= 1);
    }
    CHECK_EQ(renderer_ptr->shown().size(), size_t{0});   // 全程没有任何帧被呈现

    std::printf("  [F] dropped_delta1=%lld dropped_delta2=%lld (cap=%d)\n",
                static_cast<long long>(dropped_delta1), static_cast<long long>(dropped_delta2),
                syp::media::kMaxDropsPerStep);

    // 滞回带覆盖——整段论证"迟到 40~80ms
    // 的帧正确的处理就是呈现它"（三分支判定的"其余一律呈现"兜底分支，
    // 不能写成对称区间 |diff|<=kPresentWindowUs，见 track_player.h 顶部
    // 长注释）。这类帧的 |drift| 落在 (kPresentWindowUs, kDropThresholdUs)
    // 区间——此前九条场景里从没有一条真正把一帧落进过这条带子（这套
    // 确定性驱动天然倾向于要么很快追上要么大幅超前）。这里用独立的
    // Pipeline/FakeClock，先靠"早到"
    // 同一个手法（step() 报出的 pts_us）读出某一帧的真实 pts——不猜、
    // 不依赖具体数值，再把时钟精确拨到 pts+60000（60ms 迟到，严格落在
    // 40~80ms 之间），断言这一帧确实被呈现，不是 Dropped、也不是
    // 继续 Waiting。
    {
        PipelineConfig cfg3;
        cfg3.max_frames_per_track = 8;
        syp_status err3           = SYP_OK;
        auto       pipeline3 =
            Pipeline::create_file(fixture("bframes_faststart.mp4"), cfg3, &err3);
        REQUIRE(pipeline3 != nullptr);
        prime_pipeline(*pipeline3, 5000);

        auto           clock3        = std::make_unique<FakeClock>();
        FakeClock*     clock3_ptr    = clock3.get();
        auto           renderer3     = std::make_unique<FakeRenderer>(clock3_ptr);
        FakeRenderer*  renderer3_ptr = renderer3.get();
        auto player3 = TrackPlayer::create(std::move(pipeline3), nullptr, std::move(renderer3),
                                            std::move(clock3), &err3, BufferPolicy::disabled());
        REQUIRE(player3 != nullptr);

        clock3_ptr->set(-1'000'000'000LL);
        bool    got_pts    = false;
        int64_t target_pts = -1;
        for (int i = 0; i < 200 && !got_pts; ++i) {
            const PlayOutcome o = player3->step();
            REQUIRE(o.kind != PlayOutcome::Kind::Error);
            if (o.kind == PlayOutcome::Kind::Waiting && o.track_index >= 0 &&
                o.pts_us != AV_NOPTS_VALUE) {
                got_pts    = true;
                target_pts = o.pts_us;
            }
        }
        REQUIRE(got_pts);

        // diff = target_pts - clock = -60000：严格落在
        // (-kDropThresholdUs, -kPresentWindowUs) = (-80000, -40000) 之间。
        clock3_ptr->set(target_pts + 60000);
        const PlayOutcome hyst = player3->step();
        REQUIRE(hyst.kind != PlayOutcome::Kind::Error);
        std::printf("  [F-hysteresis] target_pts=%lld drift=-60000us kind=%d presented=%zu\n",
                    static_cast<long long>(target_pts), static_cast<int>(hyst.kind),
                    renderer3_ptr->shown().size());
        CHECK(hyst.kind == PlayOutcome::Kind::Presented);
        REQUIRE(renderer3_ptr->shown().size() == size_t{1});
        CHECK_EQ(renderer3_ptr->shown()[0].pts_us, target_pts);
    }
}

// =======================================================================
// 场景 G：无音频轨。自动选 SystemClock，播到尾且满足 A 的断言。
// =======================================================================
TEST_CASE(g_no_audio_track_falls_back_to_system_clock) {
    syp::test::Watchdog wd("g_no_audio_track_falls_back_to_system_clock", /*soft_ms=*/60000,
                           /*hard_ms=*/180000, nullptr);

    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    // 30fps × 600 帧 = 20 秒，稳态无丢帧下应出 600 帧，留够余量满足
    // "呈现帧数 > 500" 这条空转防护。
    const std::string path = synth_video_only(tmp.path, /*fps=*/30, /*frame_count=*/600);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);

    auto           clock        = std::make_unique<FakeClock>();
    FakeClock*     clock_ptr    = clock.get();
    auto           renderer     = std::make_unique<FakeRenderer>(clock_ptr);
    FakeRenderer*  renderer_ptr = renderer.get();
    auto player = TrackPlayer::create(std::move(pipeline), nullptr, std::move(renderer),
                                       std::move(clock), &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);
    // clock_kind() 只由"有没有可用的音频轨 + sink"这件事决定，跟测试是
    // 否注入了假时钟无关（track_player.h create() 声明处注释）——这里
    // 传了 sink=nullptr，构造完立即就该是 System，不需要跑过一次
    // step() 才能观察到。
    REQUIRE(player->clock_kind() == ClockKind::System);

    RunResult rr = run_to_eof_clock(*player, *clock_ptr, /*chunk_us=*/2000);
    rr.shown          = renderer_ptr->shown();
    const SyncReport rep = analyze(rr.shown);

    std::printf("  [G] presented=%lld dropped=%lld max_drift=%lldus dup=%lld eof=%d clock=%d\n",
                static_cast<long long>(rep.presented), static_cast<long long>(rr.dropped),
                static_cast<long long>(rep.max_abs_drift_us),
                static_cast<long long>(rep.duplicates), static_cast<int>(rr.reached_eof),
                static_cast<int>(rr.clock));

    REQUIRE(rr.reached_eof);
    REQUIRE(rep.presented > 500);
    // 无遗漏（同场景 A/B）：这份素材是现场合成的，精确帧数
    // 已知（600），不需要借道参照解码——直接比字面量。
    CHECK_EQ(rr.dropped, int64_t{0});
    CHECK_EQ(rep.presented, int64_t{600});
    // I3/I4（同场景 A）：字面量绝对上限 + 符号版判据改用 kDropThresholdUs。
    CHECK(rep.max_abs_drift_us <= syp::media::kDropThresholdUs);
    CHECK(rep.max_abs_drift_us <= 80000);
    CHECK_EQ(rep.duplicates, int64_t{0});
    CHECK(rep.monotonic);
    CHECK(rr.clock == ClockKind::System);
}

// =======================================================================
// 场景 H：音频 sink 中途失败。退回 SystemClock 继续播不终止；
// clock_kind() 如实变化；失败前确实呈现过帧。
// =======================================================================
TEST_CASE(h_audio_sink_failure_degrades_and_keeps_playing) {
    syp::test::Watchdog wd("h_audio_sink_failure_degrades_and_keeps_playing", /*soft_ms=*/60000,
                           /*hard_ms=*/180000, nullptr);

    auto fx = make_player_from_file(fixture("bframes_faststart.mp4"), wide_frame_config());
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    const BudgetRunResult warm = run_budget(*fx.player, *fx.sink, 500'000);
    REQUIRE(!warm.errored);
    const size_t presented_before_failure = fx.renderer->shown().size();
    // > 0 太软——一帧就过，实测常见值在两位数。收紧到
    // > 5，跟"确实呈现过帧、不是一开始就失败"这条意图匹配得更紧。
    REQUIRE(presented_before_failure > 5);

    fx.sink->inject_failure();
    // 还没经过一次 step()：降级检查在 step() 内部做，这一刻应该还没发生。
    CHECK(fx.player->clock_kind() == ClockKind::Audio);
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind != PlayOutcome::Kind::Error);
    CHECK(fx.player->clock_kind() == ClockKind::System);   // 如实变化

    // 降级之后靠真墙钟继续推进——sink 已经 failed()，FakeAudioSink::
    // advance() 对失败的 sink 是空操作，唯一能验证"没有终止、真的还在
    // 播"的手段是用真实时间跑一段，用真实 sleep（不是零延迟——
    // 零延迟会巧合掩盖这类缺陷）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    int        iterations = 0;
    bool       stayed_system = true;
    while (std::chrono::steady_clock::now() < deadline && iterations < 50'000'000) {
        const PlayOutcome oo = fx.player->step();
        REQUIRE(oo.kind != PlayOutcome::Kind::Error);
        if (fx.player->clock_kind() != ClockKind::System) stayed_system = false;
        ++iterations;
    }
    CHECK(stayed_system);   // 不应该退回 Audio

    const size_t presented_after = fx.renderer->shown().size();
    std::printf("  [H] presented_before=%zu presented_after=%zu iterations=%d\n",
                presented_before_failure, presented_after, iterations);
    CHECK(presented_after > presented_before_failure);   // 降级后继续真的呈现新帧，不是卡死

    // 降级 × 倍速交叉——上面这条主线程从未测过
    // speed≠1 时的降级；有一条规则（`degrade_to_system_clock()`
    // 必须继承 `speed_`，否则 2× 播放中 sink 被抢占会悄悄跌回 1.0×，
    // 数值上看不出"崩"，只是速度不对）在 test_track_player.cpp 有单元级
    // 回归（`degrade_preserves_playback_speed`），但这条 E2E 路径上此前
    // 完全没有独立覆盖——H 不变速，D 不降级，九条组合起来也测不到这个
    // 交叉点。
    {
        auto fx2 = make_player_from_file(fixture("bframes_faststart.mp4"), wide_frame_config());
        REQUIRE(fx2.player != nullptr);
        REQUIRE(fx2.player->set_speed(2.0) == SYP_OK);
        fx2.player->pause();
        for (int32_t k = 0; k < 3000; ++k) {
            const PlayOutcome prime_o = fx2.player->step();
            REQUIRE(prime_o.kind != PlayOutcome::Kind::Error);
        }
        fx2.player->play();
        const BudgetRunResult warm2 = run_budget(*fx2.player, *fx2.sink, 500'000);
        REQUIRE(!warm2.errored);
        REQUIRE(fx2.player->clock_kind() == ClockKind::Audio);
        REQUIRE(fx2.player->speed() == 2.0);

        fx2.sink->inject_failure();
        const PlayOutcome deg = fx2.player->step();   // 触发降级
        CHECK(deg.kind != PlayOutcome::Kind::Error);
        CHECK(fx2.player->clock_kind() == ClockKind::System);
        CHECK(fx2.player->speed() == 2.0);   // speed() 镜像不受降级影响（如实报）

        // 降级后靠真墙钟继续推进：2× 播放，100ms 墙钟应推进约 200ms；
        // 若降级丢了 speed_（新 SystemClock 按 1.0× 走），只会推进约
        // 100ms——跟 test_track_player.cpp 的 degrade_preserves_
        // playback_speed 同一个判据，这里是它在真实素材 + 真实 sink
        // 失败路径上的 E2E 版本。真实 sleep，不是零延迟（零延迟会巧合掩盖
        // "忘记结算"这类缺陷）。
        const int64_t before2 = fx2.player->position_us();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // 多跑几次 step()，让 SystemClock 的新读数真的被 position_us()
        // 观察到（跟 H 主线程的降级后循环同一个理由：需要驱动过一次才
        // 能读到新值，不是被动累积）。
        for (int k = 0; k < 5; ++k) {
            const PlayOutcome oo = fx2.player->step();
            REQUIRE(oo.kind != PlayOutcome::Kind::Error);
        }
        const int64_t after2 = fx2.player->position_us();
        const int64_t delta2 = after2 - before2;
        std::printf("  [H] degrade_at_2x: speed=%.1f delta_after_100ms_sleep=%lld "
                    "(expect ~200000)\n",
                    fx2.player->speed(), static_cast<long long>(delta2));
        CHECK(delta2 > 150'000);
    }
}

// =======================================================================
// 场景 I：played_us() 的延迟换算。注入已知设备延迟，断言确实扣除了；
// 倍速下换算比例正确。
// =======================================================================
TEST_CASE(i_played_us_reflects_device_latency_and_speed) {
    syp::test::Watchdog wd("i_played_us_reflects_device_latency_and_speed", /*soft_ms=*/60000,
                           /*hard_ms=*/180000, nullptr);

    auto fx = make_player_from_file(fixture("bframes_faststart.mp4"), wide_frame_config());
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    // 先跑一段，累计足够的 consumed_frames_，扣掉延迟对应的帧数之后仍
    // 然 effective>0（否则 played_us() 会钳位到 base_us_，测不出扣减）。
    const BudgetRunResult warm = run_budget(*fx.player, *fx.sink, 500'000);
    REQUIRE(!warm.errored);
    REQUIRE(fx.player->position_us() > 200'000);

    const int64_t pos_no_latency = fx.player->position_us();
    fx.sink->set_device_latency_us(50'000);   // 注入 50ms 设备延迟
    const int64_t pos_with_latency = fx.player->position_us();
    const int64_t latency_delta    = pos_no_latency - pos_with_latency;
    std::printf("  [I] latency_delta=%lld (expect ~50000)\n",
                static_cast<long long>(latency_delta));
    CHECK(latency_delta > 40'000);
    CHECK(latency_delta < 60'000);

    // 倍速下换算比例：两份全新的 fixture 各自跑同样次数的 step() 调用
    // （不是固定设备时间预算、也不是播到 EOF）——量法、理由跟场景 D 顶部
    // 长注释是同一件事，包括反向自检挖出的那条新死角：只有
    // FrameQueue 深度需要从 wide_frame_config() 的 64 深调到 512（用
    // 变异隔离验证过，是三个旋钮里唯一必需的一个）；sink 容量、暂停预热
    // 步数都保持默认/原值——调大反而会把 audio_blocked 这条真实
    // 反压路径从覆盖里摘掉，完整推导见场景 D 顶部长注释。
    constexpr int32_t kStepsI = 3000;
    PipelineConfig hugecfg_i;
    hugecfg_i.max_frames_per_track = 512;
    auto fx1 = make_player_from_file(fixture("bframes_faststart.mp4"), hugecfg_i);
    REQUIRE(fx1.player != nullptr);
    fx1.player->pause();
    for (int32_t k = 0; k < 3000; ++k) {
        const PlayOutcome o = fx1.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
    }
    fx1.player->play();
    const BudgetRunResult at1x = run_budget(*fx1.player, *fx1.sink, /*device_us_budget=*/INT64_MAX,
                                             /*chunk_us=*/5000, /*max_steps=*/kStepsI);
    REQUIRE(!at1x.errored);
    REQUIRE(at1x.presented_delta > 5);   // 空转防护

    auto fx2 = make_player_from_file(fixture("bframes_faststart.mp4"), hugecfg_i);
    REQUIRE(fx2.player != nullptr);
    REQUIRE(fx2.player->set_speed(2.0) == SYP_OK);
    fx2.player->pause();
    for (int32_t k = 0; k < 3000; ++k) {
        const PlayOutcome o = fx2.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
    }
    fx2.player->play();
    const BudgetRunResult at2x = run_budget(*fx2.player, *fx2.sink, /*device_us_budget=*/INT64_MAX,
                                             /*chunk_us=*/5000, /*max_steps=*/kStepsI);
    REQUIRE(!at2x.errored);

    const int64_t p_1x = at1x.presented_delta;
    const int64_t p_2x = at2x.presented_delta;
    std::printf("  [I] presented(%d steps): 1x=%lld 2x=%lld\n", kStepsI,
                static_cast<long long>(p_1x), static_cast<long long>(p_2x));
    CHECK(p_2x > p_1x);                  // 2x 应该比 1x 呈现得更多（同样次数的 step() 调用内）
    CHECK(p_2x > p_1x + p_1x / 2);        // 比例宽松但有判别力：> 1.5×

    // 延迟 × 倍速交叉——此前只在 1.0x 测过延迟扣减
    // （上面那段），九条从未验证过"倍速下换算比例"这半句真正测的是
    // "延迟"，不是把场景 D 的呈现速率断言换个地方再写一遍。公式是
    // "先在样本数层减 latency_samples，再 ×speed"；若被错误
    // 地写成"先 ×speed 换算成媒体时间、再减 device_latency_us（不按
    // speed 缩放）"，1x 下两种写法恒等（×1 不影响），只有 speed≠1 才会
    // 露馅：正确实现扣减量应该是 device_latency_us × speed
    // （≈50000×2=100000us），错误实现会恒扣 50000us，不随 speed 变化
    // ——跟 D/I 的呈现速率断言完全是两件事。复用 fx2（已在 2.0x 稳定
    // 运行过 kStepsI 步，consumed_frames_ 应该远超"扣掉延迟对应帧数后
    // 仍 effective>0"这条门槛）。
    {
        REQUIRE(fx2.player->position_us() > 200'000);
        const int64_t pos_no_latency_2x = fx2.player->position_us();
        fx2.sink->set_device_latency_us(50'000);
        const int64_t pos_with_latency_2x = fx2.player->position_us();
        const int64_t latency_delta_2x    = pos_no_latency_2x - pos_with_latency_2x;
        std::printf("  [I] latency_delta_2x=%lld (expect ~100000, 即 50000×speed)\n",
                    static_cast<long long>(latency_delta_2x));
        CHECK(latency_delta_2x > 90'000);
        CHECK(latency_delta_2x < 110'000);
    }
}

int main() { return tiny_test_main(); }
