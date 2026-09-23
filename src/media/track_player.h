// track_player.h — 同步核心：把 Pipeline、主时钟、音频输出、视频呈现
// 串成一条链，对外只暴露一个 step()（与 Pipeline::step() 同构：调用方
// 驱动，一次推进一个单位，如实报告做了什么，无时钟依赖之外的不确定性）。
//
// step() 的调度顺序固定，按此顺序判定（第 0 步
// 与"暂停仍驱动 Pipeline"两条是修掉两个静默稳定错之后补的，音频
// 背压不再拖住视频是发现原设计骨架有误之后改的）：
//
//   0. 排空所有不受管的轨（unbound_tracks_：第二条音轨、封面图这类
//      attached_pic 视频流……）——Pipeline 按 codec_type 托管**每一条**
//      video/audio 流（pipeline.cpp），不是只托管 TrackPlayer 绑定的
//      那一条。不排空的话，任何双音轨文件、或带封面图的 mp4（这在真实
//      mp4 里极其常见）都会在那条没人碰的轨的 FrameQueue 堆满时触发
//      Pipeline 的联合背压（pipeline.h 顶部警告：任何一条被管理轨一旦
//      队列堆满，demux 分支就整体停摆，拖死其它轨），整条管线卡死——
//      这是此前实测出的严重缺陷（双音轨 2 秒素材应出 50
//      帧，3000 次 step() 只呈现 2 帧）。这一步无论暂不暂停都要做。
//   0.5 缓冲状态机（evaluate_buffering()，降级检查之后、第 0 步之前）：
//      按 Pipeline::buffer_stats() 与当前位置进入/离开缓冲（规则见 BufferPolicy）。
//      缓冲中与暂停走同一个分支（下面第 1 步）：首帧/seek 首帧照常立即呈现、驱动
//      Pipeline、报 Waiting；时钟冻结（sink pause + 系统时钟 pause），不 flush。
//   1. 暂停或缓冲中：paused_ || buffering() 为真时跳过下面第 2、3 步（不呈现、不写音频），
//      但仍然要驱动一次 Pipeline::step()（见第 4 步）——明确要求
//      "暂停期间不应停止 Pipeline 的推进……恢复时才能立即出
//      画"。原实现在 paused_ 为真时
//      直接 `return Waiting`，第 4 步永远够不着，恢复后第一次 step()
//      还是黑的。
//      暂停中并非对视频完全不闻不问：这一步开头会先
//      检查两件"快速出画"的事——seek 后第一帧（just_sought_，语义不变，
//      只是不再被"暂停"挡住）、打开后第一帧（preview_pending_，只在
//      暂停中生效一次，出的这一帧不动时钟、不 flush sink，纯粹是"让用户
//      暂停打开时不对着黑屏"）。哪一个命中且真的拿到了帧，就直接把这
//      一帧的呈现结果当作这一步的产出返回（不再往下走）；两者都没命中，
//      或命中了但这一刻还没帧可取、或渲染器 BUSY（帧与标记都保留，
//      下次重试），才继续走"驱动 Pipeline + 折叠成 Waiting"这条老路。
//      preview_pending_ 被这一帧消费掉之后不会再触发第二次——恢复播放
//      后走的是正常三分支判定，从下一帧开始（decided at planning
//      time：预览帧本就已经"出过画"，不重复算）。
//   2. 音频优先：音频是时钟，喂不上就没有正确的时间基准。有音频帧、且
//      sink 可用（已成功 open()、且没有 failed()）→ 尝试 write()；写
//      成功返回 Queued。**写不进去（背压）不再直接 return Blocked**——
//      帧留在 pending_audio_ 里，置一个局部 audio_blocked 标志，跳出
//      音频循环、继续走视频判定（下面第 3 步）；音频、视频这一步都没
//      产出时才真正返回 Blocked。原本的条件是"且 write() 能接受"，
//      音频写不进去不
//      该拖住视频——两者有各自独立的队列和时钟判据。（最初的设计草案
//      写的是"写不进去就 return Blocked"，那是写错的，这里改正。）
//      没有可用 sink（未提供、open() 失败、或中途降级失败）时，音频帧
//      仍然要被 pop 并丢弃——理由同第 0 步，这条轨也要按"不能选择性
//      忽略"处理。
//      seek 之后、seek 首帧复位（seek_rebased_）之前 sink 可用也不写：
//      帧留在 pending_audio_、跳出看视频（同 audio_blocked 的形状，但最终报 Waiting
//      不报 Blocked）；否则首帧复位的 flush 抹掉已写音频、造成持续音频领先。视频轨
//      失败/排空时 step() 开头清 just_sought_，扣音频随之解除。
//   2.5 第二次降级检查（见本文件末尾"降级检查"一节）：
//      音频判定之后、视频判定之前，若 clock_kind_ 仍是 Audio 而
//      sink_->failed() 已为真 → 就地 degrade_to_system_clock()。
//      **降级点是两个，不是一个**——第 0 步之前那次只能看见"上一次
//      step() 之后到这一次之前"发生的失败，而 sink_ 在 write() 路径上
//      有四处会把自己置成 failed（AudioUnitSink 的 ensure_swr_for()
//      失败、swr_get_out_samples()<0 ×2、swr_convert()<0），那四处全在
//      上面第 2 步、也就是第一次检查**之后**。少了这一次检查，同一次
//      step() 里 clock_ 就是一具尸体：AudioClock::now_us() 转发
//      sink_->played_us()，失败时按接口契约返回 AV_NOPTS_VALUE
//      （= INT64_MIN），第 3 步的 diff = pts − now_us() 是**有符号整数
//      溢出（UB）**，且溢出后的 diff 恒小于 -kDropThresholdUs，这一次
//      step() 会把 kMaxDropsPerStep 的丢帧预算打满、丢掉 8 帧本该呈现
//      的真实帧，还把 pts_us = INT64_MIN 当合法结果交给调用方。
//      这是用 UBSan 实测出来的严重缺陷。回归用例：
//      tests/test_track_player.cpp
//      sink_failing_inside_write_degrades_before_video_branch_same_step。
//   3. 视频：取一帧，diff = frame.pts_us() − clock_->now_us()，
//      **按此顺序判定，三分支穷尽**：
//        diff > kPresentWindowUs   → 早了：不呈现，帧留在队列，Waiting
//        diff < -kDropThresholdUs  → 太迟：丢弃（不呈现），Dropped
//        其余一律                   → 呈现，Presented；渲染器 BUSY 时帧留在
//                                     pending_video_，形状同"早了"（不消费、
//                                     跳出、驱动 Pipeline、报 Waiting），下次
//                                     step() 重新判定后重试
//      第三条必须写成"其余一律"而不是 `|diff| <= kPresentWindowUs`——
//      kPresentWindowUs 与 kDropThresholdUs 不相等是刻意的滞回（时钟
//      抖动会让边界上的帧在"呈现"/"丢弃"间反复跳），若第三分支也写成
//      对称区间，落在 (-kDropThresholdUs, -kPresentWindowUs) 的帧
//      （迟到 40~80ms）会一个分支都不匹配。
//      一次 step() 最多丢 kMaxDropsPerStep 帧——没有这个上限，一次
//      step() 可能丢光整条队列，"一次推进一个单位"就不成立了
//      （Pipeline::step() 内部循环用 packets->size()+2 做上限是同一个
//      理由，见 pipeline.h 顶部注释）。
//      **"早了"分支不 return**（跟上面暂停分支短路是同一个形状的缺陷、
//      同一个思路的修法）：只是记下"这一步该报 Waiting、带上这一帧的
//      track_index/pts"，跳出视频判定，仍然要走到下面第 4 步驱动
//      Pipeline::step()。不这样做的后果不是理论上的——这一刻音频优先
//      分支（第 2 步）大概率也没有产出（要么它自己的队列真的空了，要么
//      写不进 sink），若视频这里也直接 return，第 4 步永远够不着，
//      Pipeline 的两条 FrameQueue、以及 sink 的环形缓冲都不会再有新
//      输入；`set_speed()`/`seek()` 都会 flush() 掉环，一旦这一刻恰好
//      也撞上"音频 FrameQueue 空"，时钟从此再没有任何输入能被推进——
//      这一帧永远"早"，回到这个分支的条件永远成立，**闭环，永久**。
//      详见本文件顶部长注释"pending_audio_ / pending_video_"一节。
//      回归用例：
//      tests/test_track_player.cpp
//      step_does_not_livelock_when_audio_queue_empty_and_video_early。
//      真正丢弃的每一帧（`++dropped_frames_` 那一行，不
//      含"撞了 kMaxDropsPerStep 上限、帧还留着"那条早退；BUSY 本身不丢帧，
//      只有 BUSY 重试到迟到超过 kDropThresholdUs、被这条迟到路径丢弃时才计入）
//      都会喂进追帧窗口
//      （note_late_drop()）：1 秒内攒够 kCatchupDropsToEnable 次迟到丢帧
//      就打开追帧（Pipeline::set_video_catchup(true)，跳非参考帧追赶），
//      追帧期间连续 kCatchupOnTimeToDisable 帧按时呈现（note_on_time_
//      present()）则关闭；seek()/pause() 复位。见 catching_up() 与本文件
//      下方 kCatchupDropsToEnable 等常量。
//   4. 暂停中、或音频、视频这一步都没活干（音频背压、视频早到都算
//      "没活干"——但两者各自留下的局部信号 audio_blocked / 上面第 3
//      步记的"早到"信息，第 5 步会用到，不是被这一步驱动 Pipeline 的
//      动作抹掉）→ 驱动 Pipeline::step() 补充帧。暂停中这一步的结果
//      一律折叠成 Waiting（Error 除外，如实转述）——调用方不需要在
//      暂停时区分"Pipeline 这一下到底做了什么"，只需要知道"还在推进、
//      没出错"。
//   5. 非暂停时，第 4 步驱动完 Pipeline 之后该报什么，优先级（替换掉
//      "都不行 → 如实转述 Blocked/Eof/Error"这句过时表述）：
//        Pipeline::step() 返回 Error → 如实转述 Error，最高优先级。
//        video 早到（第 3 步记下的信号）→ Waiting，带上那一帧的
//          track_index/pts。**必须排在 Pipeline 自己的 Eof 前面**：
//          Pipeline 的 Eof 判据只问它自己的 PacketQueue/FrameQueue 是否
//          排空，对 TrackPlayer 自己攥着、已经从队列里 pop 出来但还没
//          呈现的 pending_video_ 一无所知——原样转述 Eof 会让调用方以为
//          播放结束，这一帧永远不会被呈现。这条冲突在这处修复之前无法
//          触发（早到分支直接 return，压根走不到这里问 Eof），是
//          主动打开这条路径之后，反向自检当场撞见的第二个坑，
//          见 tests/test_track_player.cpp
//          early_frame_waits_and_is_not_presented 的注释。
//        audio_blocked → Blocked，同一个理由，同一优先级顺序：
//          pending_audio_ 里还压着一帧没写出去，原样转述 Eof 会让它
//          再也没机会被 write()。
//        都没有 → 如实转述 Pipeline::step() 自己的判定：
//          Eof → Eof，Blocked → Blocked，否则（这一次驱动确实做了事，
//          但对外可观测的"吃到了什么"要等下一次 step() 才能报出来）
//          → Waiting，不虚报。
//
// 上面这套顺序判定要守住的不变量，
// 显式写下来，比让后人从九条分支里自己反推更可靠：**每次 step() 要么
// 产出一个可观测结果（Queued/Presented/Dropped/Error——即"这一步真的
// 干了点什么，调用方看得见"），要么驱动一次 pipeline_->step()，绝不
// 两者皆无。** 那条活锁就是这条不变量被破坏的后果（视频早到分支
// 既不产出、也不驱动 Pipeline）；下次有人往 step() 里加分支时，只要
// 守住这条不变量，就不会重新引入同一形状的问题。
//
// 降级检查有**两处**，第 0 步之前一处、第 2.5 步一处：sink 中途失败
// （IAudioSink::failed()）之后，本轮判定就要换用系统时钟，不能拿一个已
// 失效的时钟去决定丢不丢帧——见 degrade_to_system_clock()。为什么一处
// 不够、以及只查一次会产生什么后果，见上面第 2.5 步；一句话：**守卫检查
// 的时刻必须晚于唯一能让它失效的那个动作**，而让 sink_ 失效的四个动作
// 就在第 2 步的 write() 里。**判据不只看 sink_->failed()**：音频轨自身
// 解码失败（Pipeline::track_failed(audio_track_)）也要触发降级——
// pipeline.h 明确要求调用方轮询
// track_failed()，原实现没这么做，音频轨失效时 sink_->failed() 仍是
// false，环放空后 AudioClock 的读数停止推进，所有视频帧永远"早了"，
// 表现为画面静止、无错误、无降级——又一条静默稳定错。
//
// 这两个检查点除了失败降级，还紧跟着一处"音频播完
// 切钟"检查（maybe_switch_on_audio_end()），两者同位：音频轨比视频轨短时，
// 音频轨已排空（Pipeline::track_drained）、pending_audio_ 为空、sink 缓冲已被
// 设备取空（IAudioSink::output_drained()，与设备延迟无关）→ 切到系统时钟，基准取当时的
// 音频时钟读数（零跳变），让视频尾部照常播完并报 Eof。此前音频耗尽后
// AudioClock 冻结，尾部视频帧永远"早了"，step() 永远 Waiting。
// clock_switch_reason() 区分两种切钟：AudioFailed 不可逆；AudioEnded 在
// seek() 时恢复 AudioClock（seek 回去音频又有内容可播）。两者共用内核
// switch_to_system_clock()：继承 speed_，暂停中切钟则新时钟也暂停。
//
// ---------------------------------------------------------------------
// pending_audio_ / pending_video_：
//
// pop_frame() 之后如果下游这一刻不收（音频：sink 背压满；视频：还没到
// 呈现时刻、渲染器 BUSY，或已经丢过 kMaxDropsPerStep 帧撞了本轮上限），帧就留在这两
// 个成员里，下次 step() 先看它们，不重新 pop。
//
// 选择让 TrackPlayer 自己持有这两个成员、完全不碰 Pipeline 一个字（不
// 新增 peek_frame_pts()，也不新增"塞回去"的方法），理由：
//   1. 音频侧本来就必须先 pop 才能 write()——IAudioSink::write() 收的
//      是 `const Frame&`，"先 peek 时钟再决定要不要 pop" 这条路子对
//      音频根本不成立；
//   2. 不给 Pipeline 新增契约。新增的契约极易"立起来但没人守"——这条
//      分支上这个模式已经出现多次，不该再添一个；
//   3. Frame 是 move-only，放在 std::optional 里，谁持有所有权在类型
//      上就是确定的。
//
// **seek() 必须清空这两个成员**（本头文件 seek() 声明处再提一遍）：
// 否则 seek 完了还会呈现/写入一帧 seek 之前遗留的旧帧。
//
// ---------------------------------------------------------------------
// 成员声明顺序即析构顺序的反序（C++：成员按声明顺序构造、按声明的逆序
// 析构）——跟 Pipeline 对 Demuxer 的同款处理是同一个原理（pipeline.h：
// demuxer_ 声明在 tracks_ 之前，所以 tracks_ 先析构、demuxer_ 后析构）。
//
// 这里要保证的是"clock_ 必须先于 sink_ 析构"——AudioClock
// （time_source.h）只持有裸指针 `IAudioSink*`，注释写的是"不持有所有权，
// 生命周期由 TrackPlayer 保证"；这条契约要在本类落地，就必须让 clock_
// 在 sink_ 被销毁之前先被销毁。"先析构"意味着"声明在后面"（声明顺序的
// 逆序才是析构顺序）。所以实际声明顺序是：
//
//     pipeline_ → sink_ → renderer_ → clock_
//
// clock_ 声明在最后，因此最先被析构，此时 sink_ 还活着（AudioClock 析构
// 本身不碰 sink_，但这个顺序是"以后有人往 AudioClock 析构里加逻辑"也
// 天然安全的唯一保证）。别在不确认这一点的前提下调换。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include <syplayer/syp_types.h>

#include "media/audio_sink.h"
#include "media/frame.h"
#include "media/pipeline.h"
#include "media/time_source.h"
#include "media/video_renderer.h"

extern "C" {
#include <libavutil/avutil.h>   // AV_NOPTS_VALUE
}

namespace syp::media {

inline constexpr int64_t kPresentWindowUs = 40000;   // ±40ms
inline constexpr int64_t kDropThresholdUs = 80000;   // 迟到超过 80ms 才丢
inline constexpr int32_t kMaxDropsPerStep = 8;

// 追帧丢帧策略：1 秒窗口内迟到丢帧达到这个数就打开追帧
// （Pipeline::set_video_catchup(true)，跳非参考帧追赶）；追帧期间连续这
// 么多帧按时呈现就关闭。见 TrackPlayer::catching_up()、note_late_drop()、
// note_on_time_present()。
inline constexpr int32_t kCatchupDropsToEnable   = 5;
inline constexpr int64_t kCatchupWindowUs        = 1000000;
inline constexpr int32_t kCatchupOnTimeToDisable = 30;

// 主时钟从音频切到系统时钟的原因。AudioEnded 在 seek 后可逆，AudioFailed 不可逆。
enum class ClockSwitchReason { None, AudioFailed, AudioEnded };

// 缓冲原因。与用户暂停正交：时钟运行 ⇔ 未暂停且未缓冲。
enum class BufferingReason { None, Startup, Seek, Stall };

// 缓冲水位策略。默认即生产值；既有测试夹具显式传 disabled()，保持此前语义。
//   · Startup/Seek：create()/seek() 进入，已缓冲 ≥ startup_buffer_ms 离开；
//   · Stall：播放中、未读到文件尾、已缓冲 < rebuffer_trigger_ms 进入，≥ rebuffer_resume_ms 离开；
//   · 任一缓冲在 demux_eof 或 BufferStats::full 时也离开；离开水位按
//     min(水位, PipelineConfig::max_buffer_ms) 生效。
//   · 【控制器约束】进入只由欠载触发，BufferStats::full 只在已经缓冲时充当恢复条件
//     ——线程模式稳态下 full 没有滞回、约每帧翻转一次，拿它的边沿进/出
//     缓冲会让播放器每帧抖动。
//   · 【卡顿闩锁】：若离开缓冲时已缓冲 < 触发线（靠 full/文件尾离开），
//     须先回到触发线以上（或 seek）才允许再次进入 Stall；full 为真时也不进入 Stall。
//     覆盖某轨提前结束钉住 buffered_until、字节上限令 full 常真的场景，与同步模式
//     单轨 128 包即 full 的交织不良文件，否则每隔一步 Stop/Start 音频设备。
//   · 冻结时钟只能冻结 TrackPlayer 自己拥有的时钟（AudioClock 经 sink pause、自有
//     SystemClock 经 pause）；create() 注入了 clock_override 时注入的时钟不受控，
//     缓冲期间它照走，由调用方（测试）负责。
struct BufferPolicy {
    bool    enabled             = true;
    int64_t startup_buffer_ms   = 500;
    int64_t rebuffer_trigger_ms = 100;
    int64_t rebuffer_resume_ms  = 2000;
    static constexpr BufferPolicy disabled() noexcept {
        BufferPolicy p;
        p.enabled = false;
        return p;
    }
};

// create() 的初始设置：音量 / 静音 / 填充方式。
//
// 为什么要在 create() 里就拿到，而不是 create() 之后再 set_volume()/set_muted()：
// create() 在 sink_->open() **之前**调用 apply_gain()，AudioUnitSink::open()
// 在构造 render_ctx_ 那一句里快照 gain_current = gain_target_（
// "open() 时 gain_current = gain_target，首次打开不做淡入"）。如果调用方
// create() 之后才改目标增益，open() 快照到的是默认 1.0，开始出声前 render_cb
// 拿不到样本、advance_gain 按"0 帧不推进"原地不动，于是第一批真实样本从 1.0
// 线性降到目标——持久静音的用户每次 open 都会听到约 15ms 近满音量的声音。
// 这正是此前发现的一个缺陷（桥当时在 create() 之后才重新下发三者）。
//
// volume 按 set_volume() 同一口径夹取（NaN → 0、+inf → 1、-inf/负数 → 0、>1 → 1）；
// 桥这一层原样透传调用方的值（SYPBridge.h -setVolume: 注释），夹取在 create() 里做。
// 缺省值 = 此前 create() 自己落的默认值，既有调用点不传就是原行为。
struct InitialSettings {
    double              volume  = 1.0;
    bool                muted   = false;
    syp::media::Gravity gravity = syp::media::Gravity::AspectFit;
};

struct PlayOutcome {
    enum class Kind {
        Presented,   // 呈现了一帧视频
        Dropped,     // 丢弃了一帧过期视频
        Queued,      // 写入了一帧音频
        Waiting,     // 有帧但还没到呈现时刻（或暂停中）
        Blocked,     // 下游没空位（环形缓冲满 / FrameQueue 满）
        Eof,
        Error,
    };
    Kind       kind        = Kind::Waiting;
    int32_t    track_index = -1;
    int64_t    pts_us      = AV_NOPTS_VALUE;
    syp_status status      = SYP_OK;
};

enum class ClockKind { Audio, System };

class TrackPlayer {
public:
    TrackPlayer(const TrackPlayer&)            = delete;
    TrackPlayer& operator=(const TrackPlayer&) = delete;
    ~TrackPlayer();

    // sink / renderer 允许为空（无音频轨、或只验解码不出画的场景）。
    // sink 为空、或该轨不存在、或 open() 失败 → 自动选用 SystemClock。
    // clock_override 是唯一的测试缝：生产代码传 nullptr，由 create() 按
    // 第 4 节的规则自行选择时钟；测试传 FakeClock。注意：clock_override
    // 只替换"实际拿去调用 now_us() 的那个对象"，不影响 clock_kind() 的
    // 判定——clock_kind() 如实反映"有没有可用的音频轨 + sink"这件事，
    // 跟测试是否注入了一个假时钟无关（否则用假时钟测视频判定时
    // clock_kind() 就会失真）。
    static std::unique_ptr<TrackPlayer> create(std::unique_ptr<Pipeline>       pipeline,
                                               std::unique_ptr<IAudioSink>     sink,
                                               std::unique_ptr<IVideoRenderer> renderer,
                                               std::unique_ptr<TimeSource>     clock_override,
                                               syp_status*                     out_err,
                                               const BufferPolicy&             policy = BufferPolicy{},
                                               const InitialSettings&          initial = InitialSettings{});

    PlayOutcome step();

    // 打断正阻塞着的 IO，此后 step() 一律报 Error(SYP_ERR_CANCELED)。
    // **可在任意线程调用、与 step() 并发**——这是本类
    // 唯一不要求"调用方单线程驱动"的方法：它存在的意义就是让另一条线程
    // 把卡在 step() 里的泵线程叫醒（demo 壳的 close 路径正是这么用）。
    // 各打开路径怎么打断 IO 见 Pipeline::request_abort()。不可逆。
    // 生命周期：不得与 ~TrackPlayer() 重叠。
    void request_abort() noexcept;

    void       play();
    void       pause();
    bool       paused() const noexcept;

    // 四步顺序定死：
    //   1. 记下当前 now_us() 当基准
    //   2. IAudioSink::flush(base)
    //   3. IAudioSink::set_speed(speed)——调用点固定在 flush() 之后
    //   4. 更新 speed_（TrackPlayer 自己的镜像），以及（若拥有）
    //      SystemClock::set_speed()
    // [0.5, 2.0]，越界（含 NaN——NaN 参与比较恒为 false，天然落进
    // "越界"分支，不需要用 std::isnan 特判）返回 SYP_ERR_INVALID_ARG，
    // 范围校验的责任照 time_source.h:28-35 SystemClock::set_speed() 的
    // 写法，留在这一层（IAudioSink::set_speed() 不做防御性检查）。
    syp_status set_speed(double speed);
    double     speed() const noexcept;

    // 四步，顺序定死，失败也要走完后三步（与
    // Pipeline::seek() 同一个立场，见 pipeline.h 顶部注释）：
    //   1. Pipeline::seek(ts_us)
    //   2. IAudioSink::flush(ts_us)（sink 存在时）
    //   3. 时钟基准复位到 ts_us（只对 TrackPlayer 自己拥有的 SystemClock
    //      生效；AudioClock 靠第 2 步的 flush 间接复位；clock_override
    //      场景下 TrackPlayer 拿不到任何"复位基准"的接口，复位是调用方
    //      —— 测试里就是用例自己 —— 的责任，这是 TimeSource 接口本身
    //      只有 now_us() 一个方法带来的必然限制）
    //   4. 清空 pending_audio_ / pending_video_ —— 否则 seek 完还会呈现/
    //      写入一帧 seek 之前遗留的旧帧
    // dropped_frames()/present_failures() 是生命周期累计值，不在复位
    // 范围内——跟 Pipeline::seek() 对 skipped_packets()/track_failed()
    // 的立场一致（那两个"跟当前播放位置无关，不该被 seek 清零"）。
    //
    // 还标记 just_sought_ = (video_track_ >= 0)：没有视频轨（纯音频文件）
    // 时置真也没有消费方——video_track_ < 0 时视频循环的
    // `pop_frame(video_track_)` 恒返回 nullopt，永远走不到"消费这个
    // 标记"那一行，标记会一直悬空地挂着。有视频轨时：
    // seek 落点是最近的关键帧，不是请求的 ts_us，两者通常不同——按正常
    // 三分支判定，第一帧几乎总会落进"早了"分支（关键帧间距常常远大于
    // kPresentWindowUs），表现为 seek 完先黑一下屏。要求
    // "seek 后的第一帧不等时钟，立即呈现，随后把时钟基准设为该帧的实际
    // pts"——just_sought_ 就是让 step() 对 seek 后遇到的第一帧视频跳过
    // 三分支判定、直接呈现，然后用它的真实
    // pts 再复位一次基准（第 3 步用的是请求值 ts_us，这里用真实值纠偏）。
    syp_status seek(int64_t ts_us);

    int64_t    position_us() const noexcept;   // 即当前 TimeSource::now_us()
    ClockKind  clock_kind() const noexcept;    // 降级发生后如实变化
    ClockSwitchReason clock_switch_reason() const noexcept { return clock_switch_reason_; }
    // 仅测试：推进 TrackPlayer 自己拥有的 SystemClock（切钟/降级后才有）的基准，让切到系统时钟
    // 之后的用例不必真跑墙钟。clock_override 或 AudioClock 时是空操作。
    void debug_advance_system_clock_us(int64_t us) noexcept;

    int64_t    dropped_frames()   const noexcept;
    int64_t    present_failures() const noexcept;
    // 渲染器返回 SYP_ERR_BUSY（资源暂满，这次没画）的累计次数——语义是"BUSY 重试
    // 次数"：BUSY 的帧不被消费，留着下次 step() 重试，同一帧重试
    // 几次就计几次；不计入 present_failures()，也不计入 dropped_frames()（重试到
    // 迟到超过 kDropThresholdUs 被丢弃时，那一次丢弃计入 dropped_frames()）。
    int64_t    render_busy_frames() const noexcept;

    // 这一次 open 绑定的视频轨索引；< 0 表示**没有绑定任何视频轨**。
    // 追加于此前一版，形状照 dropped_frames() 那几个只读访问器。
    //
    // 不是"给测试开的口子"：调用方此前根本无法知道有没有绑上视频轨——
    // demo 的 SypPlayerSnapshot.hasVideo 曾经用 `renderer_ != nullptr`
    // 这个代理判据，而 demo 的 open() 路径无条件构造 renderer，那个表达式
    // 恒为真、对"素材里到底有没有视频"零信息量。带封面图的音乐文件正是它
    // 答错的场景：素材里有一条 is_video 的封面图轨，但 TrackPlayer 按既有
    // 判据不会绑定它，真实答案是"没有视频"。**demo 现在就是用这个访问器
    // 算 hasVideo 的**（bridge.mm 的 snapshot），不是一个只有测试在用的
    // 口子。同时它也让相关回归用例能直接断言"绑的是哪一条"，而不是靠
    // 一串间接症状去推。
    int32_t    video_track_index() const noexcept;

    // 是否处于追帧状态（1 秒内迟到丢帧达到
    // kCatchupDropsToEnable 次触发，连续 kCatchupOnTimeToDisable 帧按时
    // 呈现、或 seek()/pause() 关闭）。
    bool catching_up() const noexcept { return catching_up_; }
    // 只读转发 Pipeline::video_catchup()——TrackPlayer 已接管 Pipeline
    // 所有权，测试拿不到 Pipeline 本身，只能经这里查追帧状态是否已经
    // 真的传导给了 Pipeline（不是只有 TrackPlayer 自己的标记翻转）。
    bool video_catchup_active() const noexcept { return pipeline_->video_catchup(); }

    // 缓冲状态与统计。
    bool            buffering() const noexcept { return buffering_reason_ != BufferingReason::None; }
    BufferingReason buffering_reason() const noexcept { return buffering_reason_; }
    // 各在播轨已缓冲到的最小时刻 − 当前位置（下限 0）；无在播轨时为 kBufferedUntilUnbounded。
    int64_t         buffered_us() const noexcept;
    int64_t         rebuffer_count() const noexcept { return rebuffer_count_; }
    // 卡顿缓冲累计时长（单调时钟，含正在进行的这一次）。按墙钟计：卡顿中用户暂停的
    // 那段时间也计入（缓冲原因仍是 Stall）。
    int64_t         rebuffer_total_us() const noexcept;
    // create() 到首次离开缓冲的耗时；从未离开为 -1（策略关闭时恒 -1）。
    int64_t         startup_us() const noexcept { return startup_us_; }
    // 转发 IAudioSink::underrun_count()（欠载段数）。**不等于卡顿次数**：
    // 除了网络卡顿造成的断粮，还包括自然播完（音频轨解到底、环被取空）那一段，以及
    // 起播/seek/缓冲恢复后首批样本写入之前设备先拉空的"预热"段。卡顿次数看
    // rebuffer_count()。
    int64_t         audio_underruns() const noexcept { return sink_ != nullptr ? sink_->underrun_count() : 0; }

    // ---- 音量与显示几何 ----
    // 线程契约与 play()/pause()/set_speed() 同一份：TrackPlayer 自己不
    // 加锁，调用方（桥）负责串行调用——桥的每个 ObjC setter 先
    // lock_guard lk(_core->mu_) 再转发（SYPBridge.mm 的 play/pause/
    // setSpeed 同一模式）。
    //
    // 这里的状态是"本实例内"
    // 的镜像与落地，不跨 open 保留——TrackPlayer::create() 是静态工厂，
    // 桥每次 open 都新建一个实例，sink/renderer 随之新建，物理上不可能
    // 让 volume_/muted_/gravity_ 跨越两次 create() 存活。"跨 open 保留
    // 用户设置过的音量/gravity"是桥层的范围（持久值存在桥的 PlayerCore 上）。
    // 【订正时序】桥把持久值作为 InitialSettings 传进 create()，
    // create() 在 sink_->open() **之前**就把它落到 sink/renderer 上（见
    // InitialSettings 上方注释）；不再是"create() 落默认值、成功之后桥再
    // 重新下发"——那样 sink 在 open() 时快照到的是 1.0。
    //
    // 音量状态存在这里而不是直接转发给 sink：sink 只有一个增益旋钮，
    // 收到的恒是 muted ? 0.0 : volume_——要在"静音不擦掉
    // 音量"这件事上给出正确行为，volume_ 必须单独存一份、不能靠读 sink
    // 的增益反推（静音时 sink 的增益就是 0，反推不出静音前的音量）。
    void   set_volume(double v) noexcept;
    double volume() const noexcept;
    void   set_muted(bool m) noexcept;
    bool   muted() const noexcept;

    // 【线程契约，统一口径】IVideoRenderer::set_source_geometry()/
    // set_gravity() **与 present() 由调用方串行**——MetalRenderer 把
    // sar_num/sar_den/rotation/gravity 存成四个独立的原子量、present() 逐个
    // 读取（无锁方案），并发的一次几何设置可能让 present() 读到
    // "半新半旧"的组合。本类不加锁（见上方"线程契约"），串行由桥的
    // _core->mu_ 保证：create()、set_gravity() 与泵线程的 step()（进而
    // present()）都在 mu_ 下；测试缝 -debugSetSourceGeometryForTest 在首帧之后
    // 调用，安全也正因为它持 mu_。create() 本身不触发 present()。
    // 将来若出现不能与 present() 串行的调用方（例如播放中途在别的线程改几何），
    // **必须先把这四个值打包成单个原子量**再整体替换——只有整体替换才能保证
    // present() 读到的永远是同一次设置里的一整组值，不会撕裂。
    void   set_gravity(syp::media::Gravity g) noexcept;
    // 无视频轨（纯音频源、或唯一的 is_video 轨是封面图）时恒为 {0,0}。
    syp::media::DisplaySize video_display_size() const noexcept;

private:
    TrackPlayer() = default;

    // 把 muted_ ? 0.0 : volume_ 落到 sink_ 上（sink 只有一个增益旋钮，
    // 见 set_volume() 上方注释）。set_volume()/set_muted()/create() 三处
    // 调用点统一走这里，避免"落地公式"在多处重复、将来改一处漏一处。
    void apply_gain() noexcept;
    // set_volume() 与 create()（InitialSettings::volume）共用的夹取：NaN → 0、
    // +inf → 1、-inf/负数 → 0、>1 → 1。
    static double clamp_volume(double v) noexcept;

    // sink 中途失败（IAudioSink::failed()）后调用：clock_kind_ 改为
    // System；若 clock_ 当前是 TrackPlayer 自己拥有的 AudioClock（不是
    // 测试注入的 override），换成一个新的 SystemClock，基准设成"降级
    // 前最后一次观察到的有效位置"（last_known_position_us_，不是直接
    // 再读一次 clock_->now_us()——那一刻 sink 已经 failed()，
    // AudioClock::now_us() 此时返回 AV_NOPTS_VALUE，拿它当基准就是
    // "时间跳变"本身，跟这一步要避免的事正好相反）。
    // 现在它只是"失败降级"入口：以 AudioFailed 调 switch_to_system_clock()。
    void degrade_to_system_clock();

    // 切到系统时钟（原因见参数）。原 degrade_to_system_clock() 改名为本函数的内核。
    // 暂停中切钟时新时钟也处于暂停（R3：暂停中的播放器切钟后时钟不得自己走）。
    void switch_to_system_clock(ClockSwitchReason reason, int64_t base_us);
    // #23：音频轨解到底、pending 为空、sink 缓冲已被设备取空（output_drained）→ true。
    bool audio_played_out() const noexcept;
    // clock_kind_ 仍是 Audio 且 audio_played_out() → 以 AudioEnded 切钟（基准选择见实现处注释）。
    void maybe_switch_on_audio_end();

    // AV_NOPTS_VALUE（INT64_MIN）钳位：flush()/set_base() 的所有调用点
    // 都过这一道，不管这次传入的值理论上"应该"总是有效——set_speed() 在
    // sink 已经 failed() 但 TrackPlayer 还没来得及
    // 降级的那个 step() 窗口里，position_us() 会转发一个已经 failed 的
    // AudioClock 的读数（AV_NOPTS_VALUE），若不钳位会把这个哨兵值原样
    // 喂给 flush()/set_base() 当"新的播放基准"，比不做任何事更糟。
    int64_t sanitize_position(int64_t v) const noexcept;

    // 把一帧交给渲染器，统一处理 due 计算与返回码分级。
    // diff_us = pts − now（媒体时间）；due 按倍速换算为真实时间，迟到的帧 due=0。
    // Done：渲染器收下了这一帧（SYP_OK，或非 BUSY 的失败——计 present_failures_），
    //       调用方消费它。
    // Busy：SYP_ERR_BUSY，计 render_busy_frames_；调用方**不得消费**这一帧，留在
    //       pending_video_ 里下次 step() 重试（理由见实现处注释）。
    enum class PresentResult { Done, Busy };
    PresentResult present_frame(const Frame& f, int64_t diff_us);

    // seek 后第一帧：不等时钟、立即呈现，随后用它的真实
    // pts 纠偏时钟基准并 flush sink——原 just_sought_ 分支的函数体，原样
    // 抽出（step() 里非暂停路径与暂停路径共用这一个入口）。自己负责 pop
    // 帧：拿不到帧（Pipeline 这一刻还没解出来）时返回
    // PlayOutcome{Kind::Waiting}，且**不清** just_sought_——下一次 step()
    // 还要再来这里试。拿到帧但渲染器 BUSY 同样返回 Waiting（带该帧
    // track_index/pts）、不清 just_sought_、帧留在 pending_video_；复位副作用（flush、
    // 基准）只在第一次拿到帧时做一次（seek_rebased_），重试不重复。渲染器收下帧才消费 just_sought_，同时清 preview_pending_
    // （seek 后的第一帧已经是"已出画"，不需要再单独出一次打开后的预览帧）。
    PlayOutcome present_first_frame_after_seek();

    // 追帧丢帧策略三件套，见 kCatchupDropsToEnable 等
    // 常量、以及本文件顶部长注释第 3 步末尾那段。
    // 真正迟到丢弃了一帧时调用（now_us 是本次判定用的时钟读数，调用方
    // 复用已经读过的那个值，不再多读一次）：把这次丢帧计入 1 秒窗口，
    // 窗口外的旧记录顺带清掉；不管这次有没有触发追帧，都清零
    // on_time_presents_——追帧的"连续按时"计数经不起中间插一次迟到丢帧。
    // 窗口内记录数达到 kCatchupDropsToEnable 且尚未追帧时打开追帧。
    void note_late_drop(int64_t now_us);
    // 正常（非 just_sought_）路径呈现成功一帧时调用；只在追帧中计数，
    // 连续达到 kCatchupOnTimeToDisable 帧就关闭追帧。
    void note_on_time_present();
    // seek()/pause() 复位追帧状态：清空窗口记录与连续计数；若当前正在
    // 追帧则关闭（Pipeline::set_video_catchup(false)）。
    void reset_catchup();

    // 缓冲状态机，见 BufferPolicy 与顶部长注释 0.5 步。
    void    evaluate_buffering();
    void    enter_buffering(BufferingReason reason);   // 若此前时钟在走则冻结
    // 若未暂停则恢复时钟；记统计；按离开时的已缓冲时长设卡顿闩锁（stall_armed_）
    void    leave_buffering(int64_t buffered_us);
    int64_t buffered_us_for(const BufferStats& s) const noexcept;
    bool    clock_running() const noexcept { return !paused_ && !buffering(); }
    void    freeze_clock();     // sink pause + 自有系统时钟 pause
    void    unfreeze_clock();   // 两者 resume

    BufferPolicy                          policy_ = BufferPolicy::disabled();
    BufferingReason                       buffering_reason_  = BufferingReason::None;
    int64_t                               rebuffer_count_    = 0;
    int64_t                               rebuffer_total_us_ = 0;
    int64_t                               startup_us_        = -1;
    // 【卡顿闩锁】：靠 full/demux_eof 在水位低于触发线时离开缓冲后解除
    // 武装，直到已缓冲时长回到触发线以上（或 seek）才重新武装。没有它，"full 离开 →
    // 已缓冲仍 < 触发线 → 下一步又进 Stall"会每隔一步 Stop/Start 一次音频设备。
    bool                                  stall_armed_       = true;
    std::chrono::steady_clock::time_point created_at_{};
    std::chrono::steady_clock::time_point stall_started_at_{};

    bool                 catching_up_       = false;
    int32_t              on_time_presents_  = 0;
    std::deque<int64_t>  late_drop_times_us_;   // 迟到丢帧发生时的时钟读数（媒体时间）

    // 声明顺序即析构顺序的反序：pipeline_ → sink_ → renderer_ → clock_，
    // clock_ 最后声明、最先析构，销毁 clock_（可能是持着 sink_ 裸指针的
    // AudioClock）时 sink_ 还没被销毁。见文件顶部长注释。
    std::unique_ptr<Pipeline>       pipeline_;
    // request_abort() 置位、step() 入口读。原子：request_abort() 与 step()
    // 并发是本类明示允许的（见 request_abort() 上方）。
    std::atomic<bool>               aborted_{false};
    std::unique_ptr<IAudioSink>     sink_;
    std::unique_ptr<IVideoRenderer> renderer_;
    std::unique_ptr<TimeSource>     clock_;

    // clock_ 是 TrackPlayer 自己构造的 SystemClock 时，这里存一份裸指针
    // ——pause()/play()/set_speed()/seek() 需要调 SystemClock 特有的
    // pause()/resume()/set_speed()/set_base()，TimeSource 基类只有
    // now_us() 一个方法，拿不到。clock_ 是 AudioClock 或外部注入的
    // clock_override 时，这里是 nullptr。
    SystemClock* owned_system_clock_ = nullptr;
    // clock_ 是外部注入的测试替身（create() 的 clock_override 非空）时
    // 为真。为真时 degrade_to_system_clock()/seek()/set_speed() 都不
    // 替换或改写 clock_ 本身——那是调用方（测试）持有的对象，只更新
    // clock_kind_ 这个标签。
    bool clock_is_override_ = false;

    std::optional<Frame> pending_audio_;
    std::optional<Frame> pending_video_;

    int32_t audio_track_ = -1;
    int32_t video_track_ = -1;

    // 除了 audio_track_/video_track_ 各绑定的那一条，Pipeline 按
    // codec_type 托管的其余 video/audio 轨（第二条音轨、封面图……）。
    // create() 里填，step() 第 0 步逐条排空——见文件顶部长注释。
    // 字幕/数据轨不在此列：Pipeline 压根不托管它们
    // （pipeline.h："字幕等不管"），pop_frame() 对它们恒返回
    // std::nullopt，没有堆积风险，不需要收进这个列表。
    std::vector<int32_t> unbound_tracks_;

    ClockKind clock_kind_   = ClockKind::System;
    bool      sink_opened_  = false;   // create() 时 sink_->open() 是否成功
                                        // （区别于 IAudioSink::failed()：
                                        // FakeAudioSink 上 open() 失败时
                                        // failed() 仍是 false——它是"从没
                                        // 打开过"，不是"打开后失败"，两件
                                        // 事必须分开记，否则 open 失败的
                                        // sink 会被判定成"可用"，audio 分
                                        // 支永久 write() 失败又永久不肯
                                        // 丢弃，堵死整条 Pipeline）
    bool      paused_       = false;
    double    speed_        = 1.0;
    // seek() 置真；step() 对遇到的第一帧视频消费掉这个标记（无论那次
    // step() 是不是紧跟在 seek() 后面调的——中间可能先处理了几次音频
    // Queued，也可能几次 step() 都在暂停中原地返回 Waiting）。
    bool      just_sought_  = false;
    // 本次 seek 的首帧复位副作用（flush sink、时钟基准、兜底位置）是否
    // 已经做过。首帧遇到渲染器 BUSY 会保留重试，重试时不再重复复位；seek() 清、首帧
    // 真正呈现后清。见 present_first_frame_after_seek()。
    bool      seek_rebased_ = false;
    // create() 成功时置为 (video_track_ >= 0)：打开后还
    // 没出过预览帧、且有视频轨可出。只在暂停中被消费（见 step() 暂停
    // 分支）——未暂停的首次播放保持现有三分支判定不变（决定：大量既有
    // 用例依赖首帧走正常判定，且未暂停时时钟本就从首帧附近起步）。
    //
    // 消费时机（取代此前"视频循环第一次 pop 到帧
    // 时清"的写法）：暂停中出过这一帧、或非暂停 step() 走到音频/视频
    // 判定之前无条件清一次——不依赖视频循环有没有被进入、这一帧最终是
    // 呈现还是丢弃。理由：preview_pending_ 语义是"打开后一直暂停、还没
    // 看过任何一帧"，只要发生过一次非暂停的真实推进（哪怕那次 step() 全
    // 程只碰了音频、直接 Queued 返回，视频循环压根没被进入），这件事就
    // 已经不成立了——原来那个"视频循环第一次 pop 到帧才清"的写法在"连续
    // 多次 step() 全部从音频分支提前 return"这个窗口里清不到，这是此前
    // 点名的缺口，回归用例见
    // tests/test_track_player.cpp preview_pending_cleared_by_any_unpaused_step_even_audio_only
    // （preview_pending_cleared_by_normal_path_present_or_drop 是基础覆盖）。
    // 暂停中预览帧遇到渲染器 BUSY 不算"出过"：preview_pending_ 保持、帧保留重试
    // （回归用例 paused_preview_busy_retains_frame_and_retry_presents_it）。
    bool      preview_pending_ = false;

    int64_t   dropped_frames_       = 0;
    int64_t   present_failures_     = 0;
    int64_t   render_busy_frames_   = 0;
    // 降级前最后一次观察到的有效（非 AV_NOPTS_VALUE）媒体时间，供
    // degrade_to_system_clock() 当新 SystemClock 的基准，避免用一个
    // 已经失效的 AudioClock::now_us() 读数（AV_NOPTS_VALUE）当基准。
    int64_t   last_known_position_us_ = 0;

    // 主时钟切到系统时钟的原因；seek() 在 AudioEnded 时恢复 AudioClock 并清回 None。
    ClockSwitchReason clock_switch_reason_ = ClockSwitchReason::None;

    // ---- 音量与显示几何（本实例内的镜像，不跨 open 保留，见
    // set_volume() 上方注释）。----
    double                   volume_       = 1.0;
    bool                     muted_        = false;
    syp::media::Gravity      gravity_      = syp::media::Gravity::AspectFit;
    syp::media::DisplaySize  display_size_{0, 0};
};

}  // namespace syp::media
