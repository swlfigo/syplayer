#include "media/pipeline.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <utility>

#include "media/hls/hls_session.h"

extern "C" {
#include <libavutil/error.h>
}

namespace syp::media {

// 定义必须在这里（而不是头里内联）：hls_ 是 std::unique_ptr<HlsSession>，
// 而 pipeline.h 只前置声明了 HlsSession——unique_ptr 的析构要看到完整
// 类型，本 TU 上面那条 include 提供了它。
//
// 线程模式：先停加载线程再析构任何成员。顺序：置 stop_loader_ →
// request_abort()（打断可能阻塞着的读 + 唤醒 cv）→ join。join 返回之前加载线程
// 可能还在碰 demuxer_/tracks_/hls_，所以这三步必须在函数体里完成。
Pipeline::~Pipeline() {
    if (loader_.joinable()) {
        {
            std::lock_guard<std::mutex> g(load_mu_);
            stop_loader_ = true;
        }
        request_abort();
        loader_.join();
    }
}

namespace {

// FFmpegAudioDecoder::Receive 是 IVideoDecoder::Receive 的 using 别名
// （见 ffmpeg_audio_decoder.h 顶部注释），两者是同一个类型，模板可以
// 靠鸭子类型对两个没有公共基类的解码器类复用同一份 send/receive 驱动逻辑。
using Receive = IVideoDecoder::Receive;

enum class DriveResult { Frame, NoWork, Eof, Error };

// 驱动单条轨的解码器直到「吐出一帧」或「暂时没有更多能做的事」。
//
// 这个内部循环不可省，但真正的理由不是最初以为的那个：
//
// 最初的理由（错的）：以为循环是为了应付「AAC encoder priming / 解码器
// 攒够包才吐帧」这类"send 一个包但 receive() 仍是 NeedInput"的情况。
// 用变异测试证伪了这个理由——在本项目实际素材上，这个循环从未迭代
// 超过一轮（FFmpeg 的乐观解码机制总能在 send() 内部顺手把一帧解出来，
// 见 libavcodec/decode.c 的实现）。还构造过一个「严格
// 一次一发」的变异体（send 一个包就返回，把"取帧"挪到下一次 step()）：
// 5/5 用例照样全绿，证明这个循环对"能不能推进、最终能不能到 Eof"（活性）
// 不是必需的。
//
// 真正的理由：场景 D 的断言是「音视频双轨交织，交织顺序与参照
// （FFmpeg 直接解码）一致」。把全局出帧序列 (track_index, pts_us)
// 跟参照逐项比对：当前实现（本函数内部循环到吐帧为止才换轨）6397/6397
// 交织顺序逐项全等；换成「严格一次一发」后，第 0 项就分叉
// （ours "t1 t0 t1 t1 t0…" vs 参照 "t0 t1 t1 t0 t1…"）。
// 根因：把"决定换到哪条轨"的时机拆散到 send 和 receive 两次不同的
// step() 调用之间，会改变轨间的相对推进节奏；而参照路径（FFmpeg 直接
// 解码）对每条轨都是连续 send/receive 直到吐帧才处理下一条轨的下一个
// 单位。本函数把"让这条轨吐出一帧"当成不可分割的最小工作单元，才能
// 复现参照路径的推进节奏——这是场景 D 的直接承重件，去掉这个
// 循环场景 D 会直接变红。
//
// 循环体每一轮：
//   - receive() 到 Frame → 把帧交给调用方，返回 Frame。
//   - receive() 到 Eof   → 该轨解码器已完全排空（flush 之后终会走到这里），
//                          标记 ts.decoder_eof，返回 Eof。
//   - receive() 到 Error → 解码器内部错误：该轨终止（不是整体终止），
//                          由调用方标记 ts.failed，返回 Error。
//   - receive() 到 NeedInput → 按契约（video_decoder.h）先排空过 receive()
//                          才能 send()：如果 PacketQueue 还有包，send 一个
//                          再循环；如果包队列空了但 demux 已经整体 EOF、
//                          且还没给这条轨发过 flush 信号，发一个
//                          send(nullptr) 再循环；否则确实没有更多能做的
//                          事，返回 NoWork。
//
// 边界：这个循环的迭代次数受 PacketQueue 当前的包数严格限住（每轮至多
// 消费一个包，或者消费一次只会发生一次的 flush 信号），是有限次同步
// 调用，不是内部阻塞等待——但这意味着 step() 的解码分支最坏情况下会在
// 一次调用里消费掉某条轨 max_packets_per_track 个包才最终吐出一帧（或
// 确认这条轨吐不出帧）。「一次 step() 推进一个单位」说的是「一次对外
// 可观测的产出（DecodedFrame/DemuxedPacket/Eof/Blocked 之一）」，不是
// 「一次只碰一个 packet」。
template <typename Decoder>
DriveResult drive_decoder(Decoder& dec, Pipeline::TrackState& ts, bool demux_eof, Frame* out) {
    for (;;) {
        Frame f;
        const Receive rr = dec.receive(&f);
        if (rr == Receive::Frame) {
            *out = std::move(f);
            return DriveResult::Frame;
        }
        if (rr == Receive::Eof) {
            ts.decoder_eof = true;
            // 跟 ts.failed 那条路径（step() 里
            // `ts.failed = true; ts.packets->clear(); ts.frames->clear();`）
            // 对称补一句。当前不可达——drive_decoder() 的循环结构保证
            // 走到这里之前 ts.packets 必然已经排空（NeedInput 分支只有
            // 在包队列空了才会发 flush，flush 之后才可能收到 Eof），
            // 已打点验证过。但那是"当前实现细节"给出的不变量，不是这个
            // 分支自身声明的契约——留一个"decoder_eof 置位但队列非空"的
            // 结构性陷阱（无诊断永久 Blocked，同一类脆弱性）没有
            // 必要，一行免费消除。
            ts.packets->clear();
            return DriveResult::Eof;
        }
        if (rr == Receive::Error) {
            return DriveResult::Error;
        }

        // NeedInput：先看包队列里还有没有待送的包。
        if (!ts.packets->empty()) {
            AVPacket* pkt = ts.packets->pop();
            const syp_status s = dec.send(pkt);
            av_packet_free(&pkt);
            if (s != SYP_OK) return DriveResult::Error;
            continue;
        }
        // 包队列空了：如果整条流已经 demux 完，且还没给这条轨发过 flush
        // 信号，发一次再继续排（drain 阶段可能还会吐出缓冲帧）。
        //
        // `!ts.eof_sent` 这个条件，实测在
        // draining 触发的重入调用里从未走到过假分支——一旦 send(nullptr)
        // 送进去，receive() 只会给 0（帧）或 AVERROR_EOF，不会再给
        // EAGAIN（NeedInput）；也就是说"已经发过 flush、又在这里被再问
        // 一次要不要发"这个状态在当前 libavcodec 语义下不可达。不要删掉
        // 这个条件——它是对 receive() 契约变化的防御：换个 libavcodec
        // 版本或换个编码器万一确实会在 EOF 之后又吐一次 NeedInput，没有
        // 这个条件会对同一条轨重复 send(nullptr)，多半被 FFmpeg 拒绝、
        // `s != SYP_OK`，整条轨因此在 step() 里被标记 failed、静默丢弃
        // 它队列里还没取走的帧。只是不要把它当成一条常年被走到的活代码
        // 去测（比如指望靠某条用例覆盖它的假分支）——它是防御性的死代码。
        if (demux_eof && !ts.eof_sent) {
            ts.eof_sent = true;
            const syp_status s = dec.send(nullptr);
            if (s != SYP_OK) return DriveResult::Error;
            continue;
        }
        return DriveResult::NoWork;
    }
}

// M-2：任何一个容量字段为 0 都会造成「无诊断的永久挂起」——full() 恒真、
// decode/demux 分支恒不满足、Eof 判定也恒不满足（队列压根没机会变空，
// 因为从来没能装进去过任何东西），实测 max_frames_per_track=0 时
// 5000 步内 blocked=4999、reached_eof=0，且没有任何错误返回值提示调用方
// 「配置本身就是坏的」。选择在创建时就拒绝，而不是悄悄 clamp 到 1——
// 跟 Demuxer::open_avio(nullptr, ...) / 解码器 open() 遇到不匹配
// codec_type 时的风格一致：非法输入直接报错，不猜调用方想要什么。
bool config_is_valid(const PipelineConfig& cfg) noexcept {
    return cfg.max_packets_per_track >= 1 &&
           cfg.max_bytes_per_track   >= 1 &&
           cfg.max_frames_per_track  >= 1 &&
           (!cfg.demux_thread || (cfg.max_buffer_ms >= 1 && cfg.max_buffer_bytes >= 1));
}

}  // namespace

std::unique_ptr<Pipeline> Pipeline::create_common(std::unique_ptr<Demuxer> demuxer,
                                                    const PipelineConfig& cfg,
                                                    syp_status* err) {
    auto p       = std::unique_ptr<Pipeline>(new Pipeline());
    p->demuxer_  = std::move(demuxer);
    p->cfg_      = cfg;

    AVFormatContext* fmt = p->demuxer_->raw();
    const unsigned   n   = fmt->nb_streams;
    p->tracks_.resize(n);

    for (unsigned i = 0; i < n; ++i) {
        const AVStream*          st  = fmt->streams[i];
        const AVCodecParameters* par = st->codecpar;
        TrackState&              ts  = p->tracks_[i];

        // 挑轨道用 codec_type，不用 is_video/!is_video——demuxer.h 的
        // TrackInfo 上方警示：字幕/数据/附件轨的 is_video 也是 false，
        // 但没有对应解码器，不能被当成「音频轨」误管理。
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            ts.managed  = true;
            ts.is_video = true;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            ts.managed  = true;
            ts.is_video = false;
        } else {
            continue;   // 不托管：该轨的 packet 在 demux 阶段直接丢弃。
        }

        // 线程模式下包队列不设上限：停读由加载线程在读之前按水位判定，
        // push 永不因容量失败——失败就意味着一个已从源里读走的包丢了。
        const std::size_t q_packets = cfg.demux_thread ? std::numeric_limits<std::size_t>::max()
                                                       : cfg.max_packets_per_track;
        const int64_t     q_bytes   = cfg.demux_thread ? std::numeric_limits<int64_t>::max()
                                                       : cfg.max_bytes_per_track;
        ts.packets = std::make_unique<PacketQueue>(q_packets, q_bytes, st->time_base);
        ts.frames  = std::make_unique<FrameQueue>(cfg.max_frames_per_track);

        syp_status open_err = SYP_OK;
        if (ts.is_video) {
            const bool cover_art = (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
            ts.cover_art = cover_art;
            const VideoDecodeMode mode = cover_art ? VideoDecodeMode::Software : cfg.video_decode;
            ts.video_dec = std::make_unique<FFmpegVideoDecoder>(mode, cfg.hw_backend);
            open_err     = ts.video_dec->open(par, st->time_base);
            // 选了硬解却打不开 → 整体失败，不退化成"视频轨终止、只剩音频"
            // ——那本身就是一种静默兜底。任何非 OK 都算：
            // 不支持/prepare 失败是 NOT_IMPLEMENTED，avcodec_open2 失败是 IO，
            // parameters_to_context 失败是 INVALID_ARG——原错误码原样上报。
            if (mode == VideoDecodeMode::Hardware && open_err != SYP_OK) {
                if (err != nullptr) *err = open_err;
                return nullptr;
            }
        } else {
            ts.audio_dec = std::make_unique<FFmpegAudioDecoder>();
            open_err     = ts.audio_dec->open(par, st->time_base);
        }
        // 该轨终止，不是整体终止：这条轨的解码器打不开（不支持的编码等），
        // 其它轨继续工作，上层用 track_failed() 查得到。
        if (open_err != SYP_OK) ts.failed = true;
    }

    // 水位参与判定。tracks() 与 fmt->streams 一一对应（build_tracks 按流号建）；
    // discard 在 open_prepared 的 after_open 里已定（HLS 选轨），此刻是最终值。
    const std::vector<TrackInfo>& infos = p->demuxer_->tracks();
    p->buffer_track_.assign(n, 0);
    p->live_.assign(n, 0);   // 此时加载线程尚未启动，不需要锁
    for (unsigned i = 0; i < n; ++i) {
        const TrackState& ts      = p->tracks_[i];
        const bool        discard = i < infos.size() && infos[i].discard;
        p->buffer_track_[i] = (ts.managed && !ts.cover_art && !discard) ? 1 : 0;
        p->live_[i]         = (ts.managed && !ts.failed) ? 1 : 0;
    }

    return p;
}

std::unique_ptr<Pipeline> Pipeline::create_file(const std::string& path,
                                                  const PipelineConfig& cfg,
                                                  syp_status* err) {
    syp_status local = SYP_OK;
    if (err == nullptr) err = &local;
    *err = SYP_OK;

    if (!config_is_valid(cfg)) {
        *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    auto d = Demuxer::open_file(path, err);
    if (d == nullptr) return nullptr;
    auto p = create_common(std::move(d), cfg, err);
    if (p != nullptr) p->start_loader();
    return p;
}

std::unique_ptr<Pipeline> Pipeline::create_avio(AVIOContext* ctx,
                                                  const PipelineConfig& cfg,
                                                  syp_status* err,
                                                  std::function<void()> io_abort) {
    syp_status local = SYP_OK;
    if (err == nullptr) err = &local;
    *err = SYP_OK;

    if (!config_is_valid(cfg)) {
        *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    // 不接管 ctx 的所有权：Demuxer::open_avio 本身就不关闭调用方的
    // AVIOContext（见 demuxer.h/.cpp 顶部注释），Pipeline 在这之上什么
    // 都不额外做，所有权规则原样传导。
    auto d = Demuxer::open_avio(ctx, err);
    if (d == nullptr) return nullptr;
    auto p = create_common(std::move(d), cfg, err);
    if (p != nullptr) {
        p->io_abort_ = std::move(io_abort);
        p->start_loader();
    }
    return p;
}

std::unique_ptr<Pipeline> Pipeline::create_hls(const std::string& url,
                                                const PipelineConfig& cfg,
                                                const syp_config& dl_cfg,
                                                const HlsOptions& hls_opts,
                                                syp_status* err) {
    syp_status local = SYP_OK;
    if (err == nullptr) err = &local;
    *err = SYP_OK;

    if (!config_is_valid(cfg)) {
        *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    // create() 只装配、不 open——全工程 avformat_open_input 只有
    // Demuxer 那一处调用点，见 hls_session.h 里 create() 上方的注释。
    auto session = hls::HlsSession::create(url, dl_cfg, hls_opts, err);
    if (session == nullptr) return nullptr;

    AVFormatContext* fmt = session->release_fmt();
    // after_open 挂选轨：AVProgram 要 avformat_open_input 之后才
    // 存在，而 build_tracks() 读的 st->discard 必须已经是最终值——
    // open_prepared() 把回调点固定在这两步之间正是为此，见 demuxer.h。
    // 裸指针捕获是安全的：回调只在 open_prepared() 内部同步调用一次，
    // 而 session 在本函数整段都活着。
    hls::HlsSession* sess = session.get();
    // options 不能传空：漏掉 http_persistent=0 会让 hls 走 keepalive 分支
    // 去对我们的自定义 AVIOContext 做 av_assert0(ffio_geturlcontext(pb))，
    // 进程直接 abort（实测）。
    auto d = Demuxer::open_prepared(fmt, session->ffmpeg_url(), session->open_options(),
                                    [sess](AVFormatContext* f) { sess->select_variant(f); },
                                    err);
    if (d == nullptr) {
        // 【把归一化的 SYP_ERR_IO 换回精确原因】avformat_open_input 失败时
        // Demuxer::open_prepared() 只报 SYP_ERR_IO——它的错误粒度到"打不
        // 开"为止，不区分"为什么"。HlsSession 在 io_open 里主动拒绝时
        // （比如加密判定）记得更精确的原因，这里换回去：调用方要的是
        // "为什么拒"，不是"哪一层拒的"。session 没拒绝过时
        // precise_open_error() 是 SYP_OK，不覆盖 open_prepared() 已经写
        // 好的 SYP_ERR_IO。
        const syp_status precise = sess->precise_open_error();
        if (precise != SYP_OK) *err = precise;
        return nullptr;
    }

    auto p = create_common(std::move(d), cfg, err);
    if (p == nullptr) return nullptr;
    // open 阶段到此结束：之后的播放列表失败才算播放期错误（#46/#47），
    // 见 HlsSession::mark_playback_started() 上方注释。
    session->mark_playback_started();
    // 顺序无关紧要（成员声明顺序才是承重件），但赋值必须发生：
    // 没有它，HlsSession 会在本函数返回时析构，而 io_open 回调在播放
    // 期间还要用它。
    p->hls_ = std::move(session);
    p->start_loader();
    return p;
}

StepOutcome Pipeline::step() {
    // 【中止之后不再往下走，三条打开路径同一个契约】
    // 入口先查：调用方在两次 step() 之间 request_abort() 时，队列里剩下的
    // 包/帧不再解，直接报 CANCELED。正卡在读里的那次 step() 由下面 demux
    // Error 分支与 Eof 出口两处改写兜住（被打断的读在 HLS 上收敛成 EOF，
    // 在调用方 AVIOContext 上以 AVERROR_EXIT 冒成 Error）。
    if (aborted_.load(std::memory_order_acquire)) {
        return StepOutcome{StepOutcome::Kind::Error, -1, SYP_ERR_CANCELED};
    }
    // 线程模式：终止标记 → demux_eof_ / demux_term_status_（见 loader_main 上方注释）。
    if (cfg_.demux_thread) absorb_load_terminal();
    // 外层 for(;;) 只在「demux 刚发现整体 EOF」这一刻才 continue 一次
    // ——让本次 step() 调用能立刻回头把 flush 信号发给还没排空的解码器，
    // 不必多等一轮 step()。这个 continue 至多发生一次（demux_eof_ 从
    // false 翻到 true 只会发生一次，直到下次 seek() 复位），不是无限
    // 内部等待：每一轮 continue 都伴随着真实状态推进。
    for (;;) {
        // 1) 解码分支：按轨号升序，谁先满足条件就处理谁。
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            TrackState& ts = tracks_[i];
            if (!ts.managed || ts.failed || ts.decoder_eof) continue;
            if (ts.frames->full()) continue;

            const bool has_pending_input = !ts.packets->empty();
            const bool has_pending_flush = demux_eof_ && !ts.eof_sent;
            // flush 信号发出的那一刻（drive_decoder 里
            // send(nullptr) 一执行，ts.eof_sent 立刻置位）不等于这条轨
            // 已经排空——解码器内部可能还压着 B 帧重排序延迟的缓冲帧，
            // 要再receive() 几次才会真的吐到 Receive::Eof、进而
            // ts.decoder_eof 才会置位。has_pending_input/has_pending_flush
            // 两者都只问「还有没有新东西可以 send」，回答不了「已经发过
            // flush、可 receive() 还没吐到底」这第三种状态——这正是原句
            // 漏掉的分支：两个条件都假就直接 continue 跳过这条轨，而
            // ts.decoder_eof 仍是 false，下一轮 demux/Eof 判定都卡在它
            // 身上，永久 Blocked。draining 补的就是这第三种状态：已经
            // demux 到底、已经给这条轨发过 flush、但它还没排空到
            // decoder_eof——这时哪怕没有新包可送，也必须再进
            // drive_decoder() 让它 receive() 一次，才有机会把缓冲帧
            // 吐出来或者最终吃到 Eof。
            //
            // 不会反过来造成空转：draining 为真的分支只会在两种结局收尾——
            //   a) receive() 吐出一帧或 Eof/Error：跟平常一样返回给
            //      调用方或标记 decoder_eof/failed，这条轨从此不再进
            //      入这个 for 循环（:212 已经排除 decoder_eof/failed）。
            //   b) receive() 仍是 NeedInput：drive_decoder 内部因为包
            //      队列空、且 eof_sent 已经是 true，直接落到
            //      `return DriveResult::NoWork`（不会再发一次 flush，
            //      send(nullptr) 只发一次是 eof_sent 本身的契约）——
            //      NoWork 情况下 step() 只是看下一条轨/进 demux 分支/
            //      判 Eof，不会原地重复横跳。
            //
            // 三态穷举，无第四种：!demux_eof_ 时只可能是 (a)；demux_eof_
            // 之后 eof_sent 非真即假，必然落在 (b)/(c) 之一——所以
            // (b)||(c) 恒等于 demux_eof_，本行等价于
            // `if (!has_pending_input && !demux_eof_)`。有意不写成那个
            // 等价式：判据的语义是「这条轨这一轮有事可做」的状态枚举
            // （此前的根因就是漏了其中一态），写成 demux_eof_ 会把「为
            // 什么 demux 到底之后一定有事可做」这层推理彻底藏进注释，
            // 且将来加第四态没有挂载点。改这里必须同步改 pipeline.h
            // 顶部 1.a/b/c。
            const bool draining = demux_eof_ && ts.eof_sent;
            if (!has_pending_input && !has_pending_flush && !draining) continue;

            Frame       produced;
            DriveResult r = ts.is_video
                ? drive_decoder(*ts.video_dec, ts, demux_eof_, &produced)
                : drive_decoder(*ts.audio_dec, ts, demux_eof_, &produced);
            // drive_decoder 可能刚从包队列取走了包：唤醒可能停在水位上的加载线程。
            if (cfg_.demux_thread) wake_loader();

            if (r == DriveResult::Frame) {
                const bool pushed = ts.frames->push(std::move(produced));
                // 进入这个分支前已经确认 !ts.frames->full()，且本函数是
                // 单线程同步调用，push 在此处不可能失败。
                (void)pushed;
                return StepOutcome{StepOutcome::Kind::DecodedFrame,
                                    static_cast<int32_t>(i), SYP_OK};
            }
            if (r == DriveResult::Error) {
                // 该轨终止：清空它的队列，不再让它参与后续任何判定
                // （Eof 判定、demux 背压判定都要把失效轨当成「已完成」）。
                mark_track_failed(i);
                continue;
            }
            // NoWork 或 Eof：这条轨这一轮没有帧可交，看下一条轨。
        }

        // 2) demux 分支：只有当「所有还需要收包的被管理轨」都还有空位时才
        // 读下一个 packet——av_read_frame 是顺序读，读到的包属于哪条轨不
        // 由我们选择；只要有任何一条还需要收包的被管理轨已经满了，读到
        // 的下一个包就可能恰好是它的、push 会失败且这个包已经从底层源里
        // 消费掉、无法放回去。保守到「全员有空位才读」是唯一不丢包的
        // 做法。
        //
        // 「还需要收包」排除两类轨：failed（该轨终止）与 decoder_eof
        // （该轨解码器已经排空到底，flush 信号也发过了，往后这条轨永远
        // 不会再被解码分支处理）——这两类轨的 packet 队列此后只会被
        // demux 分支丢弃（见下面 push 那一段），绝不会再变空，若仍然
        // 拿它们的 full() 状态去决定能不能继续 demux，一旦它们的队列
        // 恰好是满的，就会把其它还活着的轨也一起锁死在 Blocked：
        // decoder_eof 的轨此前漏了这条排除，注入验证过会造成
        // 300000 步内 blocked=299637、reached_eof=0 的永久卡死。
        //
        // 线程模式下 demux 由加载线程做，这一步整个跳过。
        if (!cfg_.demux_thread && !demux_eof_) {
            bool all_have_room = true;
            for (const TrackState& ts : tracks_) {
                if (ts.managed && !ts.failed && !ts.decoder_eof && ts.packets->full()) {
                    all_have_room = false;
                    break;
                }
            }

            if (all_have_room) {
                AVPacket* pkt = nullptr;
                int32_t   ti  = -1;
                const Demuxer::ReadResult rr = demuxer_->read(&pkt, &ti);

                if (rr == Demuxer::ReadResult::Packet) {
                    const bool in_range = ti >= 0 &&
                        static_cast<std::size_t>(ti) < tracks_.size();
                    TrackState* ts = in_range ? &tracks_[static_cast<std::size_t>(ti)]
                                                : nullptr;
                    if (ts != nullptr && ts->managed && !ts->failed && !ts->decoder_eof) {
                        const bool pushed = ts->packets->push(pkt);
                        // all_have_room 已经确认这条轨没满；push 不该失败，
                        // 但仍兜底释放，绝不吞掉一个悬空指针。
                        if (!pushed) av_packet_free(&pkt);
                    } else {
                        // 未被管理的轨（字幕/数据/附件）、已失效轨、或
                        // 已经排空到底的轨：这个包没有队列可去（或者说
                        // 去了也永远不会再被取走），直接丢弃。
                        av_packet_free(&pkt);
                    }
                    return StepOutcome{StepOutcome::Kind::DemuxedPacket, ti, SYP_OK};
                }
                if (rr == Demuxer::ReadResult::Eof) {
                    demux_eof_ = true;
                    continue;   // 回到 for(;;) 顶部，立刻给还没排空的轨发 flush。
                }
                // ReadResult::Error：demux 层的错误（含 dl 层报错冒上来）
                // 属于整体终止——不再推进。被 request_abort() 打断的读
                // （io_abort 钩子让调用方的读回调返回 AVERROR_EXIT）也从
                // 这里出来，报 CANCELED 而不是笼统的 IO。
                if (aborted_.load(std::memory_order_acquire)) {
                    return StepOutcome{StepOutcome::Kind::Error, -1, SYP_ERR_CANCELED};
                }
                return StepOutcome{StepOutcome::Kind::Error, -1, SYP_ERR_IO};
            }
        }

        // 3) Eof：demux 已经整体结束，且每条被管理轨都已经终止/排空，
        // 且它们的 packet/frame 队列都空了（frame 队列空意味着调用方已经
        // 把该轨解出的帧都取走了——没取走的帧不算「排空」）。
        bool all_done = demux_eof_;
        if (all_done) {
            for (const TrackState& ts : tracks_) {
                if (!ts.managed || ts.failed) continue;
                if (!ts.decoder_eof) { all_done = false; break; }
                if (!ts.packets->empty() || !ts.frames->empty()) { all_done = false; break; }
            }
        }
        if (all_done) {
            // 【被中止过就不许报 Eof】见 pipeline.h 里 request_abort() 上方
            // 的长注释：FFmpeg 把一次被打断的读收敛成 AVERROR_EOF，照抄的话
            // 调用方拿到的是"正常播完了"。这里是 Eof 的唯一出口，改写一处
            // 就够。status 用 SYP_ERR_CANCELED（syp_types.h：「被
            // syp_source_interrupt 打断」），跟底层实际发生的事对得上。
            if (aborted_.load(std::memory_order_acquire)) {
                return StepOutcome{StepOutcome::Kind::Error, -1, SYP_ERR_CANCELED};
            }
            // 【线程模式的错误终止标记】已缓冲的数据此刻已经全部交出，
            // 现在才报错。同步模式下 demux_term_status_ 恒为 SYP_OK。
            if (demux_term_status_ != SYP_OK) {
                return StepOutcome{StepOutcome::Kind::Error, -1, demux_term_status_};
            }
            // 【分片打开失败可能被 hls.c 吞成"干净的" EOF——见
            // HlsSession::open_segment / pending_segment_error() 上方注释】
            // hls 解封装器对分片失败的默认策略是跳过、继续下一片，
            // playlist 耗尽时一样走 AVERROR_EOF 这条路，Demuxer::read()
            // 那一层完全分辨不出来。这里是 Eof 的唯一出口，跟上面那条
            // aborted_ 改写同一处堵：用户不该看到"播完了"，其实是一半
            // 内容没下下来（分片 404 之类）。
            if (hls_ != nullptr) {
                const syp_status seg_err = hls_->pending_segment_error();
                if (seg_err != SYP_OK) {
                    return StepOutcome{StepOutcome::Kind::Error, -1, seg_err};
                }
                // 【播放列表那条通道的同一个洞，#46/#47】直播重拉失败或
                // 中途变加密时，hls.c 不重试、直接结束那一路列表，同样
                // 收敛成干净的 EOF。排在分片错误之后：两者同时成立时，
                // 分片那次通常更早发生、也更具体。
                const syp_status pl_err = hls_->pending_playlist_error();
                if (pl_err != SYP_OK) {
                    return StepOutcome{StepOutcome::Kind::Error, -1, pl_err};
                }
            }
            return StepOutcome{StepOutcome::Kind::Eof, -1, SYP_OK};
        }

        // 4) 都不满足：背压是返回值，不是内部阻塞——直接把 Blocked 交还
        // 给调用方，由它决定怎么排空（测试/差分代码，真实
        // 消费者）。绝不在这里 while 等待。
        return StepOutcome{StepOutcome::Kind::Blocked, -1, SYP_OK};
    }
}

std::optional<Frame> Pipeline::pop_frame(int32_t track_index) {
    if (track_index < 0 || static_cast<std::size_t>(track_index) >= tracks_.size()) {
        return std::nullopt;
    }
    TrackState& ts = tracks_[static_cast<std::size_t>(track_index)];
    if (!ts.managed) return std::nullopt;
    return ts.frames->pop();
}

syp_status Pipeline::seek(int64_t ts_us) {
    if (cfg_.demux_thread) return seek_async(ts_us);
    // 顺序固定，第 1 步先清队列，不管后面 seek 会不会成功：
    // 就算 avformat_seek_frame 失败，旧的队列内容对调用方也已经没有意义。
    for (TrackState& ts : tracks_) {
        if (!ts.managed) continue;
        ts.packets->clear();
        ts.frames->clear();
    }

    // I-1：demuxer_->seek() 失败时不能提前 return——本函数头文件注释
    // 承诺的是「同步、原子」的 seek：调用方要么拿到一个完全复位好的
    // Pipeline（可以放心继续 step()），要么状态未定义要重建，不能是
    // 「队列已经清空、但解码器没 flush、EOF 相关标志也没复位」这种半
    // 吊子状态。这里选前者：不管 rc 是否为 SYP_OK，第 3、4 步都照跑，
    // 只是把失败状态原样透传给调用方（rc 仍在函数末尾返回）。
    const syp_status rc = demuxer_->seek(ts_us);

    // 【#48：成功的 seek 开启新的一轮播放】HLS 的分片/播放列表错误账回答的
    // 是"从上一次定位起有没有漏内容"，旧位置上的失败与新一轮无关。必须在
    // demuxer_->seek() 返回之后清：hls.c 的 seek 会关掉旧分片，on_io_close
    // 可能恰在那时补记一笔。seek 失败时不清——位置没变成新的，旧账仍然
    // 描述着当前这一轮。aborted_ 不在此列（不可逆，见 pipeline.h）。
    if (rc == SYP_OK && hls_ != nullptr) hls_->clear_playback_errors();

    // 各解码器 flush：丢弃内部缓冲状态（包括 FFmpeg send/receive 状态机
    // 里「乐观解码」预先解出、还没被 receive() 取走的那一帧）。这条机制
    // 见 libavcodec/decode.c、avcodec.c::avcodec_flush_buffers
    // 的实现：avcodec_flush_buffers 对
    // AVCodecInternal 私有的 buffer_frame 做 av_frame_unref，公开 API 里
    // 唯一能清掉它的就是这一步。ffmpeg_video_decoder.cpp 的 flush() 本身
    // 只是转发这一次调用，没有额外逻辑。
    for (TrackState& ts : tracks_) {
        if (!ts.managed || ts.failed) continue;
        if (ts.is_video) ts.video_dec->flush();
        else             ts.audio_dec->flush();
    }

    // 复位 EOF 相关标志与各轨状态。track_failed 不复位：解码器初始化
    // 失败是跟编码格式相关的持久状态，seek 到别的位置不会让不支持的
    // 编码变得支持，把它当成「跟位置无关」的失效更诚实。
    // skipped_packets（在各解码器内部累加，本类不持有单独计数）也不
    // 复位：它是「自 Pipeline 创建以来的累计值」，seek 不清零对场景 F
    // 类的断言（累计计入损坏包）更有用——「复位各轨
    // 计数」特指这里的 eof_sent/decoder_eof，不含 skipped_packets。
    demux_eof_ = false;
    for (TrackState& ts : tracks_) {
        if (!ts.managed) continue;
        ts.eof_sent    = false;
        ts.decoder_eof = false;
    }
    return rc;
}

// =====================================================================
// 加载线程
// =====================================================================
//
// 【谁拥有什么】demuxer_ 的 read/seek 在线程启动后只由加载线程调用；解码器、
// FrameQueue、TrackState 的 failed/eof_sent/decoder_eof、demux_eof_、
// demux_term_status_ 只由泵线程碰；PacketQueue 自带锁；load_mu_ 保护
// stop_loader_/seek_gen_/served_gen_/seek_target_us_/load_term_/load_term_status_/
// live_。锁序 load_mu_ → PacketQueue::mu_。
//
// 【代号】泵线程 seek：持 load_mu_ 清空包队列并 ++seek_gen_。加载线程每次读/seek
// 返回后重新持锁比对代号，不符就丢弃结果——入队与清空在同一把锁下互斥，旧位置的包
// 不可能在清空之后进入队列，所以包本身不需要带代号。
//
// 【终止标记】EOF/IO 错误在最后一个包入队之后、同一把锁下写 load_term_；泵线程在
// step() 入口 absorb_load_terminal() 把它折进 demux_eof_，此后复用同步模式的
// flush/排空/Eof 判定。

void Pipeline::start_loader() {
    if (!cfg_.demux_thread) return;
    loader_ = std::thread([this] { loader_main(); });
}

bool Pipeline::loader_has_work_locked() const {
    if (stop_loader_ || aborted_.load(std::memory_order_acquire)) return true;
    if (served_gen_ != seek_gen_) return true;
    return load_term_ == LoadTerminal::None && !water_full_locked();
}

// 【停读水位】加载线程不知道播放位置，"已缓冲时长"用各在播轨包队列的
// 队列时长（队尾结束 − 队首开始）近似——差的是已解码未播出的那一截（FrameQueue 与
// 音频环，亚秒级）。参与 min 的轨：buffer_track_（受管、非封面、非 discard）
// 且 live_；没有这样的轨时时长条件不成立（包反正都会被丢弃，读到 EOF 为止）。字节
// 条件统计全部受管轨。判定在读之前做，所以两个水位都至多超出一个包。
//
// 【锁】持 load_mu_ 调，内部再取各 PacketQueue::mu_（锁序 load_mu_ → mu_）。泵线程
// pop 不持 load_mu_，所以这里读到的是某个时刻的快照——可能偏"满"，但泵线程 pop 之后
// 必然 wake_loader()，加载线程会重新评估，不会停死。
bool Pipeline::water_full_locked() const {
    int64_t total_bytes = 0;
    int64_t min_dur     = std::numeric_limits<int64_t>::max();
    bool    any         = false;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const TrackState& ts = tracks_[i];
        if (!ts.managed) continue;
        total_bytes += ts.packets->bytes();
        if (buffer_track_[i] == 0 || live_[i] == 0) continue;
        any     = true;
        min_dur = std::min(min_dur, ts.packets->queued_duration_us());
    }
    if (total_bytes >= cfg_.max_buffer_bytes) return true;
    return any && min_dur >= cfg_.max_buffer_ms * 1000;
}

// 不丢唤醒：pop 发生在拿锁之前；加载线程从评估谓词到进入等待全程持锁——泵线程拿到
// 锁时它要么还没评估（会看见 pop 之后的队列），要么已在等待（收到 notify）。
// notify 放在解锁之后：被唤醒的加载线程不必立刻撞上泵线程还持着的锁。
void Pipeline::wake_loader() {
    {
        std::lock_guard<std::mutex> g(load_mu_);
    }
    load_cv_.notify_one();
}

void Pipeline::push_loaded_packet_locked(AVPacket* pkt, int32_t track_index) {
    const bool in_range = track_index >= 0 &&
                          static_cast<std::size_t>(track_index) < tracks_.size();
    if (!in_range) {
        av_packet_free(&pkt);
        return;
    }
    const std::size_t i = static_cast<std::size_t>(track_index);
    // 不受管（字幕/数据）、已失效：跟同步模式 demux 分支同一口径，直接丢弃。
    if (!tracks_[i].managed || live_[i] == 0) {
        av_packet_free(&pkt);
        return;
    }
    // 线程模式下队列无上限，push 不会失败；仍兜底释放，绝不泄漏。
    if (!tracks_[i].packets->push(pkt)) av_packet_free(&pkt);
}

void Pipeline::loader_main() {
    std::unique_lock<std::mutex> lk(load_mu_);
    for (;;) {
        load_cv_.wait(lk, [this] { return loader_has_work_locked(); });
        if (stop_loader_ || aborted_.load(std::memory_order_acquire)) return;

        if (served_gen_ != seek_gen_) {
            const uint64_t gen    = seek_gen_;
            const int64_t  target = seek_target_us_;
            loading_.store(true, std::memory_order_release);
            lk.unlock();
            const syp_status rc = demuxer_->seek(target);
            // #48：成功的 seek 开启新一轮，旧位置的 HLS 错误账作废（与同步模式同一时机：
            // demuxer seek 返回之后）。
            if (rc == SYP_OK && hls_ != nullptr) hls_->clear_playback_errors();
            lk.lock();
            loading_.store(false, std::memory_order_release);
            if (gen != seek_gen_) continue;   // 期间又来了新的 seek：这次作废，重做
            served_gen_ = gen;
            if (rc != SYP_OK) {
                load_term_        = LoadTerminal::Error;
                load_term_status_ = rc;
            }
            continue;
        }

        const uint64_t gen = served_gen_;
        loading_.store(true, std::memory_order_release);
        lk.unlock();
        AVPacket*                 pkt = nullptr;
        int32_t                   ti  = -1;
        const Demuxer::ReadResult rr  = demuxer_->read(&pkt, &ti);
        lk.lock();
        loading_.store(false, std::memory_order_release);
        if (stop_loader_ || aborted_.load(std::memory_order_acquire)) {
            av_packet_free(&pkt);   // pkt 可能为空，av_packet_free 对此安全
            return;
        }
        if (gen != seek_gen_) {     // 读的是旧位置：整体作废
            av_packet_free(&pkt);
            continue;
        }
        if (rr == Demuxer::ReadResult::Packet) {
            push_loaded_packet_locked(pkt, ti);
            continue;
        }
        load_term_        = (rr == Demuxer::ReadResult::Eof) ? LoadTerminal::Eof : LoadTerminal::Error;
        load_term_status_ = (rr == Demuxer::ReadResult::Eof) ? SYP_OK : SYP_ERR_IO;
    }
}

void Pipeline::absorb_load_terminal() {
    if (demux_eof_) return;
    std::lock_guard<std::mutex> g(load_mu_);
    if (load_term_ == LoadTerminal::None) return;
    demux_eof_         = true;
    demux_term_status_ = load_term_status_;
}

void Pipeline::mark_track_failed(std::size_t i) {
    TrackState& ts = tracks_[i];
    ts.failed = true;
    {
        // 先断加载线程的入队（持锁置 live_），再清队列：清空之后不会再有这条轨的包进来。
        std::lock_guard<std::mutex> g(load_mu_);
        live_[i] = 0;
    }
    ts.packets->clear();
    ts.frames->clear();
    if (cfg_.demux_thread) wake_loader();   // 清掉的字节、少了一条参与 min 的轨，都可能让水位降下来
}

syp_status Pipeline::seek_async(int64_t ts_us) {
    // 旧包在锁内摘出、锁外释放：清空必须与 ++seek_gen_ 同在 load_mu_ 下（代号不变量），
    // 但几十 MB 的 av_packet_free 没必要让加载线程等着——它醒来第一件事就要拿这把锁。
    std::vector<std::deque<AVPacket*>> stale;
    stale.reserve(tracks_.size());
    {
        std::lock_guard<std::mutex> g(load_mu_);
        for (TrackState& ts : tracks_) {
            if (ts.managed) stale.push_back(ts.packets->take_all());
        }
        ++seek_gen_;
        seek_target_us_   = ts_us;
        load_term_        = LoadTerminal::None;
        load_term_status_ = SYP_OK;
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            live_[i] = (tracks_[i].managed && !tracks_[i].failed) ? 1 : 0;   // failed 不复位
        }
    }
    load_cv_.notify_all();
    for (std::deque<AVPacket*>& q : stale) {
        for (AVPacket*& pkt : q) av_packet_free(&pkt);
    }

    // 以下是泵线程自己的状态，与同步 seek 第 1/3/4 步相同。
    for (TrackState& ts : tracks_) {
        if (ts.managed) ts.frames->clear();
    }
    for (TrackState& ts : tracks_) {
        if (!ts.managed || ts.failed) continue;
        if (ts.is_video) ts.video_dec->flush();
        else             ts.audio_dec->flush();
    }
    demux_eof_         = false;
    demux_term_status_ = SYP_OK;
    for (TrackState& ts : tracks_) {
        if (!ts.managed) continue;
        ts.eof_sent    = false;
        ts.decoder_eof = false;
    }
    return SYP_OK;
}

// 【为什么转发这两步不加锁】hls_ 在构造之后到析构之前是只读的（只有
// create_hls 写过它一次，那时管线还没交给任何人），HlsSession::request_abort()
// 自己负责内部同步。（末尾那次 load_mu_ 只为唤醒加载线程，转发时不持锁：
// io_abort_ 可能要拿调用方自己的锁，持 load_mu_ 调它会引入锁序。）而生命周期的边界由调用方保证：request_abort() 不能与
// ~Pipeline() 重叠——这跟 avio_bridge.h 里 AvioBridge::request_abort() 那条
// 契约是同一条，往上传了一级。看门狗线程必须先 join / 停掉，再销毁管线。
void Pipeline::request_abort() noexcept {
    // 先置本地标志再往下转发：step() 可能正并发跑着，先置位保证它在任何
    // 交错下都不会把这次中止导致的收尾误报成 Eof。反过来的顺序有一个窗口
    // ——底层已经被打断、Eof 已经冒上来，而 aborted_ 还是 false。
    aborted_.store(true, std::memory_order_release);
    if (hls_ != nullptr) hls_->request_abort();
    if (io_abort_) io_abort_();
    // 唤醒可能停在 cv 上的加载线程。先拿一次锁再 notify：加载线程从评估谓词
    // 到进入等待全程持锁，这样不会丢掉这次唤醒。同步模式下没有等待者，无副作用。
    {
        std::lock_guard<std::mutex> g(load_mu_);
    }
    load_cv_.notify_all();
}

int64_t Pipeline::skipped_packets() const noexcept {
    int64_t total = 0;
    for (const TrackState& ts : tracks_) {
        if (!ts.managed) continue;
        total += ts.is_video ? ts.video_dec->skipped_packets() : ts.audio_dec->skipped_packets();
    }
    return total;
}

bool Pipeline::track_failed(int32_t track_index) const noexcept {
    if (track_index < 0 || static_cast<std::size_t>(track_index) >= tracks_.size()) {
        return false;
    }
    return tracks_[static_cast<std::size_t>(track_index)].failed;
}

bool Pipeline::track_drained(int32_t track_index) const noexcept {
    if (track_index < 0 || static_cast<std::size_t>(track_index) >= tracks_.size()) return false;
    const TrackState& ts = tracks_[static_cast<std::size_t>(track_index)];
    if (!ts.managed) return false;
    return ts.decoder_eof && ts.packets->empty() && ts.frames->empty();
}

void Pipeline::set_video_catchup(bool on) noexcept {
    video_catchup_ = on;
    for (TrackState& ts : tracks_) {
        if (ts.managed && ts.is_video && !ts.cover_art && ts.video_dec != nullptr) {
            ts.video_dec->set_skip_nonref(on);
        }
    }
}

bool Pipeline::track_skip_nonref(int32_t track_index) const noexcept {
    if (track_index < 0 || static_cast<std::size_t>(track_index) >= tracks_.size()) return false;
    const TrackState& ts = tracks_[static_cast<std::size_t>(track_index)];
    if (!ts.managed || !ts.is_video || ts.video_dec == nullptr) return false;
    // create_common() 只往 video_dec 里放 FFmpegVideoDecoder（唯一的
    // IVideoDecoder 实现），这个 static_cast 因此成立（同 video_hardware_decoding()）。
    const auto* d = static_cast<const FFmpegVideoDecoder*>(ts.video_dec.get());
    return d->skip_nonref();
}

bool Pipeline::video_hardware_decoding() const noexcept {
    // create_common() 只往 video_dec 里放 FFmpegVideoDecoder（唯一的
    // IVideoDecoder 实现），这个 static_cast 因此成立。
    for (const TrackState& ts : tracks_) {
        if (ts.managed && ts.is_video && ts.video_dec != nullptr) {
            const auto* d = static_cast<const FFmpegVideoDecoder*>(ts.video_dec.get());
            if (d->hardware()) return true;
        }
    }
    return false;
}

BufferStats Pipeline::buffer_stats() const {
    if (buffer_stats_override_) return buffer_stats_override_();
    BufferStats s;
    int64_t until = kBufferedUntilUnbounded;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const TrackState& ts = tracks_[i];
        if (!ts.managed) continue;
        s.queued_bytes += ts.packets->bytes();
        // 同步模式的"读不进更多"：demux 分支的 all_have_room 同一判据。
        if (!ts.failed && !ts.decoder_eof && ts.packets->full()) s.full = true;
        if (buffer_track_[i] == 0 || ts.failed || ts.decoder_eof) continue;
        // AV_NOPTS_VALUE 即 INT64_MIN：任一在播轨还没有包，min 天然落到它上面。
        until = std::min(until, ts.packets->buffered_until_us());
    }
    s.buffered_until_us = until;
    s.demux_eof         = demux_eof_;
    s.loading           = loading_.load(std::memory_order_acquire);
    if (cfg_.demux_thread) {
        // 加载线程写的是 load_term_（持 load_mu_），demux_eof_ 本身泵线程独占；
        // 这里持锁读，终止标记一写入即可见，不必等下一次 step() 吸收。
        std::lock_guard<std::mutex> g(load_mu_);
        s.demux_eof = demux_eof_ || load_term_ != LoadTerminal::None;
        // 线程模式的包队列无上限，上面的 packets->full() 恒假；"读不进更多"= 已达停读水位。
        s.full = water_full_locked();
    }
    return s;
}

}  // namespace syp::media
