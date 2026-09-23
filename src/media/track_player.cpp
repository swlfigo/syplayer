#include "media/track_player.h"

#include "media/video_geometry.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/samplefmt.h>
}

namespace syp::media {

std::unique_ptr<TrackPlayer> TrackPlayer::create(std::unique_ptr<Pipeline>       pipeline,
                                                  std::unique_ptr<IAudioSink>     sink,
                                                  std::unique_ptr<IVideoRenderer> renderer,
                                                  std::unique_ptr<TimeSource>     clock_override,
                                                  syp_status*                     out_err,
                                                  const BufferPolicy&             policy,
                                                  const InitialSettings&          initial) {
    if (out_err != nullptr) *out_err = SYP_OK;
    if (pipeline == nullptr) {
        if (out_err != nullptr) *out_err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    // 私有构造函数，只能在这里（本类的静态成员函数）用 new 造实例。
    auto tp = std::unique_ptr<TrackPlayer>(new TrackPlayer());
    tp->pipeline_ = std::move(pipeline);
    tp->sink_     = std::move(sink);
    tp->renderer_ = std::move(renderer);

    // 找被管理的视频轨/音频轨，并把没被绑定的其余受管轨收进
    // unbound_tracks_（双音轨、带封面图的 mp4 都会有不止
    // 一条受管视频/音频轨，见头文件顶部长注释与 step() 第 0 步）。
    // demuxer.h 顶部警告过：`!is_video` 不等于"是音频轨"——字幕/数据轨
    // 的 is_video 也是 false，但 sample_rate 是 0（那条注释的正确判据
    // 是 codec_type == AVMEDIA_TYPE_AUDIO，经 codecpar 才能查）。
    // TrackPlayer 拿不到 codecpar（Pipeline 没有公开这个口子，不该为了
    // 这里新增），只能退一步用 TrackInfo 里已经填好的 sample_rate 字段
    // 当代理判据——它本身就是从 AVMEDIA_TYPE_AUDIO 的轨填出来的，视频/
    // 字幕/数据轨这个字段恒为 0，所以 `!is_video && sample_rate > 0`
    // 在效果上等价于那条注释要求的判据。字幕/数据轨两者都不满足，
    // 既不绑定也不进 unbound_tracks_——Pipeline 压根不托管它们，没有
    // 堆积风险（见头文件 unbound_tracks_ 注释）。
    //
    // 封面图（AV_DISPOSITION_ATTACHED_PIC）**永远不能当主视频
    // 轨**，无论它排在第几条。它是一条 is_video 为真、只有一个采样的轨，
    // 在 mp4 里极常见（带专辑封面的音乐文件、很多带封面的视频文件）。
    // 此前这里只认"第一条 is_video"：
    //   - 封面图流号更小的 mp4 → 封面图被绑成主视频轨，真视频轨被扔进
    //     unbound_tracks_ 排空掉，用户看到一张静止封面而不是视频；
    //   - 音频 + 封面图的音乐文件（封面图是唯一的 is_video 轨）→ 封面图
    //     被绑成主视频轨，还会让 seek() 把 just_sought_ 置真却永远没有
    //     视频循环去消费它（正是之前为"纯音频文件"修掉的那
    //     个悬空状态，从这条路又漏了回来）。
    // 跳过之后它落进 unbound_tracks_，跟第二条音轨一样按第 0 步排空，
    // 不会堆满自己的 FrameQueue 触发 Pipeline 的联合背压。
    const TrackInfo* audio_info = nullptr;
    const TrackInfo* video_info = nullptr;
    for (const auto& t : tp->pipeline_->tracks()) {
        if (t.is_video) {
            // discard：HLS 选轨未选中的 variant。
            // AVDISCARD_ALL **不**把流从 AVFormatContext::streams 里移除，
            // 所以未选中那几档的视频流照样出现在 tracks() 里，而且流号
            // 往往比选中那档更小（hls 按 master 里 EXT-X-STREAM-INF 的
            // 顺序建流）。认"第一条 is_video"就会绑上一条永远不会有帧的
            // 轨——画面永远黑、无错误、无降级。
            // attached_pic：封面图。
            // 两者都是"is_video 为真但不能当主视频轨"，处理完全一样——
            // 落进 unbound_tracks_ 按第 0 步排空，不会堆满自己的
            // FrameQueue 触发 Pipeline 的联合背压。
            if (t.discard || t.attached_pic) {
                tp->unbound_tracks_.push_back(t.index);
            } else if (tp->video_track_ < 0) {
                tp->video_track_ = t.index;
                video_info       = &t;
            } else {
                tp->unbound_tracks_.push_back(t.index);
            }
        } else if (t.sample_rate > 0) {
            // 被 discard 的音频轨同理：不能当主音轨（主时钟会挂在一条
            // 永远不前进的 sink 上），但仍要进 unbound_tracks_ 排空。
            if (t.discard) {
                tp->unbound_tracks_.push_back(t.index);
            } else if (tp->audio_track_ < 0) {
                tp->audio_track_ = t.index;
                audio_info       = &t;
            } else {
                tp->unbound_tracks_.push_back(t.index);
            }
        }
    }

    // 把初始设置（InitialSettings，缺省即默认值：音量
    // 1.0、未静音、AspectFit）下发到这次 open 新建的 sink/renderer 上——
    // 无论这次 open 有没有视频轨。
    //
    // apply_gain() **必须**在下面 sink_->open() 之前：
    // AudioUnitSink::open() 在构造 render_ctx_ 时快照 gain_current =
    // gain_target_（首次打开不淡入）。这里落下的若不是调用方的
    // 持久值（例如先落 1.0、create() 之后再改），open() 就从 1.0 起播、再
    // 斜坡降到目标——持久静音每次 open 开头都漏出约 15ms 近满音量。钉住这
    // 条顺序的是 tests/test_track_player.cpp
    // create_applies_initial_settings_before_the_sink_opens（FakeAudioSink::
    // gain_at_open()）。
    //
    // set_gravity 的时机：与 present() 由调用方串行（头文件 set_gravity()
    // 上方注释）；create() 本身不触发 present()，桥调 create() 时持 mu_。
    tp->volume_  = clamp_volume(initial.volume);
    tp->muted_   = initial.muted;
    tp->gravity_ = initial.gravity;
    if (tp->renderer_) tp->renderer_->set_gravity(tp->gravity_);
    tp->apply_gain();

    // 把显示几何推给渲染器，并记下显示尺寸供上层查询
    // （video_display_size()）。封面图轨与纯音频源走不到这里（video_info
    // 为 nullptr），两者的 display_size_ 保持构造时的默认值 {0,0}。
    if (video_info != nullptr) {
        tp->display_size_ = syp::media::display_size(video_info->width, video_info->height,
                                                       video_info->sar_num, video_info->sar_den,
                                                       video_info->rotation_deg);
        if (tp->renderer_) {
            tp->renderer_->set_source_geometry(video_info->sar_num, video_info->sar_den,
                                                video_info->rotation_deg);
        }
    }

    // 主时钟选择：有音频轨、且 sink 打开成功 → Audio；
    // 否则 → System。
    //
    // 【别按 "TrackPlayer 晚点调 open()" 做】sample_fmt 不在 TrackInfo 里
    // （demuxer.h 的 TrackInfo 只给 sample_rate/channels），Pipeline 的
    // 公开接口也没有暴露 codecpar 的口子——这里传 AV_SAMPLE_FMT_NONE
    // 占位。FakeAudioSink::open() 忽略这个参数，测试不受影响；但这是
    // 一个真实的接口空白。将来解决它时：
    //   1. clock_kind() 仍然在 create() 这里定死，判据改成"意图"（有
    //      音频轨 && sink_ != nullptr），不要改成"open() 成功与否"再
    //      往后挪一步去调用——clock_kind_ == Audio 到 sink 真正 open()
    //      成功之间不能有一个"决定了 Audio 但 sink 还没打开"的窗口。
    //      把 open() 挪到收到第一帧音频之后再调（"惰性 open"）会打破
    //      这条隐式不变量：clock_kind_ == Audio ⇒ sink 已经 open。
    //      played_us() 在 sink 未 open 时按接口契约返回
    //      AV_NOPTS_VALUE（INT64_MIN），届时 step() 里
    //      `diff = pts - clock_->now_us()` 就是 `pts - INT64_MIN`——
    //      有符号整数溢出，UB。
    //   2. 真正的修法是让 sink 自己处理"尚未 open"这件事：
    //      played_us() 在未 open 时返回**上一次 flush() 的基准**，
    //      而不是 AV_NOPTS_VALUE（AV_NOPTS_VALUE 应该保留给"已经
    //      failed()"这一种情况）。
    //   3. 格式来源改用 Frame（Frame::sample_fmt() 等，收到第一帧后才
    //      能读到），不要给 TrackInfo 加 sample_fmt 字段——demuxer.h 里
    //      TrackInfo 目前只承载"打开解码器需要什么"，往
    //      里加一个只有音频输出路径用得上的字段会让这个结构体的职责
    //      变得模糊。
    bool audio_ready = false;
    if (audio_info != nullptr && tp->sink_ != nullptr) {
        const syp_status oerr = tp->sink_->open(audio_info->sample_rate, audio_info->channels,
                                                 static_cast<int32_t>(AV_SAMPLE_FMT_NONE));
        audio_ready = (oerr == SYP_OK);
    }
    tp->sink_opened_ = audio_ready;
    tp->clock_kind_  = audio_ready ? ClockKind::Audio : ClockKind::System;

    if (clock_override != nullptr) {
        tp->clock_             = std::move(clock_override);
        tp->clock_is_override_ = true;
    } else if (tp->clock_kind_ == ClockKind::Audio) {
        tp->clock_ = std::make_unique<AudioClock>(tp->sink_.get());
    } else {
        auto sc = std::make_unique<SystemClock>();
        tp->owned_system_clock_ = sc.get();
        tp->clock_              = std::move(sc);
    }

    // 有视频轨就有一次"打开后预览帧"可出（只在暂停中
    // 消费，见 step() 暂停分支与头文件 preview_pending_ 注释）。
    tp->preview_pending_ = (tp->video_track_ >= 0);

    // 起播缓冲：首帧照常立即呈现（preview_pending_ 在缓冲分支里消费），时钟等水位。
    tp->policy_     = policy;
    tp->created_at_ = std::chrono::steady_clock::now();
    if (policy.enabled) tp->enter_buffering(BufferingReason::Startup);

    return tp;
}

TrackPlayer::~TrackPlayer() = default;

// ---- 音量与显示几何 ----

void TrackPlayer::apply_gain() noexcept {
    if (sink_) sink_->set_gain(muted_ ? 0.0 : volume_);
}

double TrackPlayer::clamp_volume(double v) noexcept {
    // 非有限值的口径是 NaN → 0、+inf → 1、-inf → 0，
    // 不是"非有限值一律夹到 0"——跟 AudioUnitSink::set_gain() 同一口径
    // （audio_unit_sink.mm 该方法上方注释）。只需要显式
    // 处理 NaN：std::isnan(v) 参与的任何比较恒为 false，不特判的话 NaN
    // 会绕过下面两条钳位比较、直接存进 volume_。±inf 不需要单独分支：
    // IEEE 754 下 -inf < 0.0 与 +inf > 1.0 都成立，会被下面两条既有的
    // 钳位语句自然夹到 0.0/1.0。
    if (std::isnan(v)) v = 0.0;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return v;
}

void TrackPlayer::set_volume(double v) noexcept {
    // 夹取口径见 clamp_volume()；create() 的 InitialSettings::volume 走同一个函数。
    volume_ = clamp_volume(v);
    apply_gain();
}

double TrackPlayer::volume() const noexcept { return volume_; }

void TrackPlayer::set_muted(bool m) noexcept {
    muted_ = m;
    apply_gain();
}

bool TrackPlayer::muted() const noexcept { return muted_; }

void TrackPlayer::set_gravity(syp::media::Gravity g) noexcept {
    gravity_ = g;
    if (renderer_) renderer_->set_gravity(g);
}

syp::media::DisplaySize TrackPlayer::video_display_size() const noexcept { return display_size_; }

int64_t TrackPlayer::sanitize_position(int64_t v) const noexcept {
    return (v == AV_NOPTS_VALUE) ? last_known_position_us_ : v;
}

void TrackPlayer::degrade_to_system_clock() {
    // 基准这里曾经写成 sanitize_position(last_known_position_us_)
    // ——sanitize_position() 的定义就是"是 AV_NOPTS_VALUE 就换成
    // last_known_position_us_"，作用在它自己的兜底源上是恒等变换、纯死
    // 代码，看起来在守什么其实什么都没守（把这一行整个删掉用例也全绿存活，
    // 这一处是原因之一）。真正让这一行安全的不变量在别处：
    // last_known_position_us_ 的**全部三个写入点**都保证写进去的不是
    // AV_NOPTS_VALUE ——
    //   1. step() 开头：`if (now != AV_NOPTS_VALUE)` 显式挡住；
    //   2. just_sought_ 分支：`sanitize_position(pts)`（此前是裸 pts，
    //      见该处注释）；
    //   3. seek()：`base = sanitize_position(ts_us)`。
    // 成员的初值 0 也满足。所以这里直接用即可，把不变量写在注释里，而不是
    // 留一个恒等调用假装它在被检查。（内核 switch_to_system_clock() 对
    // base_us 仍过一道 sanitize_position()——那是给 #23 播完切钟那条入口的
    // 基准兜底用的，对这里传入的值依旧是恒等变换。）
    switch_to_system_clock(ClockSwitchReason::AudioFailed, last_known_position_us_);
}

void TrackPlayer::switch_to_system_clock(ClockSwitchReason reason, int64_t base_us) {
    clock_kind_          = ClockKind::System;
    clock_switch_reason_ = reason;
    if (clock_is_override_) return;   // 外部对象，不能替换，只改标签

    auto sc = std::make_unique<SystemClock>();
    sc->set_base(sanitize_position(base_us));
    // 新 SystemClock 默认 1.0×，不继承
    // speed_。降级发生在非 1.0× 播放时（比如 2× 播放中 sink 被抢占），
    // 若漏了这一行，墙钟走 100ms 播放位置只推进 100ms 而不是
    // 该有的 200ms——数值上看不出"崩"，只是速度悄悄跌回 1.0×。
    sc->set_speed(speed_);
    // 暂停中切钟：新时钟也必须处于暂停。此前的降级实现漏了
    // 这一步——暂停中 sink 失败（或 #23 暂停中恰好判定播完）后，新
    // SystemClock 会自己往前走，play() 时 resume() 对一个没暂停的时钟是
    // 空操作，位置已经偷偷跳过了整段暂停时长。
    // 缓冲冻结同理：时钟运行 ⇔ 未暂停且未缓冲。
    if (!clock_running()) sc->pause();
    owned_system_clock_ = sc.get();
    clock_              = std::move(sc);   // 旧的 AudioClock 在这里被销毁
    last_known_position_us_ = sanitize_position(base_us);
}

// #23：音频轨已解到底（Pipeline::track_drained）、pending_audio_ 为空、sink
// 报告缓冲已被设备取空（IAudioSink::output_drained()）→ 声音已播完。
//
// 此前判据是"played_us() ≥ 最后写入帧的结束 pts − 20ms 容差"。
// played_us() 扣除了设备输出延迟，而 sink 只能消费已写入的样本，读数上限
// 就是"结束读数 − 延迟"：延迟一旦大于容差（加上 AAC priming 带来的约一帧
// 超前量）判据永远不成立，#23 原样重现（FakeAudioSink 延迟 45ms/60ms 实测
// eof=0、presented=51）。output_drained() 只问"还有没有样本没被取走"，与
// 延迟无关。回归用例：audio_shorter_than_video_with_large_device_latency_reaches_eof（150ms）。
//
// flush()（seek/set_speed）之后 sink 里没有已接受未取走的样本，output_drained()
// 天然为 true，不再需要"flush 后一帧未写"的特判：seek 到音频结束之后、或
// 音频轨已排空时 set_speed() 丢掉尾巴，都直接判定播完（回归用例
// seek_past_audio_end_after_switch_still_reaches_eof、
// set_speed_after_audio_drained_still_reaches_eof）。
// 不会在"还没开始解码"时误触发：track_drained() 要求 decoder_eof，Pipeline
// 创建与 seek() 都把它清成 false。
bool TrackPlayer::audio_played_out() const noexcept {
    return audio_track_ >= 0 && sink_ != nullptr && !pending_audio_.has_value() &&
           pipeline_->track_drained(audio_track_) && sink_->output_drained();
}

// #23 播完切钟的共同落点（step() 里与两处降级检查同位）。
//
// 基准取"此刻音频时钟的读数"，不取最后写入帧的结束 pts：
//   - sink 读数是"已消费样本数 ÷ 采样率"，不是 pts。AAC 的 priming 样本也被
//     解码写入，读数比 pts 超前约一帧（#23 素材实测音频耗尽时读数 2020136，
//     最后一帧结束 pts 是 2000000）——拿结束 pts 当基准会往回跳约 20ms；
//   - 读数扣除了设备延迟，此刻延迟那一截尾巴还在出声，读数正对应"耳朵听到
//     的位置"。
// 取当前读数则切钟前后位置连续（零跳变）、不回退。读数失效（AV_NOPTS_VALUE，
// 理论上不可达：sink 失败会先被降级检查截走）时由 switch_to_system_clock()
// 里的 sanitize_position() 兜底成 last_known_position_us_。
void TrackPlayer::maybe_switch_on_audio_end() {
    if (clock_kind_ == ClockKind::Audio && audio_played_out()) {
        switch_to_system_clock(ClockSwitchReason::AudioEnded, clock_->now_us());
    }
}

// 见头文件 note_late_drop() 上方注释。
void TrackPlayer::note_late_drop(int64_t now_us) {
    late_drop_times_us_.push_back(now_us);
    while (!late_drop_times_us_.empty() &&
           now_us - late_drop_times_us_.front() > kCatchupWindowUs) {
        late_drop_times_us_.pop_front();
    }
    on_time_presents_ = 0;
    if (!catching_up_ && static_cast<int32_t>(late_drop_times_us_.size()) >= kCatchupDropsToEnable) {
        catching_up_ = true;
        pipeline_->set_video_catchup(true);
    }
}

void TrackPlayer::note_on_time_present() {
    if (!catching_up_) return;
    if (++on_time_presents_ >= kCatchupOnTimeToDisable) reset_catchup();
}

void TrackPlayer::reset_catchup() {
    late_drop_times_us_.clear();
    on_time_presents_ = 0;
    if (catching_up_) {
        catching_up_ = false;
        pipeline_->set_video_catchup(false);
    }
}

void TrackPlayer::request_abort() noexcept {
    // 先置本地标志再往下转发，理由同 Pipeline::request_abort()：反过来有
    // 一个窗口——底层已被打断、这一步的产出已经冒上来，本地标志还是 false。
    aborted_.store(true, std::memory_order_release);
    pipeline_->request_abort();
}

// 把一帧交给渲染器，统一处理 due 计算与返回码分级。
// diff_us = pts - now（媒体时间）；due 按倍速换算为真实时间，迟到的帧 due=0。
//
// 返回 Busy 时调用方**不得消费这一帧**：留在 pending_video_ 里，
// 下一次 step() 原样重试（届时按新的时钟读数重新判定，迟到超过 kDropThresholdUs
// 就由正常迟到路径丢弃）。此前 BUSY 即丢弃——真实窗口上挂 layer 的在途上限是
// 2、名额在 presentedHandler（上屏之后）才归还，而播放器最多提前
// kPresentWindowUs（40ms）提交，每个名额被占 40~57ms，60fps 下 BUSY 是常态，
// 实测 600 帧丢 202/277/252 帧（提前 0ms 提交也丢 102/111）；window-less 测试
// layer 在命令缓冲完成时就触发 presentedHandler，ctest 看不见这件事。改为 2ms
// 轮询重试后原型实测 0/600 丢失。
TrackPlayer::PresentResult TrackPlayer::present_frame(const Frame& f, int64_t diff_us) {
    if (renderer_ == nullptr) return PresentResult::Done;
    const int64_t media_due = diff_us > 0 ? diff_us : 0;
    const int64_t due_in_us = static_cast<int64_t>(static_cast<double>(media_due) / speed_);
    const syp_status st = renderer_->present(f, due_in_us);
    if (st == SYP_ERR_BUSY) {
        ++render_busy_frames_;   // 语义：BUSY 重试次数（同一帧重试多次就计多次）
        return PresentResult::Busy;
    }
    if (st != SYP_OK) ++present_failures_;   // 错误分级里最轻的一级：计数，继续
    return PresentResult::Done;
}

// seek 后第一帧：不等时钟，立即呈现，
// 随后用它的真实 pts 再纠偏一次基准——seek() 里第 3 步用的是请求值
// ts_us，落点通常是更早的关键帧，两者不同。自己负责 pop 帧（暂停分支、
// 非暂停视频循环都直接调用它，前者手上不一定已经有 pending_video_）；
// 拿不到帧时返回 Waiting，且不清 just_sought_——下一次 step() 还要再来
// 这里试，语义跟原来内联在视频循环里时一致（循环外层 `break` 掉，
// just_sought_ 保持置位）。
//
// 这里的 sink_->flush(pts) 曾经会抹掉
// seek 之后、首帧复位之前已经写进 sink 的新位置音频（step() 里音频优先于
// 视频），而 Pipeline 已交出的下一帧音频领先被抹掉的那一段，造成持续到下一次
// seek 的音频领先——线程模式下 Seek 缓冲很快满足、音频轨号在前的文件实测
// +183ms。顺序现已定义：seek 之后到首帧复位（seek_rebased_ 置真）之前，step()
// 音频分支不写 sink、帧留在 pending_audio_（见 step() 第 1 步注释）；视频轨
// 失败/排空时 step() 开头放弃 just_sought_，不会永久扣住。于是这次 flush 在
// 正常路径上只清 seek() 之后本就为空的环。回归：test_track_player.cpp
// seek_audio_first_file_*、seek_with_failed_video_track_*。
PlayOutcome TrackPlayer::present_first_frame_after_seek() {
    if (!pending_video_.has_value()) {
        pending_video_ = pipeline_->pop_frame(video_track_);
    }
    if (!pending_video_.has_value()) {
        return PlayOutcome{PlayOutcome::Kind::Waiting};
    }

    // 渲染器可能 BUSY——此时帧留在 pending_video_、just_sought_ 保持置位，
    // 下一次 step() 原样重试。复位副作用（flush sink、时钟基准、兜底位置）每次 seek 只做
    // **一次**，在第一次拿到帧、交给渲染器之前做（seek_rebased_ 记录），重试时跳过：
    //   - 仍在呈现之前复位：呈现那一刻时钟已经是该帧 pts，与此前的顺序一致
    //     （test_sync_e2e 场景 E 按呈现时刻的时钟读数量漂移，依赖这一点）；
    //   - 不重复做：重复 flush 会把重试期间（非暂停时音频分支照常写入）写进 sink 的
    //     新位置音频反复抹掉，时钟也会被反复拉回该帧 pts。
    // BUSY 时报 Waiting 带上 pts，调用方（非暂停视频循环 / 暂停分支）据此继续驱动
    // Pipeline，不违反"要么产出、要么驱动 Pipeline"的不变量。
    const int64_t pts = pending_video_->pts_us();
    if (!seek_rebased_) {
        seek_rebased_ = true;
        if (sink_ != nullptr) sink_->flush(sanitize_position(pts));
        if (!clock_is_override_ && owned_system_clock_ != nullptr) {
            owned_system_clock_->set_base(sanitize_position(pts));
        }
        // 必须过 sanitize_position()：这里曾经是全文件唯一
        // 一处不经钳位就写进兜底源的赋值。pts 来自 Frame::pts_us()，而
        // frame.cpp 明写会把 AV_NOPTS_VALUE 原样透传（"上层需要能分辨『没有
        // pts』与『pts 是 0』"）。今天不可达——本仓库的 FFmpeg 只
        // --enable-demuxer=mov，mov 恒有 pts——但一旦解封装器集合扩大，一帧
        // 没有 pts 的视频就会把 last_known_position_us_ 投毒成 INT64_MIN，
        // 而它正是降级时 SystemClock 的基准（见 degrade_to_system_clock()）。
        last_known_position_us_ = sanitize_position(pts);
    }
    if (present_frame(*pending_video_, 0) == PresentResult::Busy) {
        return PlayOutcome{PlayOutcome::Kind::Waiting, video_track_, pts, SYP_OK};
    }

    just_sought_  = false;
    seek_rebased_ = false;
    // seek 后的第一帧已经是"已出画"——不需要再单独出一次打开后的预览帧
    // （这条帧本身就比预览帧更新，出过它之后没有理由再回头出旧的那帧）。
    preview_pending_ = false;
    pending_video_.reset();
    return PlayOutcome{PlayOutcome::Kind::Presented, video_track_, pts, SYP_OK};
}

PlayOutcome TrackPlayer::step() {
    // 【中止先于一切】手里有现成帧时下面的路径不一定调 Pipeline::step()，
    // Pipeline 入口那道检查替不了这一层。
    if (aborted_.load(std::memory_order_acquire)) {
        return PlayOutcome{PlayOutcome::Kind::Error, -1, AV_NOPTS_VALUE, SYP_ERR_CANCELED};
    }
    // 记住"降级前最后一次观察到的有效位置"——必须在降级检查之前做，
    // 这样即使这一次 step() 就是发现 sink 失败的那一次，我们手里也还
    // 攥着上一次（sink 还没坏时）的读数，不会拿 AV_NOPTS_VALUE 当基准。
    if (clock_kind_ == ClockKind::Audio) {
        const int64_t now = clock_->now_us();
        if (now != AV_NOPTS_VALUE) last_known_position_us_ = now;
    }

    // 降级检查放在最前：sink 中途失败后，本轮判定就要用系统时钟，不能
    // 拿一个已失效的时钟去决定丢不丢帧。判据不只看 sink_->failed()——
    // 音频轨自身解码失败也要触发降级：pipeline.h 明确
    // 要求调用方轮询 track_failed()，漏了这条的话，音频轨失效后环放空、
    // AudioClock 的读数停止推进，所有视频帧会永远判成"早了"，表现为
    // 画面静止、无错误、无降级——静默且稳定地错。
    if (clock_kind_ == ClockKind::Audio && sink_ != nullptr &&
        (sink_->failed() || pipeline_->track_failed(audio_track_))) {
        degrade_to_system_clock();
    }
    // #23：音频轨正常播完（不是失败）→ 切系统时钟把视频尾部播完。与降级检查同位。
    maybe_switch_on_audio_end();

    // 缓冲状态机：降级/#23 检查之后、音视频判定之前。
    evaluate_buffering();

    // seek 首帧永远不会来：视频轨失败，或 seek 落点已过视频轨末尾
    // （音频比视频长）——视频轨已排空、手上也没有待处理帧。放弃 seek 首帧语义，否则
    // 下面音频分支会以"等首帧复位"为由永久扣住音频。
    if (just_sought_ && video_track_ >= 0 && !pending_video_.has_value() &&
        (pipeline_->track_failed(video_track_) || pipeline_->track_drained(video_track_))) {
        just_sought_  = false;
        seek_rebased_ = false;
    }

    // 0. 排空所有不受管的轨（第二条音轨、封面图这类 attached_pic 视频
    // 流……）——见头文件顶部长注释。这一步无论暂不暂停都
    // 要做，且不算这一步的"产出"。
    for (int32_t idx : unbound_tracks_) {
        while (pipeline_->pop_frame(idx).has_value()) {
            // 丢弃：这些轨没有消费方，留着不放会让它们各自的
            // FrameQueue 堆满、触发 Pipeline 的联合背压。
        }
    }

    if (paused_ || buffering()) {
        // 缓冲中与暂停同形：不写音频、不推进视频，首帧/seek 首帧照常出。
        // 暂停中也把"打开后第一帧 / seek 后第一帧"画出来，
        // 时钟不动——seek 后第一帧照 just_sought_ 原有语义（会 flush sink
        // 并复位时钟基准到该帧真实 pts，这是"seek 后立即出画"本来就要做
        // 的事，不是本任务新加的副作用）；打开后的预览帧则纯粹呈现一帧，
        // 不碰 sink、不碰时钟。just_sought_ 优先于 preview_pending_：seek
        // 发生在打开之后，这一刻该出的是 seek 落点的那一帧，不是旧的
        // "打开后第一帧"。present_first_frame_after_seek() 拿不到帧时报
        // Waiting，不当作"这一步产出"，落到下面继续驱动 Pipeline。
        if (just_sought_ && video_track_ >= 0) {
            PlayOutcome o = present_first_frame_after_seek();
            if (o.kind != PlayOutcome::Kind::Waiting) return o;
        } else if (preview_pending_ && video_track_ >= 0) {
            if (!pending_video_.has_value()) pending_video_ = pipeline_->pop_frame(video_track_);
            if (pending_video_.has_value()) {
                const int64_t pts = pending_video_->pts_us();
                // BUSY：preview_pending_ 保持、帧留在 pending_video_，
                // 落到下面驱动 Pipeline 报 Waiting，下一次暂停 step() 重试同一帧。
                if (present_frame(*pending_video_, 0) == PresentResult::Done) {
                    pending_video_.reset();
                    preview_pending_ = false;   // 只出一次：恢复播放后从下一帧开始走正常判定
                    return PlayOutcome{PlayOutcome::Kind::Presented, video_track_, pts, SYP_OK};
                }
            }
        }

        // 跳过下面的音频/视频判定（不呈现、不写音频），但仍然要
        // 驱动一次 Pipeline::step()——暂停期间不停止
        // 预解码，恢复时才能立即出画。这一步的结果一律
        // 折叠成 Waiting，Error 照实转述。
        const StepOutcome so = pipeline_->step();
        if (so.kind == StepOutcome::Kind::Error) {
            return PlayOutcome{PlayOutcome::Kind::Error, -1, AV_NOPTS_VALUE, so.status};
        }
        return PlayOutcome{PlayOutcome::Kind::Waiting};
    }

    // preview_pending_ 只对"打开后一直暂停、
    // 还没看过任何一帧"这件事有意义。走到这里就已经确定这一次 step()
    // 是非暂停的一次真实推进——不管接下来音频、视频哪个分支产出什么
    // （甚至哪个分支都没碰到视频、直接从音频分支 Queued 返回），"已经
    // 开始播放"这件事本身就足够让预览语义作废，所以放在音频/视频判定
    // **之前**、每次非暂停 step() 都无条件清一次，不依赖"视频循环有没
    // 有被进入""这一帧是呈现还是丢弃"这些更下游的细节。
    //
    // 原来分别在视频
    // 循环第一次 pop 到帧时、以及呈现/丢弃分支上各清一次——三处全部只在
    // 视频循环被进入之后才有机会执行，而音频分支写成功会直接
    // `return Queued`，视频循环那一次 step() 根本不会被进入：一串
    // "连续多次 step() 全部从音频分支提前返回"的窗口里 preview_pending_
    // 会一直卡在 true，直到某次视频循环终于被进入才清掉——这正是那个
    // 缺口。清除点挪到这里、且不依赖视频循环是否执行，缺口直接消失，
    // 原来那三处清除全部变成真死代码，已经删掉。
    preview_pending_ = false;

    // 1. 音频优先：音频是时钟，喂不上就没有正确的时间基准。写不进去
    // （背压）不再直接 return Blocked——那会让视频永远轮不到（正确的
    // 条件本就是"且 write() 能接受"，此前这里写错了）。帧留在 pending_audio_ 里，置
    // audio_blocked，跳出循环去看视频；视频这一步也没产出时，仍然会
    // 走到下面第 3 步驱动 Pipeline（不会因为 audio_blocked 就跳过），
    // 见第 3 步注释。
    const bool sink_usable = sink_opened_ && sink_ != nullptr && !sink_->failed();
    bool       audio_blocked = false;
    // seek 首帧复位之前扣住音频，见下面循环内注释。
    bool       audio_held_for_seek = false;
    for (;;) {
        if (!pending_audio_.has_value()) {
            pending_audio_ = pipeline_->pop_frame(audio_track_);
        }
        if (!pending_audio_.has_value()) break;   // 没有音频帧可用，看视频

        if (sink_usable) {
            // seek 之后、首帧复位（flush sink + 时钟基准改到首帧 pts）
            // 之前，不写音频：这期间写进 sink 的新位置音频会被首帧那次 flush 抹掉，而
            // Pipeline 已经交出的下一帧音频领先被抹掉的那一段——音频领先一直保持到下一次
            // seek（线程模式、音频轨号在前的文件实测 +183ms）。帧留在 pending_audio_ 里
            // 跳出去看视频，视频循环 / 第 3 步照常驱动 Pipeline 把首帧解出来。
            // 为什么以 !seek_rebased_ 为界而不是等首帧真正呈现：复位在第一次交给渲染器
            // 之前就做了，渲染器持续 BUSY 时扣音频会拖住时钟；复位之后写入的音频不会再被
            // 抹掉。视频轨失败/排空时 step() 开头已清 just_sought_，不会永久扣住。
            if (just_sought_ && !seek_rebased_ && video_track_ >= 0) {
                audio_held_for_seek = true;
                break;
            }
            if (sink_->write(*pending_audio_)) {
                const int64_t pts = pending_audio_->pts_us();
                pending_audio_.reset();
                return PlayOutcome{PlayOutcome::Kind::Queued, audio_track_, pts, SYP_OK};
            }
            audio_blocked = true;   // 背压，帧留着，改看视频
            break;
        }

        // 没有可用 sink（未提供、open 失败、或中途降级失败）：丢弃这
        // 帧，避免 Pipeline 的联合背压把整条管线卡死（pipeline.h 顶部
        // 警告：调用方必须消费它托管范围内的每一条轨）。这不算这一步
        // 的"产出"，继续排空，直到这条轨暂时没有更多已解码帧。
        pending_audio_.reset();
    }

    // 1.5 【守卫本身也要被守卫】重新问一次 sink_->failed()。
    //
    // 上面第 0 步之前那次降级检查（本函数开头）发生在**唯一能让它翻转的
    // 那个动作之前**：failed_ 的翻转点就是本次 step() 第 1 步自己调的
    // sink_->write()——audio_unit_sink.mm 有四处在 write() 路径里
    // `failed_ = true;`（ensure_swr_for() 建重采样器失败、两处
    // swr_get_out_samples() < 0、swr_convert() < 0）。于是同一次 step()
    // 完全可能是：开头检查时 sink 还好好的 → 不降级、clock_ 仍是
    // AudioClock → write() 当场翻脸、返回 false → audio_blocked、跳出去
    // 看视频 → 下面 `pts - clock_->now_us()` 里的 now_us() 转发的是一个
    // 已失效 sink 的 played_us()，按接口契约返回 AV_NOPTS_VALUE
    // （INT64_MIN）。这条减法是有符号整数溢出（UB，UBSan 实测报在下面
    // diff 那一行），且后果不止是 UB：溢出后的 diff 恒小于
    // -kDropThresholdUs，这一次 step() 会把丢帧预算打满 kMaxDropsPerStep
    // 帧（全是本该呈现的真实帧），并把 pts_us = INT64_MIN 当合法结果交
    // 给调用方。
    //
    // 为什么是"就地降级"而不是"给那条减法套一层 sanitize_position()"：
    // 开头那段注释写的是「本轮判定就要用系统时钟，不能拿一个已失效的
    // 时钟去决定丢不丢帧」——只钳位读数的话 clock_ 仍然是那具尸体，丢帧
    // 判定用的是一个冻结在 last_known_position_us_ 上的常数，
    // clock_kind()/position_us() 对外也要到下一次 step() 才变诚实。就地
    // 降级才是那句话本来的意思。两处都做则必有一处是死代码——正好是本轮
    // 第 3 步刚从 degrade_to_system_clock() 里清掉的那种恒等守卫。
    //
    // 这一次检查自己的不变量：从这里往下到视频三分支之间，没有任何代码
    // 会碰 sink_（唯一的 sink_ 调用是 just_sought_ 分支里的 flush()，
    // 而 flush() 不会置 failed_）。所以它不会再被它后面的动作作废。
    if (clock_kind_ == ClockKind::Audio && sink_ != nullptr && sink_->failed()) {
        degrade_to_system_clock();
    }
    // #23 同位检查（与上面第 2.5 步降级检查同位）：本轮视频判定前再问一次，
    // 播完了就让本轮视频判定直接用新时钟，不白等一轮。
    maybe_switch_on_audio_end();

    // 2. 视频：三分支穷尽。
    int32_t dropped_this_step  = 0;
    // 早到分支不再直接 return——见该
    // 分支内的注释。这两个变量记的是第 3 步要用的"如果这一帧还没被更
    // 紧急的事盖过，就该报的那个 Waiting"。
    //
    // 这里曾经写成"如果第 3 步没有更紧急的事
    // （Eof / Error / Pipeline 自己的 Blocked），最终该如实报成的那个
    // Waiting，供第 3 步的兜底 switch 用"——原样引用在这里是因为它错得
    // 有代表性：那句话描述的是**修复前**的优先级（Eof/Blocked 比本地
    // 信号更紧急），本次修复的要点恰恰是反过来——Eof 和 Pipeline 自己
    // 的 Blocked 都不再比 video_waiting_early/audio_blocked 更紧急，
    // 只有 Error 才是（见第 3 步末尾的 if 链，那个"兜底 switch"已经被
    // 取代，不存在了）。真实的优先级顺序见下面第 3 步落地处的注释、
    // 以及 track_player.h 顶部 step() 总览第 5 步——两处都写对了，唯独
    // 这里（离 video_waiting_early 的赋值点最近、最容易被当成权威）
    // 写反了。
    bool    video_waiting_early = false;
    // 渲染器 BUSY、帧留着重试——与早到同形：不消费、跳出、落到
    // 第 3 步驱动 Pipeline，最终报 Waiting（带这一帧 pts），优先级与早到相同。
    bool    video_busy_retry    = false;
    int64_t video_waiting_pts   = AV_NOPTS_VALUE;
    for (;;) {
        if (!pending_video_.has_value()) {
            pending_video_ = pipeline_->pop_frame(video_track_);
        }
        if (!pending_video_.has_value()) break;

        // seek 首帧的 BUSY 重试必须有迟到上限。它不消费帧、不 pop
        // 后续视频：渲染器持续 BUSY（如 drawableSize 为 0×0 时 nextDrawable 恒 nil）时，
        // 视频 FrameQueue 满 → 联合背压停解封装 → 音频断粮 → AudioClock 停走 → 帧永远
        // "不迟"，音画永久冻结。所以复位做过之后（seek_rebased_，即重试阶段）按时钟判：
        // 迟到超过 kDropThresholdUs 就放弃 seek 首帧特殊路径，落到下面正常判定——由迟到
        // 路径丢弃（计丢帧、喂追帧窗口），后续帧照常。复位之前不判：seek 落点关键帧 pts
        // 常早于请求值、时钟还没纠偏到它，此时判迟到会误丢 seek 首帧。
        if (just_sought_ && seek_rebased_) {
            const int64_t now = clock_->now_us();
            if (now != AV_NOPTS_VALUE && now - pending_video_->pts_us() > kDropThresholdUs) {
                just_sought_  = false;
                seek_rebased_ = false;
            }
        }

        if (just_sought_) {
            // seek 后遇到的第一帧视频：不等时钟，立即呈现——函数体见
            // present_first_frame_after_seek()。
            // 这里 pending_video_ 非空，它只可能报 Presented，或因 BUSY 报
            // Waiting（帧与 just_sought_ 都保留）——后者不能直接
            // return（既不产出也不驱动 Pipeline），跳出去走第 3 步。
            PlayOutcome o = present_first_frame_after_seek();
            if (o.kind != PlayOutcome::Kind::Waiting) return o;
            video_busy_retry  = true;
            video_waiting_pts = o.pts_us;
            break;
        }

        const int64_t now  = clock_->now_us();
        const int64_t diff = pending_video_->pts_us() - now;

        if (diff > kPresentWindowUs) {
            // 早了：不呈现，帧留在 pending_video_ 里，下次 step() 先看它。
            //
            // 这里不能再直接 return——
            // 跟前面暂停分支短路、不驱动
            // Pipeline 是同一个形状的缺陷，修法也同一个思路：跳过这一
            // 步该有的产出（不呈现），但仍然要走到第 3 步驱动一次
            // Pipeline::step()。
            //
            // 不这样改的后果是真实的永久停摆，不是理论风险：这一刻音频
            // 优先分支（第 1 步）也没有产出——要么它自己的 FrameQueue
            // 真的空了（`break` 出那个 for 循环），要么写不进 sink
            // （audio_blocked，见上面第 1 步）——如果视频这里也直接
            // return，第 3 步永远够不着，Pipeline 的两条 FrameQueue 都
            // 不会再补货。更要命的是：sink 的环形缓冲这一刻很可能也没有
            // 可消费的存量——`set_speed()`/`seek()` 都会 `flush()` 掉环，
            // 一旦这一刻 Pipeline 的音频 FrameQueue 也恰好接近排空（冷
            // 启动、刚变速、刚 seek 完），时钟就再也没有任何输入能把它
            // 推进——这一帧永远"早"，回到这个分支的条件永远成立，
            // 闭环，永久。回归用例：
            // test_track_player.cpp
            // step_does_not_livelock_when_audio_queue_empty_and_video_early。
            //
            // 跳出循环而不是 continue：这一帧还没到呈现时刻，不该被当成
            // "这一步的产出"消费掉（既不呈现也不丢弃），第 3 步驱动完
            // Pipeline 之后，下一次 step() 会重新走到这里，用新的
            // clock_->now_us() 再判一次。
            video_waiting_early = true;
            video_waiting_pts   = pending_video_->pts_us();
            break;
        }

        if (diff < -kDropThresholdUs) {
            if (dropped_this_step >= kMaxDropsPerStep) {
                // 撞了本轮丢帧上限：这一帧留着，下次 step() 继续判定，
                // 不能因为"还想多丢"就突破"一次推进一个单位"。
                return PlayOutcome{PlayOutcome::Kind::Dropped, video_track_,
                                    pending_video_->pts_us(), SYP_OK};
            }
            pending_video_.reset();
            ++dropped_this_step;
            ++dropped_frames_;
            // 真正迟到丢弃（不是撞了 kMaxDropsPerStep 上限
            // 那条早退，那条帧还留着没被丢）：喂进追帧窗口。
            note_late_drop(now);
            continue;
        }

        // 其余一律呈现——第三分支绝不能写成 |diff| <= kPresentWindowUs，
        // 见本文件顶部长注释与头文件顶部注释。
        const int64_t pts = pending_video_->pts_us();
        if (present_frame(*pending_video_, diff) == PresentResult::Busy) {
            // 渲染器暂满：帧不消费、不计丢帧、不计"按时呈现"
            // （没有追上什么），跳出去驱动 Pipeline；下一次 step() 用新的时钟
            // 读数重新判定这一帧——仍在范围内就重试，迟到超过 kDropThresholdUs
            // 就走上面的迟到丢弃（计入 dropped_frames、喂追帧窗口）。
            video_busy_retry  = true;
            video_waiting_pts = pts;
            break;
        }
        pending_video_.reset();
        note_on_time_present();
        return PlayOutcome{PlayOutcome::Kind::Presented, video_track_, pts, SYP_OK};
    }
    if (dropped_this_step > 0) {
        // 这一步确实丢了帧。两种方式跳出上面的循环都会落到这里：输入
        // 耗尽（pop_frame 返回 nullopt，队列真的空了），或者丢完之后紧
        // 接着的下一帧撞上了"早了"分支或渲染器 BUSY（video_waiting_early /
        // video_busy_retry 也可能同时为真，帧留着下次再判）——两种情况都没有具体的 pts 可报（要报也该报刚丢的那
        // 一帧，但它已经被丢弃，不是"待处理"的那一帧）。丢帧本身就是
        // 这一步的产出，不需要为了让第 3 步顺带驱动一次 Pipeline 而把
        // 这个已经发生的产出压下去——下一次 step() 会自然重新走到
        // video_waiting_early 那条分支，一样能落到第 3 步。
        return PlayOutcome{PlayOutcome::Kind::Dropped, video_track_};
    }

    // 3. 音频、视频这一步都没直接产出：仍然要驱动 Pipeline::step()。
    // 「没直接产出」现在有两种成因，都要落到这里：
    //   a）音频、视频两条队列这一刻都真的空着（原有形状）；
    //   b）视频有一帧待处理，但它还"早"，不该呈现（见上面
    //      video_waiting_early 的赋值点）。
    // 即使 audio_blocked 为真（sink 的环形缓冲满，这一步没能把音频送
    // 出去），驱动 Pipeline 对视频（以及音频自己的 FrameQueue，给
    // pending_audio_ 空出来的那个位置补货）依然有意义——backpressure
    // 卡在 sink 这个下游端点，跟 Pipeline 自己的 FrameQueue/PacketQueue
    // 是两回事，不驱动只会让视频的预解码队列跟着一起干涸。这里曾经
    // 有一版在 audio_blocked 为真时直接 return Blocked、跳过驱动
    // Pipeline——反向自检时才发现：那样写
    // 会让视频的 FrameQueue 一旦排空（默认容量 8 帧）就永远补不上，
    // 音频永久背压的场景下视频反而会跟着一起停摆，属于"修复引入的新
    // 缺陷"，已经改成现在这样。
    const StepOutcome so = pipeline_->step();

    // Error 永远最优先如实转述——Pipeline 层面的错误（当前唯一来源：
    // demux 遇到底层 I/O 错误，见 pipeline.cpp）跟"这一步局部有没有活
    // 干"是两个维度的事，不该被任何本地信号盖住。
    if (so.kind == StepOutcome::Kind::Error) {
        return PlayOutcome{PlayOutcome::Kind::Error, -1, AV_NOPTS_VALUE, so.status};
    }

    // 【反向自检挖出的第二个坑，一并在这里堵上】
    // video_waiting_early / audio_blocked 必须排在 so.kind == Eof 前面
    // 判——不能只是"排在 Blocked 前面"那么简单。Pipeline::step() 的
    // Eof 判据只问它自己内部的 PacketQueue/FrameQueue 是否已经排空
    // （pipeline.cpp "3) Eof" 那段注释），对 TrackPlayer 自己攥着的
    // pending_video_/pending_audio_ 一无所知——这两个成员里的帧已经从
    // Pipeline 的队列里 pop 出来了，从 Pipeline 的视角看"轨已经排空"，
    // 但从调用方的视角看，这一帧还没被呈现/写出去，播放并没有真的结
    // 束。这条冲突在本轮修复之前无法触发（视频早到分支直接 return，
    // 压根不会走到这里去问 Pipeline 是不是 Eof 了）；本轮修复主动让
    // 它有机会驱动 Pipeline 之后，第一版反向自检当场撞见了它——
    // early_frame_waits_and_is_not_presented 用的单帧素材，第一次
    // step() 就会命中"video_waiting_early 为真、这一步驱动的
    // pipeline_->step() 恰好是这条轨最后一帧、Pipeline 因此如实报
    // Eof"这个组合。若原样转述 Eof，这唯一一帧视频就永远不会被呈现，
    // 用例断言的 Kind::Waiting 直接翻红——不是这条回归用例本身脆弱，
    // 是它诚实地测出了这个此前从未被覆盖过的死角。audio_blocked 同
    // 理：pending_audio_ 里还压着一帧没写出去的音频，原样转述 Eof 会
    // 让它再也没机会被 write()。
    if (video_waiting_early || video_busy_retry) {
        return PlayOutcome{PlayOutcome::Kind::Waiting, video_track_, video_waiting_pts, SYP_OK};
    }
    if (audio_blocked) return PlayOutcome{PlayOutcome::Kind::Blocked};
    // 扣住的音频同理排在 Eof 前面（pending_audio_ 里有帧没写）；报
    // Waiting 而不是 Blocked：不是下游背压，下一次 step() 解出首帧就能推进，
    // 不该让调用方退避。
    if (audio_held_for_seek) return PlayOutcome{PlayOutcome::Kind::Waiting};

    // 到这里，本地已经没有"还攥着一帧没交付"的信号了——video_waiting_
    // early、audio_blocked 都是假，如实转述 Pipeline 自己的判定：
    // Eof → Eof，Blocked → Blocked，否则（DemuxedPacket/DecodedFrame，
    // 这一次驱动确实做了事，但对外可观测的"吃到了什么"要等下一次
    // step() 的 pop 才能报出来）→ Waiting，不虚报。
    if (so.kind == StepOutcome::Kind::Eof) return PlayOutcome{PlayOutcome::Kind::Eof};
    if (so.kind == StepOutcome::Kind::Blocked) return PlayOutcome{PlayOutcome::Kind::Blocked};
    return PlayOutcome{PlayOutcome::Kind::Waiting};
}

void TrackPlayer::play() {
    if (!paused_) return;
    paused_ = false;
    // 与缓冲冻结取或：仍在缓冲时不恢复，离开缓冲时再恢复。
    if (clock_running()) unfreeze_clock();
}

void TrackPlayer::pause() {
    if (paused_) return;
    const bool was_running = clock_running();
    paused_ = true;
    if (was_running) freeze_clock();
    // 暂停复位追帧：暂停期间不产出真正的迟到丢帧（暂停
    // 分支跳过三分支判定），追帧状态没有继续存在的理由。
    reset_catchup();
}

bool TrackPlayer::paused() const noexcept { return paused_; }

syp_status TrackPlayer::set_speed(double speed) {
    // speed >= 0.5 && speed <= 2.0 在 speed 是 NaN 时恒为 false（NaN 跟
    // 任何值比较都是 false），天然落进"越界"分支返回
    // SYP_ERR_INVALID_ARG，不需要额外调 std::isnan 特判。
    if (!(speed >= 0.5 && speed <= 2.0)) return SYP_ERR_INVALID_ARG;

    // 四步顺序定死：
    //   1. 记下当前 now_us() 当基准：sink 已经
    //      failed() 但 TrackPlayer 还没来得及降级的那个窗口里，
    //      position_us() 会转发一个失效 AudioClock 的读数
    //      （AV_NOPTS_VALUE），sanitize_position() 钳位成
    //      last_known_position_us_。
    const int64_t base = sanitize_position(position_us());
    //   2. IAudioSink::flush(base) —— 清环、重设基准
    if (sink_ != nullptr) sink_->flush(base);
    //   3. IAudioSink::set_speed(speed) —— 调用点固定在 flush() 之后，
    //      见 audio_sink.h 该接口的三条约定注释。
    if (sink_ != nullptr) sink_->set_speed(speed);
    //   4. 更新 speed_（TrackPlayer 自己的镜像），此后 played_us() 与
    //      SystemClock 都按新值换算。
    speed_ = speed;
    if (!clock_is_override_ && owned_system_clock_ != nullptr) {
        owned_system_clock_->set_speed(speed);
    }
    return SYP_OK;
}

double TrackPlayer::speed() const noexcept { return speed_; }

syp_status TrackPlayer::seek(int64_t ts_us) {
    // 四步，顺序定死，失败也要走完后三步——见头文件 seek() 声明处注释，
    // 与 Pipeline::seek() 同一个立场。
    const syp_status rc = pipeline_->seek(ts_us);

    const int64_t base = sanitize_position(ts_us);
    if (sink_ != nullptr) sink_->flush(base);

    if (!clock_is_override_ && owned_system_clock_ != nullptr) {
        owned_system_clock_->set_base(base);
    }
    // AudioClock 的场景不需要额外动作：它的 now_us() 直接转发
    // sink_->played_us()，上面的 flush(base) 已经让它复位了。
    // clock_override 场景下 TrackPlayer 没有接口可以复位它——那是调用
    // 方（测试）的责任，见头文件 seek() 声明处注释。

    // #23：播完切钟是可逆的——seek 回去音频又有内容可播，恢复 AudioClock
    // （上面 flush(base) 已让它复位）。失败降级（AudioFailed）不可逆，不在此列；
    // clock_override 场景 TrackPlayer 换不了时钟，保持现状。
    if (clock_switch_reason_ == ClockSwitchReason::AudioEnded && !clock_is_override_ &&
        sink_opened_ && sink_ != nullptr && !sink_->failed() &&
        audio_track_ >= 0 && !pipeline_->track_failed(audio_track_)) {
        owned_system_clock_  = nullptr;
        clock_               = std::make_unique<AudioClock>(sink_.get());
        clock_kind_          = ClockKind::Audio;
        clock_switch_reason_ = ClockSwitchReason::None;
    }

    last_known_position_us_ = base;
    // 纯音频文件（video_track_ < 0）没有视频循环去消费 just_sought_，
    // 置真也是悬空状态——见头文件该字段声明处注释。
    just_sought_ = (video_track_ >= 0);
    seek_rebased_ = false;   // 新的 seek：首帧复位副作用重新做一次

    // 清空 pending_audio_/pending_video_：否则 seek 完还会呈现/写入一帧
    // seek 之前遗留的旧帧。
    pending_audio_.reset();
    pending_video_.reset();

    // dropped_frames_/present_failures_ 是生命周期累计值，不复位——跟
    // Pipeline::seek() 对 skipped_packets()/track_failed() 的立场一致。

    // seek 复位追帧：新位置的迟到/按时统计跟旧位置的
    // 追帧窗口无关，不该带过去。
    reset_catchup();

    // seek 后进入 Seek 缓冲（缓冲中 seek 则原因改为 Seek），按起播水位判定。
    if (policy_.enabled) enter_buffering(BufferingReason::Seek);

    return rc;
}

int64_t TrackPlayer::position_us() const noexcept { return clock_->now_us(); }

ClockKind TrackPlayer::clock_kind() const noexcept { return clock_kind_; }

int64_t TrackPlayer::dropped_frames() const noexcept { return dropped_frames_; }

int64_t TrackPlayer::present_failures() const noexcept { return present_failures_; }

int64_t TrackPlayer::render_busy_frames() const noexcept { return render_busy_frames_; }

int32_t TrackPlayer::video_track_index() const noexcept { return video_track_; }

// =====================================================================
// 缓冲状态机
// =====================================================================

namespace {
int64_t steady_elapsed_us(std::chrono::steady_clock::time_point since) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - since).count();
}
}  // namespace

void TrackPlayer::freeze_clock() {
    if (sink_ != nullptr) sink_->pause();
    if (!clock_is_override_ && owned_system_clock_ != nullptr) owned_system_clock_->pause();
}

void TrackPlayer::unfreeze_clock() {
    if (sink_ != nullptr) sink_->resume();
    if (!clock_is_override_ && owned_system_clock_ != nullptr) owned_system_clock_->resume();
}

int64_t TrackPlayer::buffered_us_for(const BufferStats& s) const noexcept {
    if (s.buffered_until_us == kBufferedUntilUnbounded) return kBufferedUntilUnbounded;
    if (s.buffered_until_us == AV_NOPTS_VALUE) return 0;
    const int64_t pos = sanitize_position(clock_->now_us());
    return s.buffered_until_us > pos ? s.buffered_until_us - pos : 0;
}

int64_t TrackPlayer::buffered_us() const noexcept {
    return buffered_us_for(pipeline_->buffer_stats());
}

int64_t TrackPlayer::rebuffer_total_us() const noexcept {
    int64_t total = rebuffer_total_us_;
    if (buffering_reason_ == BufferingReason::Stall) total += steady_elapsed_us(stall_started_at_);
    return total;
}

void TrackPlayer::enter_buffering(BufferingReason reason) {
    const bool was_running = clock_running();
    if (reason == BufferingReason::Seek) stall_armed_ = true;   // 新位置，旧的闩锁不再成立
    if (buffering_reason_ == BufferingReason::Stall && reason != BufferingReason::Stall) {
        rebuffer_total_us_ += steady_elapsed_us(stall_started_at_);   // 卡顿中 seek：结算这段卡顿
    }
    if (reason == BufferingReason::Stall && buffering_reason_ != BufferingReason::Stall) {
        ++rebuffer_count_;
        stall_started_at_ = std::chrono::steady_clock::now();
    }
    buffering_reason_ = reason;
    if (was_running) freeze_clock();
}

void TrackPlayer::leave_buffering(int64_t buffered_us) {
    if (buffering_reason_ == BufferingReason::Stall) {
        rebuffer_total_us_ += steady_elapsed_us(stall_started_at_);
    }
    // 首次离开任何缓冲即记起播耗时：起播缓冲中被 seek 改成 Seek 缓冲时同样给出值。
    if (startup_us_ < 0) startup_us_ = steady_elapsed_us(created_at_);
    buffering_reason_ = BufferingReason::None;
    // 闩锁：水位低于触发线时离开（full/文件尾），不许下一步立刻又进 Stall。
    stall_armed_ = (buffered_us >= policy_.rebuffer_trigger_ms * 1000);
    if (clock_running()) unfreeze_clock();
}

// 进入缓冲只看欠载（已缓冲 < 触发线），从不看 BufferStats::full：
// 线程模式稳态下 full 没有滞回、约每帧翻转一次。full 只在已经缓冲时充当恢复条件
// （字节上限先到、已缓冲低于恢复水位时读不进更多，不能永久缓冲）。
// 回归用例 buffering_full_flicker_with_healthy_level_never_transitions。
void TrackPlayer::evaluate_buffering() {
    if (!policy_.enabled) return;
    const BufferStats s        = pipeline_->buffer_stats();
    const int64_t     buffered = buffered_us_for(s);
    const int64_t     trigger_us = policy_.rebuffer_trigger_ms * 1000;
    if (buffering_reason_ == BufferingReason::None) {
        if (buffered >= trigger_us) stall_armed_ = true;
        if (paused_ || s.demux_eof) return;
        // !s.full 与闩锁一起用：full 会翻转，单靠 !full 挡不住抖动；
        // 闩锁要求水位先回到触发线以上。回归用例
        // buffering_full_exit_below_trigger_does_not_flap_until_rearmed。
        if (stall_armed_ && !s.full && buffered < trigger_us) enter_buffering(BufferingReason::Stall);
        return;
    }
    const int64_t level_ms = (buffering_reason_ == BufferingReason::Stall)
                                 ? policy_.rebuffer_resume_ms
                                 : policy_.startup_buffer_ms;
    // 恢复水位大于停读上限（配置错误）时按上限生效，否则永远等不到。
    const int64_t level_us = std::min(level_ms, pipeline_->config().max_buffer_ms) * 1000;
    if (s.demux_eof || s.full || buffered >= level_us) leave_buffering(buffered);
}

void TrackPlayer::debug_advance_system_clock_us(int64_t us) noexcept {
    if (clock_is_override_ || owned_system_clock_ == nullptr) return;
    owned_system_clock_->set_base(owned_system_clock_->now_us() + us);
}

}  // namespace syp::media
