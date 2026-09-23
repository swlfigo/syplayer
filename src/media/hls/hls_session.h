// hls_session.h —— 唯一知道"HLS"这个概念的对象。
//
// 它装配一个 AVFormatContext：接管 io_open/io_close2，把子资源打开分成
// 两条通道（播放列表 → PlaylistFetcher 零缓存；分片 → syp_source 带缓存），
// 并在把 AVFormatContext 交出去之前完成选轨。
//
// 【为什么必须在 avformat_open_input 之前装好】hls 解封装器在
// read_header 里就要打开子播放列表（hls.c:2165 → parse_playlist →
// hls.c:834 `c->ctx->io_open(...)`，其中 `c->ctx == s`，见 hls.c:2150），
// 而顶层播放列表本身也是经 init_input → `s->io_open` 打开的（hls 解封装
// 器没有 AVFMT_NOFILE）。也就是说 io_open 必须在 open 之前就位，不能
// "先 open 再补"——这正是 Demuxer::open_prepared() 存在的理由。
//
// 【两条通道的缓存语义是相反的】播放列表在直播下必须永不缓存（同一 URL
// 内容每几秒变一次，dl 层在区间已缓存时一个请求都不发，
// source_bridge.cpp:911），分片是不可变内容、URL 唯一、缓存它永远正确。
// 所以播放列表走 fetch_playlist（零缓存），分片走 syp_source + AvioBridge。
//
// 【结构护栏】protocol_whitelist 只留 "file"。背景：本项目为了过
// hls.c:688 那道"协议名必须以 http 开头"的检查而编进了 http 协议，这意味着
// FFmpeg **有能力**自己开 socket；某条路径一旦没走 io_open，它会悄悄绕过
// dl 层——缓存、预加载、统计全部失效，而且没有任何现象。
//
// 【它在正常路径上不生效，也不该生效】io_open 一装上，io_open_default
// （options.c:140-155，s->protocol_whitelist 的唯一消费者）就再也到不了；
// 子上下文那边 nested_io_open（hls.c:1793）无条件 EPERM。把这一行注掉，
// ctest 26/26 照样全绿（实测）。
//
// 【它守的是哪一种失效——只有一种，已实测划清】"io_open 根本没装上"。
// 把 io_open（装上但返回错误 / 根本不装）× whitelist（保留 "file" /
// 放开成 "file,http,tcp"）这 2×2 四格都跑了一遍（本机，FFmpeg 8.1.2，日志
// 开到 AV_LOG_DEBUG，盯 io_open_default 的 `Opening '…' for reading`
// ——options.c:153）：
//
//   ┌ io_open 装上但返回错误 ─────────────────────────────────────────┐
//   · + 保留 whitelist（自检 A）⇒ 打不开，报错收敛在 hls 自己那层。
//   · + 放开 whitelist（自检 B）⇒ hls **不会**回落到 io_open_default：
//     日志里一条 `Opening 'http://fake.invalid/…'` 都没有，只有
//     `Failed to open an initialization section` /
//     `Error when loading first segment`。对照组证明日志级别没问题——
//     同一份日志里 create_file 走 io_open_default 打开本地 source.mp4 的
//     `Opening '…/source.mp4' for reading` 照常打出来。
//   └───────────────────────────────────────────────────────────────┘
//   ┌ io_open 根本不装 ──────────────────────────────────────────────┐
//   · + 保留 whitelist（自检 D）⇒ 当场失败，打印
//     `Protocol 'http' not on whitelist 'file'!`
//   · + 放开 whitelist（自检 C）⇒ **FFmpeg 自己
//     开 socket**：`Opening 'http://fake.invalid/hls/master.m3u8' for
//     reading` + `[tcp] Failed to resolve hostname fake.invalid`。
//     这一格的实测清单（两次独立运行一致）：test_hls_e2e
//     **6 红 8 绿**。红的 6 条是 vod_single_variant_plays_to_eof /
//     https_master_is_rewritten… / multi_variant_selects_by_bandwidth… /
//     encrypted_playlist_is_rejected… / segment_404_is_reported… /
//     every_byte_goes_through_our_dl_layer；绿的 8 条包括直播刷新、两条
//     中止、两条选轨绑定、vod_demuxed 双轨那几条。
//   └───────────────────────────────────────────────────────────────┘
//
// 所以这一行的作用要说得比以前更窄、也更确定：它是把"io_open 没装上"这
// **一种**失效从静默绕过换成可见失败的唯一装置；对"我们的通道装上了但
// 坏掉了"它是冗余的（那条路径上 FFmpeg 压根不退回）。"将来某条路径回落到
// io_open_default" 这句此前是未经验证的推测，本版本下不成立，已删。
//
// 【顺带把"哪些断言有牙"说准，这里以前写得太一刀切】
//   · srv.requests_for(...) 的**正向**断言（`>= 1`，"这个 URL 被请求过"）
//     确实对"谁开的 socket"零分辨率——FFmpeg 自己开的 socket 打的也是同一个
//     127.0.0.1。
//   · 但**负向**断言（`== 0`，"这个 URL 绝不该被请求"）在自检 C 下**有**辨别
//     力：FFmpeg 自己开 socket 时会去要我们的 dl 层永远不会要的 URL。上面
//     那 6 条红里就有 4 条是这种负向断言（明文 server 上的 master、被
//     discard 档的 seg_high1/seg_low1、加密流的 key.bin）。
//   · 落的那两条缓存断言里，自检 C 下**只有一条**真的红：
//     `cached_a_segment`（分片确实进了缓存）。另一条
//     `!ends_with(u, ".m3u8")` 在这一格里缓存目录是空的，**空转通过**——
//     这正是 test_hls_e2e.cpp 那两条断言上方注释自己写明的理由（"只有第一
//     条的话，'一个字节都没缓存'也会通过"）。
//   · 真正把"谁开的 socket"变成一条直接判据的是
//     every_byte_goes_through_our_dl_layer：它把全局
//     syp_http_backend 换成内存桩、主机名用 DNS 上永不可解析的
//     fake.invalid，于是唯一的字节来源就是我们自己。
//
// 留 "file" 而不是空串——空串在某些 FFmpeg 路径上被当成"没设置"，留一个
// 不会被用到的真实协议名更保险。
//
// 【生命周期】release_fmt() 把 AVFormatContext 交给 Demuxer，但
// HlsSession 必须活得比它久：io_open/io_close2 回调在播放期间还会被调用
// （下一个分片、直播刷新），回调里用的就是本对象；而
// avformat_close_input() 自己也会经 ff_format_io_close() 回调一次
// io_close2 来关顶层播放列表（libavformat/demux.c，FFmpeg 8.1.2）。
// 见 pipeline.h 里 hls_ 成员声明顺序上方的注释。
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <syplayer/syp_config.h>
#include <syplayer/syp_source.h>
#include <syplayer/syp_types.h>

#include "media/avio_bridge.h"
#include "media/hls/hls_options.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

namespace syp::media::hls {

// 播放列表那条内存 AVIOContext 的缓冲区大小。播放列表整份已经在内存里，
// 这个缓冲只影响 FFmpeg 每次从我们这儿搬多少字节，32 KiB 足够。
inline constexpr int kAvioBufSize = 32 * 1024;

class HlsSession {
public:
    // 装配（**不** open）。失败返回 nullptr 并写 *err。
    //
    // 真正的 avformat_open_input 由 Demuxer::open_prepared() 来做——
    // 全工程只能有一处调，否则"谁拥有 AVFormatContext、失败时谁负责释放"
    // 会分裂成两套规则。
    static std::unique_ptr<HlsSession> create(const std::string& url,
                                              const syp_config&  dl_cfg,
                                              const HlsOptions&  opts,
                                              syp_status*        err);
    ~HlsSession();
    HlsSession(const HlsSession&)            = delete;
    HlsSession& operator=(const HlsSession&) = delete;

    // 装配好、尚未 open 的 AVFormatContext。所有权**转移给调用方**
    // （Pipeline 的 Demuxer 会接管），但 HlsSession 必须活得比它久，
    // 见本文件顶部【生命周期】。第二次调用返回 nullptr。
    AVFormatContext* release_fmt() noexcept;

    // ---- 读回缝：容量/TTL 三件套 ----
    //
    // 本会话**真正会交给每一条媒体分片源**的那份 syp_config（open_segment()
    // 里 syp_source_open 的第四个实参就是它）。存在的理由与
    // MediaInfoProvider::config_for_test() / Preloader::dl_config_for_test()
    // 完全一样，而且这一处是三处里**最大的那个**：
    //
    // 播放侧的读回缝拍照点在 SYPBridge.mm 的 -openURLString:（`_lastOpenCache
    // Config`），发生在 create() **之前**；create() 这里把 dl_cfg 整份拷进
    // dl_cfg_ 之后还能再改一手，而那之后就再没有任何断言看得见它了。实测
    // （在 cache_dir 改指之后把三个字段清零）：
    // **ctest 33/33 + xcodebuild 82/0 双绿存活**。后果不是预加载而是 **HLS
    // 播放**——每一条分片源都不看上限、不过期、不看可用空间，分片永久累积。
    //
    // 断的是"对象自己手里那份"，不是中间变量：任何在 create() 里、或将来
    // 有人在别处对 dl_cfg_ 的后置修改，都会被 hls_carries_capacity_and_ttl_
    // into_its_segment_config 当场抓住。
    const syp_config& dl_config_for_test() const noexcept { return dl_cfg_; }

    // 递给 avformat_open_input 的 options 字典（http_persistent=0 /
    // allowed_extensions=ALL）。所有权仍归本对象，析构时释放；
    // avformat_open_input 会把未被消费的条目写回同一个指针，这也是为什么
    // 返回的是 AVDictionary** 而不是值。
    //
    // 【http_persistent=0 是承重件，实测过】不关掉的话 hls.c:708/815 的
    // keepalive 分支会复用已有的 AVIOContext 而**不重新走 io_open**，
    // 而且后果不是"静默绕过"这么温和：open_url_keepalive() 第一件事就是
    // ffio_geturlcontext(*pb) + av_assert0(uc)（hls.c:644-646），我们给的
    // 是内存 AVIOContext，没有 URLContext ⇒ 进程直接 abort。反向自检实测：
    // 把 options 传成 nullptr，用例以
    // "Assertion uc failed at src/libavformat/hls.c:646" 崩掉。
    //
    // 【allowed_extensions=ALL 目前是纯防御，不是承重件——别按"它在挡住
    // 什么"去理解】hls.c:681 那道扩展名检查整段在
    // `if (av_strstart(proto_name, "file", NULL))` 分支里（hls.c:680-687），
    // 也就是说它**只对 file: 协议生效**；本项目把所有 URL 改写成 http:，
    // 永远走不到那个分支。反向自检实测：把这一行拿掉，用例照样绿。留着
    // 的理由是它零成本，且一旦以后出现 file:/data: 这类子资源（或 FFmpeg
    // 把这道检查挪出 file 分支），少了它就是一条难查的打不开。
    //
    // 两者都是 hls 解封装器的 AVOption，**必须**经 avformat_open_input 的
    // options 参数传进去，否则静默不生效。
    AVDictionary** open_options() noexcept { return &open_opts_; }

    // 递给 FFmpeg 的 URL（https 已改写成 http，见 url_rewrite.h）。
    const std::string& ffmpeg_url() const noexcept { return ffmpeg_url_; }

    // open_playlist() 主动拒绝一次 open（比如加密判定）时留下的精确原因；
    // 没拒绝过则是 SYP_OK。
    //
    // 【它补的是什么洞】avformat_open_input 失败时，
    // Demuxer::open_prepared() 把**任何**负返回值都归一成 SYP_ERR_IO
    // （demuxer.cpp——它的错误粒度只到"打不开"，不区分"为什么打不开"）。
    // io_open 返回的 AVERROR 会先被 FFmpeg 的 hls 解封装器包一层
    // （parse_playlist 报错、hls_read_header 收敛），原始 AVERROR 值
    // 在这条路径上到不了 open_prepared() 的判断——它只看 rc<0。
    // 我们自己在 open_playlist() 里主动拒绝时，原因是精确已知的
    // （比如"加密，不支持"），这个字段把它带出这层归一化，供
    // Pipeline::create_hls() 在 open_prepared() 失败之后换回去。
    //
    // 【不是"只在 open 阶段被写"——但今天确实不需要原子/加锁，理由换一条】
    // 写它的是 open_playlist()（hls_session.cpp:357），而 open_playlist()
    // 挂在 io_open 上，io_open 在**两个**时机被 FFmpeg 同步调用：
    //   · avformat_open_input 期间 —— create_hls() 的线程；
    //   · **直播播放列表重拉**期间 —— av_read_frame() 里面。同步模式
    //     （PipelineConfig::demux_thread=false）下是 Pipeline::step() 的
    //     线程；线程模式下 av_read_frame()（以及 seek 的
    //     av_seek_frame）挪到了 Pipeline 内部的加载线程，io_open 跟着在
    //     加载线程上跑，不再是 step() 的线程。
    // 所以"只在单线程的 open 阶段被写"是错的。
    //
    // 【那为什么还是裸成员】两件事各自成立：
    //   1. 两个写点不可能并发。create_hls() 返回之前 Pipeline 还不存在，
    //      没人能 step()，加载线程也还没启动（start_loader() 在 create
    //      成功的最后一步）；而 step() 自己不是可并发调用的（pipeline.h：
    //      只有 request_abort() 承诺"可在任意线程、与 step() 并发"，
    //      而它只碰原子）。调用方若把管线交到另一条线程上 step()，那次
    //      交接本身必须同步（否则整个 Pipeline 都在竞争），happens-before
    //      由它提供；线程模式下 std::thread 的创建本身提供 happens-before。
    //   2. 唯一的读点（pipeline.cpp:271）在 create_hls() 的失败分支里，
    //      跟那次写在同一线程、同一调用栈上，纯粹是顺序执行。
    //
    // 【播放期间不再写这个字段】上面说的"直播重拉时
    // 也会写"是修复前的形状。现在 open_playlist() 在 mark_playback_started()
    // 之后改记 playlist_errors_（受 mu_ 保护，step() 经
    // pending_playlist_error() 读），本字段的写点只剩 create_hls() 线程上
    // 的 open 阶段，第 1、2 两条都照旧成立。别把播放期的判定挪回这里。
    syp_status precise_open_error() const noexcept { return precise_open_error_; }

    // Pipeline::create_hls() 在 open 成功之后调一次。此后 open_playlist()
    // 的失败（拉取失败/变加密）才记进 pending_playlist_error()——open 阶段
    // 某一档失败会被 hls.c 跳过、其余档照常播，不能算账。见
    // open_playlist() 里的长注释。
    void mark_playback_started() noexcept;

    // 播放阶段取回过的播放列表里，是否有一路的**最后一次**取回不是"正常
    // 结束"的样子（失败 / 变加密 / 成功但没有 EXT-X-ENDLIST）——有就返回
    // 原因（SYP_ERR_HTTP_STATUS / SYP_ERR_TIMEOUT / SYP_ERR_OOM /
    // SYP_ERR_NOT_IMPLEMENTED，具体原因优先；只有"仍是直播"时为
    // SYP_ERR_IO），否则 SYP_OK。中止不算。只在 Eof 出口读才有意义：直播
    // 播放中途它恒非 OK。
    // hls.c 对播放期重拉失败不重试、直接结束那一路，最终报的是干净的
    // AVERROR_EOF；Pipeline::step() 的 Eof 出口用这个信号把它改写成 Error。
    syp_status pending_playlist_error() const noexcept;

    // 清掉"本轮播放"的错误账：分片错误（粗粒度 + 精确）与播放列表错误。
    // Pipeline::seek() 成功之后调，见 pipeline.h seek() 上方契约。
    // 必须在 Demuxer::seek() **返回之后**调：hls.c 的 seek 会关掉旧位置上
    // 还开着的分片，on_io_close 可能在那时补记旧位置的失败。
    void clear_playback_errors() noexcept;

    // 选轨：把**未选中** variant 的流设成 AVDISCARD_ALL。
    //
    // 【调用时机是契约，不是实现细节】必须在 avformat_open_input +
    // avformat_find_stream_info 之后（AVProgram 是 hls_read_header
    // 在 hls.c:2221 `av_new_program` 建的，open 之前根本不存在）、
    // Demuxer::build_tracks() 之前（TrackInfo::discard 读的就是这里写的
    // st->discard）。落地形状是 Demuxer::open_prepared() 的 after_open
    // 回调，接线在 pipeline.cpp::create_hls。
    //
    // 【它省的是真流量，不只是"解码时忽略"】playlist_needed()
    // （hls.c:1528-1572）+ recheck_discard_flags()（hls.c:2445）：一条
    // 播放列表的 main_streams 若全被 AVDISCARD_ALL，pls->needed 变 0，
    // read_data 那条路整个不再为它取分片。**但 read_header 已经发生过的
    // 请求收不回来**——hls.c:2178 会逐个 parse 每一档的播放列表，
    // hls.c:2240 起还会为每一档探测首个分片。所以"未选中档零请求"是
    // 错的；正确的断言是"未选中档在 open 之后不再产生新的分片请求"，
    // 见 tests/test_hls_e2e.cpp 里那条用例的注释。
    void select_variant(AVFormatContext* fmt) noexcept;

    // 看门狗：打断所有阻塞中的分片读取。可在任意线程调用。
    void request_abort() noexcept;

    // 会话生命周期内，是否发生过一次真实的分片失败（非中止）——发生过
    // 就返回它的 syp_status，从未发生过则是 SYP_OK。
    //
    // 【它守的是什么】见 open_segment()/on_io_close() 里的长注释：hls
    // 解封装器对分片失败（无论是 open 时还是 read 时）的默认策略都是
    // 跳过、继续下一片，playlist 耗尽时照样返回干净的 AVERROR_EOF——
    // 那次失败从 av_read_frame() 的返回值上完全看不出来。
    // Pipeline::step() 在真正报 Kind::Eof 之前用这个信号分辨"播完了"和
    // "漏了一片、凑巧也到底了"，见 pipeline.cpp。
    //
    // 【粘到下一次成功的 seek 为止】它回答的是"从上一次定位起，这一轮
    // 播放有没有漏内容"：之后哪怕后续分片全部正常，这一轮也不该让调用方
    // 以为"完整播完了"；但 seek 之后是新的一轮，旧位置上漏过的那一片
    // 与这一轮无关（见 clear_playback_errors()）。可在任意
    // 线程调用（Pipeline::step() 在调用方线程上读）。
    //
    // 【返回值的精度分两层】"该不该报 Error"这个判定
    // （即"是不是 SYP_OK"）由粗粒度检测（open_segment()/on_io_close() 里
    // 记的 pending_segment_error_，兜底 gate，下面还会看到）保证；但
    // 具体返回**哪个** syp_status 优先用 precise_segment_error()——
    // syp_source_open() 的 on_error 回调能给到精确得多的原因（真实
    // syp_status + HTTP 状态码），比粗粒度那个恒定的 SYP_ERR_IO 有意义
    // 得多（一个叫"错误分级"的任务，总不能把 HTTP 404 分级成"本地读写
    // 失败"）。on_error 没触发过时才退回粗粒度的值。
    syp_status pending_segment_error() const noexcept {
        const syp_status precise = precise_segment_error();
        if (precise != SYP_OK) return precise;
        return static_cast<syp_status>(pending_segment_error_.load(std::memory_order_acquire));
    }

    // on_error 回调记的精确原因；没触发过是 SYP_OK。
    //
    // 【为什么不能取代 pending_segment_error_、只留这一条】on_error 只在
    // SourceBridge 自己判定"致命"（内部重试耗尽，见
    // src/dl/scheduler.cpp 的 max_consecutive_errors 逻辑）时才触发，不是
    // 每一种分片失败都会走到这条路（比如 AvioBridge::create() 的 OOM、
    // seek 失败等）。"该不该把 Eof 改写成 Error"这个判定必须不依赖
    // on_error 有没有触发，所以粗粒度检测（open_segment/on_io_close）
    // 留着当兜底的 gate，这里只负责让报出去的值更精确。
    //
    // 中止（SYP_ERR_CANCELED）不算，同 pending_segment_error_ 那条纪律，
    // 在 on_source_error() 里过滤掉，不会走到这里。
    syp_status precise_segment_error() const noexcept {
        const int64_t packed = precise_segment_error_.load(std::memory_order_acquire);
        return static_cast<syp_status>(packed >> 32);
    }

    // on_error 回调记的 HTTP 状态码；配 precise_segment_error() 一起看，
    // 没触发过或该次失败不是 HTTP 状态错误时是 0。目前只用于
    // 诊断/未来扩展——StepOutcome 不带这个字段（不新增错误码/不动
    // include/syplayer/ 的边界内，暂不往上层传），先记在这里。
    int32_t precise_segment_http_status() const noexcept {
        const int64_t packed = precise_segment_error_.load(std::memory_order_acquire);
        return static_cast<int32_t>(packed & 0xFFFFFFFFLL);
    }

private:
    HlsSession() = default;

    static int on_io_open(AVFormatContext* s, AVIOContext** pb, const char* url,
                          int flags, AVDictionary** options);
    static int on_io_close(AVFormatContext* s, AVIOContext* pb);
    static int on_interrupt(void* opaque);
    // syp_source_open() 的 on_error 回调：syp_source 内部重试耗尽、判定
    // "致命"时触发，可能在任意内部线程调用。见 precise_segment_error()
    // 上方注释。
    static void on_source_error(void* ctx, syp_status status, int32_t http_status);

    int open_playlist(AVIOContext** pb, const std::string& real_url);
    int open_segment (AVIOContext** pb, const std::string& real_url);

    // 播放列表那条内存 AVIOContext 的读/定位回调。opaque 是 OpenPlaylist*。
    static int     playlist_read(void* opaque, uint8_t* buf, int buf_size);
    static int64_t playlist_seek(void* opaque, int64_t offset, int whence);

    struct OpenSegment {
        syp_source*                 src = nullptr;
        std::unique_ptr<AvioBridge> bridge;
    };
    // 【必须堆分配、地址稳定】AVIOContext 的 opaque 指着它，而
    // avio_alloc_context() 要在它进 map 之前就拿到这个指针。map 的
    // mapped_type 换成值类型的话，"先建条目再取地址"会把构造顺序拧成
    // 一个不必要的死结（还得先知道 key，而 key 恰恰是 alloc 的结果）。
    struct OpenPlaylist {
        std::vector<uint8_t> body;      // AVIOContext 直接从它取字节
        int64_t              pos = 0;
    };

    syp_config       dl_cfg_{};
    // dl_cfg_.cache_dir 是裸指针，调用方的字符串不保证活到会话结束
    // （典型写法是一个栈上的临时 std::string）。整条会话的每一个分片都要
    // 用它开 syp_source，所以这里自己留一份并把指针改指到它。
    std::string      cache_dir_;
    HlsOptions       opts_{};
    std::string      real_scheme_;      // 原始 master URL 的 scheme
    std::string      ffmpeg_url_;       // to_ffmpeg_url(原始 URL)
    AVFormatContext* fmt_       = nullptr;   // release_fmt() 之后为 nullptr
    AVDictionary*    open_opts_ = nullptr;
    // 裸成员（非原子）的理由见 precise_open_error() 上方注释——不是
    // "只有一条线程写"，而是"两个写点不可能并发 + 唯一的读点跟写在同一
    // 线程同一调用栈"。给它加读点之前先回去看那段。
    syp_status       precise_open_error_ = SYP_OK;

    // mutable：pending_playlist_error() 是 const 的读方法，也要拿这把锁。
    mutable std::mutex                                           mu_;
    std::map<AVIOContext*, OpenSegment>                          segments_;
    // 播放阶段失败的播放列表：真实 URL → 原因。mu_ 保护。见
    // pending_playlist_error() 与 open_playlist() 的注释。
    std::map<std::string, syp_status>                            playlist_errors_;
    std::atomic<bool>                                            playback_started_{false};
    std::map<AVIOContext*, std::unique_ptr<OpenPlaylist>>        playlists_;
    std::atomic<bool>                                            abort_{false};
    // pending_segment_error() 的存储；见该方法上方注释。存 int32_t（而不是
    // syp_status 本身）是因为 std::atomic<syp_status> 要求 syp_status 是
    // trivially copyable 的具名枚举类型——它是，但项目里原子类型统一存
    // 底层整数，跟 abort_/Pipeline::aborted_ 的写法保持一致。
    std::atomic<int32_t>                                         pending_segment_error_{SYP_OK};
    // precise_segment_error()/precise_segment_http_status() 的存储；见
    // precise_segment_error() 上方注释。打包进一个 int64_t 里单次原子读写
    // （高 32 位 syp_status，低 32 位 http_status），避免"状态"和"HTTP 码"
    // 分成两个 atomic 时读到一半写的撕裂组合——0（未置位时的初值）刚好
    // 解出 (SYP_OK, 0)，跟"从没触发过 on_error"这句话对得上，不需要另外
    // 的哨兵。
    std::atomic<int64_t>                                         precise_segment_error_{0};
};

}  // namespace syp::media::hls
