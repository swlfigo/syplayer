// test_track_player.cpp — TrackPlayer：同步核心。
//
// 九条用例（early/on_time/late_but_not_too_late/very_late/drop-cap/
// present-failure/clock_kind ×2/degrade），逐字未改字面断言。真实素材的
// 端到端验证在 test_sync_e2e.cpp
// （场景 A~I），本文件只管同步判定本身，用现场合成的极小素材隔离判定
// 逻辑，不掺进真实素材的解码细节。
//
// 素材构造：Pipeline 是具体类、没有虚接口，明确要求不为了测试给
// 它加虚函数。这里用 SYP_FFMPEG_CLI_PATH（跟 test_decode_e2e.cpp 场景 G
// 同一个模式）现场合成 CFR、无 B 帧（-preset ultrafast，这个仓库已经
// 验证过这个 preset 下 libx264 不产 B 帧，见 tools/gen-fixtures.sh 顶部
// 注释）的视频，并显式传 -video_track_timescale <fps> 把时间刻度锁死成
// 1/fps，让每帧 pts 精确等于 i × (1'000'000/fps) 微秒——没有编码器自选
// 时间刻度带来的舍入。选 fps 使得请求的 pts 序列正好落在整数帧位上
// （这几组数字：100000、40000 都是常见 CFR fps 的整数倍：
// fps=10 → 100000us/帧；fps=25 → 40000us/帧），因此这些字面
// pts 数值不需要改写成"反推值"——实测确认过。
// 只有 64 帧那组用不到具体数值（那条用例只关心"丢帧数≤上限"，不关心
// 具体 pts），用同一套机制生成，fps 按 1ms 间距选（1000fps，64 帧只有
// 64ms 时长，合成飞快）。
#include "media/track_player.h"
#include "support/fake_audio_sink.h"
#include "support/fake_clock.h"
#include "support/fake_hw_backend.h"
#include "support/fake_renderer.h"
#include "support/synth_media.h"
#include "support/watchdog.h"
#include "tiny_test.h"

#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <unistd.h>   // mkdtemp
#include <libavformat/avformat.h>
}

using syp::media::ClockKind;
using syp::media::ClockSwitchReason;
using syp::media::kCatchupOnTimeToDisable;
using syp::media::Pipeline;
using syp::media::PipelineConfig;
using syp::media::PlayOutcome;
using syp::media::StepOutcome;
using syp::media::TrackPlayer;
using syp::media::BufferingReason;
using syp::media::BufferPolicy;
using syp::media::InitialSettings;
using syp::media::BufferStats;
using syp::media::kBufferedUntilUnbounded;

namespace {

// ---------------------------------------------------------------------
// 素材合成的地基
// ---------------------------------------------------------------------

// ffmpeg_cli_path()/TempDir 挪到了 support/synth_media.h（
// test_pipeline.cpp 的封面图用例也需要用到同一份构造逻辑），这里只 using
// 进来，字面调用点不用改。
using syp::test::ffmpeg_cli_path;
using syp::test::TempDir;

bool ffmpeg_available() {
    return std::system(("\"" + ffmpeg_cli_path() + "\" -version > /dev/null 2>&1").c_str()) == 0;
}

// 这几个 fixture 构造函数不是 TEST_CASE，是普通函数——tiny_test.h 的
// REQUIRE 宏里有个裸 `return;`，只能用在 void 函数里，这里用本地小宏
// FX_REQUIRE 代替（要求调用处的返回值变量固定叫 fx）。
#define FX_REQUIRE(expr)                                                 \
    do {                                                                 \
        if (!(expr)) {                                                  \
            ::tiny_test::fail(__FILE__, __LINE__, #expr);                \
            return fx;                                                  \
        }                                                                \
    } while (0)

// video-only、CFR、无 B 帧的合成素材。-video_track_timescale 锁死时间
// 刻度，见文件顶部注释。
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

// video + audio 的合成素材，给需要"有音频轨、sink 能 open 成功"的用例。
// 不关心精确 pts，只关心两条轨都存在、都能正常解码。
//
// -g 5：每 5 帧（200ms @ 25fps）一个关键帧。没有这条，ultrafast 预设
// 默认的 keyint（250）会让这 1 秒、25 帧的短素材整个只有一个关键帧
// （第 0 帧）——seek() 到任何位置都会落回文件开头，seek 相关用例测不出
// "落点是不是新位置"这件事（本文件 seek_clears_stale_pending_audio_frame
// 用例第一次没加这条时就撞上了：seek(500000) 之后第一帧队到的音频 pts
// 是 0，不是预期的"新位置附近"——不是 TrackPlayer 的缺陷，是素材只有
// 一个关键帧）。
std::string synth_video_audio(const std::string& dir) {
    const std::string  path = dir + "/synth_av.mp4";
    std::ostringstream  cmd;
    cmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-f lavfi -i \"testsrc2=size=64x64:rate=25:duration=1\" "
        << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=1\" "
        << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 5 "
        << "-c:a aac -ar 44100 -ac 2 "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(cmd.str().c_str()) == 0) ? path : std::string();
}

// #23：视频 3 秒、音频 2 秒。两路分别编码再 -c copy 封装，保证音频真的比视频短。
std::string synth_video3_audio2(const std::string& dir) {
    const std::string v = dir + "/v3.mp4", a = dir + "/a2.m4a", out = dir + "/v3a2.mp4";
    std::ostringstream vc, ac, mc;
    vc << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
       << "-f lavfi -i \"testsrc2=size=64x64:rate=25:duration=3\" "
       << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 5 \"" << v << "\" > /dev/null 2>&1";
    ac << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
       << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=2\" "
       << "-c:a aac -ar 44100 -ac 2 \"" << a << "\" > /dev/null 2>&1";
    mc << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
       << "-i \"" << v << "\" -i \"" << a << "\" -map 0:v -map 1:a -c copy \"" << out << "\" > /dev/null 2>&1";
    if (std::system(vc.str().c_str()) != 0 || std::system(ac.str().c_str()) != 0 ||
        std::system(mc.str().c_str()) != 0) return std::string();
    return out;
}

// 一条视频轨 + 两条音频轨的合成素材——回归用例专用：
// Pipeline 按 codec_type 托管每一条 video/audio 流（不只是 TrackPlayer
// 绑定的那一条），双音轨文件（或带封面图的 mp4，效果同理）如果第二条
// 受管轨没人排空，会在它的 FrameQueue 堆满时触发联合背压，拖死整条
// 管线。2 秒时长、25fps，稳态应出 50 帧视频。
std::string synth_video_two_audio(const std::string& dir) {
    const std::string path = dir + "/synth_two_audio.mp4";
    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-f lavfi -i \"testsrc2=size=64x64:rate=25:duration=2\" "
        << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=2\" "
        << "-f lavfi -i \"sine=frequency=880:sample_rate=44100:duration=2\" "
        << "-map 0:v -map 1:a -map 2:a "
        << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p "
        << "-c:a aac -ar 44100 -ac 2 "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(cmd.str().c_str()) == 0) ? path : std::string();
}

// 带封面图（AV_DISPOSITION_ATTACHED_PIC）的素材，两份。
//
// 封面图在 mp4 里是一条 is_video 为真、只有一个采样的轨。TrackPlayer::
// create() 此前只认"第一条 is_video"，完全没查 attached_pic。
//
// 关于素材构造的一条实测事实，写在这里免得下一个人再试一遍：**用 ffmpeg
// CLI 造不出"封面图流号更小"的 mp4**。mov 复用器在看到
// -disposition:...:attached_pic 时会把这条轨挪到最后：
//
//   $ ffmpeg -i cover.png -i main.mp4 -map 0:v -map 1:v -map 1:a -c copy \
//            -disposition:v:0 attached_pic out.mp4
//   $ ffprobe ... out.mp4
//   stream|index=0|codec_name=h264|width=64|height=64|attached_pic=0
//   stream|index=1|codec_name=aac
//   stream|index=2|codec_name=png|width=32|height=16|attached_pic=1
//
//   （不加 -disposition 时顺序是保留的：png 在 index=0——所以确实是
//    attached_pic 这条路径在挪，不是 -map 顺序没生效。）
//
// 修复本身不看流号、只看 disposition，所以顺序对它没影响；但这意味着
// "封面图排在真视频前面"这半个场景在本仓库的工具链下没有素材可造。真正
// 能钉死这条缺陷的是下面第一份素材：**音频 + 封面图**（音乐文件，极其
// 常见），封面图是唯一的 is_video 轨，修复前必然被绑成主视频轨，跟流号
// 无关。
//
// 封面图必须是 png/mjpeg（mp4 的 attached_pic 只接受这两种 tag：拿
// libx264 编的单帧当封面，复用器会直接报 "Could not find tag for codec
// h264 in stream #0"）。本仓库的 FFmpeg 只 --enable-decoder=h264/aac，
// 所以这条封面图轨在运行期解不出帧——这不影响用例要测的东西（"绑没绑
// 上"发生在 create() 的选轨阶段，早于任何解码），但它解释了为什么断言
// 落在 video_track_index() 上而不是"呈现出来的是不是封面图那张画面"。

// 音频 + 封面图：封面图是唯一的 is_video 轨。修复前它会被绑成主视频轨。
// 定义挪到了 support/synth_media.h（test_pipeline.cpp 的封面图
// 恒软解用例复用同一份构造逻辑），这里只 using 进来。
using syp::test::synth_audio_with_cover_art;

// 真视频 + 音频 + 封面图。见上方注释：mov 复用器会把封面图挪到最后，所以
// 这份素材测的是"修复没有误伤真视频轨"这一半，不是"封面图抢跑"那一半。
std::string synth_video_audio_with_cover_art(const std::string& dir) {
    const std::string path       = dir + "/synth_av_cover.mp4";
    const std::string cover_path = dir + "/cover_av.png";
    const std::string main_path  = dir + "/cover_av_main.mp4";

    std::ostringstream ccmd;
    ccmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
         << "-f lavfi -i \"color=c=red:size=32x16\" -frames:v 1 "
         << "\"" << cover_path << "\" > /dev/null 2>&1";
    if (std::system(ccmd.str().c_str()) != 0) return std::string();

    std::ostringstream mcmd;
    mcmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
         << "-f lavfi -i \"testsrc2=size=64x64:rate=25:duration=1\" "
         << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=1\" "
         << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 5 "
         << "-c:a aac -ar 44100 -ac 2 "
         << "\"" << main_path << "\" > /dev/null 2>&1";
    if (std::system(mcmd.str().c_str()) != 0) return std::string();

    std::ostringstream mux;
    mux << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-i \"" << cover_path << "\" -i \"" << main_path << "\" "
        << "-map 0:v -map 1:v -map 1:a -c copy -disposition:v:0 attached_pic "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(mux.str().c_str()) == 0) ? path : std::string();
}

// video + audio 的合成素材，比 synth_video_audio() 更长（默认 3 秒）——
// 下面的回归用例需要视频侧能在"暂停预热"阶段攒出一条
// 深度接近 kWideFramesPerTrack 的解码领先队列，1 秒素材（25 帧视频）撑
// 不起这个深度的余量（帧数本身就不够填满 64 深的 FrameQueue）。-g 5 同
// synth_video_audio()：给足关键帧密度，不是这条用例的关注点，但保持跟
// 其它 fixture 一致，避免"关键帧稀疏"这个无关变量混进来。
//
// 音频比视频多请求 1 秒富余（反向自检踩出来的教训，记在这里，教训本身
// 比数字更重要）：本用例第一版视频、音频都请求同一个 duration，一起喂
// 给同一次 ffmpeg 调用（两路 -f lavfi 输入 + 一次输出），结果撞出一个
// 完全没预料到的真实收尾状态——音频解码器先于视频到 decoder_eof（音频
// 真的没有更多样本了，demux 也已经整体到底，不是背压、不是队列没排
// 空），视频侧最后 1~2 帧解出来后一直搁在 Pipeline 的 FrameQueue 里没
// 人取。这不是活锁（Pipeline 如实报 Blocked——`all_done` 判据要求"该轨
// frame 队列也排空"，这里明确不满足，拒绝谎报 Eof，见 pipeline.cpp 该
// 处注释），是 TrackPlayer 只持有 1 帧 pending_video_ 的既有设计（老帧
// 不消费就不会去 pop 新的）撞上"音频时钟已经到顶、再也无法追上"这个此
// 前没覆盖过的边界——真实世界完全可能出现（音频轨天然比视频轨短半帧到
// 一帧），但那是另一个独立的、更大的问题（音频耗尽后如何呈现尾部视
// 频），不是 #22 的范围，本用例存心避开它。
//
// 第一次尝试"给音频多请求 1 秒 duration"没有效果——ffmpeg 一次调用里
// 视频用 `-frames:v` 卡住帧数后，会把整个输出提前收尾，音频照样被剪到
// 跟视频一样短（实测验证过：ffprobe 显示音频 duration 还是 ~3 秤，不是
// 4 秒）。改成分两步：视频、音频各自单独编码成独立文件（互不影响对方
// 的收尾时机），再用 `-c copy` 封装到一起——这样音频真的会有比视频长
// 1 秒的富余内容，时钟顶到底时早已经远远盖过视频最后一帧的 pts，不会
// 撞上这条边界。
std::string synth_video_audio_long(const std::string& dir, int seconds) {
    const std::string path         = dir + "/synth_av_long.mp4";
    const std::string video_path   = dir + "/synth_av_long_v.mp4";
    const std::string audio_path   = dir + "/synth_av_long_a.m4a";
    const int         video_frames = seconds * 25;
    const int         audio_seconds = seconds + 1;   // 见上方长注释：故意留出富余

    std::ostringstream vcmd;
    vcmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
         << "-f lavfi -i \"testsrc2=size=64x64:rate=25:duration=" << seconds << "\" "
         << "-frames:v " << video_frames << " "
         << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 5 "
         << "\"" << video_path << "\" > /dev/null 2>&1";
    if (std::system(vcmd.str().c_str()) != 0) return std::string();

    std::ostringstream acmd;
    acmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
         << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=" << audio_seconds
         << "\" -c:a aac -ar 44100 -ac 2 "
         << "\"" << audio_path << "\" > /dev/null 2>&1";
    if (std::system(acmd.str().c_str()) != 0) return std::string();

    std::ostringstream muxcmd;
    muxcmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
           << "-i \"" << video_path << "\" -i \"" << audio_path << "\" "
           << "-map 0:v -map 1:a -c copy "
           << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(muxcmd.str().c_str()) == 0) ? path : std::string();
}

int32_t find_managed_video_track(const Pipeline& p) {
    for (const auto& t : p.tracks()) {
        if (t.is_video) return t.index;
    }
    return -1;
}

int32_t find_managed_audio_track(const Pipeline& p) {
    for (const auto& t : p.tracks()) {
        if (!t.is_video && t.sample_rate > 0) return t.index;
    }
    return -1;
}

// 反复驱动 pipeline->step()，给它机会把已经 demux 到的包解码成帧。
// Pipeline 没有公开"FrameQueue 还剩几个"的查询（也不该为了测试新增），
// 用一个远大于所需帧数的迭代上限顶上：FrameQueue 撞到自己的容量上限
// （cfg.max_frames_per_track）之后 step() 自然转成 Blocked，不会因为
// 迭代次数不够而少解出该解的帧。
void drive_pipeline_best_effort(Pipeline& p, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        const StepOutcome so = p.step();
        if (so.kind == StepOutcome::Kind::Eof) break;
        if (so.kind == StepOutcome::Kind::Error) break;   // 调用方后续 REQUIRE 会发现
    }
}

// 只给回归用例用的 IAudioSink：played_us() 走真实墙钟
// （std::chrono::steady_clock），跟 write() 是否成功完全脱钩——这才
// 是真实设备的行为（硬件按自己的节奏消费环形缓冲，跟软件这一刻有没有
// 写成功毫无关系）。FakeAudioSink 做不到这件事：它的 played_us() 靠
// advance() 从 written_frames_ 里扣，而 written_frames_ 只在 write()
// 成功时才会增长——想构造"write() 永远失败，但时钟仍然照常前进"这种
// 组合，FakeAudioSink 的模型天然把两者耦合在一起（试过用 advance()
// 强行模拟，见本用例上方失败过的两版设计：不管怎么调 capacity/
// advance() 的比例，"音频卡住的时长"跟"两次成功写入之间必然相隔一帧
// AAC（~23ms）的样本数"这条底层约束摆脱不掉，怎么调参数都做不出比
// kDropThresholdUs（80ms）更长的单次卡顿，测不出这条修复）。
class RealTimeStubSink final : public syp::media::IAudioSink {
public:
    syp_status open(int32_t, int32_t, int32_t) override { return SYP_OK; }
    bool       write(const syp::media::Frame&) override { return accept_writes_; }
    int64_t    played_us() const noexcept override {
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - anchor_)
                                     .count();
        return base_us_ + static_cast<int64_t>(elapsed_us);
    }
    void pause() override {}
    void resume() override {}
    void flush(int64_t base_us) override {
        base_us_ = base_us;
        anchor_  = std::chrono::steady_clock::now();
    }
    void set_speed(double) noexcept override {}   // 这条用例不测倍速
    bool failed() const noexcept override { return false; }   // 这条用例不测降级
    // 按墙钟推进、不建模缓冲：始终视为没有待播样本（这条用例的素材音视频等长，不涉及 #23 切钟）。
    bool output_drained() const noexcept override { return true; }

    void set_accept_writes(bool v) noexcept { accept_writes_ = v; }

private:
    int64_t                              base_us_       = 0;
    std::chrono::steady_clock::time_point anchor_        = std::chrono::steady_clock::now();
    bool                                  accept_writes_ = true;
};

// ---------------------------------------------------------------------
// make_fixture_with_video_pts
// ---------------------------------------------------------------------

struct VideoPtsFixture {
    std::unique_ptr<TempDir>     tmp;
    std::unique_ptr<TrackPlayer> player;
    syp::test::FakeClock*        clock    = nullptr;
    syp::test::FakeRenderer*     renderer = nullptr;
};

// 只出指定 pts 序列（视频轨）的假素材 + 假时钟 + 假渲染器，隔离同步判定
// 本身。没有音频轨、没有 sink——clock_override 传一个 FakeClock，
// TrackPlayer::create() 因此判定 clock_kind()==System（没有可用 sink），
// 但 now_us() 实际转发到这个 FakeClock，用例直接拨它。
VideoPtsFixture make_fixture_with_video_pts(const std::vector<int64_t>& pts_list) {
    VideoPtsFixture fx;
    FX_REQUIRE(!pts_list.empty());
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return fx;
    }

    // spacing：多元素时取前两个的差；单元素时取该值本身（把它当成 CFR
    // 序列里第 1 个位置，第 0 个位置是 pts=0 的帧，之后丢弃）；单元素且
    // 值为 0 时用默认 spacing（不需要跳过任何帧）。
    int64_t spacing = 40000;
    if (pts_list.size() >= 2) {
        spacing = pts_list[1] - pts_list[0];
    } else if (pts_list[0] > 0) {
        spacing = pts_list[0];
    }
    FX_REQUIRE(spacing > 0);
    FX_REQUIRE(1'000'000 % spacing == 0);   // 保证 fps 是整数、pts 精确
    const int     fps        = static_cast<int>(1'000'000 / spacing);
    const int64_t skip_count = pts_list.front() / spacing;
    // 恰好 skip_count + pts_list.size() 帧，不多加"富余"帧：多加的帧会在
    // 丢帧类用例里被继续消费掉（时钟追上之后下一帧可能恰好落进"呈现"
    // 分支），让断言的帧数/呈现状态偏离用例本来要测的那个单一判定。
    // FrameQueue 的容量富余单独在下面 cfg.max_frames_per_track 里给，
    // 那不影响 ffmpeg 实际吐出的帧数。
    const int frame_count = static_cast<int>(skip_count) + static_cast<int>(pts_list.size());

    fx.tmp = std::make_unique<TempDir>();
    FX_REQUIRE(!fx.tmp->path.empty());
    const std::string path = synth_video_only(fx.tmp->path, fps, frame_count);
    FX_REQUIRE(!path.empty());

    syp_status     err = SYP_OK;
    PipelineConfig cfg;
    cfg.max_frames_per_track = static_cast<std::size_t>(frame_count) + 4;
    auto pipeline            = Pipeline::create_file(path, cfg, &err);
    FX_REQUIRE(pipeline != nullptr);
    const int32_t video_idx = find_managed_video_track(*pipeline);
    FX_REQUIRE(video_idx >= 0);

    drive_pipeline_best_effort(*pipeline, frame_count * 4 + 64);

    for (int64_t i = 0; i < skip_count; ++i) {
        auto f = pipeline->pop_frame(video_idx);
        FX_REQUIRE(f.has_value());   // 素材没解出这么多帧，spacing/skip 算错了
    }

    auto clock = std::make_unique<syp::test::FakeClock>();
    fx.clock   = clock.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(fx.clock);
    fx.renderer   = renderer.get();

    fx.player = TrackPlayer::create(std::move(pipeline), nullptr, std::move(renderer),
                                     std::move(clock), &err, BufferPolicy::disabled());
    FX_REQUIRE(fx.player != nullptr);
    return fx;
}

// ---------------------------------------------------------------------
// make_fixture_with_audio_and_video
// ---------------------------------------------------------------------

struct AvFixture {
    std::unique_ptr<TempDir>     tmp;
    std::unique_ptr<TrackPlayer> player;
    syp::test::FakeAudioSink*    sink     = nullptr;
    syp::test::FakeRenderer*     renderer = nullptr;
};

// 音频轨 + 视频轨都真实存在、都能正常解码的素材，sink 用 FakeAudioSink
// （真的会 open() 成功），不传 clock_override——TrackPlayer::create()
// 走生产路径自己选时钟，这里应该选出 AudioClock。
//
// "sink 提供了但 open() 失败"那条分支不走这个 fixture
// ——它需要一份容量调小的 PipelineConfig 才能让联合背压真正触发（见
// sink_provided_but_open_fails_falls_back_and_keeps_playing 用例顶部
// 注释），这里的默认 PipelineConfig{} 对 1 秒素材来说太宽松，测不出
// 那条修复的区分力，单独在那条用例里现场构造。
AvFixture make_fixture_with_audio_and_video() {
    AvFixture fx;
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return fx;
    }
    fx.tmp = std::make_unique<TempDir>();
    FX_REQUIRE(!fx.tmp->path.empty());
    const std::string path = synth_video_audio(fx.tmp->path);
    FX_REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    FX_REQUIRE(pipeline != nullptr);
    FX_REQUIRE(find_managed_audio_track(*pipeline) >= 0);
    FX_REQUIRE(find_managed_video_track(*pipeline) >= 0);

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    fx.sink   = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);   // 不看呈现时刻，只看有没有呈现
    fx.renderer   = renderer.get();

    fx.player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                     nullptr, &err, BufferPolicy::disabled());
    FX_REQUIRE(fx.player != nullptr);
    return fx;
}

// ---------------------------------------------------------------------
// 音量 / 显示几何接线用的素材与 fixture
// ---------------------------------------------------------------------

// 专门验证"create() 真的调用过一次 set_gravity(默认值)"这件事本身的
// 假 IVideoRenderer——FakeRenderer（tests/support/fake_renderer.h）的
// gravity_ 默认初值恰好也是 AspectFit，只断言
// `renderer->gravity() == AspectFit` 测不出"create() 里那行调用被删掉
// 了"这个变异（删掉之后 FakeRenderer 自己的默认值原样让断言绿）——这
// 是实测出的一个真实覆盖漏洞，不是猜的。这份替身把默认初
// 值故意设成 Resize（跟 create() 要下发的 AspectFit 不同），并且记调用
// 次数，两者合起来才能把"从没调用过"和"调用了、且传的恰好是默认值"
// 区分开。不改 fake_renderer.h——那是别处的交付物，这里不碰。
class GravityCountingRenderer final : public syp::media::IVideoRenderer {
public:
    syp_status present(const syp::media::Frame&, int64_t) override { return SYP_OK; }
    void set_gravity(syp::media::Gravity g) noexcept override {
        gravity_ = g;
        ++calls_;
    }
    syp::media::Gravity gravity() const noexcept { return gravity_; }
    int                 calls() const noexcept { return calls_; }

private:
    syp::media::Gravity gravity_ = syp::media::Gravity::Resize;
    int                 calls_   = 0;
};

// 纯音频、没有任何视频轨（连封面图都没有）——验证几何缝在这种源上
// 完全不触发（geometry_calls() 恒为 0）。
std::string synth_audio_only(const std::string& dir) {
    const std::string path = dir + "/synth_audio_only.m4a";
    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=1\" "
        << "-c:a aac -ar 44100 -ac 2 "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(cmd.str().c_str()) == 0) ? path : std::string();
}

struct GeometryFixture {
    std::unique_ptr<TrackPlayer> player;
    syp::test::FakeRenderer*     renderer = nullptr;
};

// 只关心 create() 有没有把显示几何推给渲染器、推的是什么值，不关心
// 呈现——renderer 不带时钟（clock_ = nullptr），也不驱动 step()。
// path 由调用方现场合成（各用例自己选 synth_video_with_display_rotation
// 还是 synth_video_with_sar），这里只管装配。
GeometryFixture make_geometry_fixture(const std::string& path) {
    GeometryFixture fx;
    FX_REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    FX_REQUIRE(pipeline != nullptr);
    FX_REQUIRE(find_managed_video_track(*pipeline) >= 0);

    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    fx.renderer   = renderer.get();

    fx.player = TrackPlayer::create(std::move(pipeline), nullptr, std::move(renderer),
                                     nullptr, &err, BufferPolicy::disabled());
    FX_REQUIRE(fx.player != nullptr);
    return fx;
}

// ---------------------------------------------------------------------
// make_wide_fixture_with_audio_and_video —— 下面这组回归
// 用例专用。
// ---------------------------------------------------------------------
//
// 跟 make_fixture_with_audio_and_video() 的区别：
//   1. 素材更长（3 秒，synth_video_audio_long()）——1 秒素材帧数太少，
//      不够反复触发下面第 2 步要构造的"音频 FrameQueue 排空"这个状态。
//      FrameQueue 深度本身**没有**调大——
//      `cfg.max_frames_per_track` 就是默认值 8，跟
//      make_fixture_with_audio_and_video() 用的默认 `PipelineConfig{}`
//      一样（这条用例的复现不需要深的 FrameQueue，见
//      step_does_not_livelock_when_audio_queue_empty_and_video_early
//      顶部长注释：默认深度 8 就足够触发早到）。上一版这里写着"FrameQueue
//      深度调到 64"，是这份注释自己写错了、代码从来没这么做过，已订正
//      ——真正调大的是下面这条。
//   2. `max_packets_per_track` 调到 256（默认 128）——3 秒素材的音频轨
//      （AAC，~130 个包）加上视频轨的包数，在默认 128 上限下会在"暂停
//      预热"阶段提前撞上 PacketQueue 背压，把"Pipeline 的音频 FrameQueue
//      排空"这个本用例要构造的状态跟"PacketQueue 满了"这个不相关的状态
//      混在一起；调大之后素材的全部包数都能在预热阶段被安稳读进
//      PacketQueue，不会中途被这条无关的背压打断。
//   3. sink 的写入容量调到远超默认 48000 帧（约 1 秒）——这条用例要构造
//      的是"Pipeline 的音频 FrameQueue 真的空了"（对应已知缺陷描述的
//      根因），不是"sink 写入容量满了"（那是另一条已经被堵住的路径，
//      audio_blocked 分支本身没问题，见 track_player.cpp
//      第 3 步注释）。默认容量下，暂停预热攒出的音频帧一次性 Queued
//      进 sink 会在中途撞上写入背压，把两条本该分开验证的路径混在一
//      起——调大容量把这个混淆源去掉。
struct AudioDrainPipelineConfig {
    PipelineConfig cfg;
    AudioDrainPipelineConfig() {
        cfg.max_packets_per_track = 256;   // 见上方第 2 条；max_frames_per_track 维持默认 8
    }
};

AvFixture make_wide_fixture_with_audio_and_video() {
    AvFixture fx;
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return fx;
    }
    fx.tmp = std::make_unique<TempDir>();
    FX_REQUIRE(!fx.tmp->path.empty());
    const std::string path = synth_video_audio_long(fx.tmp->path, /*seconds=*/3);
    FX_REQUIRE(!path.empty());

    syp_status                     err = SYP_OK;
    const AudioDrainPipelineConfig wide;
    auto pipeline = Pipeline::create_file(path, wide.cfg, &err);
    FX_REQUIRE(pipeline != nullptr);
    FX_REQUIRE(find_managed_audio_track(*pipeline) >= 0);
    FX_REQUIRE(find_managed_video_track(*pipeline) >= 0);

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);   // 见上方注释：去掉写入背压这个混淆源
    fx.sink       = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    fx.renderer   = renderer.get();

    fx.player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                     nullptr, &err, BufferPolicy::disabled());
    FX_REQUIRE(fx.player != nullptr);
    return fx;
}

// ---------------------------------------------------------------------
// make_fixture_that_fails_seek
// ---------------------------------------------------------------------
//
// MovBox/read_boxes/find_path/put_be32/blank_out_video_keyframe_table/
// MemSource/mem_read/mem_seek 跟 tests/test_pipeline.cpp 里
// pipeline_failed_seek_still_flushes_decoders 上方那份已验证过的手法
// 逐字同源（那条长注释记录了完整推导）。这里复制一份而不是跨文件共享
// ——tests/support/ 目前只装真正跨多个测试可执行文件复用的替身，这套
// 是"让 demuxer_->seek() 真失败"这一件事专用的造假素材机制，两个测试
// 可执行文件是独立编译单元。
//
// 要点（跟那条注释一致）：
//   1. 把视频轨 stss 改成 entry_count=1、唯一那条记录指向不存在的样本号
//      0x0FFFFFFF——keyframe_absent 仍是 0（box 非空，走不到"stss 为空
//      ⇒ 全部样本当关键帧"那条兜底），mov_build_index() 因此给整条视频
//      轨建出一个没有任何 AVINDEX_KEYFRAME 的索引，
//      ff_index_search_timestamp() 在 AVSEEK_FLAG_BACKWARD 下找不到可
//      回退的关键帧 ⇒ mov_read_seek() 返回 AVERROR_INVALIDDATA。
//   2. 但光这一步不够：av_seek_frame() 在 read_seek 失败后会落到
//      seek_frame_generic()，index_entries 本身没被清空（stsz/stts/
//      stco/stsc 全部完好），seek_frame_generic() 会摸到最后一条索引项
//      做一次 avio_seek 再线性扫描——在普通文件 IO 上这次 avio_seek 稳
//      赢，扫描也稳赢，于是 av_seek_frame() 整体照样成功、看不出任何
//      失败迹象。必须换一个我们自己完全掌控的 AVIOContext（MemSource），
//      只在这唯一一次 seek() 调用期间让它的 seek 回调报 EIO，把
//      seek_frame_generic() 的这次 avio_seek 也打断，才能让整个
//      av_seek_frame() 真正失败——同时不影响它前后任何一次正常读包。
bool blank_out_video_keyframe_table(std::vector<uint8_t>* data) {
    struct MovBox {
        std::string fourcc;
        std::size_t start   = 0;
        std::size_t end     = 0;
        std::size_t payload = 0;
    };
    struct Local {
        static std::vector<MovBox> read_boxes(const std::vector<uint8_t>& d, std::size_t start,
                                               std::size_t end) {
            std::vector<MovBox> out;
            std::size_t         pos = start;
            while (pos + 8 <= end) {
                const uint32_t size32 = (uint32_t(d[pos]) << 24) | (uint32_t(d[pos + 1]) << 16) |
                                        (uint32_t(d[pos + 2]) << 8) | uint32_t(d[pos + 3]);
                const std::string fourcc(reinterpret_cast<const char*>(&d[pos + 4]), 4);
                std::size_t       hdr  = 8;
                uint64_t           size = size32;
                if (size32 == 1) {
                    if (pos + 16 > end) break;
                    uint64_t s = 0;
                    for (int i = 0; i < 8; ++i) s = (s << 8) | d[pos + 8 + static_cast<std::size_t>(i)];
                    size = s;
                    hdr  = 16;
                } else if (size32 == 0) {
                    size = end - pos;
                }
                if (size < hdr || pos + size > end) break;
                out.push_back(MovBox{fourcc, pos, pos + static_cast<std::size_t>(size), pos + hdr});
                pos += static_cast<std::size_t>(size);
            }
            return out;
        }
        static void find_path(const std::vector<uint8_t>& d, const std::vector<std::string>& path,
                               std::size_t start, std::size_t end, std::vector<MovBox>* out) {
            const std::vector<MovBox> boxes = read_boxes(d, start, end);
            for (const MovBox& b : boxes) {
                if (b.fourcc != path[0]) continue;
                if (path.size() == 1) {
                    out->push_back(b);
                } else {
                    const std::vector<std::string> rest(path.begin() + 1, path.end());
                    find_path(d, rest, b.payload, b.end, out);
                }
            }
        }
        static void put_be32(std::vector<uint8_t>* d, std::size_t off, uint32_t v) {
            (*d)[off]     = static_cast<uint8_t>(v >> 24);
            (*d)[off + 1] = static_cast<uint8_t>(v >> 16);
            (*d)[off + 2] = static_cast<uint8_t>(v >> 8);
            (*d)[off + 3] = static_cast<uint8_t>(v);
        }
    };

    std::vector<MovBox> moovs;
    Local::find_path(*data, {"moov"}, 0, data->size(), &moovs);
    if (moovs.empty()) return false;
    std::vector<MovBox> traks;
    Local::find_path(*data, {"trak"}, moovs[0].payload, moovs[0].end, &traks);
    bool patched = false;
    for (const MovBox& trak : traks) {
        std::vector<MovBox> hdlrs;
        Local::find_path(*data, {"mdia", "hdlr"}, trak.payload, trak.end, &hdlrs);
        bool is_video = false;
        for (const MovBox& h : hdlrs) {
            if (h.payload + 12 > h.end) continue;
            if (std::string(reinterpret_cast<const char*>(&(*data)[h.payload + 8]), 4) == "vide")
                is_video = true;
        }
        if (!is_video) continue;
        std::vector<MovBox> stsses;
        Local::find_path(*data, {"mdia", "minf", "stbl", "stss"}, trak.payload, trak.end, &stsses);
        for (const MovBox& b : stsses) {
            if (b.payload + 12 > b.end) continue;
            Local::put_be32(data, b.payload + 4, 1);
            Local::put_be32(data, b.payload + 8, 0x0FFFFFFFu);
            patched = true;
        }
    }
    return patched;
}

struct MemSource {
    const std::vector<uint8_t>* buf       = nullptr;
    int64_t                     pos       = 0;
    bool                        fail_seek = false;
    // 跟 fail_seek 同一个模式，但挡的是常规
    // 顺序读（av_read_frame() 用的是 read 回调，不是 seek 回调）——
    // fail_seek 只能让 Pipeline::seek() 本身失败，让不了 demux 分支的
    // 常规一次 step() 真正报 Kind::Error；要测"Error 优先级最高"这条，
    // 必须能让常规读也失败。跟 fail_seek 一样是调用方显式置位/复位的
    // 一次性开关，不影响素材构造阶段（TrackPlayer::create() 内部的
    // 探测读）。
    bool                         fail_read = false;
};

int mem_read(void* opaque, uint8_t* out, int n) {
    MemSource* s = static_cast<MemSource*>(opaque);
    if (s->fail_read) return AVERROR(EIO);
    const int64_t size = static_cast<int64_t>(s->buf->size());
    if (s->pos >= size) return AVERROR_EOF;
    const int64_t left = size - s->pos;
    const int     m    = static_cast<int>(left < n ? left : n);
    std::memcpy(out, s->buf->data() + s->pos, static_cast<std::size_t>(m));
    s->pos += m;
    return m;
}

int64_t mem_seek(void* opaque, int64_t off, int whence) {
    MemSource* s = static_cast<MemSource*>(opaque);
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return static_cast<int64_t>(s->buf->size());
    if (s->fail_seek) return AVERROR(EIO);
    const int     w  = whence & ~AVSEEK_FORCE;
    const int64_t np = (w == SEEK_CUR)   ? s->pos + off
                      : (w == SEEK_END)  ? static_cast<int64_t>(s->buf->size()) + off
                                          : off;
    if (np < 0) return AVERROR(EINVAL);
    s->pos = np;
    return np;
}

// data/src/pb 三者必须比 player（以及它内部的 Pipeline/Demuxer）活得久
// ——create_avio() 明确"不接管 ctx 的所有权"（pipeline.h 该函数声明处
// 注释），Demuxer 在自己整个生命周期内都会用这个 AVIOContext 读数据。
// 用 std::unique_ptr<SeekFailMaterial> 整体堆分配、只挪不拷，避免这个
// struct 若按值搬动导致 MemSource::buf（指向 data 这个子对象本身）失效
// ——跟本文件其它 fixture 用 std::unique_ptr<TempDir> 而不是直接内嵌
// TempDir 是同一个理由。
struct SeekFailMaterial {
    std::vector<uint8_t> data;
    MemSource             src;
    AVIOContext*          pb = nullptr;

    SeekFailMaterial()                                    = default;
    SeekFailMaterial(const SeekFailMaterial&)              = delete;
    SeekFailMaterial& operator=(const SeekFailMaterial&)   = delete;

    ~SeekFailMaterial() {
        if (pb != nullptr) {
            av_free(pb->buffer);
            avio_context_free(&pb);
        }
    }
};

struct SeekFailFixture {
    std::unique_ptr<SeekFailMaterial> material;   // 先声明：后析构，见上面注释
    std::unique_ptr<TrackPlayer>      player;
    syp::test::FakeAudioSink*         sink     = nullptr;
    syp::test::FakeRenderer*          renderer = nullptr;
};

#define SF_REQUIRE(expr)                                                 \
    do {                                                                 \
        if (!(expr)) {                                                  \
            ::tiny_test::fail(__FILE__, __LINE__, #expr);                \
            return fx;                                                  \
        }                                                                \
    } while (0)

// 构造一份"demuxer_->seek() 真的会失败，但两条轨在此之前此之后都照常
// 出包出帧"的素材，装进一个开箱即用的 TrackPlayer（音频轨完好，clock
// 走生产路径选出的 AudioClock）。调用方自己决定什么时候调
// fx.material->src.fail_seek = true 再 seek()——本函数不预置这个标志，
// 因为素材构造阶段（TrackPlayer::create() 内部会先驱动几次 Pipeline
// 读 stream info）本身也会真的调用底层 IO，不该被这个标志误伤。
SeekFailFixture make_fixture_that_fails_seek() {
    SeekFailFixture fx;
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return fx;
    }
    TempDir tmp;
    SF_REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio(tmp.path);
    SF_REQUIRE(!path.empty());

    fx.material = std::make_unique<SeekFailMaterial>();
    fx.material->data = [&] {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }();
    SF_REQUIRE(!fx.material->data.empty());
    SF_REQUIRE(blank_out_video_keyframe_table(&fx.material->data));

    fx.material->src.buf = &fx.material->data;
    constexpr int  kBuf  = 32768;
    unsigned char* iobuf = static_cast<unsigned char*>(av_malloc(static_cast<std::size_t>(kBuf)));
    fx.material->pb = avio_alloc_context(iobuf, kBuf, 0, &fx.material->src, mem_read, nullptr, mem_seek);
    fx.material->pb->seekable = AVIO_SEEKABLE_NORMAL;

    syp_status     err = SYP_OK;
    PipelineConfig cfg;
    auto           pipeline = Pipeline::create_avio(fx.material->pb, cfg, &err);
    SF_REQUIRE(pipeline != nullptr);
    SF_REQUIRE(find_managed_audio_track(*pipeline) >= 0);
    SF_REQUIRE(find_managed_video_track(*pipeline) >= 0);

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    fx.sink   = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    fx.renderer   = renderer.get();

    fx.player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                     nullptr, &err, BufferPolicy::disabled());
    SF_REQUIRE(fx.player != nullptr);
    return fx;
}

#undef SF_REQUIRE
#undef FX_REQUIRE

}  // namespace

// =======================================================================
// Step 1：同步算法三分支 —— 九条用例，字面断言未改。
// =======================================================================

// 这条字面断言一直没改过，但它意外地也是另一个坑
// 的回归守卫，值得记一笔：`make_fixture_with_video_pts()`
// 在构造阶段就用 `drive_pipeline_best_effort()` 把素材（仅 2 帧）整条
// 驱动到底、只留 pts=100000 这一帧待取——所以 Pipeline 内部这时已经
// demux_eof/decoder_eof，只是这一帧还压在 FrameQueue 里没被取走。这里
// 唯一一次 `step()` 调用会依次：pop 到这唯一一帧 → 判定"早了" → 走到
// 第 4 步驱动 `pipeline_->step()` → 这次驱动发现该轨的 `PacketQueue`/
// `FrameQueue` 都已排空（刚被上一步 pop 干净），`Pipeline::step()` 的
// Eof 判据因此满足，如实报 `Kind::Eof`。若 track_player.cpp 的优先级
// 把 Pipeline 自己的 `Eof` 排在 `video_waiting_early` 前面，这里会返回
// `Kind::Eof` 而不是 `Kind::Waiting`，这条断言会翻红——最初的实现
// 反向自检就是被这条用例当场抓住的，见 track_player.cpp 里这段判断
// 的注释、以及 track_player.h 顶部 step() 总览第 5 步。
TEST_CASE(early_frame_waits_and_is_not_presented) {
    // 帧比时钟早超过 kPresentWindowUs：不呈现，帧留在队列。
    auto fx = make_fixture_with_video_pts({100000});   // 帧 pts = 100ms
    fx.clock->set(0);                                  // 时钟在 0，早了 100ms
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Waiting);
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});
}

TEST_CASE(on_time_frame_is_presented) {
    auto fx = make_fixture_with_video_pts({100000});
    fx.clock->set(100000);
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Presented);
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK_EQ(fx.renderer->shown()[0].pts_us, int64_t{100000});
}

TEST_CASE(late_but_not_too_late_frame_is_still_presented) {
    // 这条守的是那条兜底分支：迟到 40~80ms 落在两个阈值
    // 之间，必须呈现而不是"一个分支都不匹配"。写成对称区间的实现会在
    // 这里露馅（见 Step 6 反向自检 #1）。
    auto fx = make_fixture_with_video_pts({100000});
    fx.clock->set(160000);                             // 迟到 60ms
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(fx.renderer->shown().size(), size_t{1});
}

TEST_CASE(very_late_frame_is_dropped_not_presented) {
    auto fx = make_fixture_with_video_pts({100000});
    fx.clock->set(300000);                             // 迟到 200ms
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Dropped);
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{1});
}

TEST_CASE(one_step_drops_at_most_kMaxDropsPerStep) {
    // 没有上限的话一次 step() 会丢光整个队列，"一次推进一个单位"就不
    // 成立。Pipeline::step() 的内部循环用 packets->size()+2 约束是同一
    // 个理由（见 Step 6 反向自检 #2）。
    std::vector<int64_t> pts;
    for (int i = 0; i < 64; ++i) pts.push_back(i * 1000);   // 全都远早于时钟
    auto fx = make_fixture_with_video_pts(pts);
    fx.clock->set(10'000'000);                              // 全部迟到 10 秒

    const int64_t before = fx.player->dropped_frames();
    (void)fx.player->step();
    const int64_t dropped_in_one_step = fx.player->dropped_frames() - before;
    CHECK(dropped_in_one_step >= 1);
    CHECK(dropped_in_one_step <= syp::media::kMaxDropsPerStep);
}

TEST_CASE(present_failure_is_counted_and_does_not_stop_playback) {
    auto fx = make_fixture_with_video_pts({0, 40000, 80000});
    fx.renderer->inject_failure_every(1);      // 每次都失败
    fx.clock->set(0);
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Presented);   // 已消费该帧
    CHECK_EQ(fx.player->present_failures(), int64_t{1});
    CHECK(o.status == SYP_OK);                        // 不上报为整体错误
}

TEST_CASE(clock_kind_is_audio_when_sink_opens) {
    auto fx = make_fixture_with_audio_and_video();
    CHECK(fx.player->clock_kind() == ClockKind::Audio);
}

TEST_CASE(clock_kind_falls_back_to_system_when_no_audio_track) {
    auto fx = make_fixture_with_video_pts({0});
    CHECK(fx.player->clock_kind() == ClockKind::System);
}

TEST_CASE(sink_failure_mid_playback_degrades_to_system_clock) {
    // 「降级」这一级：不是错误也不是正常，是可观测的状态
    // 变化。
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);
    fx.sink->inject_failure();
    (void)fx.player->step();
    CHECK(fx.player->clock_kind() == ClockKind::System);
    // 降级之后仍然能继续推进，不是终止
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind != PlayOutcome::Kind::Error);
}

// 【守卫检查的时刻，早于唯一能让它失效的那个动作】
//
// step() 开头那次 sink_->failed() 检查决定要不要降级，注释写着「不能拿一
// 个已失效的时钟去决定丢不丢帧」；但 failed_ 唯一的翻转点，就是同一次
// step() 第 1 步自己调的 sink_->write()（真身 AudioUnitSink 有四处在
// write() 内部 `failed_ = true; return false;`，见 fake_audio_sink.h 顶部
// 注释的四个文件:行号）。于是同一次 step() 内：开头检查通过 → 不降级 →
// write() 当场翻脸 → 视频分支拿着已失效的 AudioClock 算
// `pts - clock_->now_us()`，而失效 AudioClock 的读数按接口契约是
// AV_NOPTS_VALUE（INT64_MIN）——有符号整数溢出（UBSan 实测报在
// track_player.cpp 的 diff 那一行），溢出后的 diff 恒小于
// -kDropThresholdUs，这一次 step() 把丢帧预算打满 kMaxDropsPerStep 帧
// （全是本该呈现的真实帧），并把 pts_us = INT64_MIN 当合法结果交给调用方。
//
// 这条用例必须靠 fail_inside_next_write() 才写得出来：inject_failure()
// 只能在两次 step() 之间带外注入，构造出的永远是"开头那次守卫看得见的
// 失败"——恰好只覆盖已经被挡住的那一半（上面
// sink_failure_mid_playback_degrades_to_system_clock 测的就是那一半）。
//
// 断言全部针对**write() 翻脸的那一次 step()**，不是"最终会收敛到"：
// 用 sink->failed() 从假变真作为"就是这一次"的判据，然后立刻断言。
// 把 track_player.cpp 的「1.5 步」重新检查注掉，三条断言会各自变红：
// clock_kind 停在 Audio、丢帧数打满 8、返回 pts 是 INT64_MIN。
TEST_CASE(sink_failing_inside_write_degrades_before_video_branch_same_step) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    // 预热用**暂停态**驱动，不是"边播边攒"：暂停分支只驱动
    // Pipeline::step()、不消费任何一条轨，所以
    // 跑完这一段，音频和视频两条 FrameQueue 都是满的（默认容量各 8），
    // 且一帧都没被消费掉。这是缺陷要的现场，而且是**确定性**的：翻脸的
    // 那一次 step() 里，音频有帧可写（注入面才有机会在 write() 内部翻
    // 脸），视频分支手上有一整队本该呈现的真实帧可以被错误地丢光。
    //
    // 换成"边播边攒"（跑到第一次 Queued 就停）不行：实测那一刻视频
    // FrameQueue 常常是空的，缺陷路径无帧可丢，step() 只会如实返回
    // Blocked（它的 pts_us 按设计就是 AV_NOPTS_VALUE），用例会变成一条
    // 断言错对象的空测试。
    //
    // 暂停中现在会先出一次"打开后预览帧"（Presented，
    // 不消费 FrameQueue 的存量以外的任何东西——它本来就是从这条 FrameQueue
    // 里 pop 出来呈现掉的一帧，之后 Pipeline 会把它重新灌满），这条例外
    // 只应该出现一次；120 次里刨掉这一次，其余仍然必须是纯粹的 Waiting。
    fx.player->pause();
    bool saw_preview_frame = false;
    for (int i = 0; i < 120; ++i) {
        const PlayOutcome po = fx.player->step();
        if (po.kind == PlayOutcome::Kind::Presented) {
            REQUIRE(!saw_preview_frame);   // 只应该出现这一次
            saw_preview_frame = true;
            continue;
        }
        REQUIRE(po.kind == PlayOutcome::Kind::Waiting);   // 暂停期间只驱动，不消费
    }
    REQUIRE(saw_preview_frame);
    fx.player->play();
    REQUIRE(!fx.sink->failed());
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    const int64_t dropped_before = fx.player->dropped_frames();

    // 一次 step()，不是"跑到收敛"：缺陷就活在这一次调用里。
    fx.sink->fail_inside_next_write();
    const PlayOutcome o = fx.player->step();
    REQUIRE(fx.sink->failed());   // 确认注入面真的在这一次 write() 里翻了脸

    // 1）本轮判定必须已经改用系统时钟——不是等到下一次 step() 才降级。
    CHECK(fx.player->clock_kind() == ClockKind::System);
    // 2）丢帧预算没有被打满：溢出后的 diff 恒小于 -kDropThresholdUs，
    //    缺陷下这一次 step() 会把队列里 8 帧真实帧全丢掉。实测修复后是 0。
    const int64_t dropped_in_that_step = fx.player->dropped_frames() - dropped_before;
    CHECK(dropped_in_that_step < syp::media::kMaxDropsPerStep);
    // 3）交给调用方的 pts 不是哨兵值。缺陷下这一次返回的是
    //    Dropped{pts = AV_NOPTS_VALUE}（丢光队列后从循环外那条 return
    //    出去，pts 用的是 PlayOutcome 的默认值）。
    CHECK(o.pts_us != std::numeric_limits<int64_t>::min());

    // 降级之后仍然能继续推进，不是终止。
    const PlayOutcome next = fx.player->step();
    CHECK(next.kind != PlayOutcome::Kind::Error);
}

// sanitize_position() 的钳位在此前**零覆盖**：曾经把它整个
// 删成 `return v;`，全量 ctest 23/23 全绿。它有三类调用点——
//   1. just_sought_ 分支（`sanitize_position(pts)`，后来补的）：
//      今天不可达，本仓库 FFmpeg 只 --enable-demuxer=mov，mov 恒有 pts；
//   2. set_speed()：`sanitize_position(position_us())`；
//   3. seek()：`sanitize_position(ts_us)`。
// 后两条今天就可达，缺的只是没人测。下面两条用例各钉死一条。
//
// 可观测出口是 IAudioSink::flush(base)——两处都把钳位后的值经由它递给
// sink，所以断言落在 FakeAudioSink::last_flush_base_us() 上。
// 把 sanitize_position() 改成 `return v;`，两条都会红。

TEST_CASE(set_speed_while_sink_failed_does_not_flush_a_sentinel_base) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    // 先播一段，让 last_known_position_us_ 有一个真实的非零值可回退。
    // FakeAudioSink 的 played_us() 只被 advance() 推进（它按可注入的速率
    // 消费样本），光调 step() 时钟是不走的——见 fake_audio_sink.h 顶部。
    for (int i = 0; i < 200; ++i) {
        (void)fx.player->step();
        fx.sink->advance(2000);
    }
    const int64_t before = fx.player->position_us();
    REQUIRE(before != AV_NOPTS_VALUE);
    REQUIRE(before > 0);

    // 关键窗口：sink 已 failed，但还没有任何一次 step() 把它降级掉，
    // 所以 clock_ 仍是那个失效的 AudioClock，position_us() 转发的是
    // AV_NOPTS_VALUE。inject_failure() 在这里是对的工具——这条用例要的
    // 恰恰是"两次 step() 之间"的那个窗口。
    fx.sink->inject_failure();
    REQUIRE(fx.player->position_us() == AV_NOPTS_VALUE);

    REQUIRE(fx.player->set_speed(2.0) == SYP_OK);

    // 钳位生效：flush 收到的是回退值，不是哨兵。没有钳位的话这里是
    // AV_NOPTS_VALUE（INT64_MIN），sink 的整个时间基准会被投毒。
    //
    // 不断言与 before 逐位相等：回退值是 last_known_position_us_，它只在
    // 呈现一帧时更新，比失效前一刻的 AudioClock 读数落后不到一帧。断言
    // "是一个贴近真实播放位置的正数"才是这条钳位要保证的性质。
    const int64_t base = fx.sink->last_flush_base_us();
    CHECK(base != AV_NOPTS_VALUE);
    CHECK(base > 0);
    CHECK(before - base < 100000 && base - before < 100000);
}

TEST_CASE(seek_to_sentinel_timestamp_does_not_flush_a_sentinel_base) {
    auto fx = make_fixture_with_audio_and_video();
    for (int i = 0; i < 200; ++i) {
        (void)fx.player->step();
        fx.sink->advance(2000);
    }
    const int64_t before = fx.player->position_us();
    REQUIRE(before != AV_NOPTS_VALUE);
    REQUIRE(before > 0);

    // seek() 的 ts_us 来自调用方，TrackPlayer 不对它设防（没有范围校验，
    // 见头文件）。调用方递一个 AV_NOPTS_VALUE 进来是 sanitize_position()
    // 在这条路径上唯一要挡的东西。seek 本身成不成功不影响这条断言——
    // 头文件明确要求"失败也要走完后三步"。
    (void)fx.player->seek(AV_NOPTS_VALUE);

    const int64_t base = fx.sink->last_flush_base_us();
    CHECK(base != AV_NOPTS_VALUE);
    CHECK(base > 0);
    CHECK(before - base < 100000 && base - before < 100000);
}

// =======================================================================
// 状态机组合覆盖 ——「SystemClock 状态机组合覆盖度」
// 那一节点名 TrackPlayer 会继承这个形状（play/pause/set_speed/seek 加上
// pending_*/first_after_seek_/clock_kind_ 更多状态），要求至少覆盖这几
// 条组合。故意留出有意义的时间间隔/步数，不用零延迟场景——那一节明确
// 点过"零延迟会巧合掩盖忘记结算类缺陷"这个陷阱。
// =======================================================================

// 组合 1：暂停中调 step() 反复多次——除了后来新加的"打开后预览
// 帧"（只出这一次，不动时钟），其余每次都必须是 Waiting、时钟冻结（用
// 例自己不拨时钟，只是反复 step()，跟 SystemClock 那次教训一样故意留出
// "多次调用"这个跨度，而不是构造后立刻查一次）。
//
// 此前这里断言"5 次全部 Waiting、shown().size()==0"
// ——那是更早的行为。现在暂停中会先把打开后的第一帧画出来一次
// （不动时钟），之后才是纯粹的 Waiting；断言改成"恰好一次 Presented（首
// 帧 pts=0），其余 Waiting"，仍然覆盖"暂停不呈现别的帧、不丢帧、不推进
// 时钟"这条组合本来要测的东西。
TEST_CASE(pause_then_repeated_step_stays_waiting_and_frozen) {
    auto fx = make_fixture_with_video_pts({0, 40000, 80000, 120000});
    fx.clock->set(0);
    fx.player->pause();
    REQUIRE(fx.player->paused());

    int presented_count = 0;
    for (int i = 0; i < 5; ++i) {
        const PlayOutcome o = fx.player->step();
        if (o.kind == PlayOutcome::Kind::Presented) {
            ++presented_count;
            CHECK_EQ(o.pts_us, int64_t{0});
        } else {
            CHECK(o.kind == PlayOutcome::Kind::Waiting);
        }
    }
    CHECK_EQ(presented_count, 1);
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK_EQ(fx.renderer->shown()[0].pts_us, int64_t{0});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});
    // 暂停期间 now_us() 本身不由 TrackPlayer 冻结（clock_override 场景
    // 下时钟的"冻结"是用例自己不拨表这件事保证的）——这里断言的是
    // TrackPlayer 侧真正该保证的事：暂停时预览帧之外不呈现别的帧、不
    // 丢帧、不推进内部状态（预览帧本身也不碰时钟）。
    CHECK_EQ(fx.player->position_us(), int64_t{0});
}

// 组合 2：暂停 → seek() → 恢复。暂停中 seek 不应该被吞掉。
//
// 此前这条断言"暂停期间 seek 完仍然是 Waiting，
// play() 之后第一帧才立即呈现"——那是更早的行为，注释里明写
// "暂停优先于'刚 seek 完该立即呈现'这条"。后来把这条反过来：暂停不
// 再压住 seek 后第一帧的"立即呈现"语义（第二条用例
// paused_seek_presents_first_frame_after_seek 就是在钉这个新行为）。这
// 里改成断言"暂停中就能看到 seek 后的第一帧"，play() 之后走正常判定
// （不会对第二帧也搞"立即呈现"）。
//
// 注意：seek() 会清空 Pipeline 的队列并把 demuxer 重新定位——之后要靠
// TrackPlayer::step() 自己反复驱动 Pipeline::step() 才能重新解出帧
// （TrackPlayer 每次 step() 只驱动 Pipeline 一次，见 track_player.cpp
// 第 3 步），不是一次 step() 就能立即拿到。这里用一个有上限的循环等
// 帧出现，断言"最终确实呈现了，且是 Presented 而不是 Dropped/Waiting
// 一直原地打转"。
TEST_CASE(pause_then_seek_then_play_presents_first_frame_immediately) {
    auto fx = make_fixture_with_video_pts({0, 40000, 80000, 120000, 160000});
    fx.clock->set(0);
    fx.player->pause();

    const syp_status rc = fx.player->seek(0);
    CHECK(rc == SYP_OK);

    bool    presented     = false;
    int64_t presented_pts = -1;
    for (int i = 0; i < 200 && !presented; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        REQUIRE(o.kind != PlayOutcome::Kind::Dropped);
        if (o.kind == PlayOutcome::Kind::Presented) {
            presented     = true;
            presented_pts = o.pts_us;
        }
    }
    CHECK(presented);
    CHECK_EQ(presented_pts, int64_t{0});
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK(fx.player->paused());   // 暂停中出的这一帧不等于恢复播放

    fx.player->play();
    REQUIRE(!fx.player->paused());
    // 时钟设一个跟下一帧 pts 完全不匹配的值——just_sought_ 已经被暂停中
    // 那次 present_first_frame_after_seek() 消费掉了，恢复播放后不应该
    // 再对任何帧搞"立即呈现"；这一段应该走正常三分支判定，命中 Dropped
    // 而不是 Presented。
    fx.clock->set(999'999'999);
    for (int i = 0; i < 50; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        REQUIRE(o.kind != PlayOutcome::Kind::Presented);
    }
    CHECK_EQ(fx.renderer->shown().size(), size_t{1});   // 还是只有暂停中呈现的那一帧
}

// =======================================================================
// 暂停中快速出画——打开后预览第一帧（不动时钟）、暂停中
// seek 后立即呈现。决定：打开后的预览帧只在暂停中生
// 效；未暂停的首次播放保持现有三分支判定不变——大量既有用例（如
// early_frame_waits_and_is_not_presented）依赖首帧走正常判定，且未暂停
// 时时钟本就从首帧附近起步。
// =======================================================================

// 打开后先暂停，step 若干次即可看到第一帧，时钟不动。
TEST_CASE(paused_after_open_presents_first_frame_without_moving_clock) {
    auto fx = make_fixture_with_video_pts({400000, 440000});   // 首帧远"早于"时钟 0
    fx.player->pause();
    fx.clock->set(0);
    bool presented = false;
    for (int i = 0; i < 64 && !presented; ++i) {
        presented = fx.player->step().kind == PlayOutcome::Kind::Presented;
    }
    CHECK(presented);
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK_EQ(fx.renderer->shown()[0].pts_us, int64_t{400000});
    CHECK_EQ(fx.player->position_us(), int64_t{0});
    // 预览帧只出一次：继续暂停 step 不再呈现。
    for (int i = 0; i < 16; ++i) fx.player->step();
    CHECK_EQ(fx.renderer->shown().size(), size_t{1});
}

// 暂停中 seek：seek 后第一帧也立即出现。
TEST_CASE(paused_seek_presents_first_frame_after_seek) {
    auto fx = make_fixture_with_audio_and_video();
    fx.player->pause();
    for (int i = 0; i < 64; ++i) fx.player->step();   // 可能已出预览帧
    const size_t before = fx.renderer->shown().size();
    REQUIRE(fx.player->seek(500000) == SYP_OK);
    bool presented = false;
    for (int i = 0; i < 256 && !presented; ++i) {
        presented = fx.player->step().kind == PlayOutcome::Kind::Presented;
    }
    CHECK(presented);
    CHECK(fx.renderer->shown().size() > before);
}

// 未暂停时首帧仍走三分支判定（行为不变的守卫）。
//
// 【实现时订正】原计划的 pts 是 400000——单元素时
// make_fixture_with_video_pts() 把它当 spacing，要求 1'000'000 % spacing
// == 0（保证 fps 是整数）才能生成素材；400000 不整除 1'000'000
// （2.5），FX_REQUIRE 会在夹具构造阶段就失败，player 是空指针，测试体
// 里的 fx.player->step() 会段错误。换成 500000（同样"远早于"时钟 0，
// 且整除 1'000'000），不改变这条用例要守住的语义。
TEST_CASE(unpaused_first_frame_still_waits_when_early) {
    auto fx = make_fixture_with_video_pts({500000});
    fx.clock->set(0);
    CHECK(fx.player->step().kind == PlayOutcome::Kind::Waiting);
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});
}

// 补一条 BUSY 覆盖 seek 后第一帧路径的
// 回归。只有音视频轨都真实存在的 fixture 才有完整的 seek() 语义，能用
// 的只有 make_fixture_with_audio_and_video()——它没有 clock_override，
// 走真实 AudioClock，"clock 基准复位到该帧 pts"这条副作用在这份 fixture
// 上无法通过 owned_system_clock_ 直接观察。
//
// BUSY 不再消费帧：真实窗口上挂 layer 的在途上限是 2、
// 名额在 presentedHandler（上屏之后）才归还，而播放器最多提前 40ms 提交，
// 60fps 下 BUSY 是常态——此前"BUSY 即丢"实测丢掉 17%~45% 的帧。现在
// seek 后第一帧遇到 BUSY：just_sought_ 保持置位、帧留在 pending_video_，
// 不报 Dropped；后续 step() 重试**同一帧**，成功后才消费 just_sought_。
// 断言：BUSY 期间无 Presented/Dropped、重试的始终是同一个 pts、恢复后呈现的
// 正是那一帧（pts 与 busy_pts 一致）、dropped_frames 不变、之后继续暂停
// step() 不再呈现（just_sought_ 已消费）。
TEST_CASE(paused_seek_first_frame_busy_retains_just_sought_and_retries_same_frame) {
    auto fx = make_fixture_with_audio_and_video();
    fx.player->pause();
    for (int i = 0; i < 64; ++i) fx.player->step();   // 消费掉打开后的预览帧（如果有）
    const size_t  shown_before   = fx.renderer->shown().size();
    const int64_t dropped_before = fx.player->dropped_frames();
    fx.renderer->inject_busy_every(1);                // 之后每次 present 都 BUSY
    REQUIRE(fx.player->seek(500000) == SYP_OK);
    const int64_t flush_after_seek = fx.sink->flush_count();

    for (int i = 0; i < 64; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        REQUIRE(o.kind != PlayOutcome::Kind::Presented);
        REQUIRE(o.kind != PlayOutcome::Kind::Dropped);   // BUSY 不再报 Dropped
    }
    // 每次暂停 step() 都重试一次：计数 > 1 说明 just_sought_ 没被 BUSY 消费。
    REQUIRE(fx.player->render_busy_frames() > int64_t{1});
    const auto& busy = fx.renderer->busy_pts();
    REQUIRE(!busy.empty());
    for (int64_t p : busy) CHECK_EQ(p, busy.front());   // 重试的始终是同一帧
    CHECK_EQ(fx.player->dropped_frames(), dropped_before);
    // 首帧复位副作用只做一次：多次 BUSY 重试不重复 flush sink（重复 flush 会抹掉
    // 重试期间写进去的音频）。seek() 自己 flush 一次，首帧再 flush 一次。
    CHECK_EQ(fx.sink->flush_count(), flush_after_seek + 1);

    fx.renderer->inject_busy_every(0);
    bool    presented     = false;
    int64_t presented_pts = AV_NOPTS_VALUE;
    for (int i = 0; i < 16 && !presented; ++i) {
        const PlayOutcome o = fx.player->step();
        if (o.kind == PlayOutcome::Kind::Presented) {
            presented     = true;
            presented_pts = o.pts_us;
        }
    }
    REQUIRE(presented);
    REQUIRE(fx.renderer->shown().size() == shown_before + 1);
    CHECK_EQ(fx.renderer->shown().back().pts_us, presented_pts);
    CHECK_EQ(presented_pts, busy.front());
    CHECK(presented_pts <= 500000);   // 落点是 ≤ 请求位置的关键帧
    CHECK_EQ(fx.player->dropped_frames(), dropped_before);
    CHECK_EQ(fx.sink->flush_count(), flush_after_seek + 1);   // 成功呈现也没有再 flush

    // just_sought_ 已被成功的那次呈现消费：继续暂停 step() 不再出画。
    for (int i = 0; i < 16; ++i) fx.player->step();
    CHECK_EQ(fx.renderer->shown().size(), shown_before + 1);
}

// 暂停中打开后的预览帧遇到 BUSY——preview_pending_ 保持、帧保留，
// 之后重试呈现同一帧，时钟不动。
TEST_CASE(paused_preview_busy_retains_frame_and_retry_presents_it) {
    auto fx = make_fixture_with_video_pts({400000, 440000});
    fx.player->pause();
    fx.clock->set(0);
    fx.renderer->inject_busy_every(1);
    for (int i = 0; i < 32; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Presented);
        REQUIRE(o.kind != PlayOutcome::Kind::Dropped);
    }
    REQUIRE(fx.player->render_busy_frames() > int64_t{1});   // 每次都在重试
    for (int64_t p : fx.renderer->busy_pts()) CHECK_EQ(p, int64_t{400000});
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});

    fx.renderer->inject_busy_every(0);
    bool presented = false;
    for (int i = 0; i < 8 && !presented; ++i) {
        presented = fx.player->step().kind == PlayOutcome::Kind::Presented;
    }
    REQUIRE(presented);
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK_EQ(fx.renderer->shown()[0].pts_us, int64_t{400000});
    CHECK_EQ(fx.player->position_us(), int64_t{0});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});
    // 只出一次。
    for (int i = 0; i < 16; ++i) fx.player->step();
    CHECK_EQ(fx.renderer->shown().size(), size_t{1});
}

// 非暂停 seek 后第一帧遇到 BUSY——同样保留并重试，不报 Dropped，
// 不会被下一帧顶替。
TEST_CASE(unpaused_seek_first_frame_busy_retries_same_frame) {
    auto fx = make_fixture_with_video_pts({500000, 600000, 700000});
    fx.clock->set(0);
    REQUIRE(fx.player->seek(600000) == SYP_OK);
    fx.renderer->inject_busy_every(1);
    for (int i = 0; i < 32; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        REQUIRE(o.kind != PlayOutcome::Kind::Presented);
        REQUIRE(o.kind != PlayOutcome::Kind::Dropped);
    }
    REQUIRE(fx.player->render_busy_frames() > int64_t{1});
    for (int64_t p : fx.renderer->busy_pts()) CHECK_EQ(p, int64_t{600000});

    fx.renderer->inject_busy_every(0);
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(o.pts_us, int64_t{600000});
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK_EQ(fx.renderer->shown()[0].pts_us, int64_t{600000});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});
    // just_sought_ 已消费：下一帧 700000 相对时钟 0 是"早了"，走正常判定不呈现。
    CHECK(fx.player->step().kind == PlayOutcome::Kind::Waiting);
    CHECK_EQ(fx.renderer->shown().size(), size_t{1});
}

// 非暂停 seek 后第一帧持续 BUSY 不能无限重试：它不消费帧、不 pop
// 后续视频，视频 FrameQueue 满后联合背压停掉解封装，音频随之断粮、AudioClock 停走——
// 帧永远"不迟"，音画永久冻结。复位做过之后（重试阶段）按时钟判迟到：超过
// kDropThresholdUs 就放弃"seek 首帧"特殊路径，交给正常迟到路径丢弃（计丢帧），
// 后续帧照常判定。
TEST_CASE(unpaused_seek_first_frame_sustained_busy_gives_up_when_too_late) {
    auto fx = make_fixture_with_video_pts({500000, 600000, 700000});
    fx.clock->set(0);
    REQUIRE(fx.player->seek(600000) == SYP_OK);
    fx.renderer->inject_busy_every(1);
    fx.clock->set(600000);
    // seek 后要先驱动几次 Pipeline 才有解码帧：步进到 seek 首帧第一次撞上 BUSY（复位已做）。
    for (int i = 0; i < 64 && fx.renderer->busy_pts().empty(); ++i) {
        REQUIRE(fx.player->step().kind == PlayOutcome::Kind::Waiting);
    }
    REQUIRE(fx.renderer->busy_pts().size() == 1);
    CHECK_EQ(fx.renderer->busy_pts()[0], int64_t{600000});

    fx.clock->set(600000 + syp::media::kDropThresholdUs + 1);      // 600000 迟 80.001ms；700000 迟不到 kPresentWindowUs
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Dropped);
    CHECK_EQ(fx.player->dropped_frames(), int64_t{1});
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});
    // 播放继续推进：下一帧 700000（解码好之后）在呈现范围内 → 走正常路径尝试呈现 → 仍 BUSY → 保留。
    for (int i = 0; i < 64 && fx.renderer->busy_pts().back() != int64_t{700000}; ++i) {
        REQUIRE(fx.player->step().kind == PlayOutcome::Kind::Waiting);
    }
    CHECK_EQ(fx.renderer->busy_pts().back(), int64_t{700000});

    fx.renderer->inject_busy_every(0);
    const PlayOutcome o2 = fx.player->step();
    CHECK(o2.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(o2.pts_us, int64_t{700000});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{1});
}

// 同一判据不能误伤：复位之前（第一次拿到帧）不判迟到——seek 落点关键帧的 pts 往往
// 早于请求值，时钟也还没被纠偏，这时判迟到会把 seek 首帧误丢。
TEST_CASE(unpaused_seek_first_frame_not_dropped_before_rebase_even_if_clock_ahead) {
    auto fx = make_fixture_with_video_pts({500000, 600000, 700000});
    fx.clock->set(0);
    REQUIRE(fx.player->seek(600000) == SYP_OK);
    fx.clock->set(600000 + syp::media::kDropThresholdUs + 100000);   // 覆盖时钟不被 set_base 纠偏
    PlayOutcome o = fx.player->step();
    for (int i = 0; i < 64 && o.kind == PlayOutcome::Kind::Waiting; ++i) o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(o.pts_us, int64_t{600000});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});
}

// 组合 3：有 pending_video_ 时 seek()——那帧必须被丢掉，不能 seek 完还
// 呈现一个旧帧。先用一个很早的时钟让第一帧落进"早了"分支（帧留在
// pending_video_ 里），再 seek 到*另一个*位置（不是 0，也不是留下来
// 那帧的 pts），断言 seek 之后呈现的是新位置的帧，不是被留下来的旧帧。
//
// 素材的每一帧都是关键帧（synth_video_only 传了 -g 1），seek(600000)
// 会精确落在 pts=600000 的那一帧上，不会被"只有一个关键帧"这类因素
// 干扰。如果 TrackPlayer 忘了在 seek() 里清空 pending_video_，
// just_sought_ 的"立即呈现"逻辑会直接命中那个残留的旧 Frame（pts=
// 500000）而不会重新 pop——这条用例断言的 600000 会在那种情况下变成
// 500000，测出这个缺陷。
TEST_CASE(seek_discards_pending_video_frame) {
    auto fx = make_fixture_with_video_pts({500000, 600000, 700000});
    fx.clock->set(0);   // 早于第一帧 500000 很多：落进 Waiting，帧留在 pending_video_
    const PlayOutcome before = fx.player->step();
    REQUIRE(before.kind == PlayOutcome::Kind::Waiting);
    REQUIRE(before.pts_us == int64_t{500000});   // 确认真的是"留下的那一帧"

    const syp_status rc = fx.player->seek(600000);
    CHECK(rc == SYP_OK);

    fx.clock->set(600000);
    bool    presented     = false;
    int64_t presented_pts = -1;
    for (int i = 0; i < 200 && !presented; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Presented) {
            presented     = true;
            presented_pts = o.pts_us;
        }
    }
    CHECK(presented);
    CHECK_EQ(presented_pts, int64_t{600000});
    CHECK_EQ(fx.renderer->shown().size(), size_t{1});
}

// 组合 4：降级发生后再 seek()——降级把 clock_kind_ 切到 System 之后，
// seek() 的四步仍然要全部跑完（sink_->flush() 仍然要调，即使 sink 已经
// failed；pending_* 仍然要清空），且 seek 之后仍然能继续推进，不因为
// "又叠加了一层状态变化"就出错。
TEST_CASE(seek_after_degrade_still_completes_and_keeps_playing) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    fx.sink->inject_failure();
    (void)fx.player->step();               // 触发降级
    REQUIRE(fx.player->clock_kind() == ClockKind::System);

    const syp_status rc = fx.player->seek(0);
    CHECK(rc == SYP_OK);
    CHECK(fx.player->clock_kind() == ClockKind::System);   // seek 不应该把它退回 Audio

    bool saw_non_error = true;
    for (int i = 0; i < 20 && saw_non_error; ++i) {
        const PlayOutcome o = fx.player->step();
        if (o.kind == PlayOutcome::Kind::Error) saw_non_error = false;
    }
    CHECK(saw_non_error);
}

// =======================================================================
// 两条 Critical + 若干 Important 的回归用例。
// 根因总结：Queued 这个 outcome 在修复前的测试文件里出现 0 次——
// 音频写入路径、背压路径、pause() 对 sink 的作用、set_speed() 全家都只
// 是"编译过了"。下面这批用例优先让 Queued 真的出现、真的被断言。
// =======================================================================

// 第二条受管轨（这里用第二条音轨模拟；带封面图的 mp4 是
// 同一个形状，封面图在 ffmpeg 里就是一条 attached_pic 的 video 流）不
// 排空的话，它的 FrameQueue 堆满会触发 Pipeline 的联合背压，拖死整条
// 管线——实测双音轨 2 秒素材应出 50 帧，3000 次 step() 只呈现 2 帧。
TEST_CASE(second_audio_track_does_not_deadlock_pipeline) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_two_audio(tmp.path);
    REQUIRE(!path.empty());

    // 小容量的 PipelineConfig：默认 max_packets_per_track=128 对这份
    // 2 秒（~86 个 AAC 包/音轨）素材来说太宽松——没人排空的第二条音轨
    // 的 PacketQueue 压根撑不到 128，联合背压永远不会被真正触发，这条
    // 用例会在"有没有排空第二条轨"两种实现下都通过，测不出区分力（第
    // 一版就是这样漏掉的：默认配置下未修复的实现也能让 50 帧全部出
    // 来）。调小到 20，两三秒内第二条轨的包数就会压过它，联合背压才有
    // 机会真正触发。
    PipelineConfig cfg;
    cfg.max_packets_per_track = 20;
    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, cfg, &err);
    REQUIRE(pipeline != nullptr);

    auto sink        = std::make_unique<syp::test::FakeAudioSink>();
    auto* sink_ptr    = sink.get();
    auto renderer     = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr = renderer.get();

    auto player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                       nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);

    for (int i = 0; i < 3000; ++i) {
        sink_ptr->advance(1000);   // 让 AudioClock 真的往前走，视频才有机会判定"到点"
        (void)player->step();
    }
    // 2 秒、25fps 应出约 50 帧；没有这条修复时实测只呈现 2 帧——
    // 30 是留了余量的下限，不是精确值断言。
    CHECK(renderer_ptr->shown().size() > 30);
}

// 封面图不能被绑成主视频轨。
//
// 早先就写过「封面图在 mp4 里极常见，真实文件上会
// 直接踩到」，但落地的回归用例只做了双音轨那半
// （second_audio_track_does_not_deadlock_pipeline），attached_pic 那半
// 零覆盖（有一个变异体存活）。这两条补的就是那一半。
//
// 素材构造的实测限制见 synth_audio_with_cover_art() 上方那段长注释：
// mov 复用器会把 attached_pic 轨挪到最后，"封面图流号更小"造不出来。
// 下面第一条用的是**音频 + 封面图**（封面图是唯一的 is_video 轨），跟
// 流号无关，修复前必然踩中——这条是真正有区分力的那条。

TEST_CASE(cover_art_is_not_bound_as_the_video_track) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_audio_with_cover_art(tmp.path);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);

    // 先确认素材真的是我们以为的样子——否则这条用例可能在一份"压根没有
    // 封面图轨"的素材上空绿。这一步同时也是 TrackInfo::attached_pic
    // 这条新管线（demuxer.cpp 读 st->disposition）自己的断言。
    int32_t cover_index      = -1;
    int32_t real_video_count = 0;
    for (const auto& t : pipeline->tracks()) {
        if (!t.is_video) continue;
        if (t.attached_pic) cover_index = t.index;
        else                ++real_video_count;
    }
    REQUIRE(cover_index >= 0);              // 封面图轨确实存在且被认出来了
    CHECK_EQ(real_video_count, int32_t{0}); // 且它是唯一的 is_video 轨

    auto  sink        = std::make_unique<syp::test::FakeAudioSink>();
    auto* sink_ptr    = sink.get();
    auto  renderer    = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr = renderer.get();
    auto  player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                        nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);

    // 核心断言：一条视频轨都没绑上。修复前这里是 cover_index。
    CHECK(player->video_track_index() < 0);
    CHECK(player->video_track_index() != cover_index);

    // 而且这是一份能正常播的音乐文件：时钟仍然是 Audio，音频照写不误，
    // 封面图那条轨进了 unbound_tracks_ 被排空，不会把管线卡死。
    CHECK(player->clock_kind() == ClockKind::Audio);
    bool queued = false;
    for (int i = 0; i < 300 && !queued; ++i) {
        sink_ptr->advance(1000);
        const PlayOutcome o = player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Queued) queued = true;
    }
    CHECK(queued);
    CHECK(renderer_ptr->shown().empty());   // 没有视频轨 → 一帧都不该呈现
}

// 第二条：真视频 + 音频 + 封面图。mov 复用器把封面图挪到了最后，所以修复
// 前这份素材**也**会绑对真视频轨——它测的不是"封面图抢跑"，而是"修复没有
// 误伤真视频轨、且封面图确实进了 unbound_tracks_ 被排空"。如实标注：这条
// 在修复前后都是绿的，它是护栏不是判据。
TEST_CASE(cover_art_alongside_real_video_does_not_displace_it) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio_with_cover_art(tmp.path);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);

    int32_t cover_index = -1;
    int32_t real_index  = -1;
    for (const auto& t : pipeline->tracks()) {
        if (!t.is_video) continue;
        if (t.attached_pic) cover_index = t.index;
        else if (real_index < 0) real_index = t.index;
    }
    REQUIRE(cover_index >= 0);
    REQUIRE(real_index >= 0);

    auto  sink        = std::make_unique<syp::test::FakeAudioSink>();
    auto* sink_ptr    = sink.get();
    auto  renderer    = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr = renderer.get();
    auto  player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                        nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);

    CHECK_EQ(player->video_track_index(), real_index);

    for (int i = 0; i < 2000; ++i) {
        sink_ptr->advance(1000);
        const PlayOutcome o = player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
    }
    // 1 秒、25fps；10 是留了余量的下限——真视频轨确实在出画，没有被当成
    // 不受管轨排空掉。
    CHECK(renderer_ptr->shown().size() > 10);
}

// 降级换 SystemClock 时必须带上当前 speed_，否则 2× 播放中
// sink 被抢占会悄悄跌回 1.0×——数值上看不出"崩"，只是速度不对。用真实
// sleep，不用零延迟（零延迟场景会巧合掩盖"忘记结算"
// 这类缺陷）。
TEST_CASE(degrade_preserves_playback_speed) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);
    REQUIRE(fx.player->set_speed(2.0) == SYP_OK);

    fx.sink->inject_failure();
    (void)fx.player->step();   // 触发降级
    REQUIRE(fx.player->clock_kind() == ClockKind::System);
    REQUIRE(fx.player->speed() == 2.0);   // speed() 本身如实报，不受降级影响

    const int64_t before = fx.player->position_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int64_t after = fx.player->position_us();
    const int64_t delta = after - before;
    // 2× 播放，100ms 墙钟应推进约 200ms；若降级丢了 speed_（新
    // SystemClock 按 1.0× 走），只会推进约 100ms。150ms 是分界线。
    CHECK(delta > 150000);
}

// 暂停时仍要驱动 Pipeline 预解码，恢复时才能立即出画。
//
// 不能用 make_fixture_with_video_pts()：那个 fixture 在包进 TrackPlayer
// 之前就已经用 drive_pipeline_best_effort() 把帧预先解码、摆进
// FrameQueue 了——用它测这条用例会给"暂停期间没驱动 Pipeline"这种
// 缺陷开一扇后门：反正需要的帧早就在队列里等着，暂停驱不驱动 Pipeline
// 根本不影响 play() 之后那一次 step() 的结果，第一版这样写时确实测不
// 出这条修复（自检环节才发现）。这里改成直接构造：素材现场生成，
// Pipeline 一次都没驱动过就直接扔进 TrackPlayer，让"把包解码成帧"这
// 件事只能发生在暂停期间的 step() 调用里。
TEST_CASE(pause_then_play_presents_on_first_step_after_priming) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_only(tmp.path, /*fps=*/25, /*frame_count=*/5);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);   // 这里之后一次 pipeline->step() 都没调过

    auto  clock         = std::make_unique<syp::test::FakeClock>();
    auto* clock_ptr      = clock.get();
    auto  renderer       = std::make_unique<syp::test::FakeRenderer>(clock_ptr);
    auto* renderer_ptr   = renderer.get();

    auto player = TrackPlayer::create(std::move(pipeline), nullptr, std::move(renderer),
                                       std::move(clock), &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);

    clock_ptr->set(0);
    player->pause();

    // 暂停期间多调几次 step()：这期间仍然要驱动 Pipeline，唯一
    // 能把这份素材的包解码成帧的机会就在这里——等恢复时应该已经有帧
    // 可呈现，不需要 play() 之后再等好几步。
    //
    // 此前断言"100 次全部 Waiting、shown() 为空"
    // ——那是更早的行为。现在暂停中一旦这份素材的首帧（pts=0）
    // 被解出来，就会顺带把它画出来一次（预览帧，不动时钟）；这条断言改
    // 成"恰好一次 Presented（首帧），其余 Waiting"，仍然覆盖"暂停期间
    // Pipeline 确实在解码"这条组合本来要测的东西。
    int presented_count = 0;
    for (int i = 0; i < 100; ++i) {
        const PlayOutcome o = player->step();
        if (o.kind == PlayOutcome::Kind::Presented) {
            ++presented_count;
            CHECK_EQ(o.pts_us, int64_t{0});
        } else {
            CHECK(o.kind == PlayOutcome::Kind::Waiting);
        }
    }
    CHECK_EQ(presented_count, 1);
    REQUIRE(renderer_ptr->shown().size() == 1);
    CHECK_EQ(renderer_ptr->shown()[0].pts_us, int64_t{0});

    player->play();
    const PlayOutcome o = player->step();
    // 首帧已经在暂停中被预览帧画过了，这一次出的是第二帧（pts=40000）；
    // clock_ptr 从未拨动过（仍是 0），diff=40000 不大于 kPresentWindowUs
    // （严格 >，40000 不满足），落进"其余一律呈现"分支——一次 step() 就
    // 出画，不用再等，这正是本来要守住的行为。
    CHECK(o.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(o.pts_us, int64_t{40000});
}

// sink 容量小到几乎立刻写满时，视频不该被拖着一起停摆——
// 音频、视频有各自独立的队列和时钟判据。
//
// 不能用 FakeAudioSink（见 RealTimeStubSink 类声明处那段注释）：它的
// played_us() 靠 advance() 从 written_frames_ 里扣，天然把"时钟前进"
// 和"write() 成功"耦合在一起——两次成功写入之间必然相隔恰好一帧 AAC
// 的样本数（~23ms），不管怎么调 capacity/advance() 的比例都做不出比
// kDropThresholdUs（80ms）更长的单次卡顿，第一版、第二版都在这上面
// 撞过（自检时发现两版都测不出这条修复，实际值见对应版本的提交历史）。
// 换成 RealTimeStubSink：write() 永远失败、played_us() 走真实墙钟，
// 两者彻底脱钩，才是真实设备会有的样子。
TEST_CASE(audio_backpressure_does_not_starve_video) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio(tmp.path);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);

    auto  sink     = std::make_unique<RealTimeStubSink>();
    auto* sink_ptr = sink.get();
    sink_ptr->set_accept_writes(false);   // 音频永远写不进去；真实设备时钟照常走

    auto  renderer      = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr  = renderer.get();

    auto player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                       nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);
    REQUIRE(player->clock_kind() == ClockKind::Audio);

    // 不能像别的用例那样每次 step() 之间 sleep 一小段真实时间：
    // RealTimeStubSink 的时钟跟着真墙钟走、不受 step() 调用节奏影响，
    // 但 Pipeline 解码一帧往往要好几次 pipeline_->step() 才够（demux
    // 一次、receive 一次甚至更多）——按"每次 step() 之间睡 20ms"这个
    // 节奏调用，留给 Pipeline 解码的"步数预算"跟媒体时长刚好卡得死死
    // 的，稍微不够用画面就会一直追不上真墙钟，帧因此被判成"太迟"而
    // 丢弃，而不是这条用例真正想测的"音频背压拖累视频"（这版本第一次
    // 就撞上了：45 次 Blocked、10 次 Dropped、0 次 Presented——不是
    // 不是缺陷复现，是这个用例自己的节奏跟不上）。改成不睡
    // 眠、尽快把 step() 调够多次（CPU 解码远快于 25fps 实时播放速度），
    // 只用真墙钟决定什么时候停——这样 Pipeline 解码能远远跑在媒体时钟
    // 前面，同上面其它用例的"真实 sleep"要求不冲突：这里要测的不是
    // "时间怎么流逝"，是"audio_blocked 时视频判定还能不能正常进行"。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    int        iterations = 0;
    while (std::chrono::steady_clock::now() < deadline && iterations < 50'000'000) {
        const PlayOutcome o = player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        ++iterations;
    }
    // 1 秒、25fps（约 25 帧）的素材应该已经放完，即使音频从未真正写
    // 进去过一次。10 是留了余量的下限。
    CHECK(renderer_ptr->shown().size() > 10);
}

// seek() 必须清空 pending_audio_，不能让 seek 之前卡住的
// 那帧音频在 seek 之后被当成"新位置的第一帧"写出去。
TEST_CASE(seek_clears_stale_pending_audio_frame) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);
    fx.sink->set_capacity_frames(0);   // write() 永远失败——音频帧会卡在 pending_audio_

    // 多调几次：前几次只是驱动 Pipeline 把第一帧音频解出来，之后才会
    // 真的 pop 到它、write() 失败、卡进 pending_audio_。
    for (int i = 0; i < 50; ++i) (void)fx.player->step();

    fx.sink->set_capacity_frames(48000);   // 恢复容量
    const syp_status rc = fx.player->seek(500000);
    CHECK(rc == SYP_OK);

    int64_t first_queued_pts = -1;
    for (int i = 0; i < 2000 && first_queued_pts < 0; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Queued) first_queued_pts = o.pts_us;
    }
    REQUIRE(first_queued_pts >= 0);
    // 若 seek() 没清 pending_audio_，这里会是 seek 之前卡住的那第一帧
    // （pts 接近 0），而不是 seek(500000) 之后的新位置。
    CHECK(first_queued_pts > 100000);
}

// sink 提供了、但 open() 失败——退回 SystemClock，音频帧
// 被丢弃（不是永久卡住），管线继续推进。
// 不能用 make_fixture_with_audio_and_video()（默认 PipelineConfig）：
// FakeAudioSink::write() 本身在 !opened_ 时就会返回 false（不需要
// sink_opened_ 那道判据也一样写不进去），加上前面的修复（写
// 不进去也不让视频停摆）已经让"这一帧到底有没有被 pop-and-discard 掉"
// 在短素材、宽松队列容量下看不出行为差异——第一版在这上面撞过（自检
// 时发现即使去掉 sink_opened_ 判据，用例照样绿）。真正的区分点在于：
// 没有 sink_opened_ 时，pending_audio_ 会永远卡住同一帧、永远不重新
// pop（`if (!pending_audio_.has_value())` 一直为假），Pipeline 音频轨
// 自己的 FrameQueue/PacketQueue 因此会一直堆积、最终撞上联合背压——
// 跟前面同一个机制，只是这次堵住的是"主"音频轨而不是未绑定的
// 轨。默认容量（128 包/8 帧）下这份 1 秒素材撞不到；调小容量（跟
// second_audio_track_does_not_deadlock_pipeline 同一招）才能让它在
// 合理的迭代数内真正触发。
TEST_CASE(sink_provided_but_open_fails_falls_back_and_keeps_playing) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio(tmp.path);
    REQUIRE(!path.empty());

    PipelineConfig cfg;
    cfg.max_packets_per_track = 10;
    cfg.max_frames_per_track  = 4;
    syp_status err            = SYP_OK;
    auto       pipeline       = Pipeline::create_file(path, cfg, &err);
    REQUIRE(pipeline != nullptr);

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->fail_next_open();
    auto  renderer      = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr  = renderer.get();

    auto player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                       nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);
    CHECK(player->clock_kind() == ClockKind::System);

    // clock_kind()==System 意味着这条用例的主时钟是真墙钟（SystemClock
    // 没有像 FakeClock 那样的手动拨表接口）——固定次数的紧凑循环在几
    // 毫秒内就跑完，真实时间根本来不及推进到"1 秒素材放完"，会跟
    // 前面那条用例第一版撞上的问题一模一样（断言失败但原因是
    // 用例本身节奏不对，不是被测代码的缺陷）。改成"按真实墙钟时间跑到
    // 截止点"，配一个很大的迭代次数上限兜底，不靠 ctest TIMEOUT 撞
    // 看门狗。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    int        iterations = 0;
    while (std::chrono::steady_clock::now() < deadline && iterations < 50'000'000) {
        const PlayOutcome o = player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        REQUIRE(o.kind != PlayOutcome::Kind::Queued);   // sink 没打开，不该有任何 Queued
        ++iterations;
    }
    // 1 秒、25fps 应出约 25 帧；10 是留了余量的下限。
    CHECK(renderer_ptr->shown().size() > 10);
}

// 降级基准必须来自 last_known_position_us_，不能是"降级
// 那一刻"直接读一次已经 failed() 的 AudioClock（那会读到
// AV_NOPTS_VALUE = INT64_MIN）。故意留出有意义的时间间隔——零延迟场景
// （构造完立刻 inject_failure()）会让 before_fail 巧合地停在 0 附近，
// 掩盖"基准来源"这类缺陷。
TEST_CASE(degrade_uses_last_known_position_not_nopts) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    bool wrote_audio = false;
    for (int i = 0; i < 50 && !wrote_audio; ++i) {
        const PlayOutcome o = fx.player->step();
        if (o.kind == PlayOutcome::Kind::Queued) wrote_audio = true;
    }
    REQUIRE(wrote_audio);
    fx.sink->advance(300000);   // 模拟设备已经消费了 300ms

    const int64_t before_fail = fx.player->position_us();
    REQUIRE(before_fail > 0);   // 确认真的推进过，不是巧合的 0

    // last_known_position_us_ 只在 step() 内部更新（见 track_player.cpp
    // step() 开头那段），直接调 position_us() 不会触发这次记账——这里
    // 必须先用一次"sink 还健康"的 step() 把 300ms 这个读数真的记进去，
    // 再失败，否则 TrackPlayer 从未观察过这个值，接下来的断言测的就不
    // 是它想测的那件事。
    (void)fx.player->step();

    fx.sink->inject_failure();
    (void)fx.player->step();   // 触发降级
    REQUIRE(fx.player->clock_kind() == ClockKind::System);

    const int64_t after_degrade = fx.player->position_us();
    CHECK(after_degrade > 0);   // 不是 AV_NOPTS_VALUE（INT64_MIN）那种量级
    const int64_t delta =
        after_degrade > before_fail ? after_degrade - before_fail : before_fail - after_degrade;
    CHECK(delta < 50000);   // 应该接近降级前的读数，容忍几十毫秒
}

// set_speed() 的范围校验（含 NaN），以及它确实把新比例
// 转发给了 sink（不是只更新 TrackPlayer 自己的镜像）。
TEST_CASE(set_speed_validates_range_including_nan) {
    auto fx = make_fixture_with_video_pts({0});
    CHECK(fx.player->set_speed(0.5) == SYP_OK);
    CHECK(fx.player->set_speed(2.0) == SYP_OK);
    CHECK(fx.player->set_speed(0.49) == SYP_ERR_INVALID_ARG);
    CHECK(fx.player->set_speed(2.01) == SYP_ERR_INVALID_ARG);
    CHECK(fx.player->set_speed(std::numeric_limits<double>::quiet_NaN()) == SYP_ERR_INVALID_ARG);
}

TEST_CASE(set_speed_flushes_sink_and_forwards_new_ratio) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    const int32_t flush_before = fx.sink->flush_count();
    REQUIRE(fx.player->set_speed(2.0) == SYP_OK);
    CHECK(fx.sink->flush_count() == flush_before + 1);   // flush(base) 确实被调了一次
    CHECK(fx.player->speed() == 2.0);

    // sink_->set_speed() 确实被转发了——若 TrackPlayer 忘了这一步，
    // FakeAudioSink 内部 speed_ 还停在 1.0，played_us() 的换算比例就是
    // 错的，下面这个断言会用错误的 1× 读数把自己拆穿。
    bool wrote = false;
    for (int i = 0; i < 50 && !wrote; ++i) {
        const PlayOutcome o = fx.player->step();
        if (o.kind == PlayOutcome::Kind::Queued) wrote = true;
    }
    REQUIRE(wrote);
    const int64_t base = fx.player->position_us();
    fx.sink->advance(20000);   // 20ms，明显小于一帧 AAC（~1024 样本≈23ms）已写入量
    const int64_t after = fx.player->position_us();
    // 2× 时约推进 40ms；若 sink 没收到新比例（停在 1×），只推进约 20ms。
    CHECK(after - base > 30000);
}

// flush(base) → set_speed(speed) 的**顺序**此前是一条零执行力
// 的契约：audio_sink.h 的三条约定、track_player.h:217-227、
// track_player.cpp 的 set_speed() 四处都写了它，但两个实现都让它当前不可
// 观测（flush() 不碰 speed，set_speed() 只记账），把两行对调的变异全绿
// 存活。今天不是缺陷——等真做无缝变速（flush() 要按当前比例重建
// 重采样器状态）它就承重，那时顺序写反是静默的比例错误。
//
// FakeAudioSink 为此记了一条极小的操作序号账（last_flush_seq()/
// last_set_speed_seq()，见 fake_audio_sink.h）。断言不是"两者都被调过"
// （flush_count() 已经覆盖了那个），而是"set_speed 的序号紧跟在 flush
// 的序号之后"——把顺序本身变成可断言的。
TEST_CASE(set_speed_calls_sink_flush_before_sink_set_speed) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    REQUIRE(fx.player->set_speed(1.5) == SYP_OK);
    const int64_t flush_seq     = fx.sink->last_flush_seq();
    const int64_t set_speed_seq = fx.sink->last_set_speed_seq();
    REQUIRE(flush_seq > 0);        // 两者都真的发生过，不是"都没发生所以恒等"
    REQUIRE(set_speed_seq > 0);
    // 紧邻，不只是"更大"：中间插进任何别的 sink 操作都说明顺序被改动过。
    CHECK(set_speed_seq == flush_seq + 1);
}

// 音频轨自身解码失败（Pipeline::track_failed()）也要触发
// 降级，不能只看 sink_->failed()。用一份音频轨编码成 flac 的素材
// （本项目 FFmpeg 只 --enable-decoder=h264/aac，跟
// test_decode_e2e.cpp 场景 G 同一手法）：avcodec_find_decoder() 对
// flac 必然返回 nullptr，音频轨的解码器初始化失败，track_failed()
// 从 Pipeline 创建那一刻起就恒为真——但 sink 自己从没调过
// inject_failure()，sink_->failed() 恒为假。
TEST_CASE(audio_track_decode_failure_triggers_degrade) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string  path = tmp.path + "/i7_flac_audio.mp4";
    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-f lavfi -i \"testsrc2=size=64x64:rate=25:duration=1\" "
        << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=1\" "
        << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p "
        << "-c:a flac "
        << "\"" << path << "\" > /dev/null 2>&1";
    REQUIRE(std::system(cmd.str().c_str()) == 0);

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);

    auto  sink          = std::make_unique<syp::test::FakeAudioSink>();
    auto  renderer      = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr  = renderer.get();
    auto  player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                        nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);
    REQUIRE(player->clock_kind() == ClockKind::Audio);   // 打开时 sink 本身没问题

    bool became_system = false;
    for (int i = 0; i < 60 && !became_system; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const PlayOutcome o = player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (player->clock_kind() == ClockKind::System) became_system = true;
    }
    // 没有这条修复：clock_kind() 会一直停在 Audio（sink_->failed() 从
    // 未变真），环放空后 AudioClock 的读数停止推进，所有视频帧永远
    // 判成"早了"，画面静止、无错误、无降级——静默且稳定地错。
    CHECK(became_system);

    bool presented = false;
    for (int i = 0; i < 60 && !presented; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const PlayOutcome o = player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Presented) presented = true;
    }
    CHECK(presented);
    CHECK(!renderer_ptr->shown().empty());
}

// =======================================================================
// 暂停 / 倍速 / seek 的契约回归。
//
// 原计划的七条用例里，四条已经在早期修复落地时顺带
// 被覆盖——逐条核对断言强度（不接受"名字像就算覆盖"）：
//
//   pause_freezes_clock_and_presents_nothing
//     → pause_then_repeated_step_stays_waiting_and_frozen（上面"组合
//       1"）。断言持平：都验"暂停期间反复 step() 恒 Waiting、零呈现"。
//
//   set_speed_rejects_out_of_range
//     → set_speed_validates_range_including_nan（下面这条用例）。
//       比原计划更深：原计划只测 0.4/2.1 两个越界值，这条用 0.49/2.01
//       卡在边界正外侧、外加 NaN，边界更紧。
//
//   set_speed_flushes_sink_and_rebaselines
//     → set_speed_flushes_sink_and_forwards_new_ratio（下面"Important
//       6"）。比原计划更深：原计划只验 flush_count()+1，这条还多验了
//       sink_->set_speed() 确实把新比例转发下去了（用 played_us() 的
//       换算比例把"只更新了 TrackPlayer 自己的镜像"这类缺陷拆穿）。
//
//   first_frame_after_seek_is_presented_immediately
//     → pause_then_seek_then_play_presents_first_frame_immediately
//       （上面"组合 2"）。**这一条覆盖得比原计划浅**：原计划拿
//       FakeRenderer::at_us 验"呈现时刻的时钟读数应接近帧的真实 pts"
//       （"随后把时钟基准设为该帧的实际 pts"那半句契约）；
//       组合 2 用的是 make_fixture_with_video_pts（clock_override 场
//       景），track_player.h seek()/just_sought_ 声明处注释写得很清楚：
//       clock_override 场景下 TrackPlayer 没有接口复位外部时钟，这半句
//       契约在那个 fixture 上根本没有可观测后果——组合 2 只验了"呈现的
//       是 Presented 而不是 Dropped、pts 是不是请求值"，没有、也不可能
//       验"基准有没有被真实 pts 纠偏"这半句。下面 seek_runs_all_four_
//       steps 用真实 AudioClock（这半句唯一能被观测到的场景）补上：
//       呈现那一刻 position_us() 应精确等于呈现帧的真实 pts。
//
// 剩下三条（resume_does_not_jump_position、seek_runs_all_four_steps、
// seek_failure_still_flushes_and_resets）此前零覆盖，本节补上，用
// make_fixture_with_audio_and_video()/make_fixture_that_fails_seek() 走
// 生产路径选出的真实 AudioClock，不用 clock_override——原因同上一段。
//
// 关于 dropped_frames()：seek_runs_all_four_steps / seek_failure_still_
// flushes_and_resets 都没有用字面断言
// `CHECK_EQ(fx.player->dropped_frames(), int64_t{0})`。这条断言其实
// 是个假判据——track_player.cpp seek() 实现末尾、
// track_player.h seek() 声明处注释都明确写着 dropped_frames_/
// present_failures_ 是生命周期累计值，seek() **不**复位它们（跟
// Pipeline::seek() 对 skipped_packets()/track_failed() 的立场一致）。
// 本文件两份 fixture
// 里 dropped_frames_ 也从未被递增过，删不删这条断言结果都是 0——照抄
// 会是一条测不出任何缺陷的假回归保护。下面两条改成直接验"四步"实际
// 内容：pipeline_->seek() 的 rc 如实透传、sink_->flush() 计数 +1、
// 时钟基准复位（position_us() 精确匹配）、pending_audio_/pending_video_
// 清空（seek 前卡住的旧帧此后不再以任何 Kind 出现）。
// =======================================================================

// 组合 5：暂停期间流逝的时间不得算进播放位置——
// "IAudioSink::pause()"那半句契约唯一可观测的地方。
// 「SystemClock 状态机组合覆盖度」点名的陷阱：零延迟场景会巧合掩盖
// "忘记结算"这类缺陷（早先那条 SystemClock::pause() 用例就是构造后
// 立刻 pause、base_us_ 本就是 0，结算是空操作）。这里先用真实
// FakeAudioSink::advance() 把 position_us() 推到一个非零基线，再在暂停
// 期间试图推进，确认这次不算数——advance() 不能一次调用就直接拿到
// 200000/500000 对应的 us：它按"已写入量"设上限（have = written_frames_
// - consumed_frames_），而 TrackPlayer 每个 step() 只弹一帧音频就
// return，写入量不会一口气攒很多，需要多次 step() 才能攒够；但也不能
// 一次性 step() 几千次再 advance()：视频判定用的是同
// 一个 AudioClock，只要时钟长时间冻结在 0，第 3 帧视频（pts=80000）很快
// 就会因为"早了"卡进 pending_video_ 并直接 return——一旦卡住，step()
// 每次都在同一帧上原地打转，再也不会去驱动 Pipeline::step()（那是"都
// 不行才驱动"的兜底分支，视频卡在 Waiting 分支本身就已经 return 了），
// 音频自然也不会再有新帧可 pop（实测过：step 次数从 50 加到 3000，
// position 死死钉在同一个值不动）。改成边推进边小步 advance()，时钟跟
// 着走、视频不会卡死，Pipeline 才有机会不断解出新内容。
TEST_CASE(resume_does_not_jump_position) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);
    for (int i = 0; i < 400; ++i) {
        (void)fx.player->step();
        if (i % 4 == 0) fx.sink->advance(1000);
    }
    const int64_t before = fx.player->position_us();
    REQUIRE(before > 50000);   // 确认真的推进过，不是巧合的 0

    fx.player->pause();
    REQUIRE(fx.player->paused());
    fx.sink->advance(500000);   // 暂停期间"过了" 0.5 秒——不该算数
    // 暂停中查询也不该看见这 500ms：pause() 必须真的让底层 sink 冻结
    // （FakeAudioSink::advance() 在 paused_ 为真时是空操作），不是只在
    // play() 那一刻才纠正。
    CHECK_EQ(fx.player->position_us(), before);

    fx.player->play();
    REQUIRE(!fx.player->paused());
    const int64_t after = fx.player->position_us();
    CHECK(after - before < 50000);
}

// 组合 6：seek() 的四步（pipeline_->seek() / sink_->flush() / 时钟基准
// 复位 / 清空 pending_audio_、pending_video_）当一个整体验一次——现有的
// seek_discards_pending_video_frame、seek_clears_stale_pending_audio_
// frame 都只零散验过"清空 pending_*"这一步，没有一条把四步当整体断言
// 过。
//
// 素材构造：先用 sink 容量归零（write() 永远失败）卡住第一帧音频，再
// 靠时钟冻结在 0（capacity 归零本身不影响 AudioClock——它只读
// consumed_frames_，没人调 advance() 就不会动）让第 3 帧视频
// （pts=80000，diff=80000 > kPresentWindowUs）卡进 pending_video_——跟
// resume_does_not_jump_position 顶部注释是同一个机制，这里正需要它来
// 制造一个"旧帧"。
TEST_CASE(seek_runs_all_four_steps) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    fx.sink->set_capacity_frames(0);
    int64_t stale_video_pts = -1;
    bool    video_stuck     = false;
    for (int i = 0; i < 100 && !video_stuck; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Waiting && o.pts_us > syp::media::kPresentWindowUs) {
            video_stuck     = true;
            stale_video_pts = o.pts_us;
        }
    }
    REQUIRE(video_stuck);   // 确认真的卡住了一帧旧视频（实测 pts=80000）

    const int32_t    flushes_before = fx.sink->flush_count();
    const syp_status rc             = fx.player->seek(700000);
    REQUIRE(rc == SYP_OK);                                     // 第 1 步成功
    CHECK_EQ(fx.sink->flush_count(), flushes_before + 1);       // 第 2 步
    // 第 3 步：AudioClock 场景下时钟基准靠 flush(ts_us) 间接复位（见
    // track_player.cpp seek() 里的注释）——position_us() 应精确读出
    // 刚传入的请求值，一次 step() 都还没跑，不受任何解码/呈现影响。
    CHECK_EQ(fx.player->position_us(), int64_t{700000});

    fx.sink->set_capacity_frames(48000);   // 恢复容量，才能验第 4 步
    bool    saw_stale_video = false;
    bool    presented       = false;
    int64_t presented_pts   = -1;
    for (int i = 0; i < 500 && !presented; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        // 若第 4 步被跳过，seek 前卡住的那帧（pts=stale_video_pts）会在
        // 某次 step() 里以 Waiting/Dropped/Presented 中的某一种 Kind
        // 重新出现——三种都要挡，具体是哪一种取决于新基准（700000）跟
        // 这帧旧 pts（80000）的相对关系（本用例里差远了，实测会一直
        // Waiting，但不同参数组合下可能是别的 Kind，不该只挡一种）。
        if (o.pts_us == stale_video_pts &&
            (o.kind == PlayOutcome::Kind::Presented || o.kind == PlayOutcome::Kind::Dropped ||
             o.kind == PlayOutcome::Kind::Waiting)) {
            saw_stale_video = true;
        }
        if (o.kind == PlayOutcome::Kind::Presented) {
            presented     = true;
            presented_pts = o.pts_us;
        }
    }
    CHECK(!saw_stale_video);
    CHECK(presented);
    // just_sought_ 让 seek 后第一帧不等时钟、立即呈现——它的 pts 应当是
    // seek(700000) 落点最近关键帧的真实 pts（-g 5 @ 25fps，关键帧每
    // 200ms 一个：0/200000/400000/600000/800000，实测精确落在
    // 600000），远高于旧的 stale_video_pts（80000），且远低于原始请求
    // 值本身——300000 是留了余量的下限，不是精确值断言。
    CHECK(presented_pts > 300000);
    // 呈现之后 position_us() 应精确等于这帧的真实 pts（
    // "随后把时钟基准设为该帧的实际 pts"）——这正是上面那节
    // 顶部长注释里点名"组合 2 测不出"的那半句契约，这里用真实
    // AudioClock 补上。
    CHECK_EQ(fx.player->position_us(), presented_pts);
}

// Pipeline::seek() 上踩过完全相同的坑：失败时提前 return，队列
// 清了但 EOF 标志没复位，管线停在既不 Eof 也推不动的半吊子态。那条契约
// 立了之后**零回归保护地放了很久**，直到复查才被发现——这里
// 一开始就配。
//
// 素材构造跟 make_fixture_that_fails_seek() 顶部注释、以及
// tests/test_pipeline.cpp 里 pipeline_failed_seek_still_flushes_
// decoders 上方那份已验证过的手法逐字同源：只改 stss（entry_count=1，
// 唯一记录指向不存在的样本号 0x0FFFFFFF）不够——普通文件 IO 上
// seek_frame_generic() 的线性扫描兜底会把 av_seek_frame() 救回来，必须
// 换一个自己完全掌控的 AVIOContext，只在这一次 seek() 调用期间让它的
// seek 回调报 EIO，才能真正打断整个 av_seek_frame()。
//
// 优先级、旧帧构造手法跟 seek_runs_all_four_steps 相同（capacity 归零
// 卡住第一帧音频、时钟冻结让第 3 帧视频卡住）。
TEST_CASE(seek_failure_still_flushes_and_resets) {
    auto fx = make_fixture_that_fails_seek();
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    fx.sink->set_capacity_frames(0);
    int64_t stale_video_pts = -1;
    bool    video_stuck     = false;
    for (int i = 0; i < 100 && !video_stuck; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Waiting && o.pts_us > syp::media::kPresentWindowUs) {
            video_stuck     = true;
            stale_video_pts = o.pts_us;
        }
    }
    REQUIRE(video_stuck);

    const int32_t flushes_before = fx.sink->flush_count();
    fx.material->src.fail_seek   = true;
    const syp_status rc          = fx.player->seek(700000);
    fx.material->src.fail_seek   = false;
    std::printf("  [seek_failure_still_flushes_and_resets] rc=%d (SYP_OK=%d)\n",
                static_cast<int>(rc), static_cast<int>(SYP_OK));
    CHECK(rc != SYP_OK);                                       // 第 1 步失败，如实透传
    CHECK_EQ(fx.sink->flush_count(), flushes_before + 1);       // 但第 2 步照跑

    // 注：不验第 3 步的 position_us() 精确值——pipeline_->seek() 失败时
    // Demuxer 的读位置压根没有移动（探针实测：src.pos 不变，音频/视频
    // 仍从失败前的旧位置继续吐），time_us 基准虽然照样被 flush(700000)
    // 复位（第 2 步的直接后果），但"复位到了 700000"这件事已经被上面
    // flush_count 的断言间接验过，这里不重复。

    fx.sink->set_capacity_frames(48000);   // 恢复容量，才能验第 4 步
    bool saw_stale_video = false;
    bool made_progress   = false;
    for (int i = 0; i < 500; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);   // 播放器没有停在半吊子态
        if (o.pts_us == stale_video_pts &&
            (o.kind == PlayOutcome::Kind::Presented || o.kind == PlayOutcome::Kind::Dropped ||
             o.kind == PlayOutcome::Kind::Waiting)) {
            saw_stale_video = true;
        }
        if (o.kind == PlayOutcome::Kind::Presented || o.kind == PlayOutcome::Kind::Queued) {
            made_progress = true;
        }
    }
    // 第 4 步：seek 之前卡住的那帧视频（stale_video_pts）此后不该再以
    // 任何 Kind 出现——pending_video_ 已被清空，它已经被 Pipeline
    // pop 出来过一次，不会再被重新读到。
    //
    // pending_audio_ 的清空没有单独在这里配一条同款直接证据：
    // IAudioSink::write() 失败不上报被拒帧的 pts，拿不到"旧音频帧的
    // 确切 pts"去做同款比对；但 seek() 里清空 pending_audio_/
    // pending_video_ 是相邻两行无条件语句（见 track_player.cpp
    // seek()），没有分支能只跳过其中一个——上面 saw_stale_video 的反向
    // 自检（删掉这两行）证实了这一点：那次自检下 saw_stale_video 由
    // false 变 true，同时 flush_count 也不再 +1，两个判据一起翻红，
    // 不存在"只漏一个"的中间状态。pending_audio_ 单独的、直接的反向
    // 保护由 seek_clears_stale_pending_audio_frame（成功
    // seek 路径）承担。
    CHECK(!saw_stale_video);
    // 失败之后播放器仍能继续推进——不是终止态、不是永久 Blocked。
    CHECK(made_progress);
}

// =======================================================================
// 回归：「音频 FrameQueue 空 ∧ 视频
// 早到」不能是 step() 的永久空操作。
// =======================================================================
//
// 构造手法（三段）：
//
//   1. 暂停预热：只驱动 Pipeline、不做音频/视频判定（track_player.cpp
//      第 4 步），把两条 FrameQueue 都灌到 wide 配置（64 深）附近——跟
//      test_sync_e2e.cpp 场景 D 顶部长注释同一个手法。
//   2. 恢复播放后，音频优先分支每次 step() 只处理一帧、成功就立即
//      return Queued（不额外驱动 Pipeline）——这不是测试强加的驱动方
//      式，是 track_player.cpp 第 1 步本身的行为。连续喂它 step()，
//      直到某次返回的是"视频早到"那种 Waiting（用 track_index>=0 &&
//      pts_us>kPresentWindowUs 识别，跟 test_sync_e2e.cpp 场景 F 同一
//      个判据）——命中的这一刻，Pipeline 的音频 FrameQueue 必然已经被
//      上面这串 Queued 真正掏空（sink 写入容量已调到 1000 万帧，见
//      make_wide_fixture_with_audio_and_video() 顶部注释，写入这一步
//      不会先背压），不是靠猜。
//   3. 直接调 sink->flush(0)（不经 TrackPlayer::set_speed()/seek()）：
//      把这一串 Queued 已经写进 sink、但还没被"消费"的那部分存量原地
//      清零，时钟基准不变（还是 0，因为直到这一刻测试代码从未调过
//      sink->advance()）。这精确复刻已知缺陷描述的触发条件——
//      set_speed()/seek() 都会 flush() 掉环，若这一刻 Pipeline 的音频
//      FrameQueue 恰好接近排空——只是用确定性调用代替真实时序竞态去
//      命中同一个状态，不依赖解码速度这类非确定性因素。直接调 sink
//      的 flush() 而不经 TrackPlayer 的 set_speed()/seek()，是因为
//      TrackPlayer 侧没有任何額外状态需要保持一致——flush() 影响的只是
//      sink 内部的 written_frames_/consumed_frames_/base_us_，跟
//      TrackPlayer 自己的 pending_audio_/pending_video_/clock_kind_
//      无关，直接调更精确地只改变我们想改变的那一个变量。
//
// 到这一步，Pipeline 音频 FrameQueue 空、sink 零存量、视频早到、
// pending_video_ 卡着那帧——如果 step() 在这个组合下是空操作，接下来
// 无论调用多少次都不会有任何进展：sink 没有可消费的存量（advance() 是
// 空操作，见 fake_audio_sink.cpp：`consumed_frames_ += min(want, have)`，
// have==0 时恒不动），Pipeline 的音频 FrameQueue 也没人去驱动补，视频
// 早到分支又不呈现——三方都在等对方先动，没有任何外部输入能打破。
//
// 上限：连续 Waiting 撞到 kMaxConsecutiveWaiting 就 REQUIRE 失败——不
// 靠 ctest TIMEOUT 撞看门狗（本仓库既有纪律：早先踩过"日志里看不出是哪
// 条用例卡住"这个坑）。最终要求真的推进到 Eof，不是"跑到 Blocked/
// Waiting 就算数"。
TEST_CASE(step_does_not_livelock_when_audio_queue_empty_and_video_early) {
    auto fx = make_wide_fixture_with_audio_and_video();
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    // 第 1 段：暂停预热。
    fx.player->pause();
    for (int i = 0; i < 1500; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
    }
    fx.player->play();

    // 第 2 段：驱动到"视频早到"那种 Waiting 第一次出现为止。
    bool    hit_video_early = false;
    int64_t early_pts       = AV_NOPTS_VALUE;
    for (int32_t i = 0; i < 10000 && !hit_video_early; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        REQUIRE(o.kind != PlayOutcome::Kind::Eof);   // 3 秒素材，不该这么快到尾
        if (o.kind == PlayOutcome::Kind::Waiting && o.track_index >= 0 &&
            o.pts_us != AV_NOPTS_VALUE && o.pts_us > syp::media::kPresentWindowUs) {
            hit_video_early = true;
            early_pts       = o.pts_us;
        }
    }
    REQUIRE(hit_video_early);
    REQUIRE(early_pts > syp::media::kPresentWindowUs);
    REQUIRE(fx.player->position_us() == 0);   // 时钟这一刻确实还没被推进过

    // 第 3 段：原地清空 sink 的未消费存量，时钟基准不变——见上方长注释。
    fx.sink->flush(0);
    REQUIRE(fx.sink->consumed_frames() == fx.sink->written_frames());

    // 第 4 段：数连续 Waiting，命中上限即失败；最终要求真的到 Eof。
    constexpr int32_t kMaxConsecutiveWaiting = 2000;
    constexpr int32_t kMaxTotalSteps         = 300000;
    int32_t consecutive_waiting              = 0;
    bool    reached_eof                      = false;
    for (int32_t k = 0; k < kMaxTotalSteps; ++k) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Eof) {
            reached_eof = true;
            break;
        }
        if (o.kind == PlayOutcome::Kind::Waiting) {
            ++consecutive_waiting;
            REQUIRE(consecutive_waiting < kMaxConsecutiveWaiting);
            fx.sink->advance(5000);
        } else {
            consecutive_waiting = 0;
        }
    }
    REQUIRE(reached_eof);
}

// =======================================================================
// audio_blocked 必须排在 Pipeline 自己的 Kind::Eof
// 前面——零覆盖，有一个变异体（只把 Eof 插到 audio_blocked 前、
// video_waiting_early 不动）27 条单测 + 九条 E2E 全绿地存活过。
//
// 构造难点：TrackPlayer 的既有设计是"pending_audio_ 一旦卡住（write()
// 失败）就只反复重试同一帧，绝不再从 Pipeline 的 FrameQueue 里多 pop
// 一帧"（这是正确行为，不是缺陷——见头文件 pending_audio_/pending_
// video_ 一节）。这意味着只要音频轨真实帧数 > 1，Pipeline 自己的音频
// FrameQueue 就会永远留着"pending_audio_ 之外的那些帧"没被取走，
// Pipeline 的 Eof 判据（该轨 frame 队列也要排空）就永远无法满足——
// "Pipeline 真报 Eof 时 audio_blocked 还为真"这个组合，靠 TrackPlayer
// 自然驱动是构造不出来的（AAC 编码器即使喂极短输入也至少产 2 帧，
// 实测验证过，不是猜的）。
//
// 解法：绕开 TrackPlayer，直接摆弄 Pipeline（`make_fixture_that_fails_
// seek()`、test_sync_e2e.cpp 场景 F 的 `prime_pipeline` 都是这个仓库
// 里同一手法的先例）——先用一个抛弃的 Pipeline 实例把音频轨真实解码
// 帧数数出来（`audio_total`，不能假设等于 ffprobe 的 nb_frames 估计
// 值，反向自检踩过这个坑），再在真正要用的 Pipeline 上：
// FrameQueue 深度给到 100000（整个素材的总帧数远小于这个数，解码全程
// 不会被背压打断，可以放心地把两条轨都驱动到解码器真正的尽头），驱动
// 到底之后，直接从 Pipeline 里 pop 掉 `audio_total - 1` 帧音频（只留
// 最后 1 帧不取，唯一没被取走的这一帧后面会被 TrackPlayer 自己
// pop 到并写失败）、pop 光全部视频（video_waiting_early 必须是假，
// 否则它会先一步把 audio_blocked 盖住，见 I1/坑 1 那条优先级）。
// 这样构造出来的 Pipeline，交给 TrackPlayer::create() 时已经满足：
// 音频轨只剩 1 帧待取、视频轨空、且两条轨解码器都已经到底——
// TrackPlayer 的第一次 step() 会 pop 到那 1 帧、write() 失败
// （sink 容量归零）、video 分支发现视频轨真的空了，落到第 3 步驱动
// Pipeline：这一下 Pipeline 自己的 FrameQueue/PacketQueue 都排空了，
// 如实报 Eof——正是要测的那个组合。
TEST_CASE(audio_blocked_outranks_pipeline_eof) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio(tmp.path);
    REQUIRE(!path.empty());

    PipelineConfig hugecfg;
    hugecfg.max_frames_per_track = 100000;   // 整个素材的总帧数远小于这个数，见上方长注释

    // 第 1 步：用一个抛弃的 Pipeline 实例数出音频轨真实解码帧数。
    int64_t    audio_total = 0;
    syp_status err         = SYP_OK;
    {
        auto probe_pipeline = Pipeline::create_file(path, hugecfg, &err);
        REQUIRE(probe_pipeline != nullptr);
        const int32_t a_idx = find_managed_audio_track(*probe_pipeline);
        REQUIRE(a_idx >= 0);
        for (int i = 0; i < 20000; ++i) {
            (void)probe_pipeline->step();
        }
        while (probe_pipeline->pop_frame(a_idx).has_value()) ++audio_total;
    }
    REQUIRE(audio_total >= 1);

    // 第 2 步：真正要用的 Pipeline——驱动到解码器真正的尽头，再手动摆
    // 出"音频只剩 1 帧、视频轨空"这个状态。
    auto pipeline = Pipeline::create_file(path, hugecfg, &err);
    REQUIRE(pipeline != nullptr);
    const int32_t audio_idx = find_managed_audio_track(*pipeline);
    const int32_t video_idx = find_managed_video_track(*pipeline);
    REQUIRE(audio_idx >= 0);
    REQUIRE(video_idx >= 0);
    for (int i = 0; i < 20000; ++i) {
        (void)pipeline->step();
    }
    for (int64_t i = 0; i < audio_total - 1; ++i) {
        REQUIRE(pipeline->pop_frame(audio_idx).has_value());
    }
    while (pipeline->pop_frame(video_idx).has_value()) {
        // 丢弃：这条用例不关心视频，只要它真的空了，video_waiting_early
        // 就不会抢先把 audio_blocked 盖住。
    }

    auto  sink     = std::make_unique<syp::test::FakeAudioSink>();
    auto* sink_ptr = sink.get();
    sink_ptr->set_capacity_frames(0);   // write() 永远失败——audio_blocked 永真
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);

    auto player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                       nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);
    REQUIRE(player->clock_kind() == ClockKind::Audio);
    (void)sink_ptr;

    // 第一次 step()：pop 到那唯一一帧音频、write() 失败、video 轨真的
    // 空、驱动 Pipeline 后 Pipeline 自己排空到底如实报 Eof——如果
    // audio_blocked 没有排在 Eof 前面，这里会是 Kind::Eof。
    const PlayOutcome o = player->step();
    REQUIRE(o.kind != PlayOutcome::Kind::Error);
    CHECK(o.kind == PlayOutcome::Kind::Blocked);
}

// =======================================================================
// Pipeline::step() 报 Error 必须永远最高优先级，
// 不能被 video_waiting_early/audio_blocked 盖住——零覆盖，有一个变异体
// （把 Error 判断挪到本地信号之后）27 条单测 + 九条 E2E 全绿地存活过。
//
// 关键点：修复前视频早到分支直接 return，video_waiting_early
// 这个变量根本不存在，"本地状态盖住 Pipeline 真实 Error"这条路径在
// 修复前无法触发；本轮修复让它成了活信号，这条风险是本次重排新造出
// 来的，必须补零点覆盖。
//
// 构造手法跟 tests/test_pipeline.cpp「pipeline_failed_seek_still_
// flushes_decoders」、本文件「make_fixture_that_fails_seek」同源：篡改
// mp4 的视频轨关键帧表（stss），配合一个只在指定的那一次 seek() 调用
// 期间让 seek 回调报 EIO 的 MemSource，让 Pipeline::seek() 之后紧跟的
// 首次 demux 真正失败、返回 Kind::Error——同时人为让视频"早到"（拨
// 一个远早于任何真实 pts 的时钟）以确保 video_waiting_early 与
// Pipeline 的 Error 同一次 step() 内同时为真，直接命中要测的组合。
TEST_CASE(pipeline_error_outranks_local_signals) {
    auto fx = make_fixture_that_fails_seek();
    REQUIRE(fx.player != nullptr);
    REQUIRE(fx.player->clock_kind() == ClockKind::Audio);

    // 跟 seek_runs_all_four_steps 同一个手法：sink 容量归零卡住一帧
    // 音频（audio_blocked 从此每次 step() 都为真），时钟因此冻结在 0，
    // 逼视频撞进"早了"分支并卡住（video_waiting_early 从此每次 step()
    // 都为真）——先把这两个本地信号都变成"长期为真"，再引入 Pipeline
    // 的 Error，确保 Error 出现的那一次，两个本地信号确实都在场，不是
    // 凑巧谁先谁后。
    fx.sink->set_capacity_frames(0);
    bool video_stuck = false;
    for (int i = 0; i < 100 && !video_stuck; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Waiting && o.pts_us > syp::media::kPresentWindowUs) {
            video_stuck = true;
        }
    }
    REQUIRE(video_stuck);

    // 让常规顺序读也失败——fail_seek 只能让 Pipeline::seek() 本身失败
    // （seek_failure_still_flushes_and_resets 用的是这条），让不了
    // demux 分支的常规读真正报 Kind::Error；这里用新增的 fail_read
    // （同一个 MemSource，同一个模式）。此后 video_stuck、audio_blocked
    // 两个信号继续保持（都没有被消费掉，pending_video_/pending_audio_
    // 还在原地）。置位 fail_read 不会立即命中——PacketQueue 里还有
    // video_stuck 构造阶段攒下的存量包，第 3 步会先安安静静地把它们
    // 解码掉几十次（video_waiting_early 全程为真，这些中间结果从外面
    // 看统一都是同一个 Waiting，掩盖了 Pipeline 内部到底有没有在真的
    // 读——这正是这条用例要验的东西：本地信号哪怕连续压住 Pipeline
    // 几十次真实的产出，Pipeline 一旦真的报 Error，必须立刻透出来，
    // 不能被继续压住），直到存量包耗尽、第 3 步真正需要读下一个包，
    // 才会命中 fail_read。实测第 61 次 step() 命中；1000 次留了约
    // 16 倍余量，不是掐着实测值卡的。
    fx.material->src.fail_read = true;
    bool saw_error = false;
    for (int i = 0; i < 1000 && !saw_error; ++i) {
        const PlayOutcome o = fx.player->step();
        if (o.kind == PlayOutcome::Kind::Error) {
            saw_error = true;
            // status 应该如实带着 Pipeline 转述的错误码，不是被本地
            // 信号（video_waiting_early/audio_blocked，这一刻两者都
            // 为真）盖住之后随便报的 Waiting/Blocked。
            CHECK(o.status != SYP_OK);
        } else {
            // 每一次不是 Error 的中间结果都必须还是本地信号该报的那
            // 个值（Waiting 带着卡住的视频 pts，或者 Blocked）——不能
            // 是别的什么，否则说明构造本身歪了，不是在测 Error 优先级。
            CHECK((o.kind == PlayOutcome::Kind::Waiting || o.kind == PlayOutcome::Kind::Blocked));
        }
    }
    REQUIRE(saw_error);
}

// TrackPlayer::request_abort() 转发到 Pipeline，并且中止之后
// step() 立刻报 Error(SYP_ERR_CANCELED)——哪怕队列里还有一帧正好到点。
// TrackPlayer 手里有现成帧时不一定调 Pipeline::step()，所以这一层得自己
// 挡，不能指望 Pipeline 的入口检查替它报。
TEST_CASE(request_abort_stops_step_even_with_an_on_time_frame_queued) {
    auto fx = make_fixture_with_video_pts({100000});
    fx.clock->set(100000);             // 到点：不中止的话这一步会 Presented
    fx.player->request_abort();
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Error);
    CHECK_EQ(o.status, SYP_ERR_CANCELED);
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});
}

// 渲染器返回 BUSY 不是故障，也**不消费帧**——帧留在 pending_video_，
// 这一步报 Waiting（带那一帧 pts，形状同早到分支），记 render_busy_frames（重试次数），
// 不计 present_failures、不计 dropped_frames；BUSY 解除后下一次 step() 呈现同一帧。
TEST_CASE(render_busy_retains_frame_not_failure_not_drop) {
    auto fx = make_fixture_with_video_pts({120000, 160000});
    fx.renderer->inject_busy_every(1);          // 每次都 BUSY
    fx.clock->set(120000);
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Waiting);
    CHECK_EQ(o.pts_us, int64_t{120000});
    CHECK_EQ(fx.player->render_busy_frames(), int64_t{1});
    CHECK_EQ(fx.player->present_failures(), int64_t{0});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});

    fx.renderer->inject_busy_every(0);
    const PlayOutcome o2 = fx.player->step();
    CHECK(o2.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(o2.pts_us, int64_t{120000});
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK_EQ(fx.renderer->shown()[0].pts_us, int64_t{120000});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});
}

// 偶发 BUSY（每第 2 次调用）——BUSY 的那一帧被重试并呈现，没有帧丢失。
TEST_CASE(render_busy_once_then_retry_presents_same_pts) {
    auto fx = make_fixture_with_video_pts({120000, 160000, 200000});
    fx.renderer->inject_busy_every(2);          // 第 2、4、6… 次调用 BUSY
    fx.clock->set(120000);
    CHECK(fx.player->step().kind == PlayOutcome::Kind::Presented);   // 调用 1：OK
    fx.clock->set(160000);
    const PlayOutcome busy = fx.player->step();                       // 调用 2：BUSY
    CHECK(busy.kind == PlayOutcome::Kind::Waiting);
    CHECK_EQ(busy.pts_us, int64_t{160000});
    const PlayOutcome retry = fx.player->step();                      // 调用 3：重试 OK
    CHECK(retry.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(retry.pts_us, int64_t{160000});
    REQUIRE(fx.renderer->shown().size() == 2);
    CHECK_EQ(fx.renderer->shown()[0].pts_us, int64_t{120000});
    CHECK_EQ(fx.renderer->shown()[1].pts_us, int64_t{160000});
    REQUIRE(fx.renderer->busy_pts().size() == 1);
    CHECK_EQ(fx.renderer->busy_pts()[0], int64_t{160000});
    CHECK_EQ(fx.player->render_busy_frames(), int64_t{1});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});
    CHECK_EQ(fx.player->present_failures(), int64_t{0});
}

// 持续 BUSY 直到保留的帧迟到超过 kDropThresholdUs——由正常迟到路径丢弃
// （计入 dropped_frames），下一帧接着重试。
TEST_CASE(sustained_busy_frame_goes_too_late_and_is_dropped_by_late_path) {
    auto fx = make_fixture_with_video_pts({120000, 160000, 200000});
    fx.renderer->inject_busy_every(1);
    fx.clock->set(120000);
    CHECK(fx.player->step().kind == PlayOutcome::Kind::Waiting);
    CHECK_EQ(fx.player->dropped_frames(), int64_t{0});

    fx.clock->set(120000 + syp::media::kDropThresholdUs + 1);   // 120000 迟到 80.001ms；160000 迟到 40.001ms
    const PlayOutcome o = fx.player->step();
    CHECK(o.kind == PlayOutcome::Kind::Dropped);
    CHECK_EQ(fx.player->dropped_frames(), int64_t{1});
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});
    // 丢掉 120000 后，160000 仍在呈现范围 → 尝试呈现 → 仍 BUSY → 保留。
    REQUIRE(!fx.renderer->busy_pts().empty());
    CHECK_EQ(fx.renderer->busy_pts().back(), int64_t{160000});

    fx.renderer->inject_busy_every(0);
    const PlayOutcome o2 = fx.player->step();
    CHECK(o2.kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(o2.pts_us, int64_t{160000});
    CHECK_EQ(fx.player->dropped_frames(), int64_t{1});
}

// due_in_us = max(0, pts - now) / speed，按真实时间传给渲染器。
TEST_CASE(present_receives_due_in_us_scaled_by_speed) {
    auto fx = make_fixture_with_video_pts({100000});
    fx.clock->set(80000);                        // 早 20ms，落在呈现窗口内
    REQUIRE(fx.player->step().kind == PlayOutcome::Kind::Presented);
    REQUIRE(fx.renderer->shown().size() == 1);
    CHECK_EQ(fx.renderer->shown()[0].due_in_us, int64_t{20000});
}

TEST_CASE(present_due_in_us_is_zero_for_late_frames) {
    auto fx = make_fixture_with_video_pts({100000});
    fx.clock->set(130000);                       // 迟 30ms，仍在呈现范围
    REQUIRE(fx.player->step().kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(fx.renderer->shown()[0].due_in_us, int64_t{0});
}

TEST_CASE(present_due_in_us_divides_by_speed) {
    auto fx = make_fixture_with_video_pts({100000});
    REQUIRE(fx.player->set_speed(2.0) == SYP_OK);
    fx.clock->set(80000);
    REQUIRE(fx.player->step().kind == PlayOutcome::Kind::Presented);
    CHECK_EQ(fx.renderer->shown()[0].due_in_us, int64_t{10000});
}

namespace {
struct AudioShortRun {
    PlayOutcome::Kind last = PlayOutcome::Kind::Waiting;
    int64_t presented = 0, dropped = 0, max_clock_jump_us = 0;
    int64_t max_backward_step_us = 0;   // 位置回退的最大幅度；AudioClock 读数持平是合法的，回退不是
    int64_t switch_pos_us = -1;          // 第一次观察到 AudioEnded 时的位置：不能在声音播完之前就切
    ClockSwitchReason reason = ClockSwitchReason::None;
    ClockKind kind = ClockKind::Audio;
    bool reached_eof = false;
};

// 以真实设备节奏推进：每次 step() 之后，sink 按"每步 2ms"消费；Waiting 时额外消费，模拟时间流逝。
// 切到系统时钟后，时间由真实 steady_clock 推进——为让测试不跑 3 秒墙钟，这里对
// TrackPlayer 暴露的 debug_advance_system_clock_us() 推进同样的量（见 Step 3）。
AudioShortRun run_audio_shorter_than_video(TrackPlayer& tp, syp::test::FakeAudioSink& sink) {
    AudioShortRun r;
    int64_t prev = tp.position_us();
    for (int i = 0; i < 400000; ++i) {
        const PlayOutcome o = tp.step();
        r.last = o.kind;
        if (o.kind == PlayOutcome::Kind::Presented) ++r.presented;
        if (o.kind == PlayOutcome::Kind::Dropped) ++r.dropped;
        if (o.kind == PlayOutcome::Kind::Eof) { r.reached_eof = true; break; }
        if (o.kind == PlayOutcome::Kind::Error) break;
        sink.advance(2000);
        tp.debug_advance_system_clock_us(2000);
        const int64_t now = tp.position_us();
        if (r.switch_pos_us < 0 && tp.clock_switch_reason() == ClockSwitchReason::AudioEnded) {
            r.switch_pos_us = now;
        }
        const int64_t jump = now - prev;
        if (jump > r.max_clock_jump_us) r.max_clock_jump_us = jump;
        if (-jump > r.max_backward_step_us) r.max_backward_step_us = -jump;
        prev = now;
    }
    r.reason = tp.clock_switch_reason();
    r.kind = tp.clock_kind();
    return r;
}
}  // namespace

// #23 回归：音频先结束，声音播完后切系统时钟，视频播完并报 Eof。修复前永远 Waiting。
TEST_CASE(audio_shorter_than_video_switches_to_system_clock_and_reaches_eof) {
    TempDir tmp;
    const std::string path = synth_video3_audio2(tmp.path);
    REQUIRE(!path.empty());
    syp_status err = SYP_OK;
    auto pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);
    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    syp::test::FakeAudioSink* sink_raw = sink.get();
    auto tp = TrackPlayer::create(std::move(pipeline), std::move(sink),
                                  std::make_unique<syp::test::FakeRenderer>(nullptr), nullptr, &err, BufferPolicy::disabled());
    REQUIRE(tp != nullptr);
    REQUIRE(tp->clock_kind() == ClockKind::Audio);

    const AudioShortRun r = run_audio_shorter_than_video(*tp, *sink_raw);
    CHECK(r.reached_eof);
    CHECK(r.kind == ClockKind::System);
    CHECK(r.reason == ClockSwitchReason::AudioEnded);
    CHECK(r.presented + r.dropped >= 70);          // 3 秒 25fps = 75 帧，允许少量丢帧
    CHECK(r.max_clock_jump_us <= 40000);           // 切钟前后无跳变（不超过一帧）
    CHECK_EQ(r.max_backward_step_us, int64_t{0});  // 也不回退
    // 声音真的播完才切：音频 2 秒（读数含 AAC priming 超前量，实测冻结在 ~2020000）。
    CHECK(r.switch_pos_us >= 1900000);
}

// #23 + 大设备延迟（150ms，蓝牙量级）：played_us() 扣除延迟后永远到不了"最后写入的
// 结束时刻"，按读数判"播完"会停摆。判据必须与延迟无关（IAudioSink::output_drained()）。
TEST_CASE(audio_shorter_than_video_with_large_device_latency_reaches_eof) {
    TempDir tmp;
    const std::string path = synth_video3_audio2(tmp.path);
    REQUIRE(!path.empty());
    syp_status err = SYP_OK;
    auto pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);
    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    sink->set_device_latency_us(150000);
    syp::test::FakeAudioSink* sink_raw = sink.get();
    auto tp = TrackPlayer::create(std::move(pipeline), std::move(sink),
                                  std::make_unique<syp::test::FakeRenderer>(nullptr), nullptr, &err, BufferPolicy::disabled());
    REQUIRE(tp != nullptr);
    REQUIRE(tp->clock_kind() == ClockKind::Audio);

    const AudioShortRun r = run_audio_shorter_than_video(*tp, *sink_raw);
    CHECK(r.reached_eof);
    CHECK(r.kind == ClockKind::System);
    CHECK(r.reason == ClockSwitchReason::AudioEnded);
    CHECK(r.presented + r.dropped >= 70);
    CHECK(r.max_clock_jump_us <= 40000);
    CHECK_EQ(r.max_backward_step_us, int64_t{0});
    // 读数扣除 150ms 延迟后冻结在 ~1870000；切钟不能早于样本被设备取空。
    CHECK(r.switch_pos_us >= 1800000);
}

// 视频先结束：行为不变——仍以音频为时钟，播完报 Eof，不切钟。
TEST_CASE(video_shorter_than_audio_keeps_audio_clock_until_eof) {
    auto fx = make_fixture_with_audio_and_video();     // 既有素材：两路等长或音频更长
    int64_t steps = 0;
    PlayOutcome o;
    do {
        o = fx.player->step();
        fx.sink->advance(2000);
        fx.player->debug_advance_system_clock_us(2000);
    } while (o.kind != PlayOutcome::Kind::Eof && o.kind != PlayOutcome::Kind::Error && ++steps < 400000);
    CHECK(o.kind == PlayOutcome::Kind::Eof);
    CHECK(fx.player->clock_switch_reason() != ClockSwitchReason::AudioFailed);
}

// 音频播完切钟后 seek 回前面：恢复音频时钟。
TEST_CASE(seek_after_audio_ended_restores_audio_clock) {
    TempDir tmp;
    const std::string path = synth_video3_audio2(tmp.path);
    REQUIRE(!path.empty());
    syp_status err = SYP_OK;
    auto pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);
    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    syp::test::FakeAudioSink* sink_raw = sink.get();
    auto tp = TrackPlayer::create(std::move(pipeline), std::move(sink),
                                  std::make_unique<syp::test::FakeRenderer>(nullptr), nullptr, &err, BufferPolicy::disabled());
    REQUIRE(tp != nullptr);
    for (int i = 0; i < 400000 && tp->clock_switch_reason() != ClockSwitchReason::AudioEnded; ++i) {
        const PlayOutcome o = tp->step();
        if (o.kind == PlayOutcome::Kind::Eof || o.kind == PlayOutcome::Kind::Error) break;
        sink_raw->advance(2000);
        tp->debug_advance_system_clock_us(2000);
    }
    REQUIRE(tp->clock_switch_reason() == ClockSwitchReason::AudioEnded);
    tp->seek(0);
    CHECK(tp->clock_kind() == ClockKind::Audio);
    CHECK(tp->clock_switch_reason() == ClockSwitchReason::None);
}

namespace {
std::unique_ptr<TrackPlayer> make_video3_audio2_player(TempDir& tmp, syp::test::FakeAudioSink** sink_out) {
    const std::string path = synth_video3_audio2(tmp.path);
    if (path.empty()) return nullptr;
    syp_status err = SYP_OK;
    auto pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    if (pipeline == nullptr) return nullptr;
    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    *sink_out = sink.get();
    return TrackPlayer::create(std::move(pipeline), std::move(sink),
                               std::make_unique<syp::test::FakeRenderer>(nullptr), nullptr, &err, BufferPolicy::disabled());
}
}  // namespace

// #23 延伸：切钟后 seek 到音频结束之后（2.5s）。seek() 恢复了 AudioClock，此后必须
// 能再次切钟并播完。
// 实测说明（探针）：mov 解封装器按流各自 seek，音频流向后取到最后一个采样
// （pts=1996916、dur=3084），seek 后仍会写进 sink 一帧、等它被取走后再判定播完；
// "seek 后一帧音频都没有"的形状本素材构造不出，由 output_drained() 在 flush 后
// 天然为 true 覆盖（见 track_player.cpp audio_played_out() 注释）。
TEST_CASE(seek_past_audio_end_after_switch_still_reaches_eof) {
    TempDir tmp;
    syp::test::FakeAudioSink* sink = nullptr;
    auto tp = make_video3_audio2_player(tmp, &sink);
    REQUIRE(tp != nullptr);
    for (int i = 0; i < 400000 && tp->clock_switch_reason() != ClockSwitchReason::AudioEnded; ++i) {
        const PlayOutcome o = tp->step();
        if (o.kind == PlayOutcome::Kind::Eof || o.kind == PlayOutcome::Kind::Error) break;
        sink->advance(2000);
        tp->debug_advance_system_clock_us(2000);
    }
    REQUIRE(tp->clock_switch_reason() == ClockSwitchReason::AudioEnded);
    REQUIRE(tp->seek(2500000) == SYP_OK);
    CHECK(tp->clock_kind() == ClockKind::Audio);
    const AudioShortRun r = run_audio_shorter_than_video(*tp, *sink);
    CHECK(r.reached_eof);
    CHECK(r.reason == ClockSwitchReason::AudioEnded);
}

// #23 延伸：音频轨已全部写进 sink（轨已排空）、声音还没播完时 set_speed()。flush 清掉
// 了尾巴，读数冻结在 base、不会再有样本写入——必须同样能切钟播完（flush 后
// output_drained() 为 true）。
TEST_CASE(set_speed_after_audio_drained_still_reaches_eof) {
    TempDir tmp;
    syp::test::FakeAudioSink* sink = nullptr;
    auto tp = make_video3_audio2_player(tmp, &sink);
    REQUIRE(tp != nullptr);
    // sink 容量极大，音频优先写入：位置还在 1 秒附近时音频轨早已排空。
    int i = 0;
    for (; i < 400000 && tp->position_us() < 1000000; ++i) {
        const PlayOutcome o = tp->step();
        if (o.kind == PlayOutcome::Kind::Eof || o.kind == PlayOutcome::Kind::Error) break;
        sink->advance(2000);
    }
    REQUIRE(tp->position_us() >= 1000000);
    REQUIRE(tp->clock_kind() == ClockKind::Audio);
    REQUIRE(tp->set_speed(1.5) == SYP_OK);
    const AudioShortRun r = run_audio_shorter_than_video(*tp, *sink);
    CHECK(r.reached_eof);
    CHECK(r.reason == ClockSwitchReason::AudioEnded);
}

// 暂停中切到系统时钟（这里走失败降级入口），新时钟必须同样处于暂停，
// 否则暂停期间位置自己往前走、play() 时跳过整段暂停时长。
TEST_CASE(degrade_while_paused_keeps_clock_frozen) {
    auto fx = make_fixture_with_audio_and_video();
    for (int i = 0; i < 200; ++i) {
        (void)fx.player->step();
        fx.sink->advance(2000);
    }
    fx.player->pause();
    fx.sink->inject_failure();
    (void)fx.player->step();
    REQUIRE(fx.player->clock_kind() == ClockKind::System);
    CHECK(fx.player->clock_switch_reason() == ClockSwitchReason::AudioFailed);
    const int64_t before = fx.player->position_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(fx.player->position_us(), before);
}

// =======================================================================
// 追帧丢帧策略——1 秒内迟到丢帧达到 kCatchupDropsToEnable 次
// 打开追帧（Pipeline::set_video_catchup(true)），连续 kCatchupOnTimeToDisable
// 帧按时呈现关闭；seek()/pause() 复位。三条用例逐字取自最初的方案。
//
// 【范围说明】不让 fixture 额外提供一个
// `pipeline_video_catchup()` 包装方法——TrackPlayer 已接管 Pipeline 所有权，
// 加这层包装纯粹是重复 `player->video_catchup_active()` 已经暴露的东西。
// 这里直接调 `fx.player->video_catchup_active()`，不加这层包装。
// =======================================================================

namespace {
std::vector<int64_t> cfr_pts(int count, int64_t start_us, int64_t spacing_us) {
    std::vector<int64_t> v;
    for (int i = 0; i < count; ++i) v.push_back(start_us + i * spacing_us);
    return v;
}
}  // namespace

// 持续迟到：1 秒窗口内迟到丢帧达到 5 次 → 打开追帧。
TEST_CASE(sustained_late_drops_enable_catchup) {
    auto fx = make_fixture_with_video_pts(cfr_pts(60, 40000, 40000));
    fx.clock->set(40000 + 5 * 40000 + 200000);   // 前 5 帧都迟到 > 80ms
    for (int i = 0; i < 10 && !fx.player->catching_up(); ++i) fx.player->step();
    CHECK(fx.player->catching_up());
    CHECK(fx.player->video_catchup_active());
}

// 持续 BUSY 造成的迟到丢帧走正常迟到路径，因此也喂追帧窗口。
TEST_CASE(sustained_busy_late_drops_feed_catchup) {
    auto fx = make_fixture_with_video_pts(cfr_pts(60, 40000, 40000));
    fx.renderer->inject_busy_every(1);
    int64_t now = 40000;
    fx.clock->set(now);
    for (int i = 0; i < 20 && !fx.player->catching_up(); ++i) {
        fx.player->step();
        now += 41000;
        fx.clock->set(now);
    }
    CHECK(fx.player->catching_up());
    CHECK(fx.player->dropped_frames() >= int64_t{syp::media::kCatchupDropsToEnable});
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});
}

// 追帧中连续 30 帧按时呈现 → 关闭。
//
// presented 计数从 phase 1（触发追帧那个循环）
// 就开始记，不是只从 phase 2（追帧后紧跟时钟那个循环）开始——kMaxDropsPerStep
// （8）> kCatchupDropsToEnable（5）：本用例这份素材、这个初始时钟值下，
// phase 1 的单次 step() 内部会连续丢掉 8 帧（late 分支的 continue 循环，
// 早于任何 return），第 5 次丢弃时 note_late_drop() 已经把 catching_up_
// 置真，但同一次 step() 调用不会提前退出，会接着丢完剩下的、直到弹到一帧
// 不再迟到（第 9 帧），呈现它——也就是说"追帧刚打开"那一刻，第一帧按时
// 呈现其实已经在 phase 1 的这次 step() 里发生了，只是 phase 1 的循环体
// 没有检查 kind、这一次呈现不会被后面 phase 2 的计数看到。只在 phase 2
// 计数会让这里少数 1（实测 29，不是 30），断言要跟 kCatchupOnTimeToDisable
// 这个真实判据对上，两段循环都要算。
TEST_CASE(catchup_disables_after_on_time_presents) {
    auto fx = make_fixture_with_video_pts(cfr_pts(60, 40000, 40000));
    fx.clock->set(40000 + 5 * 40000 + 200000);
    int presented = 0;
    for (int i = 0; i < 10 && !fx.player->catching_up(); ++i) {
        if (fx.player->step().kind == PlayOutcome::Kind::Presented) ++presented;
    }
    REQUIRE(fx.player->catching_up());
    // 之后让时钟紧跟每一帧的 pts：每次 step 前把时钟设到下一帧 pts。
    for (int i = 0; i < 400 && fx.player->catching_up(); ++i) {
        const PlayOutcome o = fx.player->step();
        if (o.kind == PlayOutcome::Kind::Presented) {
            ++presented;
            fx.clock->set(o.pts_us + 40000);
        } else if (o.kind == PlayOutcome::Kind::Waiting && o.pts_us != AV_NOPTS_VALUE) {
            fx.clock->set(o.pts_us);
        }
    }
    CHECK(!fx.player->catching_up());
    CHECK(!fx.player->video_catchup_active());
    // 跟 kCatchupOnTimeToDisable 挂钩，而不是只查"循环确实推进过"——关闭
    // 追帧的判据本来就是"连续 30 帧按时"，呈现数至少要达到这个数才站得住。
    CHECK(presented >= kCatchupOnTimeToDisable);
}

// seek / pause 复位追帧。
//
// 【素材消耗量说明】上面这套时钟值（第 6 帧起持续迟到超过
// 80ms）在 phase 1 只需要**一次** step() 调用就能把追帧打开：
// kMaxDropsPerStep(8) > kCatchupDropsToEnable(5)——视频判定内部那个
// `for(;;)` 循环在一次 step() 里连续丢帧（late 分支只 `continue`，不
// return），丢到第 5 帧时 note_late_drop() 已经把 catching_up_ 置真，但
// 循环不会因此提前退出，会接着丢完这一批里剩下的（直到某一帧不再迟到，
// 呈现它、`return`）。本用例这组时钟值下，一次 step() 恰好丢 8 帧、
// 呈现第 9 帧——即消费了 60 帧素材里的前 9 帧，下一次待处理帧是第 10
// 帧（pts=400000）。
TEST_CASE(seek_and_pause_reset_catchup) {
    auto fx = make_fixture_with_video_pts(cfr_pts(60, 40000, 40000));
    fx.clock->set(40000 + 5 * 40000 + 200000);
    for (int i = 0; i < 10 && !fx.player->catching_up(); ++i) fx.player->step();
    REQUIRE(fx.player->catching_up());
    fx.player->pause();
    CHECK(!fx.player->catching_up());
    CHECK(!fx.player->video_catchup_active());

    // 上面只覆盖了 pause()，seek() 那半从没
    // 被断言过——删掉 seek() 里的 reset_catchup() 调用不会让上面任何一条
    // 断言变红。这里恢复播放、用同一套"持续迟到"手法在剩下的帧（第 10
    // 帧 pts=400000 起还有 51 帧可用，见上方长注释，富余量足够）上重新
    // 触发一次追帧，再 seek()，钉住 seek() 自己也会复位。
    fx.player->play();
    fx.clock->set(400000 + 5 * 40000 + 200000);
    for (int i = 0; i < 10 && !fx.player->catching_up(); ++i) fx.player->step();
    REQUIRE(fx.player->catching_up());
    REQUIRE(fx.player->seek(0) == SYP_OK);
    CHECK(!fx.player->catching_up());
    CHECK(!fx.player->video_catchup_active());
}

// =======================================================================
// preview_pending_ 只在"打开后还没有任何一次非暂停 step() 真正推进过"这
// 段窗口里才应该在暂停时触发预览。后来把清除点从"视频循环第一次
// pop 到帧"挪到了"每次非暂停 step() 的音视频判定之前、无条件清一次"——
// 不再依赖视频循环有没有被进入。下面两条用例分别覆盖：
//   1. preview_pending_cleared_by_normal_path_present_or_drop：正常路径
//      已经真的呈现/丢弃过至少一帧之后再暂停，不应该再多出一帧"预览"
//      （这条在旧清除点下也成立，继续留着当基础覆盖）。
//   2. preview_pending_cleared_by_any_unpaused_step_even_audio_only：
//      点名的那个缺口本身——只调**一次**非暂停 step()（不
//      关心它做了什么，大概率这一次两条队列都还没解出东西、只是折叠成
//      Waiting、视频循环根本没被进入过），之后暂停也不该出现"预览"。旧
//      清除点（绑在视频循环第一次 pop 到帧）在这条用例上会失败——这正是
//      新清除点要解决的那个缺口，现在是一条廉价、确定性的用例，不再需要
//      靠"连续多次 step() 全部从音频分支提前返回"这种依赖实现细节的窗口
//      去构造。
// =======================================================================
TEST_CASE(preview_pending_cleared_by_normal_path_present_or_drop) {
    auto fx = make_fixture_with_audio_and_video();
    bool video_resolved = false;
    for (int i = 0; i < 400 && !video_resolved; ++i) {
        const PlayOutcome o = fx.player->step();
        fx.sink->advance(2000);
        if (o.kind == PlayOutcome::Kind::Presented || o.kind == PlayOutcome::Kind::Dropped) {
            video_resolved = true;
        }
    }
    REQUIRE(video_resolved);
    const size_t before = fx.renderer->shown().size();

    fx.player->pause();
    for (int i = 0; i < 16; ++i) fx.player->step();
    CHECK_EQ(fx.renderer->shown().size(), before);   // 没有多出一帧"预览"
}

TEST_CASE(preview_pending_cleared_by_any_unpaused_step_even_audio_only) {
    auto fx = make_fixture_with_audio_and_video();
    (void)fx.player->step();   // 随便哪一步——不检查它的产出，也不驱动到
                               // 视频真的被呈现/丢弃；这一次 step() 之后
                               // preview_pending_ 就该是 false 了。
    REQUIRE(fx.renderer->shown().size() == size_t{0});   // 这一步还没真呈现过任何东西
    fx.player->pause();
    for (int i = 0; i < 16; ++i) fx.player->step();
    CHECK_EQ(fx.renderer->shown().size(), size_t{0});   // 没有出现"预览"帧
}

// ---------------------------------------------------------------------
// 缓冲状态机
// ---------------------------------------------------------------------
//
// 【测试缝的选择】状态机的全部输入是 (BufferStats, 播放位置, 是否暂停)。BufferStats
// 经 Pipeline::debug_override_buffer_stats() 注入（同步模式、没有加载线程），播放位置
// 由 FakeAudioSink::advance() 推进（AudioClock 转发 played_us()）——两者都由用例逐步
// 拨动，判定完全确定，不依赖线程时序。真实加载线程产生的水位由 test_hls_e2e 的
// held_segment_makes_track_player_stall_then_resume_to_eof 端到端覆盖。
namespace {

// FX_REQUIRE 在上面 SeekFailFixture 之后已被 #undef，这里按原样重新定义，块尾再 #undef。
#define FX_REQUIRE(expr)                                                 \
    do {                                                                 \
        if (!(expr)) {                                                  \
            ::tiny_test::fail(__FILE__, __LINE__, #expr);                \
            return fx;                                                  \
        }                                                                \
    } while (0)

struct BufferingFixture {
    std::unique_ptr<TempDir>     tmp;
    std::unique_ptr<TrackPlayer> player;
    syp::test::FakeAudioSink*    sink     = nullptr;
    syp::test::FakeRenderer*     renderer = nullptr;
    std::shared_ptr<BufferStats> stats;
};

BufferingFixture make_buffering_fixture(const BufferPolicy& policy, int64_t max_buffer_ms = 30000) {
    BufferingFixture fx;
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return fx;
    }
    fx.tmp = std::make_unique<TempDir>();
    FX_REQUIRE(!fx.tmp->path.empty());
    const std::string path = synth_video_audio(fx.tmp->path);
    FX_REQUIRE(!path.empty());

    PipelineConfig cfg;
    cfg.max_buffer_ms = max_buffer_ms;
    syp_status err = SYP_OK;
    auto pipeline = Pipeline::create_file(path, cfg, &err);
    FX_REQUIRE(pipeline != nullptr);
    fx.stats = std::make_shared<BufferStats>();
    std::shared_ptr<BufferStats> stats = fx.stats;
    pipeline->debug_override_buffer_stats([stats] { return *stats; });

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    fx.sink = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    fx.renderer = renderer.get();
    fx.player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                    nullptr, &err, policy);
    FX_REQUIRE(fx.player != nullptr);
    return fx;
}

template <typename Pred>
bool step_until(TrackPlayer& tp, int max_steps, Pred pred) {
    for (int i = 0; i < max_steps; ++i) {
        if (pred()) return true;
        if (tp.step().kind == PlayOutcome::Kind::Error) return false;
    }
    return pred();
}

// 水位给到 5 秒，离开起播缓冲。
bool leave_startup(BufferingFixture& fx) {
    fx.stats->buffered_until_us = 5'000'000;
    return step_until(*fx.player, 10, [&fx] { return !fx.player->buffering(); });
}

// 水位压到当前位置（已缓冲 0），进入卡顿。
bool enter_stall(BufferingFixture& fx) {
    fx.stats->buffered_until_us = fx.player->position_us();
    fx.player->step();
    return fx.player->buffering_reason() == BufferingReason::Stall;
}

#undef FX_REQUIRE

}  // namespace

TEST_CASE(buffering_startup_waits_for_startup_level_and_presents_first_frame) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    CHECK(fx.player->buffering_reason() == BufferingReason::Startup);
    CHECK(fx.sink->paused());
    CHECK_EQ(fx.player->startup_us(), int64_t{-1});

    bool presented = false;   // 起播缓冲中首帧照常出（快速出画），不写音频
    for (int i = 0; i < 200 && !presented; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Queued);
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Presented) presented = true;
    }
    CHECK(presented);
    CHECK_EQ(fx.sink->written_frames(), int64_t{0});

    fx.stats->buffered_until_us = 499999;
    fx.player->step();
    CHECK(fx.player->buffering());
    fx.stats->buffered_until_us = 500000;
    fx.player->step();
    CHECK(!fx.player->buffering());
    CHECK(!fx.sink->paused());
    CHECK(fx.player->startup_us() >= 0);
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{0});
}

TEST_CASE(buffering_stall_enters_below_trigger_counts_freezes_and_resumes) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    for (int i = 0; i < 400; ++i) {
        fx.player->step();
        fx.sink->advance(1000);
    }
    const int64_t pos = fx.player->position_us();
    REQUIRE(pos > 0);

    fx.stats->buffered_until_us = pos + 100000;   // 恰好等于触发线：不卡
    fx.player->step();
    CHECK(!fx.player->buffering());

    fx.stats->buffered_until_us = pos + 99999;
    fx.player->step();
    CHECK(fx.player->buffering_reason() == BufferingReason::Stall);
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{1});
    CHECK(fx.sink->paused());
    CHECK_EQ(fx.player->buffered_us(), int64_t{99999});

    const int64_t written = fx.sink->written_frames();
    for (int i = 0; i < 50; ++i) {
        CHECK(fx.player->step().kind != PlayOutcome::Kind::Queued);
        fx.sink->advance(10000);
    }
    CHECK_EQ(fx.player->position_us(), pos);            // 时钟冻结
    CHECK_EQ(fx.sink->written_frames(), written);       // 不写音频
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(fx.player->rebuffer_total_us() >= 2000);      // 进行中的卡顿也计入

    fx.stats->buffered_until_us = pos + 1999999;
    fx.player->step();
    CHECK(fx.player->buffering());
    fx.stats->buffered_until_us = pos + 2000000;
    fx.player->step();
    CHECK(!fx.player->buffering());
    CHECK(!fx.sink->paused());
    const int64_t total = fx.player->rebuffer_total_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK_EQ(fx.player->rebuffer_total_us(), total);    // 离开后不再累计
}

TEST_CASE(buffering_no_stall_after_demux_eof) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    fx.stats->demux_eof         = true;
    fx.stats->buffered_until_us = fx.player->position_us();
    for (int i = 0; i < 5; ++i) fx.player->step();
    CHECK(!fx.player->buffering());
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{0});
}

TEST_CASE(buffering_stall_leaves_on_demux_eof_below_resume_level) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(enter_stall(fx));
    fx.stats->demux_eof = true;
    fx.player->step();
    CHECK(!fx.player->buffering());
}

// 字节上限先到、已缓冲时长低于恢复水位：读不进更多，不能永久缓冲。
TEST_CASE(buffering_stall_leaves_when_buffer_full_below_resume_level) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(enter_stall(fx));
    fx.stats->full = true;
    fx.player->step();
    CHECK(!fx.player->buffering());
}

TEST_CASE(buffering_user_pause_is_orthogonal_to_buffering) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    // 起播缓冲中 pause/play：缓冲没满足，时钟仍冻结
    fx.player->pause();
    fx.player->play();
    CHECK(fx.sink->paused());
    REQUIRE(leave_startup(fx));

    // 暂停中不判卡顿
    fx.player->pause();
    fx.stats->buffered_until_us = fx.player->position_us();
    fx.player->step();
    CHECK(!fx.player->buffering());
    fx.player->play();
    REQUIRE(enter_stall(fx));

    // 卡顿中暂停、水位满足：离开缓冲但仍暂停，时钟不走；play 才恢复
    fx.player->pause();
    fx.stats->buffered_until_us = fx.player->position_us() + 2000000;
    fx.player->step();
    CHECK(!fx.player->buffering());
    CHECK(fx.player->paused());
    CHECK(fx.sink->paused());
    fx.player->play();
    CHECK(!fx.sink->paused());
}

TEST_CASE(buffering_seek_during_stall_switches_to_seek_with_startup_level) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(enter_stall(fx));
    REQUIRE(fx.player->seek(0) == SYP_OK);
    CHECK(fx.player->buffering_reason() == BufferingReason::Seek);
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{1});
    CHECK(fx.sink->paused());
    fx.stats->buffered_until_us = 499999;
    fx.player->step();
    CHECK(fx.player->buffering());
    fx.stats->buffered_until_us = 500000;
    CHECK(step_until(*fx.player, 50, [&fx] { return !fx.player->buffering(); }));
}

TEST_CASE(buffering_seek_from_playing_enters_seek_buffering) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(fx.player->seek(0) == SYP_OK);
    CHECK(fx.player->buffering_reason() == BufferingReason::Seek);
    CHECK(fx.sink->paused());
}

TEST_CASE(buffering_set_speed_during_stall_keeps_buffering) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(enter_stall(fx));
    REQUIRE(fx.player->set_speed(2.0) == SYP_OK);
    fx.player->step();
    CHECK(fx.player->buffering_reason() == BufferingReason::Stall);
    CHECK(fx.sink->paused());
}

// 缓冲中 sink 失败降级：新系统时钟同样冻结。
TEST_CASE(buffering_degrade_during_stall_keeps_system_clock_frozen) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(enter_stall(fx));
    fx.sink->inject_failure();
    fx.player->step();
    REQUIRE(fx.player->clock_kind() == ClockKind::System);
    CHECK(fx.player->buffering());
    const int64_t p0 = fx.player->position_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(fx.player->position_us(), p0);
}

TEST_CASE(buffering_resume_level_is_capped_by_max_buffer_ms) {
    auto fx = make_buffering_fixture(BufferPolicy{}, /*max_buffer_ms=*/1000);
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(enter_stall(fx));
    fx.stats->buffered_until_us = fx.player->position_us() + 1000000;
    fx.player->step();
    CHECK(!fx.player->buffering());
}

TEST_CASE(buffering_unbounded_when_no_track_left_never_stalls) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    fx.stats->buffered_until_us = kBufferedUntilUnbounded;
    CHECK_EQ(fx.player->buffered_us(), kBufferedUntilUnbounded);
    for (int i = 0; i < 5; ++i) fx.player->step();
    CHECK(!fx.player->buffering());
}

TEST_CASE(buffering_disabled_policy_never_buffers) {
    auto fx = make_buffering_fixture(BufferPolicy::disabled());
    REQUIRE(fx.player != nullptr);
    CHECK(!fx.player->buffering());
    CHECK(!fx.sink->paused());
    bool queued = false;   // 水位为"无数据"也照常写音频
    for (int i = 0; i < 200 && !queued; ++i) {
        if (fx.player->step().kind == PlayOutcome::Kind::Queued) queued = true;
    }
    CHECK(queued);
    CHECK(!fx.player->buffering());
    CHECK_EQ(fx.player->startup_us(), int64_t{-1});
}

TEST_CASE(audio_underruns_forwards_sink_counter) {
    auto fx = make_buffering_fixture(BufferPolicy::disabled());
    REQUIRE(fx.player != nullptr);
    fx.sink->set_underrun_count(3);
    CHECK_EQ(fx.player->audio_underruns(), int64_t{3});
}

// 控制器约束 1：线程模式稳态下 BufferStats::full 没有滞回、约每帧翻转一次。进入缓冲
// 只能由欠载（已缓冲 < 触发线）触发，full 的边沿本身既不能让播放器进入缓冲，也不能
// 在非缓冲状态下产生任何"离开"动作（full 只在已经缓冲时作为恢复条件）。
TEST_CASE(buffering_full_flicker_with_healthy_level_never_transitions) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    bool ever_buffering = false;
    for (int i = 0; i < 300; ++i) {
        fx.stats->full = (i % 2 == 0);
        // 介于触发线（100ms）与恢复水位（2000ms）之间：健康但不"满"
        fx.stats->buffered_until_us = fx.player->position_us() + 500000;
        fx.player->step();
        if (fx.player->buffering()) ever_buffering = true;
        CHECK(!fx.sink->paused());
        fx.sink->advance(1000);
    }
    CHECK(!ever_buffering);
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{0});
    CHECK_EQ(fx.player->rebuffer_total_us(), int64_t{0});
}

// 控制器约束 2：线程模式下 Pipeline::step() 按轨号升序挑"帧队列有空位且有包"的轨
// 解码，视频轨号更小。验证视频按真实节奏慢慢消费时音频不被饿死：任何一步之后，
// 只要 sink 环里没有待播样本、且已缓冲时长 ≥ 200ms（包已经在队列里、只是没被解码），
// 就是解码调度饿死了音频（首批音频写入之前的起播预热段除外，见循环内注释）。
// 本地文件、真实加载线程、默认 BufferPolicy。
TEST_CASE(buffering_threaded_mode_audio_not_starved_by_slow_video_consumption) {
    syp::test::Watchdog wd("buffering_threaded_mode_audio_not_starved_by_slow_video_consumption",
                           60000, 120000);
    REQUIRE(ffmpeg_available());
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio(tmp.path);
    REQUIRE(!path.empty());

    PipelineConfig cfg;
    cfg.demux_thread = true;
    syp_status err   = SYP_OK;
    auto pipeline    = Pipeline::create_file(path, cfg, &err);
    REQUIRE(pipeline != nullptr);
    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(4410);   // 100ms 环：音频必须持续被解码补货
    syp::test::FakeAudioSink* sink_ptr = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    syp::test::FakeRenderer* renderer_ptr = renderer.get();
    auto tp = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer), nullptr,
                                  &err, BufferPolicy{});
    REQUIRE(tp != nullptr);
    REQUIRE(tp->clock_kind() == ClockKind::Audio);

    bool    eof           = false;
    int64_t starved_steps = 0;
    for (int i = 0; i < 2'000'000 && !eof; ++i) {
        const PlayOutcome o = tp->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Eof) eof = true;
        const int64_t pos = tp->position_us();
        // 首批音频写入之前不算：起播时各轨帧队列都空，视频轨号小先解满 8 帧才轮到音频，
        // 这几步（亚毫秒级）环本来就是空的、时钟也不走（FakeAudioSink 只消费已写入的），
        // 是 audio_underruns() 注释里说的"预热"段，不是稳态饿死。
        if (!tp->buffering() && tp->clock_kind() == ClockKind::Audio && pos < 800000 &&
            sink_ptr->written_frames() > 0 &&
            sink_ptr->written_frames() == sink_ptr->consumed_frames() &&
            tp->buffered_us() >= 200000) {
            ++starved_steps;
        }
        if (tp->buffering()) std::this_thread::yield();
        sink_ptr->advance(500);
    }
    CHECK(eof);
    CHECK_EQ(starved_steps, int64_t{0});
    CHECK(tp->startup_us() >= 0);
    CHECK(!tp->buffering());
    CHECK(sink_ptr->written_frames() >= 39690);   // ≥ 0.9s @ 44.1kHz
    CHECK(renderer_ptr->shown().size() + static_cast<size_t>(tp->dropped_frames()) >= size_t{20});
}

TEST_CASE(buffering_seek_while_paused_enters_seek_without_touching_clock_state) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    fx.player->pause();
    REQUIRE(fx.player->seek(0) == SYP_OK);
    CHECK(fx.player->buffering_reason() == BufferingReason::Seek);
    CHECK(fx.sink->paused());
    fx.stats->buffered_until_us = 5'000'000;
    CHECK(step_until(*fx.player, 50, [&fx] { return !fx.player->buffering(); }));
    CHECK(fx.sink->paused());   // 仍暂停
    fx.player->play();
    CHECK(!fx.sink->paused());
}


// 【卡顿闩锁】靠 full 在已缓冲 < 触发线时离开 Stall 后，full 翻转、
// 水位仍为 0 也不得反复进出（每次进出都会 Stop/Start 音频设备）；水位回到触发线以上一次
// 才重新武装。
TEST_CASE(buffering_full_exit_below_trigger_does_not_flap_until_rearmed) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(enter_stall(fx));
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{1});

    fx.stats->full              = true;
    fx.stats->buffered_until_us = fx.player->position_us();
    fx.player->step();
    REQUIRE(!fx.player->buffering());

    bool ever_buffering = false;
    bool ever_paused    = false;
    for (int i = 0; i < 20; ++i) {
        fx.stats->full              = (i % 2 == 1);
        fx.stats->buffered_until_us = fx.player->position_us();
        fx.player->step();
        if (fx.player->buffering()) ever_buffering = true;
        if (fx.sink->paused()) ever_paused = true;
    }
    CHECK(!ever_buffering);
    CHECK(!ever_paused);
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{1});

    fx.stats->full              = false;
    fx.stats->buffered_until_us = fx.player->position_us() + 500000;   // 回到触发线以上：重新武装
    fx.player->step();
    CHECK(!fx.player->buffering());
    fx.stats->buffered_until_us = fx.player->position_us();
    fx.player->step();
    CHECK(fx.player->buffering_reason() == BufferingReason::Stall);
    CHECK_EQ(fx.player->rebuffer_count(), int64_t{2});
}

// 无音频（sink 为空）→ TrackPlayer 自有 SystemClock：缓冲冻结要
// 真的暂停它，离开后恢复。
TEST_CASE(buffering_freezes_and_resumes_owned_system_clock_without_audio) {
    REQUIRE(ffmpeg_available());
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_only(tmp.path, 25, 50);
    REQUIRE(!path.empty());
    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);
    auto stats = std::make_shared<BufferStats>();
    pipeline->debug_override_buffer_stats([stats] { return *stats; });
    auto tp = TrackPlayer::create(std::move(pipeline), nullptr,
                                  std::make_unique<syp::test::FakeRenderer>(nullptr), nullptr, &err,
                                  BufferPolicy{});
    REQUIRE(tp != nullptr);
    REQUIRE(tp->clock_kind() == ClockKind::System);

    // 起播缓冲中系统时钟已冻结
    const int64_t p_start = tp->position_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(tp->position_us(), p_start);

    stats->buffered_until_us = 5'000'000;
    tp->step();
    REQUIRE(!tp->buffering());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(tp->position_us() > p_start);

    stats->buffered_until_us = tp->position_us();
    tp->step();
    REQUIRE(tp->buffering_reason() == BufferingReason::Stall);
    const int64_t p0 = tp->position_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(tp->position_us(), p0);

    stats->buffered_until_us = p0 + 2'000'000;
    tp->step();
    REQUIRE(!tp->buffering());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(tp->position_us() >= p0 + 4000);
}

// 缓冲进出不 flush sink（恢复时从原位置接着播、环里的音频保留）。
TEST_CASE(buffering_stall_enter_and_leave_never_flush_sink) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    for (int i = 0; i < 50; ++i) {
        fx.player->step();
        fx.sink->advance(1000);
    }
    const int32_t flushes = fx.sink->flush_count();
    const int64_t written = fx.sink->written_frames();
    REQUIRE(written > 0);
    REQUIRE(enter_stall(fx));
    for (int i = 0; i < 10; ++i) fx.player->step();
    fx.stats->buffered_until_us = fx.player->position_us() + 2'000'000;
    fx.player->step();
    REQUIRE(!fx.player->buffering());
    CHECK_EQ(fx.sink->flush_count(), flushes);
    CHECK(fx.sink->written_frames() >= written);   // 环里的音频没被清掉
}

// Seek 缓冲中 seek 首帧照常立即呈现，且不写音频。
TEST_CASE(buffering_seek_first_frame_presents_during_seek_buffering) {
    auto fx = make_buffering_fixture(BufferPolicy{});
    REQUIRE(fx.player != nullptr);
    REQUIRE(leave_startup(fx));
    REQUIRE(fx.player->seek(0) == SYP_OK);
    REQUIRE(fx.player->buffering_reason() == BufferingReason::Seek);
    fx.stats->buffered_until_us = 0;   // 水位不满足：全程保持 Seek 缓冲
    const int64_t written = fx.sink->written_frames();
    const size_t  shown   = fx.renderer->shown().size();
    bool presented = false;
    for (int i = 0; i < 200 && !presented; ++i) {
        const PlayOutcome o = fx.player->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        CHECK(o.kind != PlayOutcome::Kind::Queued);
        if (o.kind == PlayOutcome::Kind::Presented) presented = true;
    }
    CHECK(presented);
    CHECK(fx.player->buffering_reason() == BufferingReason::Seek);
    CHECK_EQ(fx.renderer->shown().size(), shown + 1);
    CHECK_EQ(fx.sink->written_frames(), written);
}

// ---------------------------------------------------------------------
// 【音频轨号在前的文件，seek 后音频不得先于 seek 首帧写入 sink】
//
// 线程模式下 Seek 缓冲很快满足（加载线程几毫秒就攒够 500ms），离开缓冲后同一次
// step() 里音频优先分支先 Queued 了新位置的音频；随后 seek 首帧呈现时
// present_first_frame_after_seek() 的 flush(pts) 抹掉这些音频并把时钟复位到关键帧
// pts——但 Pipeline 已经交出的下一帧音频领先了被抹掉的那 ~200ms，音频领先一直
// 保持到下一次 seek（实测 +183ms）。同步模式 Seek 缓冲要走几步、音频轨号在后，
// 实测不触发。修复：seek 首帧呈现前（just_sought_ && !seek_rebased_）扣住音频。
//
// 素材：视频 6 秒 25fps、GOP 2 秒（关键帧 0/2/4 秒），音频 7 秒；-map 音频在前，
// 音频轨号 0、视频轨号 1（线程模式 Pipeline::step() 按轨号升序解码，音频先解）。
// seek(3.9s) 落在 2.0s 关键帧上。
static std::string synth_audio_first_gop2s(const std::string& dir) {
    const std::string v = dir + "/af_v.mp4", a = dir + "/af_a.m4a", out = dir + "/audio_first.mp4";
    std::ostringstream vc, ac, mc;
    vc << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
       << "-f lavfi -i \"testsrc2=size=64x64:rate=25:duration=6\" "
       << "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 50 -keyint_min 50 -sc_threshold 0 \""
       << v << "\" > /dev/null 2>&1";
    ac << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
       << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=7\" "
       << "-c:a aac -ar 44100 -ac 2 \"" << a << "\" > /dev/null 2>&1";
    mc << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
       << "-i \"" << v << "\" -i \"" << a << "\" -map 1:a -map 0:v -c copy \"" << out
       << "\" > /dev/null 2>&1";
    if (std::system(vc.str().c_str()) != 0 || std::system(ac.str().c_str()) != 0 ||
        std::system(mc.str().c_str()) != 0) return std::string();
    return out;
}

static void run_audio_first_seek_case(const char* name, bool threaded, const BufferPolicy& policy) {
    syp::test::Watchdog wd(name, 30000, 60000);
    REQUIRE(ffmpeg_available());
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_audio_first_gop2s(tmp.path);
    REQUIRE(!path.empty());

    PipelineConfig cfg;
    cfg.demux_thread = threaded;
    syp_status err   = SYP_OK;
    auto pipeline    = Pipeline::create_file(path, cfg, &err);
    REQUIRE(pipeline != nullptr);
    const int32_t a_idx = find_managed_audio_track(*pipeline);
    const int32_t v_idx = find_managed_video_track(*pipeline);
    REQUIRE(a_idx >= 0 && v_idx >= 0);
    REQUIRE(a_idx < v_idx);   // 素材前提：音频轨号在前

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    syp::test::FakeAudioSink* sink_ptr = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto tp = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer), nullptr,
                                  &err, policy);
    REQUIRE(tp != nullptr);
    REQUIRE(tp->video_track_index() == v_idx);
    REQUIRE(tp->clock_kind() == ClockKind::Audio);

    // 先正常播 0.5 秒，确保 seek 发生在稳态播放中。
    for (int i = 0; i < 200'000 && tp->position_us() < 500'000; ++i) {
        REQUIRE(tp->step().kind != PlayOutcome::Kind::Error);
        if (tp->buffering()) std::this_thread::yield();
        sink_ptr->advance(1000);
    }
    REQUIRE(tp->position_us() >= 500'000);

    REQUIRE(tp->seek(3'900'000) == SYP_OK);
    const int64_t written_at_seek = sink_ptr->written_frames();   // seek 的 flush 不清计数
    int64_t queued_before_first   = 0;
    int64_t first_video_pts       = AV_NOPTS_VALUE;
    int64_t first_audio_after     = AV_NOPTS_VALUE;
    int64_t written_at_first      = -1;
    for (int i = 0; i < 200'000 && first_audio_after == AV_NOPTS_VALUE; ++i) {
        const PlayOutcome o = tp->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        REQUIRE(o.kind != PlayOutcome::Kind::Eof);
        if (first_video_pts == AV_NOPTS_VALUE) {
            if (o.kind == PlayOutcome::Kind::Queued) ++queued_before_first;
            if (o.kind == PlayOutcome::Kind::Presented && o.track_index == v_idx) {
                first_video_pts  = o.pts_us;
                written_at_first = sink_ptr->written_frames();
            }
        } else if (o.kind == PlayOutcome::Kind::Queued && o.track_index == a_idx) {
            first_audio_after = o.pts_us;
        }
        if (tp->buffering()) std::this_thread::yield();
    }
    REQUIRE(first_video_pts != AV_NOPTS_VALUE);
    REQUIRE(first_audio_after != AV_NOPTS_VALUE);
    std::printf("  [%s] 首帧 pts=%lld 首个音频 pts=%lld 领先=%lldus 首帧前 Queued=%lld\n", name,
                static_cast<long long>(first_video_pts), static_cast<long long>(first_audio_after),
                static_cast<long long>(first_audio_after - first_video_pts),
                static_cast<long long>(queued_before_first));
    // 落点是 2.0s 关键帧，不是请求值（否则这条用例测不到"首帧复位抹音频"的形状）。
    CHECK(first_video_pts >= 1'900'000 && first_video_pts <= 2'100'000);
    // seek 首帧呈现之前没有任何音频写进 sink。
    CHECK_EQ(queued_before_first, int64_t{0});
    CHECK_EQ(written_at_first, written_at_seek);
    // 首个写入的音频贴着首帧 pts。界 ±50ms：AAC 一帧 1024 样本 ≈ 23.2ms@44.1kHz，
    // 解封装按文件位置落点，音频首包可能早/晚于视频关键帧至多一两帧（mov 交织粒度），
    // 另留一帧余量；修复前实测领先 ~200ms（被 flush 抹掉的那批音频的时长），远在界外。
    const int64_t lead = first_audio_after - first_video_pts;
    CHECK(lead >= -50'000 && lead <= 50'000);
}

TEST_CASE(seek_audio_first_file_threaded_buffering_holds_audio_until_first_frame) {
    run_audio_first_seek_case("seek_audio_first_file_threaded_buffering", true, BufferPolicy{});
}

TEST_CASE(seek_audio_first_file_threaded_no_buffering_holds_audio_until_first_frame) {
    run_audio_first_seek_case("seek_audio_first_file_threaded_no_buffering", true,
                              BufferPolicy::disabled());
}

TEST_CASE(seek_audio_first_file_sync_buffering_holds_audio_until_first_frame) {
    run_audio_first_seek_case("seek_audio_first_file_sync_buffering", false, BufferPolicy{});
}

TEST_CASE(seek_audio_first_file_sync_no_buffering_holds_audio_until_first_frame) {
    run_audio_first_seek_case("seek_audio_first_file_sync_no_buffering", false,
                              BufferPolicy::disabled());
}

// 【扣音频的出口】视频轨失败后 seek 首帧永远不会来，不得永久扣住音频。
// 硬解模式 + 不挂设备的假后端（supports/prepare 都成功），第一次送包时 hwaccel 初始化
// 失败 → 视频轨 track_failed()（同 test_pipeline 的 run_hw_mode_without_device）。
static void run_video_failed_seek_case(const char* name, bool threaded) {
    syp::test::Watchdog wd(name, 30000, 60000);
    REQUIRE(ffmpeg_available());
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_audio_first_gop2s(tmp.path);
    REQUIRE(!path.empty());

    syp::test::FakeHwBackend be;
    PipelineConfig cfg;
    cfg.demux_thread = threaded;
    cfg.video_decode = syp::media::VideoDecodeMode::Hardware;
    cfg.hw_backend   = &be;
    syp_status err   = SYP_OK;
    auto pipeline    = Pipeline::create_file(path, cfg, &err);
    REQUIRE(pipeline != nullptr);
    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    syp::test::FakeAudioSink* sink_ptr = sink.get();
    auto tp = TrackPlayer::create(std::move(pipeline), std::move(sink),
                                  std::make_unique<syp::test::FakeRenderer>(nullptr), nullptr, &err,
                                  BufferPolicy{});
    REQUIRE(tp != nullptr);
    REQUIRE(tp->video_track_index() >= 0);

    REQUIRE(tp->seek(3'900'000) == SYP_OK);
    int64_t queued    = 0;
    int64_t presented = 0;
    for (int i = 0; i < 20'000 && queued < 10; ++i) {
        const PlayOutcome o = tp->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Queued) ++queued;
        if (o.kind == PlayOutcome::Kind::Presented) ++presented;
        if (tp->buffering()) std::this_thread::yield();
        sink_ptr->advance(1000);
    }
    CHECK_EQ(presented, int64_t{0});   // 前提：视频轨确实没出任何帧
    CHECK(queued >= 10);               // 音频没有被"等 seek 首帧"永久扣住
}

TEST_CASE(seek_with_failed_video_track_does_not_hold_audio_forever_sync) {
    run_video_failed_seek_case("seek_with_failed_video_track_sync", false);
}

TEST_CASE(seek_with_failed_video_track_does_not_hold_audio_forever_threaded) {
    run_video_failed_seek_case("seek_with_failed_video_track_threaded", true);
}

// ---------------------------------------------------------------------
// TrackPlayer 接线音量与显示几何
// ---------------------------------------------------------------------
//
// 范围说明：音量/静音/gravity 状态"跨 open 保留"不在本类
// 范围——TrackPlayer::create() 是静态工厂，桥每次 open 都新建一个实例，
// sink/renderer 随之新建，本类物理上不可能让状态跨越两次 create() 存活。
// 这里只测"本实例内"：设置与读回相互独立、create() 把默认值下发到新
// 建的 sink/renderer 上、几何缝按轨类型正确触发或不触发。跨 open 的持
// 久化是桥层的范围。

// create() 结束前把默认值（音量 1.0、AspectFit）落到这次 open 新建的
// sink / renderer 上——即便调用方一次都没调 set_volume()/set_gravity()，
// 新 sink 的增益、新 renderer 的填充方式也必须是确定的默认值，不是
// "沿用底层实现自己的默认构造值"（那样跟 TrackPlayer 自己的 volume_/
// gravity_ 初值不一致时不会被任何断言发现）。
TEST_CASE(create_pushes_default_gain_and_gravity_to_the_new_instances) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio(tmp.path);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);

    auto  sink         = std::make_unique<syp::test::FakeAudioSink>();
    auto* sink_ptr     = sink.get();
    auto  renderer     = std::make_unique<GravityCountingRenderer>();
    auto* renderer_ptr = renderer.get();

    auto player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                       nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);

    CHECK_EQ(sink_ptr->gain(), 1.0);
    CHECK(sink_ptr->gain_calls() >= 1);
    // gravity() 起始值故意不是 AspectFit（见 GravityCountingRenderer 上方
    // 注释）：这条真的能证明 create() 调用过 set_gravity(AspectFit)，
    // 不是"renderer 自己的默认值恰好一致"这种假阳性。
    CHECK(renderer_ptr->gravity() == syp::media::Gravity::AspectFit);
    CHECK(renderer_ptr->calls() >= 1);
}

// 【InitialSettings 必须在 sink_->open() 之前落到 sink 上】
//
// 断言读的是 FakeAudioSink::gain_at_open()（open() 那一刻 sink 手里的增益，
// 对应真身 AudioUnitSink::open() 的 gain_current 快照），不是 gain()：
// "先以 1.0 open、再改成 0"与"一开始就是 0"的最终 gain() 都是 0，只有前者
// 会让真身开头约 15ms 近满音量出声——gain() 区分不了，gain_at_open() 能。
//
// 期望值全是手写字面量：静音 ⇒ 0.0（静音时 sink 收到的恒是 0）；
// 不静音 ⇒ 0.42 原样；缺省 ⇒ 1.0；2.0 ⇒ 夹到 1.0；NaN ⇒ 0.0（set_volume 口径）。
// gravity 用 GravityCountingRenderer（初值故意是 Resize）验"InitialSettings
// 的 gravity 真的落到了新 renderer 上"。
TEST_CASE(create_applies_initial_settings_before_the_sink_opens) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_video_audio(tmp.path);
    REQUIRE(!path.empty());

    struct Opened {
        std::unique_ptr<TrackPlayer> player;
        syp::test::FakeAudioSink*    sink     = nullptr;
        GravityCountingRenderer*     renderer = nullptr;
    };
    auto open_with = [&](const InitialSettings* init) {
        Opened     o;
        syp_status err      = SYP_OK;
        auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
        if (pipeline == nullptr) return o;
        auto sink     = std::make_unique<syp::test::FakeAudioSink>();
        auto renderer = std::make_unique<GravityCountingRenderer>();
        o.sink        = sink.get();
        o.renderer    = renderer.get();
        o.player = (init != nullptr)
                       ? TrackPlayer::create(std::move(pipeline), std::move(sink),
                                             std::move(renderer), nullptr, &err,
                                             BufferPolicy::disabled(), *init)
                       : TrackPlayer::create(std::move(pipeline), std::move(sink),
                                             std::move(renderer), nullptr, &err,
                                             BufferPolicy::disabled());
        return o;
    };

    {   // 持久静音 + 音量 0.42：open 那一刻就是 0，静音不擦掉音量
        InitialSettings init;
        init.volume  = 0.42;
        init.muted   = true;
        init.gravity = syp::media::Gravity::AspectFill;
        auto o = open_with(&init);
        REQUIRE(o.player != nullptr);
        CHECK_EQ(o.sink->gain_at_open(), 0.0);
        CHECK_EQ(o.player->volume(), 0.42);
        CHECK(o.player->muted());
        CHECK(o.renderer->gravity() == syp::media::Gravity::AspectFill);
    }
    {   // 不静音 0.42：open 那一刻就是 0.42
        InitialSettings init;
        init.volume = 0.42;
        auto o = open_with(&init);
        REQUIRE(o.player != nullptr);
        CHECK_EQ(o.sink->gain_at_open(), 0.42);
        CHECK(!o.player->muted());
        CHECK(o.renderer->gravity() == syp::media::Gravity::AspectFit);
    }
    {   // 不传：缺省 1.0（既有调用点的原行为）
        auto o = open_with(nullptr);
        REQUIRE(o.player != nullptr);
        CHECK_EQ(o.sink->gain_at_open(), 1.0);
        CHECK(o.renderer->gravity() == syp::media::Gravity::AspectFit);
    }
    {   // 桥原样透传未夹取的值：create() 自己夹（2.0 ⇒ 1.0）
        InitialSettings init;
        init.volume = 2.0;
        auto o = open_with(&init);
        REQUIRE(o.player != nullptr);
        CHECK_EQ(o.sink->gain_at_open(), 1.0);
        CHECK_EQ(o.player->volume(), 1.0);
    }
    {   // NaN ⇒ 0.0（与 set_volume 同一口径）
        InitialSettings init;
        init.volume = std::numeric_limits<double>::quiet_NaN();
        auto o = open_with(&init);
        REQUIRE(o.player != nullptr);
        CHECK_EQ(o.sink->gain_at_open(), 0.0);
        CHECK_EQ(o.player->volume(), 0.0);
    }
}

// set_volume()/set_muted() 在同一实例内相互独立：sink 只有一个增益
// 旋钮，收到的恒是 muted ? 0.0 : volume_；静音不擦掉
// volume_，取消静音后原样恢复。
TEST_CASE(set_volume_and_set_muted_are_independent_within_one_instance) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player != nullptr);

    fx.player->set_volume(0.3);
    CHECK_EQ(fx.sink->gain(), 0.3);
    CHECK_EQ(fx.player->volume(), 0.3);

    fx.player->set_muted(true);
    CHECK_EQ(fx.sink->gain(), 0.0);
    CHECK_EQ(fx.player->volume(), 0.3);   // 静音不擦掉音量
    CHECK(fx.player->muted());

    fx.player->set_muted(false);
    CHECK_EQ(fx.sink->gain(), 0.3);       // 回到原值
    CHECK(!fx.player->muted());
}

// set_volume() 越界与非有限值的夹取：>1 → 1，<0 → 0，NaN → 0，
// +inf → 1，-inf → 0——跟 FakeAudioSink::set_gain()/真身
// AudioUnitSink::set_gain() 同一口径（见 fake_audio_sink.h 该方法上方
// 注释、audio_unit_sink.mm::set_gain() 的注释）：不是"非有
// 限值一律夹到 0"，NaN 才需要显式特判（NaN 参与的任何比较恒为 false，
// 会绕过钳位比较），±inf 靠既有的 `<0`/`>1` 钳位语句自然夹到 0/1。
// 这里断言的是 TrackPlayer 自己的 volume_ 落地值，不是 sink 二次夹取
// 后的值（正常输入下两者数值上巧合一致，是各自独立实现的结果，不是
// 同一份代码）。
TEST_CASE(set_volume_clamps_out_of_range_and_non_finite_values) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player != nullptr);

    fx.player->set_volume(2.0);
    CHECK_EQ(fx.player->volume(), 1.0);
    CHECK_EQ(fx.sink->gain(), 1.0);

    fx.player->set_volume(-1.0);
    CHECK_EQ(fx.player->volume(), 0.0);
    CHECK_EQ(fx.sink->gain(), 0.0);

    fx.player->set_volume(std::numeric_limits<double>::quiet_NaN());
    CHECK_EQ(fx.player->volume(), 0.0);
    CHECK_EQ(fx.sink->gain(), 0.0);

    fx.player->set_volume(std::numeric_limits<double>::infinity());
    CHECK_EQ(fx.player->volume(), 1.0);
    CHECK_EQ(fx.sink->gain(), 1.0);

    fx.player->set_volume(-std::numeric_limits<double>::infinity());
    CHECK_EQ(fx.player->volume(), 0.0);
    CHECK_EQ(fx.sink->gain(), 0.0);
}

// set_gravity() 转发给 renderer；create() 时已经下发过一次默认
// AspectFit（跟 create_pushes_default_gain_and_gravity_to_the_new_instances
// 重复这一条断言，是故意的——那条钉的是"create() 做了什么"，这条钉的是
// "调用方随后再调 set_gravity() 真的会覆盖它"，两件事独立，各自的变异
// 都需要各自的用例才能被杀死）。
TEST_CASE(set_gravity_forwards_to_the_renderer) {
    auto fx = make_fixture_with_audio_and_video();
    REQUIRE(fx.player != nullptr);
    CHECK(fx.renderer->gravity() == syp::media::Gravity::AspectFit);

    fx.player->set_gravity(syp::media::Gravity::AspectFill);
    CHECK(fx.renderer->gravity() == syp::media::Gravity::AspectFill);
}

// 旋转素材：create() 恰好调一次 set_source_geometry()，参数等于该轨的
// sar_num/sar_den/rotation_deg；video_display_size() 等于手算字面量。
//
// 期望值来自实测，不是从 display_size() 反推（明令禁止用
// display_size() 算期望值——那样生产代码与期望值走同一个函数，一起错
// 一起绿，早先就在这上面栽过一次）：
//   - tests/test_demuxer.cpp track_info_reports_display_rotation_ccw270_needs_cw90
//     已经钉死 synth_video_with_display_rotation(dir, 270) ⇒
//     TrackInfo{width=640, height=360, rotation_deg=90}。
//   - SAR 额外用一个独立的 C 程序直接读 codecpar->sample_aspect_ratio
//     确认过（不经过 ffprobe 的展示层归一化）：这份素材是 1/1，libx264
//     默认在 VUI 里写 "Square"，不是"未知"的 0/1。
//   - 640 * 1/1 = 640；顺时针 90 ⇒ 宽高互换 ⇒ 360x640。
TEST_CASE(open_pushes_source_geometry_once_for_rotated_video) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = syp::test::synth_video_with_display_rotation(tmp, 270);
    REQUIRE(!path.empty());

    auto fx = make_geometry_fixture(path);
    REQUIRE(fx.player != nullptr);

    CHECK_EQ(fx.renderer->geometry_calls(), 1);
    const auto geo = fx.renderer->last_geometry();
    CHECK_EQ(std::get<0>(geo), int32_t{1});    // sar_num
    CHECK_EQ(std::get<1>(geo), int32_t{1});    // sar_den
    CHECK_EQ(std::get<2>(geo), int32_t{90});   // rotation_deg

    const auto ds = fx.player->video_display_size();
    CHECK_EQ(ds.width, int32_t{360});
    CHECK_EQ(ds.height, int32_t{640});
}

// 非方像素素材：重点是 SAR 拉伸后的宽度。
//
// synth_video_with_sar(dir, 8, 9) 按 640*9/8=720 反推编码宽度（见
// synth_media.h 该函数上方注释），本身就保证 SAR 校正后的显示宽度恒为
// 640；这里独立用 C 程序读 codecpar 确认过实际编码尺寸是 720x360、
// SAR 8:9（720 * 8/9 = 640.0，整除，无需处理四舍五入），rotation_deg
// 为 0（这个合成函数不加任何旋转选项）——不交换宽高。
TEST_CASE(open_pushes_source_geometry_once_for_non_square_pixels) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = syp::test::synth_video_with_sar(tmp, 8, 9);
    REQUIRE(!path.empty());

    auto fx = make_geometry_fixture(path);
    REQUIRE(fx.player != nullptr);

    CHECK_EQ(fx.renderer->geometry_calls(), 1);
    const auto geo = fx.renderer->last_geometry();
    CHECK_EQ(std::get<0>(geo), int32_t{8});
    CHECK_EQ(std::get<1>(geo), int32_t{9});
    CHECK_EQ(std::get<2>(geo), int32_t{0});

    const auto ds = fx.player->video_display_size();
    CHECK_EQ(ds.width, int32_t{640});
    CHECK_EQ(ds.height, int32_t{360});
}

// 纯音频源（连封面图都没有）：没有视频轨，几何缝完全不触发，
// video_display_size() 保持默认 {0,0}。
TEST_CASE(audio_only_source_reports_no_display_size_and_skips_the_geometry_seam) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_audio_only(tmp.path);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);
    REQUIRE(find_managed_video_track(*pipeline) < 0);   // 素材里确实没有视频轨

    auto  sink          = std::make_unique<syp::test::FakeAudioSink>();
    auto  renderer      = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr  = renderer.get();
    auto  player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                        nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);

    CHECK_EQ(renderer_ptr->geometry_calls(), 0);
    CHECK_EQ(player->video_display_size().width, int32_t{0});
    CHECK_EQ(player->video_display_size().height, int32_t{0});
}

// 封面图轨（attached_pic）不参与显示尺寸：它没有被绑成主视频轨
// （video_track_index() < 0，既有回归 cover_art_is_not_bound_as_the_video_track
// 已经钉住这一半），几何缝因此同样不触发。
TEST_CASE(attached_pic_track_does_not_become_the_display_size) {
    if (!ffmpeg_available()) {
        tiny_test::fail(__FILE__, __LINE__, "ffmpeg CLI 不可用，无法生成用例素材");
        return;
    }
    TempDir tmp;
    REQUIRE(!tmp.path.empty());
    const std::string path = synth_audio_with_cover_art(tmp.path);
    REQUIRE(!path.empty());

    syp_status err      = SYP_OK;
    auto       pipeline = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(pipeline != nullptr);

    auto  sink          = std::make_unique<syp::test::FakeAudioSink>();
    auto  renderer      = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto* renderer_ptr  = renderer.get();
    auto  player = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                        nullptr, &err, BufferPolicy::disabled());
    REQUIRE(player != nullptr);
    REQUIRE(player->video_track_index() < 0);

    CHECK_EQ(renderer_ptr->geometry_calls(), 0);
    CHECK_EQ(player->video_display_size().width, int32_t{0});
    CHECK_EQ(player->video_display_size().height, int32_t{0});
}

int main() { return tiny_test_main(); }
