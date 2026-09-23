// pipeline.h — 解码管线编排：把 Demuxer / PacketQueue / Decoder / FrameQueue
// 串成一条链，对外只暴露一个 step()，由外层循环驱动。
//
// 组件本身不持有线程（线程模型：组件被动，线程由外层编排）。
// Pipeline 也不例外：step() 只做同步、确定性的一次调用，无 sleep、无锁等待。
// 同一份输入 + 同一串 step() 调用 = 同一串输出——这是差分比对能成立的前提。
//
// step() 的调度顺序固定（改了确定性就没了；下面这份描述是实现的准确
// 转述，不是最初字面算法的转述——字面算法里的「有待取输出」在 FFmpeg
// 公开 API 里查不出来，「任一轨 PacketQueue 未满」按字面实现会丢包，见
// pipeline.cpp 里对应位置的注释）：
//
//   1. 按轨号升序遍历各被管理轨（视频/音频；字幕等不管）：若该轨未失效
//      （!failed）、解码器未排空到底（!decoder_eof）、FrameQueue 未满，
//      且满足下面三种「这条轨这一轮有事可做」之一——
//        a) PacketQueue 非空（还有包没送）
//        b) 已经整体 demux 到 EOF，且还没给它发过 flush 信号
//        c) 已经整体 demux 到 EOF，且已经给它发过 flush 信号，但它还没
//           排空到 decoder_eof（draining：flush 信号发出的那一刻只表示
//           「没有更多包可送了」，不表示解码器内部已经吐空——B 帧一类
//           带参考重排序的编码，flush 之后解码器还可能压着好几帧要靠
//           receive() 才能取出来，见 pipeline.cpp 里 draining 那段注释）
//      → 反复 receive()；遇 NeedInput 就送一个包（或送一次 flush 信号）
//      再 receive()，直到取出一帧 / 遇到 Eof / 确认没有更多输入可送。
//      取出一帧就 push 进 FrameQueue，返回 DecodedFrame。
//   2. 否则：若「所有还需要收包的被管理轨」（排除 failed 与 decoder_eof）
//      都还有空位、且整体还没 demux 到 EOF → demux 一个 packet；它属于
//      哪条轨就入哪条轨的 PacketQueue（不托管的轨、已失效轨、已排空到底
//      的轨直接丢弃这个包），返回 DemuxedPacket。
//   3. 否则：若已经整体 demux 到 EOF，且每条被管理轨都已经 failed 或
//      decoder_eof、且它们的 PacketQueue/FrameQueue 都空了 → Eof。
//   4. 否则 → Blocked（背压是返回值，不是内部阻塞，绝不在内部 while 等待）。
//
// 两条容易忽略的边界，写在这里是因为它们不是「实现细节」而是接口契约：
//
//   - 第 1 步的内部循环最坏情况下会在一次 step() 调用里消费掉某条轨
//     max_packets_per_track 个包，才最终吐出一帧（或确认这条轨吐不出
//     帧）。「一次 step() 推进一个单位」指的是「一次对外可观测的产出」，
//     不是「一次只碰一个 packet」。这个循环不是可省的优化——它是
//     场景 D（音视频交织顺序与参照一致）的唯一保证，详见 pipeline.cpp
//     里 drive_decoder() 的注释。
//   - 第 2 步的背压是「联合」的：任何一条被管理轨（哪怕调用方根本不关心
//     它，比如只要视频、从来不调 pop_frame(音频轨) 的调用方）一旦队列
//     堆满，demux 分支就整体停摆，进而拖死其它轨——因为顺序读的下一个
//     packet 可能属于任何一条轨，选不了。这是正确的背压语义（保证不
//     丢包），但意味着调用方必须消费它托管范围内的**每一条**轨，不能
//     选择性忽略。
//   - 一条轨被标记失效（该轨终止）对调用方是静默的：它此后会丢弃这条
//     轨 FrameQueue 里还没被取走的帧（见 pipeline.cpp step() 里
//     DriveResult::Error 分支）。调用方必须每次 step() 之后（或至少
//     定期）轮询 track_failed() 才能发现这件事，step() 本身不会用任何
//     Kind 报告「某条轨刚刚失效」。
//
// 【线程模式（PipelineConfig::demux_thread）】上面的"组件不持有线程"
// 只对默认的同步模式成立。线程模式下 Pipeline 内部有一条加载线程，只做 demux
// （含网络读）：按轨把包推进线程安全的 PacketQueue，读到 EOF/IO 错误写队尾终止
// 标记。step() 不再 demux——第 2 步整个跳过，Blocked 表示"加载线程这一刻还没送到"；
// 第 3 步的 Eof 在终止标记是错误时改报 Error（先播完已缓冲数据再报）。seek() 异步，
// request_abort() 额外唤醒加载线程，析构 join 它。线程设计（锁、代号、唤醒）见
// pipeline.cpp loader_main() 上方注释「线程设计」一节。线程模式下还有三条对调用方可见的差别：
//   - step() 永远不返回 Kind::DemuxedPacket（demux 不在 step() 里发生）；依赖
//     DemuxedPacket 计步/判活的调用方要改看 DecodedFrame/Blocked。
//   - 包队列不再受 max_packets_per_track/max_bytes_per_track 限制；加载线程在各在播轨
//     包队列时长的最小值 ≥ max_buffer_ms、或全部包队列字节 ≥ max_buffer_bytes 时停读
//     （读之前判定，至多超出一个包），step() 取走包之后唤醒它续读。第 2 步的"联合
//     背压"语义换成了这两条水位：调用方仍须消费托管范围内的每一条轨。
//   - ~Pipeline() 会调 request_abort()（进而调 HLS 会话的 request_abort() 与
//     create_avio 的 io_abort 钩子）来打断阻塞中的读，再 join 加载线程。见 create_avio()
//     上方的生命周期约束。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// syp_config.h 是 create_hls 的参数类型（typedef 的匿名 struct，前置声明
// 不了）。它只 include syp_types.h，两个都是几十行的 POD 头，不构成
// "把 dl 层拖进来"——真正重的是 syp_source.h / hls_session.h 那条链。
#include <syplayer/syp_config.h>
#include <syplayer/syp_types.h>

#include "media/demuxer.h"
#include "media/ffmpeg_audio_decoder.h"
#include "media/ffmpeg_video_decoder.h"
#include "media/frame.h"
#include "media/frame_queue.h"
#include "media/hls/hls_options.h"
#include "media/hw/hw_decode_backend.h"
#include "media/packet_queue.h"
#include "media/video_decoder.h"

extern "C" {
#include <libavformat/avio.h>
}

namespace syp::media {

// 前置声明，**不** include "media/hls/hls_session.h"：那个头拖着
// libavformat/avformat.h 与 dl 层（syp_source/syp_config）的一整串头，
// 而 pipeline.h 被解码路径上几乎每个 TU include。HlsOptions 因此单独
// 住在零依赖的 media/hls/hls_options.h 里（上面那条 include）。
//
// 代价：前置声明 + std::unique_ptr<HlsSession> 成员要求 ~Pipeline() 不能
// 在这个头里内联（unique_ptr 的析构要看到完整类型），所以下面显式声明了
// ~Pipeline()，定义在 pipeline.cpp。
namespace hls { class HlsSession; }

struct StepOutcome {
    enum class Kind { DemuxedPacket, DecodedFrame, Blocked, Eof, Error };
    Kind       kind        = Kind::Blocked;
    int32_t    track_index = -1;   // DemuxedPacket / DecodedFrame 时有效
    syp_status status      = SYP_OK;   // Error 时有效
};

// buffered_until_us 的哨兵：没有任何在播轨（全部解到底/失效/不参与）时取它，
// 调用方据此把"已缓冲时长"视为无上限（不再卡顿）。
inline constexpr int64_t kBufferedUntilUnbounded = std::numeric_limits<int64_t>::max();

// 缓冲水位观测。
// "在播轨"= 受管、非封面图、TrackInfo::discard 为假、未失效、未解到底。任一在播轨
// 还没有包时 buffered_until_us 为 AV_NOPTS_VALUE（它就是 INT64_MIN，取 min 时天然
// 胜出）。TrackPlayer 未绑定的第二条音轨同样参与——它跟主轨一样在交织流里收包，
// 与 Pipeline 的联合背压口径一致。
struct BufferStats {
    int64_t buffered_until_us = AV_NOPTS_VALUE;
    bool    demux_eof         = false;   // 已读到文件尾（线程模式：终止标记已到，含错误标记）
    bool    loading           = false;   // 加载线程正在读 / 正阻塞在读里；同步模式恒 false
    int64_t queued_bytes      = 0;       // 全部受管轨包队列字节之和
    // 读不进更多数据了：同步模式 = 有在播轨包队列满（demux 分支停摆）；线程模式 =
    // 已达停读水位。TrackPlayer 把它当作离开缓冲的条件之一，防止字节上限先到而
    // 已缓冲时长低于恢复水位时永久缓冲。
    bool    full              = false;
};

struct PipelineConfig {
    std::size_t max_packets_per_track = 128;
    int64_t     max_bytes_per_track   = 8 << 20;
    std::size_t max_frames_per_track  = 8;
    // 视频解码方式由调用方显式选择，不自动兜底。
    // Hardware 时 hw_backend 为空或不支持 → create_*() 返回 SYP_ERR_NOT_IMPLEMENTED
    // （不是 SYP_ERR_INVALID_ARG：那是"平台不支持"，不是"配置写错"）；硬解模式下
    // 视频轨 open 的其它失败（avcodec_open2 → SYP_ERR_IO 等）同样让 create 整体失败、
    // 原码上报。播放中硬解被拒（hwaccel 初始化失败等）→ 该视频轨 track_failed()。
    // 封面图（attached_pic）恒软解——它不是视频流。hw_backend 非拥有，须活过 Pipeline。
    VideoDecodeMode       video_decode = VideoDecodeMode::Software;
    hw::IHwDecodeBackend* hw_backend   = nullptr;
    // 线程模式：demux 移到内部加载线程，step() 只解码。默认关，
    // 同步模式行为与此前完全一致。线程模式下 max_packets_per_track/max_bytes_per_track
    // 不再限制包队列；停读由下面两个水位决定：各在播轨包队列时长的最小值
    // ≥ max_buffer_ms，或全部包队列字节 ≥ max_buffer_bytes。线程模式下两者 < 1 时
    // create_*() 返回 SYP_ERR_INVALID_ARG。
    bool    demux_thread     = false;
    int64_t max_buffer_ms    = 30000;
    int64_t max_buffer_bytes = int64_t{64} << 20;
};

class Pipeline {
public:
    // 显式声明、在 .cpp 里定义：hls_ 是一个只前置声明过的类型的
    // unique_ptr，隐式内联析构会在每个 include 本头的 TU 里要求完整类型。
    // 线程模式下析构先打断并 join 加载线程，再析构成员。
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // 打开本地文件。失败返回 nullptr 并写 *err（整体终止：demux/IO 错误）。
    // 单条轨的解码器初始化失败不会让这里返回 nullptr——那是「该轨终止」，
    // 查 track_failed() 得知，其它轨继续。
    static std::unique_ptr<Pipeline> create_file(const std::string& path,
                                                  const PipelineConfig& cfg,
                                                  syp_status* err);

    // 打开调用方提供的 AVIOContext。不接管 ctx 的所有权——谁创建谁负责
    // 关闭；Pipeline（以及它内部的 Demuxer）析构都不会关闭它。
    //
    // io_abort（可空）：request_abort() 会调它一次，用来打断阻塞在 ctx
    // 读回调里的 IO——Pipeline 不知道 ctx 底下是什么，够不着那个阻塞点，
    // 只有调用方能打断它（典型：[b] { b->request_abort(); }，b 是
    // AvioBridge）。约束：不得抛异常；可能在任意线程、与 step() 并发调用；
    // 它引用的对象必须活到 ~Pipeline() 之后（与 ctx 本身同一条生命周期）。
    //
    // 【线程模式】上面那条"活到 ~Pipeline() 之后"从"建议"变成硬约束：析构函数
    // 自己会调一次 request_abort() → io_abort（打断加载线程可能正阻塞着的读，然后
    // join），所以钩子引用的对象、ctx 以及 ctx 底下的读回调状态都必须在 Pipeline 析构
    // **返回之后**才能销毁——先毁 AvioBridge 再 reset Pipeline 是 UAF。没交钩子时析构
    // 打断不了阻塞读，只能等那次读自己返回才 join 得回来（析构可能长时间阻塞）；
    // 线程模式下对可能无限期阻塞的源务必交钩子。HLS 路径同理：析构会调会话的
    // request_abort()，会话由 Pipeline 自己持有，调用方无需额外处理。
    static std::unique_ptr<Pipeline> create_avio(AVIOContext* ctx,
                                                  const PipelineConfig& cfg,
                                                  syp_status* err,
                                                  std::function<void()> io_abort = {});

    // 打开一个 HLS 播放列表 URL（master 或 media 均可）。所有网络 IO 都经
    // 本项目自己的 dl 层：播放列表走零缓存的 fetch_playlist，分片走
    // syp_source。FFmpeg 一个 socket 都不自己开——见 hls_session.h 顶部
    // 的【结构护栏】。
    //
    // dl_cfg 的内容会被会话拷贝（含 cache_dir 字符串），调用后可释放。
    // 失败返回 nullptr 并写 *err。
    static std::unique_ptr<Pipeline> create_hls(const std::string& url,
                                                 const PipelineConfig& cfg,
                                                 const syp_config& dl_cfg,
                                                 const HlsOptions& hls_opts,
                                                 syp_status* err);

    // 推进一个单位的工作并如实报告结果。见本文件顶部的调度顺序说明。
    StepOutcome step();

    // 从某轨的 FrameQueue 取出一帧；该轨没有帧、或 track_index 不是被
    // 管理的音视频轨时返回 std::nullopt。
    std::optional<Frame> pop_frame(int32_t track_index);

    // 同步、原子的 seek：不管第 2 步成功与否，第 3、4 步都会跑完——
    // 调用方拿到的要么是一个完全复位好的 Pipeline，绝不会是「队列已清空
    // 但解码器没 flush、EOF 标志也没复位」这种半吊子状态。顺序固定：
    //   1. 清空所有 PacketQueue 与 FrameQueue
    //   2. avformat_seek_frame(..., AVSEEK_FLAG_BACKWARD)——失败时把
    //      返回值原样传给调用方，但不提前 return，第 3/4 步照跑
    //   3. 各解码器 flush()
    //   4. 复位 EOF 相关标志（demux_eof_/eof_sent/decoder_eof）
    // track_failed 与 skipped_packets 都不在第 4 步复位范围内：前者是跟
    // 编码格式相关的持久状态，后者是自创建以来的累计值，两者都跟「当前
    // 播放位置」无关，seek 不应该让它们凭空恢复/清零。
    // 会话级错误状态分两种：
    //   · aborted_ **不复位**——中止不可逆，见 request_abort()；
    //   · HLS 的分片/播放列表错误账**仅在第 2 步成功时**清零——它回答的是
    //     "从上一次定位起这一轮有没有漏内容"，seek 成功即开启新的一轮
    //     （HlsSession::clear_playback_errors()）。第 2 步失败时保留。
    // AVSEEK_FLAG_BACKWARD 是刻意的：落在最近的前一个关键帧，seek(t) 之后
    // 第一帧的 pts 通常小于 t。是否丢弃到 t 由上层决定，这里不做。
    //
    // 【线程模式】立即返回 SYP_OK：本线程清空两端队列、flush 解码器、复位
    // EOF 标志，并递增 seek 代号唤醒加载线程；实际的 avformat seek 在加载线程上做
    // （同一个 AVSEEK_FLAG_BACKWARD 调用）。加载线程正阻塞在读里时，seek 等那次读
    // 返回才生效（不打断）。seek 失败写错误终止标记，之后的 step()
    // 报 Error(SYP_ERR_IO)，直到下一次 seek。
    syp_status seek(int64_t ts_us);

    // 看门狗：打断当前正阻塞着的网络 IO。可在任意线程调用，与 step() 并发。
    //
    // 【三条打开路径各自怎么打断阻塞 IO】
    //   · create_hls：转发给 HlsSession——interrupt_callback、每条分片的
    //     AvioBridge、播放列表抓取都接在同一个中止标志上；
    //   · create_avio：调用方交进来的 io_abort 钩子（Pipeline 够不着调用方
    //     AVIOContext 底下的阻塞点）。没交钩子时，正卡在读里的那次 step()
    //     打不断，要等读自己返回；
    //   · create_file：本地文件读不会长时间阻塞，没有需要打断的东西。
    // 三条路径共同的一半：置位之后 step() 一律报 Error(SYP_ERR_CANCELED)。
    //
    // 【不可逆】置位之后不提供复位口（seek() 也不复位）：中止的语义是
    // "这条管线不再往下走"，调用方接下来该做的是销毁它，而不是接着 step()。
    //
    // 【中止之后 step() 报 Error(SYP_ERR_CANCELED)，不是 Eof——这是刻意改写】
    // FFmpeg 那边天然给不出这个区分：AvioBridge 的读回调返回 AVERROR_EXIT，
    // 嵌套的 mov 解封装器把它当成这一片读完了，hls 随后判定播放列表放完、
    // 在顶层返回 AVERROR_EOF，于是 Demuxer::read() 走的是 Eof 分支
    // （demuxer.cpp 里 `rc == AVERROR_EOF ? Eof : Error`）。照抄这个结果的话
    // 调用方拿到的是"正常播完了"——demo 壳的 bridge.mm 正是把 Kind::Eof 转成
    // onEof（「播放完成」），用户导航离开会显示成播完了。
    // 所以 Pipeline 自己记住 aborted_，并在 step() 的 Eof 出口把它改写成
    // Error/SYP_ERR_CANCELED。一个调用方分辨不出"我叫停的"和"播完了"的中止
    // 接口等于半个接口，这一层不该推给每个调用方各自重造。
    //
    // 【线程模式】另外唤醒加载线程；加载线程读返回后看见中止标志即退出。
    void request_abort() noexcept;

    const std::vector<TrackInfo>& tracks() const noexcept { return demuxer_->tracks(); }

    // 自创建以来，所有被管理轨累计的「单包可跳过」计数之和。
    int64_t skipped_packets() const noexcept;

    // 该轨是否已终止（解码器初始化失败，或解码过程中遇到不可恢复的内部
    // 错误）。track_index 越界或指向未被管理的轨（字幕/数据等）返回 false。
    bool track_failed(int32_t track_index) const noexcept;

    // 是否有被管理的非封面视频轨工作在硬解模式。
    bool video_hardware_decoding() const noexcept;

    // 该轨已解码到底（decoder_eof）且 PacketQueue/FrameQueue 都空。越界/非托管轨返回 false。
    bool track_drained(int32_t track_index) const noexcept;
    // 对所有被管理的非封面视频轨调用 set_skip_nonref(on)（追帧）。seek() 不复位，由调用方管。
    void set_video_catchup(bool on) noexcept;
    bool video_catchup() const noexcept { return video_catchup_; }
    // 该轨当前是否处于跳非参考帧模式（追帧的可观测状态；覆盖 set_video_catchup 对
    // 封面图轨的排除逻辑）。越界/非托管/非视频轨返回 false。
    bool track_skip_nonref(int32_t track_index) const noexcept;

    // 缓冲水位观测，见 BufferStats。与 step() 同线程调用（读的是泵线程独占
    // 的解码状态）；包队列本身线程安全。
    BufferStats buffer_stats() const;
    const PipelineConfig& config() const noexcept { return cfg_; }
    // 仅测试：之后 buffer_stats() 一律返回 fn() 的结果。TrackPlayer 缓冲状态机的
    // 单测用它逐步拨动水位，隔离加载线程的时序。生产代码不调用。
    void debug_override_buffer_stats(std::function<BufferStats()> fn) {
        buffer_stats_override_ = std::move(fn);
    }

    // 一条被管理轨（视频或音频）的运行状态。字幕/数据/附件轨不在此列——
    // 它们的 packet 在 demux 阶段直接丢弃，见 pipeline.cpp。
    //
    // 公开是为了让 pipeline.cpp 匿名 namespace 里的 drive_decoder<>
    // 模板自由函数能拿它当参数类型（模板要对 FFmpegVideoDecoder /
    // FFmpegAudioDecoder 两种鸭子类型复用同一份逻辑，写成自由函数模板比
    // 写成 Pipeline 的模板成员函数更简单）。不是给外部调用方用的公开
    // 接口——Pipeline 的构造函数是私有的，外部代码拿不到能填这个结构体
    // 的 Pipeline 实例，也没有任何公开方法把它交出去。
    struct TrackState {
        bool managed     = false;
        bool is_video    = false;
        bool failed      = false;   // 该轨终止
        bool eof_sent    = false;   // 已经把 send(nullptr) 送进解码器
                                     // 注意：置位只表示「没有更多包可送
                                     // 了」，不表示解码器已经吐空——它
                                     // 内部可能还压着 B 帧重排序延迟的
                                     // 缓冲帧，要看 decoder_eof 才知道
                                     // 排空与否（见 pipeline.cpp 里
                                     // draining 那段注释；根因就是
                                     // 把这两者当成了同一件事）。
        bool decoder_eof = false;   // receive() 已经到 Eof
        bool cover_art   = false;   // 视频轨且为封面图（AV_DISPOSITION_ATTACHED_PIC）

        std::unique_ptr<PacketQueue> packets;
        std::unique_ptr<FrameQueue>  frames;
        std::unique_ptr<IVideoDecoder>      video_dec;   // managed && is_video
        std::unique_ptr<FFmpegAudioDecoder> audio_dec;   // managed && !is_video
    };

private:
    Pipeline() = default;

    static std::unique_ptr<Pipeline> create_common(std::unique_ptr<Demuxer> demuxer,
                                                     const PipelineConfig& cfg,
                                                     syp_status* err);

    // 成员声明顺序不是随意的：C++ 保证析构按声明的逆序执行，demuxer_
    // 声明在 tracks_ 前面意味着 tracks_（连同它拥有的 PacketQueue/
    // FrameQueue/解码器）先被销毁，demuxer_（连同它拥有的
    // AVFormatContext，析构时会 avformat_close_input）最后才销毁。
    // 当前没有任何 TrackState 成员持有指向 demuxer_ 内部结构（AVStream/
    // AVFormatContext）的裸指针，所以现在调换顺序不会立刻出问题——但这
    // 个顺序是「以后如果有人往 TrackState 里加这样一个裸指针」也天然
    // 安全的唯一保证，别在不确认这一点的前提下调换。
    // 【顺序重要，比上面那条更硬】声明在 demuxer_ **之前** ⇒ 析构逆序
    // 意味着 demuxer_ 先毁、hls_ 后毁。demuxer_ 的析构是
    // avformat_close_input()，它会经 hls_close() + ff_format_io_close()
    // 回调到 HlsSession::on_io_close 去关每一条还开着的分片/播放列表；
    // io_open 回调在播放期间也一直会被调用（下一个分片、直播刷新）。
    // 反过来的顺序是确定的 UAF，不是"以后可能出问题"。
    // 非 HLS 路径（create_file/create_avio）这个指针恒为空，无成本。
    std::unique_ptr<hls::HlsSession> hls_;

    // request_abort() 置位、step() 读。必须是原子的：request_abort() 的契约
    // 是"可在任意线程调用、与 step() 并发"。不随 seek() 复位——见
    // request_abort() 上方的【不可逆】。
    std::atomic<bool> aborted_{false};

    // create_avio() 的 io_abort 钩子；其它路径为空。构造后只读。
    std::function<void()> io_abort_;

    std::unique_ptr<Demuxer> demuxer_;
    std::vector<TrackState>  tracks_;
    PipelineConfig            cfg_;
    // 泵线程独占（两种模式都是）。线程模式下加载线程只写 load_term_（持 load_mu_），
    // 由泵线程在 step() 入口 absorb_load_terminal() 折进来；buffer_stats() 另持
    // load_mu_ 读 load_term_，所以任何跨线程可见的"读到尾"信息都在锁下。
    bool                       demux_eof_ = false;
    bool                       video_catchup_ = false;

    // buffer_track_[i]：该轨是否参与水位（受管 && 非封面 && 非 discard），
    // create_common() 填、此后只读。
    std::vector<uint8_t>         buffer_track_;
    std::function<BufferStats()> buffer_stats_override_;   // 仅测试缝

    // ---- 加载线程（仅 cfg_.demux_thread）。线程设计见 pipeline.cpp loader_main() ----
    enum class LoadTerminal { None, Eof, Error };

    void start_loader();                               // create_* 全部字段就位之后调用
    void loader_main();
    bool loader_has_work_locked() const;               // 持 load_mu_
    void push_loaded_packet_locked(AVPacket* pkt, int32_t track_index);   // 持 load_mu_
    void absorb_load_terminal();                       // 泵线程：终止标记 → demux_eof_/demux_term_status_
    void mark_track_failed(std::size_t i);             // 泵线程：该轨终止（两种模式共用）
    syp_status seek_async(int64_t ts_us);
    bool water_full_locked() const;   // 持 load_mu_：已达停读水位
    void wake_loader();               // 泵线程：可能消费了包，唤醒停在水位上的加载线程

    mutable std::mutex      load_mu_;
    std::condition_variable load_cv_;
    // 以下由 load_mu_ 保护
    bool                    stop_loader_       = false;
    uint64_t                seek_gen_          = 0;   // 泵线程 seek 时递增
    uint64_t                served_gen_        = 0;   // 加载线程已执行到的代号
    int64_t                 seek_target_us_    = 0;
    LoadTerminal            load_term_         = LoadTerminal::None;
    syp_status              load_term_status_  = SYP_OK;
    std::vector<uint8_t>    live_;                    // live_[i]：受管且未失效（failed 的跨线程镜像）
    // 以下原子
    std::atomic<bool>       loading_{false};
    // 以下泵线程独占（demux_eof_ 同属此列：加载线程从不写它，只写 load_term_）
    syp_status              demux_term_status_ = SYP_OK;
    // 最后声明：析构体里先 join；声明在最后也保证它最先析构（此时已 join，不会 terminate）。
    std::thread             loader_;
};

}  // namespace syp::media
