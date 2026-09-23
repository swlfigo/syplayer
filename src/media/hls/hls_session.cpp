#include "media/hls/hls_session.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include "dl/source_bridge.h"          // syp::dl::current_http_backend()
#include "media/hls/playlist_fetcher.h"
#include "media/hls/url_rewrite.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

namespace syp::media::hls {

namespace {

// 拆一条分片：先毁 bridge（它持有并释放 AVIOContext），再关 source——
// 反过来的话 bridge 的读回调可能摸到已关闭的 syp_source。
//
// 【必须在 mu_ 之外调】syp_source_close() → SourceBridge::close()
// （source_bridge.cpp:1018-1043）内含 sched_->stop() 与
// cv_.wait(in_public_ == 0)，是**可能阻塞**的。持着 mu_ 调它，
// request_abort() 就会卡在锁上——而看门狗线程的全部意义恰恰是打断阻塞
// IO。
void destroy_segment(std::unique_ptr<AvioBridge> bridge, syp_source* src) noexcept {
    bridge.reset();
    if (src != nullptr) syp_source_close(src);
}

// 拆一条播放列表的内存 AVIOContext。
//
// buffer 要从 ctx 上现取：FFmpeg 在探测/回退路径上会把 AVIOContext 的
// 缓冲区换成另一块（ffio_rewind_with_probe_data / ffio_ensure_seekback），
// 记住 av_malloc 时那个原始指针去 free 会错放一块、漏掉另一块——ASan 下
// 实测会 double-free，先前的释放者正是 ffio_rewind_with_probe_data。
void destroy_playlist_ctx(AVIOContext* ctx) noexcept {
    AVIOContext*   c   = ctx;
    unsigned char* buf = c->buffer;
    avio_context_free(&c);
    av_freep(&buf);
}

// 这个 program（variant）里有没有视频流。
//
// 【选轨必须先过滤掉不含视频的 program】ffmpeg 的 hls muxer 给
// 带 agroup 的音频轨额外写一条**自引用**的 EXT-X-STREAM-INF（只含
// CODECS="mp4a..."，URI 指回音频播放列表本身）——这是标准行为，真实 CDN
// 上同样有。fixture hls/vod_demuxed（正是目标源 Twitter amplify_video
// 的形状）实测：
//     program[0] variant_bitrate=1445528  含 video   ← 真实视频档
//     program[1] variant_bitrate=131578   纯 audio   ← 自引用的音频档
// 若不过滤，"一档都不满足时选 BANDWIDTH 最小的那档"这条兜底必然选中
// program[1]，把视频轨 discard 掉。表现是"有声音、没画面"，而且看起来
// 完全像一次正常的低码率降级，不像 bug。
//
// 纯音频 HLS（播客）没有任何含视频的 program，那时**不能**过滤成空集，
// 见 select_variant() 里的 any_video_program 分支。
bool program_has_video(const AVFormatContext* fmt, const AVProgram* pr) noexcept {
    for (unsigned k = 0; k < pr->nb_stream_indexes; ++k) {
        const unsigned si = pr->stream_index[k];
        if (si >= fmt->nb_streams) continue;   // 越界防御：不该发生
        if (fmt->streams[si]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) return true;
    }
    return false;
}

// 这个 program 的流表里有没有 si。选中档的流一律不能被别的 program 连坐
// discard（一条流可以同时属于多个 program，见 select_variant() 里那段）。
bool program_contains_stream(const AVProgram* pr, unsigned si) noexcept {
    for (unsigned k = 0; k < pr->nb_stream_indexes; ++k) {
        if (pr->stream_index[k] == si) return true;
    }
    return false;
}

// EXT-X-STREAM-INF 的 BANDWIDTH，由 hls.c:2224 写进 program 元数据。
// 取的是 BANDWIDTH（峰值）而不是 AVERAGE-BANDWIDTH——峰值才是 HLS 规范里
// 用于"这条链路撑不撑得住"判断的那个数。缺失按 0 计（当成最省的一档）。
int64_t variant_bitrate_of(const AVProgram* pr) noexcept {
    const AVDictionaryEntry* e = av_dict_get(pr->metadata, "variant_bitrate", nullptr, 0);
    if (e == nullptr || e->value == nullptr) return 0;
    return std::strtoll(e->value, nullptr, 10);
}

}  // namespace

void HlsSession::select_variant(AVFormatContext* fmt) noexcept {
    // 单码率：master 只有一条 EXT-X-STREAM-INF 时 hls 仍会建一个 program，
    // nb_programs == 0 只发生在"直接给了 media playlist"这种形状上。
    // 两种情况选轨都是恒等变换，直接返回。
    if (fmt == nullptr || fmt->nb_programs <= 1) return;

    bool any_video_program = false;
    for (unsigned i = 0; i < fmt->nb_programs; ++i) {
        if (program_has_video(fmt, fmt->programs[i])) {
            any_video_program = true;
            break;
        }
    }

    // 两个候选并行维护，不共用一个 best：
    //   best_fit —— 满足 <= max_bandwidth_bps 的档里 BANDWIDTH 最大的；
    //   smallest —— 全部候选里 BANDWIDTH 最小的，供"一档都不满足"时兜底
    //               （"网太慢"不该等于"播不了"）。
    // 此前用 best_bw<0 兼作"还没有候选"的哨兵，那在 BANDWIDTH 缺失
    // （按 0 计）时会和真实值混淆，这里用显式的 have_* 标志。
    unsigned best_fit    = 0;
    int64_t  best_fit_bw = 0;
    bool     have_fit    = false;
    unsigned smallest    = 0;
    int64_t  smallest_bw = 0;
    bool     have_any    = false;

    for (unsigned i = 0; i < fmt->nb_programs; ++i) {
        const AVProgram* pr = fmt->programs[i];
        // 见 program_has_video 上方的说明。纯音频源（一个含视频的 program
        // 都没有）时这一条不生效，全部 program 都参选。
        if (any_video_program && !program_has_video(fmt, pr)) continue;

        const int64_t bw = variant_bitrate_of(pr);
        if (!have_any || bw < smallest_bw) {
            smallest    = i;
            smallest_bw = bw;
        }
        have_any = true;

        if (opts_.max_bandwidth_bps <= 0 || bw <= opts_.max_bandwidth_bps) {
            if (!have_fit || bw > best_fit_bw) {
                best_fit    = i;
                best_fit_bw = bw;
                have_fit    = true;
            }
        }
    }
    if (!have_any) return;               // 不该发生：至少有一个候选
    const unsigned best = have_fit ? best_fit : smallest;

    // 【选中档的流一律不碰——一条流可以同时属于多个 program】
    // vod_demuxed 里 stream[0]（那条真正被消费的音频）既在 program[0]
    // （视频档，引用了 EXT-X-MEDIA 的音频 rendition）里，也在 program[1]
    // （自引用的音频档）里。照"遍历非选中 program 把它的流全设成
    // AVDISCARD_ALL"，选中档的音频会被另一档连坐丢掉——分轨源（正是目标源
    // 的形状）当场变成没有声音。回归判据是 test_hls_e2e.cpp 里那两条用例的
    // n_audio == 1。
    //
    // 【为什么是线性扫而不是先建一张 keep 表】本函数是 noexcept 的（它挂在
    // Demuxer::open_prepared 的 after_open 回调上，那条路径没有能接住异常
    // 的地方），而任何堆分配都可能抛 bad_alloc ⇒ 直接 terminate。
    // nb_stream_indexes 是一档里的流数（2~3 条），这个 O(n·m) 的扫描规模上
    // 完全无所谓，换来的是"不分配 ⇒ noexcept 名副其实"。
    const AVProgram* sel = fmt->programs[best];
    for (unsigned i = 0; i < fmt->nb_programs; ++i) {
        if (i == best) continue;
        const AVProgram* pr = fmt->programs[i];
        for (unsigned k = 0; k < pr->nb_stream_indexes; ++k) {
            const unsigned si = pr->stream_index[k];
            if (si >= fmt->nb_streams) continue;   // 越界防御：不该发生
            if (program_contains_stream(sel, si)) continue;
            fmt->streams[si]->discard = AVDISCARD_ALL;
        }
    }
}

HlsSession::~HlsSession() {
    // 正常编排下走到这里时 segments_/playlists_ 都已经空了：Pipeline 的
    // 析构顺序保证 demuxer_ 先毁（avformat_close_input → hls_close →
    // ff_format_io_close → on_io_close 逐个关掉），hls_ 后毁。这段兜底是
    // 给"release_fmt() 从未被调用"或"open 失败"这类路径的，不是给正常
    // 路径的——如果正常路径能走到这里还有残留，那是上面那条顺序被破坏了。
    //
    // 跟 on_io_close 同一条纪律：持锁只搬走容器，真正的拆解在锁外做。
    std::map<AVIOContext*, OpenSegment>                   dying_segments;
    std::map<AVIOContext*, std::unique_ptr<OpenPlaylist>> dying_playlists;
    {
        std::lock_guard<std::mutex> g(mu_);
        dying_segments.swap(segments_);
        dying_playlists.swap(playlists_);
    }
    for (auto& [ctx, seg] : dying_segments) {
        (void)ctx;
        destroy_segment(std::move(seg.bridge), seg.src);
    }
    for (auto& [ctx, pl] : dying_playlists) {
        (void)pl;
        destroy_playlist_ctx(ctx);
    }
    if (open_opts_ != nullptr) av_dict_free(&open_opts_);
    // release_fmt() 已经调过的话 fmt_ 是 nullptr，AVFormatContext 归
    // Demuxer 所有（它会 avformat_close_input）。没调过说明 open 那一步
    // 还没发生，这里负责释放——用 avformat_free_context 而不是
    // avformat_close_input：后者会去关 s->pb，而此时压根还没 open。
    if (fmt_ != nullptr) {
        avformat_free_context(fmt_);
        fmt_ = nullptr;
    }
}

std::unique_ptr<HlsSession> HlsSession::create(const std::string& url,
                                               const syp_config&  dl_cfg,
                                               const HlsOptions&  opts,
                                               syp_status*        err) {
    syp_status local = SYP_OK;
    if (err == nullptr) err = &local;
    *err = SYP_OK;

    if (url.empty()) {
        *err = SYP_ERR_INVALID_ARG;
        return nullptr;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        *err = SYP_ERR_OOM;
        return nullptr;
    }

    auto self = std::unique_ptr<HlsSession>(new HlsSession());
    self->dl_cfg_      = dl_cfg;
    self->cache_dir_   = (dl_cfg.cache_dir != nullptr) ? dl_cfg.cache_dir : "";
    self->dl_cfg_.cache_dir = self->cache_dir_.empty() ? nullptr : self->cache_dir_.c_str();
    self->opts_        = opts;
    self->real_scheme_ = scheme_of(url);
    self->ffmpeg_url_  = to_ffmpeg_url(url);

    // ---- 装配顺序不能变：opaque 必须先于回调，回调必须先于 open ----
    fmt->opaque    = self.get();       // io_open 回调靠它找回本对象
    fmt->io_open   = &HlsSession::on_io_open;
    fmt->io_close2 = &HlsSession::on_io_close;

    // 结构护栏：见头文件注释。返回值故意不判——这条设置失败只意味着
    // 护栏没装上，不该让整条会话建不起来（护栏的作用是让漏网路径可见，
    // 不是功能本身）。
    (void)av_opt_set(fmt, "protocol_whitelist", "file", AV_OPT_SEARCH_CHILDREN);

    // 当前套件无覆盖，不是死代码。
    // 变异实测：把这两行整个删掉，hls_e2e 全绿。它守的不是
    // 分片的阻塞读（那条由 open_segment() 入表时那次补中止 + AvioBridge
    // 自己的中止标志负责），而是**FFmpeg 自己在直播 reload 等待里空转的
    // 那一段**：hls.c:1638-1641 每 100ms 调一次 ff_check_interrupt，读到
    // 我们这个回调返回 1 才提前醒。没有它，用户中止的响应延迟会被拉长到
    // 下一次 reload 醒来为止。
    //
    // 失去覆盖的具体原因：把 request_abort_unblocks_a_hanging_
    // segment_read 里 killer 线程的睡眠从 300ms 改成 3000ms 之后，中止
    // 总是落在"已经在分片阻塞读里"这个时刻，再也落不进 reload 等待窗口，
    // 这个回调就彻底失去了守卫。要重新覆盖它，需要一条直播用例把中止
    // 精确打进 reload 的 sleep 里（时序敏感，本轮不补）。
    fmt->interrupt_callback.callback = &HlsSession::on_interrupt;
    fmt->interrupt_callback.opaque   = self.get();

    // hls 解封装器的 AVOption，必须经 avformat_open_input 的 options 参数
    // 传进去（见头文件 open_options() 上方注释）。
    if (av_dict_set(&self->open_opts_, "http_persistent", "0", 0) < 0 ||
        av_dict_set(&self->open_opts_, "allowed_extensions", "ALL", 0) < 0) {
        avformat_free_context(fmt);
        *err = SYP_ERR_OOM;
        return nullptr;
    }

    self->fmt_ = fmt;
    return self;
}

AVFormatContext* HlsSession::release_fmt() noexcept {
    AVFormatContext* f = fmt_;
    fmt_ = nullptr;
    return f;
}

void HlsSession::mark_playback_started() noexcept {
    playback_started_.store(true, std::memory_order_release);
}

syp_status HlsSession::pending_playlist_error() const noexcept {
    // 具体原因（404/超时/变加密…）优先于笼统的 SYP_ERR_IO（"成功取回但
    // 仍是直播"）：两路列表一路 404、一路只是没有 ENDLIST 时，报 404。
    std::lock_guard<std::mutex> g(mu_);
    syp_status generic = SYP_OK;
    for (const auto& [url, st] : playlist_errors_) {
        (void)url;
        if (st != SYP_ERR_IO) return st;
        generic = st;
    }
    return generic;
}

void HlsSession::clear_playback_errors() noexcept {
    {
        std::lock_guard<std::mutex> g(mu_);
        playlist_errors_.clear();
    }
    pending_segment_error_.store(SYP_OK, std::memory_order_release);
    precise_segment_error_.store(0, std::memory_order_release);
}

void HlsSession::request_abort() noexcept {
    abort_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> g(mu_);
    for (auto& [ctx, seg] : segments_) {
        (void)ctx;
        if (seg.bridge != nullptr) seg.bridge->request_abort();
    }
}

int HlsSession::on_interrupt(void* opaque) {
    auto* self = static_cast<HlsSession*>(opaque);
    if (self == nullptr) return 0;
    return self->abort_.load(std::memory_order_acquire) ? 1 : 0;
}

int HlsSession::on_io_open(AVFormatContext* s, AVIOContext** pb, const char* url,
                           int /*flags*/, AVDictionary** /*options*/) {
    if (s == nullptr || pb == nullptr || url == nullptr) return AVERROR(EINVAL);
    auto* self = static_cast<HlsSession*>(s->opaque);
    if (self == nullptr) return AVERROR(EINVAL);

    // FFmpeg 看到的是改写过的 http:// URL，这里换回真实 scheme。
    const std::string real = to_real_url(url, self->real_scheme_);

    return (channel_for(real) == Channel::Playlist)
               ? self->open_playlist(pb, real)
               : self->open_segment(pb, real);
}

int HlsSession::playlist_read(void* opaque, uint8_t* buf, int buf_size) {
    auto* op = static_cast<OpenPlaylist*>(opaque);
    if (op == nullptr || buf_size < 0) return AVERROR(EINVAL);
    const int64_t size = static_cast<int64_t>(op->body.size());
    if (op->pos >= size) return AVERROR_EOF;
    const int64_t n = std::min<int64_t>(size - op->pos, static_cast<int64_t>(buf_size));
    if (n == 0) return AVERROR_EOF;
    std::memcpy(buf, op->body.data() + op->pos, static_cast<std::size_t>(n));
    op->pos += n;
    return static_cast<int>(n);
}

int64_t HlsSession::playlist_seek(void* opaque, int64_t offset, int whence) {
    auto* op = static_cast<OpenPlaylist*>(opaque);
    if (op == nullptr) return AVERROR(EINVAL);
    const int64_t size = static_cast<int64_t>(op->body.size());
    // AVSEEK_FORCE 只是"别偷懒"的提示，对内存缓冲没有意义，先 mask 掉；
    // AVSEEK_SIZE 不是 whence，是"别 seek，只告诉我总长度"。
    const int w = whence & ~AVSEEK_FORCE;
    if (w == AVSEEK_SIZE) return size;

    int64_t np = 0;
    switch (w) {
        case SEEK_SET: np = offset;           break;
        case SEEK_CUR: np = op->pos + offset; break;
        case SEEK_END: np = size + offset;    break;
        default:       return AVERROR(EINVAL);
    }
    if (np < 0) return AVERROR(EINVAL);
    // 允许 seek 到末尾之后：FFmpeg 的探测路径会这么做，随后的 read 自然
    // 返回 AVERROR_EOF，这跟真实文件的行为一致。
    op->pos = np;
    return np;
}

int HlsSession::open_playlist(AVIOContext** pb, const std::string& real_url) {
    // 【&abort_ 是这条通道的看门狗接线，补上的洞】在此之前
    // open_playlist() 完全不看 abort_：分片那条走 AvioBridge →
    // syp_source_interrupt() 是通的，播放列表这条只受自身 read_timeout_ms
    // 约束，request_abort() 在播放列表抓取期间开火一点效果都没有。直播每几秒
    // 重拉一次播放列表，用户导航离开时正撞上一次重拉是常态而不是边角，所以
    // 这个洞在 HLS 下必须堵。语义见 playlist_fetcher.h 里 abort 参数的注释：
    // 进门已置位就一个请求都不发，抓取途中置位则 cancel + 等 on_complete。
    //
    // 生命周期：abort_ 是本会话自己的成员，而本函数是 io_open 回调、只可能
    // 在会话存活期间被 FFmpeg 调到，指针天然有效。
    PlaylistFetchResult r = fetch_playlist(syp::dl::current_http_backend(), real_url,
                                           dl_cfg_.connect_timeout_ms,
                                           dl_cfg_.read_timeout_ms,
                                           &abort_);
    // 【播放阶段才记账，open 阶段一律不记】
    // hls.c 对两个阶段的失败处理完全不同：
    //   · open 阶段（hls_read_header，hls.c:2178-2183）：多档 master 下某一档
    //     拉取失败只标 broken、跳过，其余档照常播——记下来就会把一次完整的
    //     播放在 Eof 出口改写成 Error（variant_broken_at_open_does_not_poison_eof）；
    //   · 播放阶段（直播重拉，hls.c:1607-1611）：一次失败不重试，直接结束
    //     这一路播放列表，最终干净地返回 AVERROR_EOF。停播已经发生，唯一的
    //     问题是报不报——所以要记。
    //
    // 【记的是每个 URL "最后一次取回"的结局，不只是取回失败】取回 200 但
    // body 解析失败（HTML 错误页，hls.c:849）、重拉始终没有新
    // 分片（m3u8_hold_counters）——这些同样让那一路列表结束，而取回层面
    // 全都是"成功"。所以判据换成结构性的一条：**直播只有在最后一次取回的
    // 播放列表带 EXT-X-ENDLIST 时才会正常播完**。据此每次播放期取回记：
    //   · 取回失败        → 那次的 syp_status；
    //   · 变加密          → SYP_ERR_NOT_IMPLEMENTED；
    //   · 成功但无 ENDLIST → SYP_ERR_IO（"还是直播"——此后若在 Eof 出口仍是
    //                        这个结局，就是异常结束）；
    //   · 成功且有 ENDLIST → 删掉条目（正常结束的样子）。
    // 按 URL 记、后一次覆盖前一次：hls.c:1963（select_cur_seq_no，列表重新
    // 变成 needed 时）会**忽略**重拉失败继续用旧列表，之后同 URL 取回
    // 成功就不该留那次失败的账；分轨源两路列表各自重拉，一路的结局也不能
    // 冲掉另一路的。点播列表播放期间从不重拉，这张表在点播下恒空。
    const bool playing = playback_started_.load(std::memory_order_acquire);
    if (r.status != SYP_OK) {
        // 中止不算——同 open_segment() 那条纪律，归 Pipeline::aborted_ 管。
        if (playing && r.status != SYP_ERR_CANCELED) {
            std::lock_guard<std::mutex> g(mu_);
            playlist_errors_[real_url] = r.status;
        }
        return syp_status_to_averror(r.status);
    }

    // 加密检测：在把播放列表交给 FFmpeg 之前拒掉，这样密钥 URL 一次都
    // 不会被请求。判据是"有 EXT-X-KEY 且 METHOD 不是
    // NONE"——METHOD=NONE 是合法的"这一段不加密"声明，不能一见
    // EXT-X-KEY 就拒。本版本不支持加密 HLS，要的是明确
    // 报错，不是崩、不是静默播出噪音、不是"看起来在播但没画面"。
    if (playlist_declares_encryption(
            std::string_view(reinterpret_cast<const char*>(r.body.data()), r.body.size()))) {
        // 记 precise_open_error_：avformat_open_input 失败时
        // Demuxer::open_prepared() 会把这次拒绝连同一切别的失败原因一起
        // 归一成 SYP_ERR_IO，调用方分不出"打不开"和"我们主动拒了"。
        // Pipeline::create_hls() 在 open_prepared() 失败之后会换回这里
        // 记的精确原因，见 precise_open_error() 上方注释。
        //
        // 播放阶段（直播中途变加密，#46）不写这个字段，走 playlist_errors_：
        // 这样 precise_open_error_ 的写点只剩 create_hls() 的线程，它不必是
        // 原子的那条论证保持成立。
        if (playing) {
            std::lock_guard<std::mutex> g(mu_);
            playlist_errors_[real_url] = SYP_ERR_NOT_IMPLEMENTED;
        } else {
            precise_open_error_ = SYP_ERR_NOT_IMPLEMENTED;
        }
        return syp_status_to_averror(SYP_ERR_NOT_IMPLEMENTED);
    }
    if (playing) {
        const bool ended = playlist_declares_endlist(
            std::string_view(reinterpret_cast<const char*>(r.body.data()), r.body.size()));
        std::lock_guard<std::mutex> g(mu_);
        if (ended) playlist_errors_.erase(real_url);
        else       playlist_errors_[real_url] = SYP_ERR_IO;
    }

    auto op  = std::make_unique<OpenPlaylist>();
    op->body = std::move(r.body);
    // 字幕 rendition 交给 FFmpeg 之前剥掉：本播放器不支持字幕、FFmpeg 也没开 webvtt
    // demuxer，hls.c 探测字幕组首段必然失败并拖垮整个 open（见 strip_subtitle_renditions）。
    {
        const std::string_view text(reinterpret_cast<const char*>(op->body.data()), op->body.size());
        std::string stripped = strip_subtitle_renditions(text);
        if (stripped.size() != op->body.size()) op->body.assign(stripped.begin(), stripped.end());
    }

    // 内存 AVIOContext：FFmpeg 拿到的是一份已经完整在内存里的播放列表，
    // 不会再有二次 IO。缓冲区由 AVIOContext 持有，io_close 时释放。
    auto* avio_buf = static_cast<unsigned char*>(av_malloc(kAvioBufSize));
    if (avio_buf == nullptr) return AVERROR(ENOMEM);

    AVIOContext* ctx = avio_alloc_context(avio_buf, kAvioBufSize, 0, op.get(),
                                          &HlsSession::playlist_read, nullptr,
                                          &HlsSession::playlist_seek);
    if (ctx == nullptr) {
        av_freep(&avio_buf);
        return AVERROR(ENOMEM);
    }

    {
        std::lock_guard<std::mutex> g(mu_);
        playlists_[ctx] = std::move(op);
    }
    *pb = ctx;
    return 0;
}

// on_error 触发时把精确原因（syp_status + HTTP 状态码）
// 记下来。可能在 syp_source 内部的任意线程调用（syp_source.h 的
// 回调契约）。
void HlsSession::on_source_error(void* ctx, syp_status status, int32_t http_status) {
    auto* self = static_cast<HlsSession*>(ctx);
    if (self == nullptr) return;
    // 中止不算——同 open_segment()/on_io_close() 那条纪律。
    //
    // 【这是**冗余**防御，真正的防线在 step() 的优先级上】即使这里
    // 把 CANCELED 也记下来，用户也不会看到"内容缺失"：Pipeline::step()
    // 的 Eof 出口（pipeline.cpp:436 起）先查 aborted_、再查
    // pending_segment_error()，中止永远赢。测试实测：把这个 if 改成恒
    // false，整套用例照样全绿——它守不住任何当前可观测的
    // 行为，覆盖那个顺序的是
    // tests/test_hls_e2e.cpp::abort_outranks_a_pending_segment_error_at_the_eof_gate。
    // 留着的理由只有一条：pending_segment_error() 的语义是"会话里发生过
    // 一次**真实的**分片失败"，把一次用户主动中止算进去就是记错了账，
    // 哪怕今天没人从这个账上读出错误的结论。
    if (status == SYP_ERR_CANCELED) return;
    const int64_t packed = (static_cast<int64_t>(status) << 32) |
                           static_cast<int64_t>(static_cast<uint32_t>(http_status));
    self->precise_segment_error_.store(packed, std::memory_order_release);
}

int HlsSession::open_segment(AVIOContext** pb, const std::string& real_url) {
    syp_source* src = nullptr;
    syp_source_callbacks cb{};
    cb.ctx      = this;
    cb.on_error = &HlsSession::on_source_error;
    const syp_status rc = syp_source_open(&src, real_url.c_str(), nullptr, &dl_cfg_, &cb);
    if (rc != SYP_OK || src == nullptr) {
        if (src != nullptr) syp_source_close(src);
        const syp_status effective = (rc != SYP_OK) ? rc : SYP_ERR_IO;
        // 【分片打开失败会被 hls.c 悄悄吞掉，这个记号是留下来的痕迹之一】
        // hls 解封装器（read_data_continuous，hls.c）对分片打开失败的默认
        // 策略是重试 seg_max_retry 次（默认 0，即不重试）之后
        // cur_seq_no++ 跳过、继续下一片；playlist 耗尽时照样干净地返回
        // AVERROR_EOF——从 Demuxer::read() 的视角，这跟"真的播完了"没有
        // 任何区别。记下这次失败，好让 step() 在报 Eof 之前有机会分辨。
        //
        // 【这只是 open 阶段那一半】实测：本项目的
        // syp_source_open() 对 HTTP 资源是乐观开——404 这类失败常常到
        // **第一次 read()** 才暴露，open 本身 rc==SYP_OK。这种情况在这里
        // 抓不到，另一半（读时失败）在 on_io_close 里补，看 AvioBridge
        // 关闭前的 diag().last_averror。
        //
        // 中止（SYP_ERR_CANCELED）不算：那条路径已经由 Pipeline::aborted_
        // + step() 里另一处改写覆盖，这里再记一遍是重复归因，
        // 且会把"用户主动中止"误标成"内容缺失"。
        //
        // 【当前套件无覆盖】把 `effective != SYP_ERR_CANCELED`
        // 这个条件连同它守着的 store 一起改成恒 false，
        // ctest 26/26 照样全绿——不是死代码，是本项目 syp_source_open()
        // 对 HTTP 资源乐观开这个既有行为（见上方【这只是 open 阶段那一半】）
        // 的自然结果：目前没有任何用例能让它同步失败，404/网络错误统统
        // 要到 read() 才暴露，走的是 on_io_close 那条路。要真正覆盖它，
        // 需要一个"open 就同步失败"的现场（比如 cache_dir 指向一个不可写
        // 的路径，或者 syp_source_open 收到畸形 URL）——留着不补是权衡：
        // 这条分支处理的是"万一将来 syp_source_open() 的实现改成同步校验
        // URL/cache_dir"这类尚未发生的情况，纯防御，收益小于新引入一条
        // 专门覆盖它的用例的成本。
        if (effective != SYP_ERR_CANCELED) {
            pending_segment_error_.store(static_cast<int32_t>(effective),
                                         std::memory_order_release);
        }
        return syp_status_to_averror(effective);
    }

    auto bridge = AvioBridge::create(src, 0);
    if (bridge == nullptr) {
        syp_source_close(src);
        // 【同样无覆盖，原因不同】这一支只有真的堆分配
        // 失败（AvioBridge::create 内部分配失败）才会走到，本项目的用例
        // 不模拟 OOM（跟 open_playlist() 里 av_malloc(kAvioBufSize) 失败
        // 走 `return AVERROR(ENOMEM)` 那支是同一类：现实中触发不了，纯
        // 防御）。留着的理由是"万一真 OOM，至少报得出一个精确原因"，
        // 不是指望它被测到。
        pending_segment_error_.store(static_cast<int32_t>(SYP_ERR_OOM),
                                     std::memory_order_release);
        return AVERROR(ENOMEM);
    }
    AVIOContext* ctx = bridge->ctx();
    {
        // 【中止之后新开的每一条分片，唯一的中止来源就是下面这一行】
        //
        // 【m2：这里原来的说法严重低估了自己，改准】原文把它写成"只防一个
        // 窄交错、漏掉最多退化到 read_timeout_ms，有界"。**不是的。**
        // HlsSession::request_abort()（:264）是**一次性**的：置 abort_，然后
        // 遍历**当时**在 segments_ 里的 bridge 各打断一次，就结束了——它不会
        // 再跑第二遍。而看门狗只调它一次。所以 request_abort() 之后
        // io_open 新开出来的每一条分片，`AvioBridge::request_abort()` 的
        // 调用点**全工程只剩下面这一行**。
        //
        // 删掉它的后果实测：
        // request_abort_unblocks_a_hanging_segment_read 撞穿软看门狗
        // （60s）又撞穿硬看门狗（180s），**整个 test_hls_e2e 二进制被强制
        // 打死**（exit 70），不是"多等一会儿"。原因是中止之后 hls.c 会继续
        // 往下开新分片，每一条都在一次谁也不会再打断的读上等满
        // read_timeout_ms（该用例设的是 30000ms），逐条累加——
        // 单条有界不等于总量有界。
        //
        // 【为什么必须在锁内做】看门狗与入表之间有一个交错：
        //   本线程读 abort_ == false
        //   → request_abort() 置位并遍历 segments_（此时本条还没入表，漏掉）
        //   → 本线程才把它插进 segments_
        // ⇒ 这条新分片一次中止都收不到。挪进锁内就彻底关上：
        // request_abort() 是**先 store 后加锁**，所以两个方向都覆盖——
        //   · 它先 store 再抢锁：本线程在锁内读到 true，自己 abort；
        //   · 本线程先拿到锁：入表与读标志在同一临界区内完成，它随后拿到锁
        //     时一定能在 segments_ 里看见这条。
        //
        // 【没有破坏"持 mu_ 不做可能阻塞的事"那条纪律】锁内多出来的只有一次
        // 原子读和（最坏情况下）一次 AvioBridge::request_abort()——后者是
        // 置原子标志 + syp_source_interrupt()，而 SourceBridge::interrupt()
        // （source_bridge.cpp:963-967）只做 lock_guard + 置 bool + notify_all，
        // 没有任何等待、没有 IO。真正可能阻塞的 syp_source_open() 仍在锁外。
        // 锁序也没引入新边：mu_ → SourceBridge::mu_ 这条 request_abort() 本来
        // 就在走（它先取 mu_ 再遍历 bridge），反方向不存在——SourceBridge
        // 不认识 HlsSession。
        std::lock_guard<std::mutex> g(mu_);
        if (abort_.load(std::memory_order_acquire)) bridge->request_abort();
        segments_[ctx] = OpenSegment{src, std::move(bridge)};
    }
    *pb = ctx;
    return 0;
}

int HlsSession::on_io_close(AVFormatContext* s, AVIOContext* pb) {
    if (s == nullptr || pb == nullptr) return 0;
    auto* self = static_cast<HlsSession*>(s->opaque);
    if (self == nullptr) return 0;

    // 【锁的范围只到 extract()】syp_source_close() 是可能
    // 阻塞的（SourceBridge::close() 里的 sched_->stop() + cv_.wait），
    // 持着 mu_ 调它会让 request_abort() 卡在锁上——看门狗线程存在的理由
    // 就是打断阻塞 IO，它自己先被锁住的话这条路径整个失效。
    // open_segment() 本来就是对的（syp_source_open 在取锁之前），
    // 这里补齐另一半。
    std::map<AVIOContext*, OpenSegment>::node_type                   seg_node;
    std::map<AVIOContext*, std::unique_ptr<OpenPlaylist>>::node_type pl_node;
    {
        std::lock_guard<std::mutex> g(self->mu_);
        seg_node = self->segments_.extract(pb);
        if (seg_node.empty()) pl_node = self->playlists_.extract(pb);
    }

    if (!seg_node.empty()) {
        // 【这段检测隐含依赖 http_persistent=0，跨
        // 文件，写在这里】只有 c->http_persistent 为假时 hls.c 才会为
        // 每个分片调 ff_format_io_close()（进而触发本函数）——为真时
        // hls.c 复用同一个 AVIOContext（v->input_read_done=1），本函数
        // 根本不会为那个分片被调（hls.c:1753-1757）。HlsSession::create()
        // 里设了 `av_dict_set(&open_opts_, "http_persistent", "0", ...)`
        // （本文件靠前处），所以这条依赖目前成立；但它是隐式的——谁哪天
        // 因为别的原因（比如追求连接复用的性能）把这个选项翻了，404 会
        // 无声地退回被当成 EOF，不会有任何用例变红，因为这条依赖没有
        // 自己的回归测试钉着它，只有这行注释。
        //
        // 【读时失败的那一半——open 成功不代表分片真的到手】见
        // open_segment() 里那段注释：syp_source_open() 对 HTTP 资源是
        // 乐观开，像 404 这样的失败常常到第一次 avio_read() 才暴露，而
        // hls.c 的 read_data_continuous 对"这次 read 返回 <=0"完全不区分
        // 干净 EOF 和真错误——统统当成"这片完了"，关掉、cur_seq_no++、
        // 换下一片（hls.c 里那段 `if (ret > 0) return ret;` 之后的分支，
        // 不看 ret 到底是 AVERROR_EOF 还是别的什么）。
        //
        // 【这里看的是"读或 seek"，不只是"读"——这个
        // bridge 没有写路径，措辞不能带"写"字】AvioBridge::on_seek()
        // 在 syp_source_seek() 返回负值时同样会写 last_averror_
        // （avio_bridge.cpp）；已知可达路径：mov 解封装器探测阶段可能
        // seek(SEEK_END)，若 SourceBridge::seek() 此时总长度还未知
        // （total_length_ < 0），会返回 SYP_ERR_INVALID_ARG
        // （source_bridge.cpp:944）。这正是我们想要的——seek 失败同样
        // 说明这条分片没有正常到手，理应被下面这段逻辑当成真失败。
        //
        // 在 AvioBridge 真正被销毁、它这辈子最后一次 read/seek 失败的痕迹
        // （diag().last_averror）还没被冲掉之前看一眼：
        //   · 0            —— 从没失败过（分片整个没读到底就被关，比如
        //                      init section 只读了一部分），不算错误；
        //   · AVERROR_EOF  —— 干净读完，这是分片正常结束的样子；
        //   · AVERROR_EXIT —— 看门狗打断，归 Pipeline::aborted_ 管。
        //                      【同 on_source_error()，
        //                      这是**冗余**防御】step() 的 Eof 出口先查
        //                      aborted_ 再查 pending_segment_error()，
        //                      中止永远赢，所以漏掉这个条件也不会让用户
        //                      看到"内容缺失"。留着是为了账记得对——一次被打断的
        //                      读不是"这条分片失败了"；
        //   · 其它任何负值 —— 真失败（404/网络错误/超时/seek 失败…），
        //                      记下来。
        if (seg_node.mapped().bridge != nullptr) {
            const int last = seg_node.mapped().bridge->diag().last_averror;
            if (last != 0 && last != AVERROR_EOF && last != AVERROR_EXIT) {
                self->pending_segment_error_.store(static_cast<int32_t>(SYP_ERR_IO),
                                                   std::memory_order_release);
            }
        }
        destroy_segment(std::move(seg_node.mapped().bridge), seg_node.mapped().src);
        return 0;
    }
    if (!pl_node.empty()) {
        destroy_playlist_ctx(pb);
        return 0;
    }
    // 既不是我们开的分片也不是我们开的播放列表——不认识的 AVIOContext
    // 一律不碰：猜着去 free 一个别处拥有的对象比漏掉一次要糟得多。
    return 0;
}

}  // namespace syp::media::hls
