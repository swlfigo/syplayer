// test_decode_e2e.cpp — 解码管线的端到端验证矩阵：场景 A~D +
// 场景 E~H（背压与错误路径）。
//
// 口径：同 tools/syp_probe/frame_digest.h 顶部注释——参照路径直接用
// libavformat + libavcodec 解码（不经过我们任何组件），被测路径经
// AvioBridge → Demuxer → PacketQueue → Decoder → FrameQueue（Pipeline），
// 逐帧比对 FrameDigest（含像素/采样数据的 SHA-256，不止元数据）。
//
// 场景字母 → TEST_CASE 对照:
//   A → a_local_file_sequential_to_eof         本地文件顺解到尾
//   B → b_dl_layer_sequential_matches_a         经 dl 层（回环服务器）顺解到尾，
//                                                且与 A 互相全等
//   C → c_multi_seek_then_decode                多点 seek 后继续解
//   D → d_av_interleave_matches_reference       音视频双轨交织，顺序与参照一致
//   E → e_queue_full_triggers_blocked_and_drains_without_loss
//                                                极小队列逼真 Blocked、排空后帧不丢不重
//   F → f_corrupted_packet_is_skipped_and_counted
//                                                损坏包被跳过且计数，其余帧仍与参照全等
//   G → g_decoder_init_failure_track_fails_others_continue
//                                                一条轨解码器打不开，该轨终止、其它轨继续
//   H → h_dl_layer_error_propagates_and_step_stops_advancing
//                                                dl 层报错冒上来，step() 报 Error 且不再推进
//
// E~H 的详细设计取舍这里只放"为什么这样设计"的要点：
//
//   E：A~D 因为 decode_pipeline() 的"产出即排空"
//      驱动纪律，Blocked 分支一次都没进过（跟 d2_ 用例上方长注释是同一个
//      结构性成因）。E 用极小的 PipelineConfig（max_frames_per_track=1）
//      在真实解码路径上把 Blocked 真正压出来，但比对策略跟 A~D 不同——
//      跟 d2_ 一样，backpressure 下 Pipeline 按轨号升序的仲裁顺序不保证
//      跟 FFmpeg 参照的真实交织顺序一致（这不是缺陷，pipeline.h 顶部写
//      得很清楚，仲裁顺序本身带任意性），所以 E 不用 diff_frames() 做
//      全局顺序比对，改成按 track_index 分组后逐轨比对（同 d_ 用例里
//      `ref_by_track == got_by_track` 那一手，只是这里它是主断言而不是
//      辅助定位）——这仍然是逐帧内容比对（含 SHA-256），只是不含"哪条轨
//      先谁后"这一维，跟 E 本身要验的东西（帧不丢不重）严格对应。
//
//      【曾经的盲区】E 最初只走了测试自己写的驱动
//      循环（decode_forcing_backpressure），库里 tools/syp_probe/
//      frame_digest.cpp::decode_pipeline() 自己那条 Blocked 排空逻辑
//      （drain_all_tracks）完全没被验证过——它默认"产出即排空"，
//      FrameQueue 从不堆积，Blocked 分支永远走不进去，跟 A~D 是同一个
//      结构性盲区，只是换了一层：实测把 drain_all_tracks 短路成
//      `return true`，16/16 全绿，插桩确认命中 0 次。修法：
//      DecodeOptions::lazy_pop（frame_digest.h，仅测试用，默认关）逼
//      decode_pipeline() 自己走到 Blocked/drain_all_tracks；E 现在同时
//      跑测试本地那条和库里这条，两条都跟 ref_by_track 比。
//   F：实测证明 test_packet_digest.cpp 里那种简单翻转 mdat 字节（XOR
//      0xFF）在 H.264 上几乎总是被解码器悄悄"容错"成不同的像素，
//      skipped_packets 恒为 0——不产生真正的解码错误。改成定位到某个
//      视频 packet 在文件里的真实字节偏移（av_read_frame 的 pkt->pos），
//      把它开头 4 字节（AVCC 格式下这是该 NAL 的长度前缀）改写成一个
//      越界巨大值，可靠触发 avcodec_send_packet 级别的整包拒绝。
//   G：本项目的 build-ffmpeg.sh 只 --enable-decoder=h264/aac
//      （tools/build-ffmpeg.sh:70-71）——用一份音频轨编码成 flac（系统
//      ffmpeg CLI 支持，我们自己这份 SYFFmpeg.xcframework 不支持）的素材，
//      不需要手工构造损坏的 codecpar，真实触发 avcodec_find_decoder()
//      返回 nullptr 的该轨终止路径。
//   H：LoopbackServer 的 close_after_bytes 会被 dl 层的重试/续传机制
//      自动吸收（这正是 test_probe_e2e.cpp 场景 F 在验的东西）——不能
//      靠它模拟"不可恢复"的错误。改成:正常配置先解出一批帧，再用
//      LoopbackServer::set_config() 切到 status_code=404（dl_task.cpp::
//      classify_http_status 里 408/429/5xx 是
//      Retryable、416 是 Unsatisfiable416，除这几个之外的所有状态码
//      （包括但不限于 404）整类都判 Fatal——404 只是取其中最典型的一个
//      代表，不是"唯一"），命中 SourceBridge::fatal_（一次置位、永久
//      粘滞，见 source_bridge.cpp:490 set_fatal_locked）之后，往后每次
//      read() 都立刻返回同一个错误——天然满足"不再推进"要真的验一遍再调
//      几次 step() 这条要求。
//
// 场景 D 原本被设计成「唯一守卫 Pipeline::step() 解码分支轨道扫描顺序
// 反转」这个遗留漏测点的用例——全部单测下这个变异体
// 5/5 全绿（确定性用例是 run()==run() 的自比较，两次运行同样反转，序列
// 仍然全等，自比较结构性地测不出「内部自洽但跟参照不一致」）。
//
// 【更正】跟参照比对（d_av_interleave_matches_reference 的
// diff_frames(ref, got)）实测**抓不住**这个变异体：在 decode_pipeline()
// 「产出即排空」的驱动纪律下，任一时刻至多一条轨的 PacketQueue 非空，
// 「两条轨同时就绪、扫描顺序说了算」这个仲裁点在顺解到尾这条路径上
// 结构性地从不发生（详见 d2_decode_branch_scans_tracks_in_ascending_order
// 上方的长注释）。真正堵住这个漏测点的是 d2_ 这条**常驻**用例——它不
// 跟参照比，直接断言 pipeline.h 文档化的调度契约本身（按轨号升序），
// 用背压人为制造仲裁点。这条已经取代了"临时改 pipeline.cpp、重编、
// 跑一遍确认变红、再还原"的验证方式。
//
// 每条场景配一次反向自检：
//   A：ffmpeg_video_decoder.cpp 里喂给 Frame::from_av 的 time_base 改错一个
//      数量级（pts 时基算错必须变红这类标准自检之一）。
//   B：avio_bridge.cpp::on_read 读到的字节翻一位——只污染经 dl 层这条路径，
//      不影响本地文件路径，直接压中"B 与 A 互相全等"这条断言。
//   C：Pipeline::seek() 里跳过解码器 flush()——针对 C 的核心主张（seek 之后
//      解码器内部状态必须真正复位）。
//   D：轨道扫描顺序反转，这是点名要抓的强制变异体——d_ 本身测不出来
//      （见上方更正），常驻用例 d2_decode_branch_scans_tracks_in_ascending_order
//      直接断言 pipeline.h 的调度契约，实测变红。
//
// 看门狗：四个场景（A/B/C/D）都各包一层，跟 test_apple_http_backend.cpp
// 同一套（软超时打诊断 + fflush + 硬超时 _exit）。
//
// 【更正】原来的说法——"A/C/D 只有本地文件与纯同步的
// Pipeline::step()，靠 ctest 自身的 TIMEOUT 兜底即可"——此前
// 只是形式主义式的谨慎；已经证明这不是假设性的风险：
// Pipeline::step() 在带 B 帧素材上真的会永久活锁（见 pipeline.cpp
// draining 那段注释），而且修复之后同族的第二个活锁（seek() 后
// eof_sent 未回归保护）又被抓到——"解码器内部可能长时间不
// 返回"不再是一句抽象的提醒，是这个文件里两次真实撞见过的
// 缺陷类别。ctest 的 TIMEOUT（300s）只会让日志里连是哪条用例卡住都看不
// 出来，跟此前一次「15 分钟 ctest 超时、日志里连测试名都没有」的教训
// 是同一类问题。soft=60s/hard=180s：当前素材（-preset medium）下
// decode_e2e 全部 5 条用例合计约 18s，单条留出的余量足够吸收机器抖动，
// 又远小于 300s 的 ctest TIMEOUT，两者不会互相抵消。
//
// 四个场景的素材从 faststart.mp4 换成了 bframes_faststart.mp4
// （gen-fixtures.sh 新增，-preset medium -bf 3，真的带 B 帧；-c copy 重排
// moov 到文件头，跟 faststart.mp4 是 moovend.mp4 的关系完全对称）。这不是
// 顺手换素材——faststart.mp4 用 -preset ultrafast 编码，libx264 在这个
// preset 下不产 B 帧，这四个场景因此从建立以来就没有真正跑过一份带 B 帧
// 的素材，而现实世界的 H.264/HEVC 几乎必带 B 帧。这曾是一个结构性盲区，
// 掩盖了 Pipeline 的一处活锁（修复见
// src/media/pipeline.cpp draining 那段注释）。换素材必须在修复 Pipeline
// 之后才做（顺序反了的话，这四个场景会在新素材上全红，把一个已知缺陷
// 变成一堆无法归因的失败）。
#include "frame_digest.h"
#include "frame_digest_internal.h"
#include "scenarios.h"
#include "support/loopback_server.h"
#include "support/watchdog.h"
#include "tiny_test.h"

#include "media/avio_bridge.h"
#include "media/pipeline.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_source.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

using namespace syp::probe;
using namespace syp::media;
using syp::dl::test::LoopbackConfig;
using syp::dl::test::LoopbackServer;

namespace {

// 环境变量没配时，原来直接拼出 "/name" 这样一个必然打不开的
// 路径，第一个冒出来的失败信息是 `ref.error_stage.empty()`（或类似的
// "解码/打开失败"断言），诊断绕远——读到日志的人会先怀疑是解码器/素材
// 本身的问题，而不是"素材路径根本没配"这种环境问题。在这里直接报，
// 让失败信息第一时间指向真正的原因。
std::string fixture(const char* name) {
    const char* dir = std::getenv("SYP_FIXTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        tiny_test::fail(__FILE__, __LINE__,
                        "SYP_FIXTURE_DIR 未设置——素材路径没配，不是解码本身失败");
    }
    return std::string(dir ? dir : "") + "/" + name;
}

// 运行期守护（跟 test_pipeline.cpp 里的
// 同名助手同一份逻辑，各自本地一份而不是共享头——两个 TU 都已经各自
// 有一套本地 FFmpeg 探测脚手架，为一个三行函数新增共享头不值得）：这四
// 个场景断言的是"帧数/diff_frames 是否全等"这类间接后果，本身不依赖
// "素材真的带 B 帧"这个前提——变异 4 证明了这一点：把
// bframes_faststart.mp4 换成零 B 帧的同名副本，四个场景照样 4/4 全绿，
// 缺陷完全逃逸。gen-fixtures.sh 生成期的 ffprobe 断言挡不住"这次跑的
// 文件不是刚生成的那份"，这里对当前实际打开的文件独立再核一次：视频轨
// AVCodecParameters::video_delay（解码器重排序延迟的直接度量）> 0。
bool material_has_b_frames(const std::string& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    bool has_delay = false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->video_delay > 0) {
            has_delay = true;
            break;
        }
    }
    avformat_close_input(&fmt);
    return has_delay;
}

// ---------------------------------------------------------------------
// 场景 B：经 dl 层解码。scenarios.h 的 run_through_source() 是 demux
// 维度的等价物（syp_source → AvioBridge → Demuxer），这里换成
// Pipeline::create_avio，走到帧这一层——scenarios.h 没有现成的帧级版本
// （Produces 是"无新接口"，不新增 scenarios.h 的能力），本地
// 搭一份，复用 ensure_apple_backend()/TempCacheDir 等既有脚手架。
struct SourceDecodeOutcome {
    DecodeResult decode;
    std::string  open_err;        // 非空 = open/bridge/pipeline 建立失败
    bool         watchdog_fired = false;
    // dl 层报的错（AvioBridge::on_read/on_seek 转换出的
    // averror）此前完全没被带出来——如果 dl 层报了错，观察到的现象只是
    // "帧数比参照少"或"diff_frames 非空"，读日志的人得先怀疑到解码器/
    // 素材，再排除掉才会想起去查 dl 层，诊断绕远。
    // 注意（实测记录，避免误读）：正常顺解到尾也会留下一个非零值——
    // FFmpeg 读到底时 avio 层的最后一次 on_read 本来就该返回
    // AVERROR_EOF（-541478725），这是**预期的终态**，不是错误；0 表示
    // "全程连这个终态都没留下"（比如整条流从未真正读到底），本身反而
    // 更值得怀疑。真正值得警觉的是 AVERROR_EOF 之外的其它非零值。
    int          last_averror = 0;
};

SourceDecodeOutcome decode_via_source(const std::string& url, const std::string& cache_dir,
                                       const DecodeOptions& opt) {
    SourceDecodeOutcome out;

    if (!ensure_apple_backend()) {
        out.open_err = "注册 Apple HTTP 后端失败";
        return out;
    }

    syp_config cfg;
    syp_config_init(&cfg);
    cfg.struct_size = sizeof(syp_config);
    cfg.cache_dir   = cache_dir.c_str();

    syp_source* src = nullptr;
    const syp_status st = syp_source_open(&src, url.c_str(), nullptr, &cfg, nullptr);
    if (st != SYP_OK || src == nullptr) {
        out.open_err = "syp_source_open 失败: " + std::string(syp_status_str(st));
        return out;
    }

    auto bridge = AvioBridge::create(src, 64 * 1024);
    if (!bridge) {
        syp_source_close(src);
        out.open_err = "AvioBridge::create 失败";
        return out;
    }

    syp_status perr = SYP_OK;
    auto pipeline = Pipeline::create_avio(bridge->ctx(), PipelineConfig{}, &perr);
    if (pipeline == nullptr) {
        bridge.reset();
        syp_source_close(src);
        out.open_err = "Pipeline::create_avio 失败: status=" + std::to_string(perr);
        return out;
    }

    {
        // 跟 test_apple_http_backend.cpp / test_scheduler.cpp 同一套：软
        // 超时打诊断（fflush 保证挂死时日志已经落地），硬超时 _exit 保证
        // 进程不会被 ctest TIMEOUT 拖到日志里连用例名都看不见。
        syp::test::Watchdog wd(
            "decode_via_source", /*soft_ms=*/30000, /*hard_ms=*/90000,
            "疑似卡在 Pipeline::step() 内部解码器，或 dl 层的阻塞读——"
            "见 pipeline.h 顶部注释：组件本身不阻塞等待，但解码器内部可能"
            "长时间不返回，AvioBridge 之下的 syp_source 同理。");
        out.decode        = decode_pipeline(*pipeline, opt);
        out.watchdog_fired = wd.fired();
    }

    // M-4：在 bridge 析构之前把它记录到的 last_averror 取出来——析构之后
    // 就再也拿不到了。0 是"没记录到任何错误"这条语义的默认值本身，不是
    // 单独判断"要不要读"的旗标。
    out.last_averror = bridge->diag().last_averror;

    pipeline.reset();
    bridge.reset();
    syp_source_close(src);
    return out;
}

// 判别力自检共用：确认真的同时测到视频轨与多声道音频轨——不是只覆盖了
// 退化的一半（那类"素材只有单声道"陷阱，gen-fixtures.sh 现在
// 产出立体声，这里显式断言用到了这一点）。
void check_saw_video_and_multichannel_audio(const std::vector<FrameDigest>& frames) {
    bool saw_video = false, saw_audio_multi = false;
    for (const FrameDigest& d : frames) {
        if (d.width > 0) saw_video = true;
        if (d.nb_samples > 0 && d.channels > 1) saw_audio_multi = true;
    }
    CHECK(saw_video);
    CHECK(saw_audio_multi);
}

// ---------------------------------------------------------------------
// 场景 C：多点 seek 后继续解。decode_reference()（frame_digest.h）不支持
// seek——不新增接口，这里在测试文件本地用原始 FFmpeg API 搭一份
// "seek 到 ts_us 再解 N 帧"的参照实现，语义上跟 Demuxer::seek()/
// Pipeline::seek() 逐字对应：av_seek_frame(fmt, -1, ts_us,
// AVSEEK_FLAG_BACKWARD) 之后 flush 每条已开的解码器。不能直接复用
// frame_digest.cpp::run_decode——那是该 TU 匿名 namespace 里的私有实现。

std::string av_err_str(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

struct RefSeekTrack {
    AVCodecContext* ctx       = nullptr;
    AVRational      time_base = {0, 1};
    bool            is_video  = false;
};

struct RefSeekCtx {
    AVFormatContext*          fmt = nullptr;
    std::vector<RefSeekTrack> tracks;

    ~RefSeekCtx() {
        for (RefSeekTrack& t : tracks) {
            if (t.ctx != nullptr) avcodec_free_context(&t.ctx);
        }
        if (fmt != nullptr) avformat_close_input(&fmt);
    }
};

bool open_ref_seek(const std::string& path, RefSeekCtx* out, std::string* err) {
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        *err = "avformat_alloc_context";
        return false;
    }
    // 跟 decode_reference()/demux_file() 同一份取值来源，两条路径探测
    // 深度不一致会让逐帧比对假红——demuxer.h 顶部专门警示过的坑。
    fmt->probesize            = syp::media::kDefaultProbeSize;
    fmt->max_analyze_duration = syp::media::kDefaultMaxAnalyzeDurationUs;

    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        *err = "open_input: " + av_err_str(rc);
        return false;
    }
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        *err = "find_stream_info: " + av_err_str(rc);
        avformat_close_input(&fmt);
        return false;
    }

    std::vector<RefSeekTrack> tracks(fmt->nb_streams);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (codec == nullptr) continue;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (ctx == nullptr) continue;
        if (avcodec_parameters_to_context(ctx, par) < 0) {
            avcodec_free_context(&ctx);
            continue;
        }
        ctx->pkt_timebase = fmt->streams[i]->time_base;
        // 跟 ffmpeg_video_decoder.cpp/ffmpeg_audio_decoder.cpp 同一条纪律：
        // 显式钉死单线程，参照路径本身要确定性可复现。
        ctx->thread_count = 1;
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            continue;
        }
        tracks[i].ctx       = ctx;
        tracks[i].time_base = fmt->streams[i]->time_base;
        tracks[i].is_video  = (par->codec_type == AVMEDIA_TYPE_VIDEO);
    }

    out->fmt    = fmt;
    out->tracks = std::move(tracks);
    return true;
}

// seek 到 ts_us（AV_TIME_BASE 即微秒）：跟 Demuxer::seek() 逐字相同的
// av_seek_frame 调用，再 flush 每条已开的解码器——对应 Pipeline::seek()
// 第 2/3 步（第 1/4 步在这里没有等价物：没有队列要清、没有 EOF 标志要
// 复位，raw FFmpeg 循环本身没有这些状态）。
bool ref_seek(RefSeekCtx* r, int64_t ts_us) {
    const int rc = av_seek_frame(r->fmt, -1, ts_us, AVSEEK_FLAG_BACKWARD);
    for (RefSeekTrack& t : r->tracks) {
        if (t.ctx != nullptr) avcodec_flush_buffers(t.ctx);
    }
    return rc >= 0;
}

// 从当前 fmt 读取位置继续解，跨所有轨累计吐出 want 帧就停，append 进
// *out。不在这里 flush——flush 只在 ref_seek() 里做一次，跟
// Pipeline::seek() 的"只在 seek 时 flush，解码过程中不 flush"对称。
bool ref_decode_n(RefSeekCtx* r, int64_t want, std::vector<FrameDigest>* out, std::string* err) {
    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) {
        *err = "av_packet_alloc";
        return false;
    }
    int64_t got = 0;
    bool    ok  = true;
    while (got < want) {
        const int rc = av_read_frame(r->fmt, pkt);
        if (rc < 0) break;   // EOF/错误：提前收工，调用方靠帧数校验发现

        const int32_t si = pkt->stream_index;
        if (si < 0 || static_cast<std::size_t>(si) >= r->tracks.size() ||
            r->tracks[static_cast<std::size_t>(si)].ctx == nullptr) {
            av_packet_unref(pkt);
            continue;
        }
        RefSeekTrack& t       = r->tracks[static_cast<std::size_t>(si)];
        const int     send_rc = avcodec_send_packet(t.ctx, pkt);
        av_packet_unref(pkt);
        if (send_rc != 0) {
            if (send_rc == AVERROR(EAGAIN) || send_rc == AVERROR_EOF) {
                *err = "avcodec_send_packet contract violation: " + av_err_str(send_rc);
                ok   = false;
                break;
            }
            continue;   // 单包可跳过，跟 run_decode() 同一条分级
        }

        while (got < want) {
            AVFrame* raw = av_frame_alloc();
            if (raw == nullptr) {
                *err = "av_frame_alloc";
                ok   = false;
                break;
            }
            const int rrc = avcodec_receive_frame(t.ctx, raw);
            if (rrc == 0) {
                Frame       f = Frame::from_av(raw, t.time_base, t.is_video);
                FrameDigest d{};
                std::string derr;
                if (!syp::probe::detail::digest_of_frame(f, si, &d, &derr)) {
                    *err = "digest_of_frame: " + derr;
                    ok   = false;
                    break;
                }
                out->push_back(std::move(d));
                ++got;
                continue;
            }
            av_frame_free(&raw);
            if (rrc == AVERROR(EAGAIN) || rrc == AVERROR_EOF) break;
            *err = "avcodec_receive_frame: " + av_err_str(rrc);
            ok   = false;
            break;
        }
        if (!ok) break;
    }
    av_packet_free(&pkt);
    return ok;
}

struct SeekStep {
    int64_t ts_us        = 0;
    int64_t frames_after = 0;
};

}  // namespace

// 场景 A：本地文件顺解到尾。diff_frames 空串；skipped_packets == 0。
TEST_CASE(a_local_file_sequential_to_eof) {
    const std::string path = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    DecodeOptions opt;
    DecodeResult  ref = decode_reference(path, opt);
    REQUIRE(ref.error_stage.empty());
    // 空转硬底线（此前在 packet 层踩过、又在帧层踩了一次的
    // 问题）：不能靠"参照也是空的"通过——先证明真的解出了东西，
    // 且给一个跟具体素材大小无关、宽松但非零的下限。
    REQUIRE(ref.frames.size() > 500);

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    DecodeResult got;
    {
        // 见文件顶部「看门狗」注释——这条路径上
        // 真实撞见过两次永久活锁，不再是形式主义的谨慎。
        syp::test::Watchdog wd("a_local_file_sequential_to_eof", /*soft_ms=*/60000,
                               /*hard_ms=*/180000,
                               "疑似卡在 Pipeline::step() 内部解码器——见 pipeline.h "
                               "顶部注释与 pipeline.cpp draining 那段（C-0 的教训）。");
        got = decode_pipeline(*p, opt);
    }

    const std::string diff = diff_frames(ref, got);
    CHECK_EQ(diff, std::string());
    // 「本场景素材未损坏、不该有坏包」——检出力由场景 F 兜底：F 反向自检
    // 已验证过把 Pipeline::skipped_packets() 焊死成恒返回 0 会让 F 变红
    // ，证明这条计数机制本身是被测过的活代码，这里
    // 断言 0 不是空转。
    CHECK_EQ(got.skipped_packets, int64_t{0});
    check_saw_video_and_multichannel_audio(got.frames);

    std::printf("  [A] ref_frames=%zu got_frames=%zu skipped=%lld\n", ref.frames.size(),
                got.frames.size(), static_cast<long long>(got.skipped_packets));
}

// 场景 B：经 dl 层顺解到尾。diff_frames 空串，且与 A 的结果互相全等——
// 后一条单独断言，不靠"两边各自跟参照相等"推传递性。
TEST_CASE(b_dl_layer_sequential_matches_a) {
    const std::string path = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    DecodeOptions opt;
    DecodeResult  ref = decode_reference(path, opt);
    REQUIRE(ref.error_stage.empty());
    REQUIRE(ref.frames.size() > 500);

    // A 侧：独立跑一遍（不复用场景 A 用例的结果——用例之间不该有隐藏依赖），
    // 拿到手用于跟 B 直接比较。
    syp_status file_err = SYP_OK;
    auto       file_p   = Pipeline::create_file(path, PipelineConfig{}, &file_err);
    REQUIRE(file_p != nullptr);
    DecodeResult a_side = decode_pipeline(*file_p, opt);
    REQUIRE(diff_frames(ref, a_side).empty());

    LoopbackConfig cfg;
    cfg.body_file = path;
    cfg.etag      = "\"fixture-v1\"";
    LoopbackServer srv(cfg);

    const TempCacheDir cache_guard("decode_b_dl_sequential");
    const SourceDecodeOutcome outcome =
        decode_via_source(srv.url("/media.mp4"), cache_guard.path, opt);

    // last_averror 一并打进诊断行——dl 层报的错此前只能通过
    // "帧数比参照少"这类间接后果猜，现在直接把 AvioBridge 记录到的
    // averror 打出来，diff_frames 一旦非空，看这一行就能先排除/坐实
    // "是不是 dl 层出的错"这一种可能性。
    std::printf("  [B] open_err=[%s] watchdog_fired=%d frames=%zu skipped=%lld "
                "last_averror=%d\n",
                outcome.open_err.c_str(), static_cast<int>(outcome.watchdog_fired),
                outcome.decode.frames.size(),
                static_cast<long long>(outcome.decode.skipped_packets),
                outcome.last_averror);

    REQUIRE(outcome.open_err.empty());
    REQUIRE(!outcome.watchdog_fired);

    const std::string diff_vs_ref = diff_frames(ref, outcome.decode);
    CHECK_EQ(diff_vs_ref, std::string());
    // 跟场景 A 同一条注记：检出力由场景 F 的反向自检兜底，见那边说明。
    CHECK_EQ(outcome.decode.skipped_packets, int64_t{0});

    // B 与 A 互相全等：独立断言，验的是"dl 层送的字节 = 本地文件的字节"
    // 在帧层面的再次确认（此前在 packet 层验过一次，这里换个高度再验一次）。
    const std::string diff_b_vs_a = diff_frames(a_side, outcome.decode);
    CHECK_EQ(diff_b_vs_a, std::string());

    check_saw_video_and_multichannel_audio(outcome.decode.frames);
}

// 线程模式（加载线程 demux）顺解到尾，逐轨与参照全等。跨轨交织顺序不比：线程模式
// 下包由加载线程提前堆进各轨队列，解码分支按轨号升序先把一条轨解空（pipeline.h 调度
// 顺序第 1 步），交织节奏本就与参照不同——比较口径同场景 E。
TEST_CASE(a2_demux_thread_sequential_matches_reference_per_track) {
    const std::string path = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    DecodeOptions opt;
    DecodeResult  ref = decode_reference(path, opt);
    REQUIRE(ref.error_stage.empty());
    REQUIRE(ref.frames.size() > 500);

    PipelineConfig cfg;
    cfg.demux_thread = true;
    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, cfg, &err);
    REQUIRE(p != nullptr);

    DecodeOptions topt;
    topt.blocked_waits = true;
    DecodeResult got;
    {
        syp::test::Watchdog wd("a2_demux_thread_sequential_matches_reference_per_track",
                               /*soft_ms=*/60000, /*hard_ms=*/180000,
                               "疑似泵线程拿不到终止标记，或停读水位之后加载线程没被消费唤醒");
        got = decode_pipeline(*p, topt);
    }
    REQUIRE(got.error_stage.empty());

    std::map<int32_t, std::vector<FrameDigest>> ref_by_track, got_by_track;
    for (const FrameDigest& d : ref.frames) ref_by_track[d.track_index].push_back(d);
    for (const FrameDigest& d : got.frames) got_by_track[d.track_index].push_back(d);
    REQUIRE(ref_by_track.size() >= 2);
    CHECK(ref_by_track == got_by_track);
    CHECK_EQ(got.skipped_packets, int64_t{0});
    std::printf("  [A2] ref_frames=%zu got_frames=%zu\n", ref.frames.size(), got.frames.size());
}

// 场景 C：多点 seek 后继续解。每次 seek 后 diff_frames 空串。
TEST_CASE(c_multi_seek_then_decode) {
    const std::string path = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    RefSeekCtx  ref;
    std::string open_err;
    REQUIRE(open_ref_seek(path, &ref, &open_err));

    syp_status perr = SYP_OK;
    auto       p    = Pipeline::create_file(path, PipelineConfig{}, &perr);
    REQUIRE(p != nullptr);

    // 覆盖开头/中段/尾段，含一次往回 seek——跟 test_probe_e2e.cpp 的
    // seek_plan() 同一种覆盖思路，往回 seek 是解码器内部状态复位最容易
    // 出错的地方。gen-fixtures.sh 产出的素材时长下限是 40 秒，25 秒这个
    // 点留有余量。
    const std::vector<SeekStep> plan = {
        {2000000, 80}, {25000000, 80}, {12000000, 80}, {1000000, 80},
    };

    // 见文件顶部「看门狗」注释。包住整个 seek+解码循环（而不是
    // 拆成 4 个短命的 Watchdog）——4 次 seek 共用同一份"疑似卡死"诊断，
    // 拆开只会让日志重复四遍、没有额外信息。
    syp::test::Watchdog wd("c_multi_seek_then_decode", /*soft_ms=*/60000, /*hard_ms=*/180000,
                           "疑似卡在 seek 之后的 Pipeline::step()——见 pipeline.cpp "
                           "draining 那段（C-0 的教训：flush 发出不等于排空）。");

    for (std::size_t i = 0; i < plan.size(); ++i) {
        const SeekStep& step = plan[i];

        REQUIRE(ref_seek(&ref, step.ts_us));
        std::vector<FrameDigest> ref_batch;
        std::string              ref_err;
        REQUIRE(ref_decode_n(&ref, step.frames_after, &ref_batch, &ref_err));
        // 空转硬底线：先确认参照这一轮真的解出了要求的帧数，不是提前碰上
        // 文件尾巴而悄悄少解——不然下面的比对可能是在比两段偶然一样短的
        // 空/退化序列。
        REQUIRE(ref_batch.size() == static_cast<std::size_t>(step.frames_after));

        REQUIRE(p->seek(step.ts_us) == SYP_OK);
        DecodeOptions batch_opt;
        batch_opt.max_frames = step.frames_after;
        DecodeResult got     = decode_pipeline(*p, batch_opt);

        DecodeResult expected;
        expected.frames = ref_batch;
        // expected（本地手搭的参照）的 skipped_packets 恒为默认
        // 值 0，而 got.skipped_packets 是 Pipeline 自创建以来的**累计**值
        // （pipeline.cpp::seek() 明确不清零这个计数）——一旦素材出现任何
        // 可跳过的坏包，diff_frames 会在"skipped_packets 不同"这条分支上
        // 报一条跟"这次 seek 解得对不对"完全无关的假红。场景 C 的主张是
        // "seek 之后解码器/demuxer 落点正确"，不是"累计坏包计数吻合"——
        // 那条由场景 A/B（顺解到尾，从零开始累计）覆盖。这里显式把
        // expected 的这一维对齐成 got 的当前值，等于告诉 diff_frames
        // "这一维不参与本场景的判定"。
        expected.skipped_packets = got.skipped_packets;

        const std::string diff = diff_frames(expected, got);
        std::printf("  [C] seek#%zu ts_us=%lld ref_frames=%zu got_frames=%zu diff_empty=%d\n", i,
                    static_cast<long long>(step.ts_us), ref_batch.size(), got.frames.size(),
                    static_cast<int>(diff.empty()));
        CHECK_EQ(diff, std::string());
        // 帧数量下限（每条场景都要断言）：这一轮真的解出了
        // 要求的帧数，不是"两边都是 0 帧所以碰巧相等"。
        CHECK(got.frames.size() == static_cast<std::size_t>(step.frames_after));
    }
    (void)wd.fired();
}

// 场景 D：音视频双轨交织。两轨各自全等；交织顺序与参照一致。
//
// 这是本文件唯一守卫「Pipeline::step() 轨道扫描顺序反转」这个
// 遗留漏测点的用例，见文件顶部长注释。diff_frames(ref, got) 逐位比较
// (track_index, pts_us, ...)，谁先吐帧的顺序错了，第一处不同立刻出现
// 在这里——这条断言本身就是"交织顺序与参照一致"的直接证据。
TEST_CASE(d_av_interleave_matches_reference) {
    const std::string path = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    DecodeOptions opt;
    DecodeResult  ref = decode_reference(path, opt);
    REQUIRE(ref.error_stage.empty());
    REQUIRE(ref.frames.size() > 500);

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    DecodeResult got;
    {
        // 见文件顶部「看门狗」注释。
        syp::test::Watchdog wd("d_av_interleave_matches_reference", /*soft_ms=*/60000,
                               /*hard_ms=*/180000,
                               "疑似卡在 Pipeline::step() 内部解码器——见 pipeline.h "
                               "顶部注释与 pipeline.cpp draining 那段（C-0 的教训）。");
        got = decode_pipeline(*p, opt);
    }

    const std::string diff = diff_frames(ref, got);
    CHECK_EQ(diff, std::string());

    // 按 track_index 拆开单独比一遍。
    //
    // 更正：这条**不是**跟上面 diff 逐位比对独立的判别力来源——
    // diff_frames 已经逐位比较过 (track_index, pts_us, ...) 整个元组，
    // 帧数相同 + 逐位相同 ⇒ 按 track_index 拆分后重新分组，结果必然还是
    // 相同（拆分是纯函数、输入相同输出必然相同）。上面 :585 的
    // `CHECK_EQ(diff, std::string())` 一旦通过，下面这条 CHECK 在数学上
    // 已经不可能失败——它不提供额外的判别力，真正的作用是**失败时的
    // 定位**：diff_frames 只报"第一处不同"，如果 diff 非空，这里按轨
    // 拆开能立刻看出是哪条轨（还是两条轨都乱了）出的问题，不需要在
    // diff_frames 的输出里手动数 track_index。原来的注释称它是"两条独立
    // 断言"，与事实不符，已更正。
    std::map<int32_t, std::vector<FrameDigest>> ref_by_track, got_by_track;
    for (const FrameDigest& d : ref.frames) ref_by_track[d.track_index].push_back(d);
    for (const FrameDigest& d : got.frames) got_by_track[d.track_index].push_back(d);
    // 判别力自检：真的有 >=2 条被管理轨参与交织，不是只测到一条轨的退化场景。
    REQUIRE(ref_by_track.size() >= 2);
    CHECK(ref_by_track == got_by_track);

    // 判别力自检：这份素材真的会在视频/音频之间来回切换（不是先吐完一条
    // 轨才开始下一条）——否则"交织顺序"这条断言测的是一个退化场景，
    // 谁先谁后根本不重要。
    int32_t last_track = -1;
    int32_t switches   = 0;
    for (const FrameDigest& d : ref.frames) {
        if (last_track != -1 && d.track_index != last_track) ++switches;
        last_track = d.track_index;
    }
    CHECK(switches > 10);

    std::printf("  [D] ref_frames=%zu got_frames=%zu tracks=%zu interleave_switches=%d\n",
                ref.frames.size(), got.frames.size(), ref_by_track.size(), switches);
}

// 场景 D 补充：轨道扫描顺序仲裁——遗留漏测点的真正
// 守卫。d_av_interleave_matches_reference 跟参照比对抓不住「轨道扫描
// 顺序反转」这个变异体：顺解到尾这条路径上「两条轨同时就绪」结构性
// 从不发生——demux 分支每次 step() 只读一个包（:346「return
// StepOutcome{DemuxedPacket,...}」）；解码分支永远先跑，且
// drive_decoder() 会把该轨包队列一次排干才返回（NoWork 分支要求包队列
// 已空）；decode_pipeline() 又是产出即排空（见该函数注释），FrameQueue
// 因此不会堆积——三者叠加：任一时刻至多一条轨的 PacketQueue 非空，
// 「两条轨同时满足 has_pending_input，谁先被扫到谁赢」这个仲裁点根本
// 不存在，diff_frames 自然测不出扫描方向。
//
// 这条不跟参照比——它压根不需要参照：pipeline.h 顶部把调度顺序写成了
// 契约的一部分（「1. 按轨号升序遍历各被管理轨」），直接断言这条契约
// 本身即可，零假红。做法是用背压人为制造仲裁点：只 step 不 pop，跑到
// Blocked（此时两条轨的 FrameQueue 都满、PacketQueue 都有货——两者
// 同时 ready），再各腾一个空位，此时两条轨同时满足 has_pending_input，
// 契约要求下一帧必须来自较小轨号（视频=0）。
//
// 诚实标注（必须留在这里）：这条钉的是 pipeline.h
// 文档化的、本身带任意性的仲裁顺序，不是从 FFmpeg 参照推出的正确性
// 性质。在 decode_pipeline() 的「产出即排空」驱动纪律下，降序扫描与
// 升序扫描在只跟参照比对时是可观测等价的（本文件其余三个场景测不出
// 区别）；只有引入背压之后两者才可区分，而背压场景下的全局交织顺序
// 契约本身并未规定「升序」与「降序」哪个在语义上更"对"——升序只是
// pipeline.h 选定并写死的实现契约。既然 pipeline.h 把它写成了契约，
// 这条断言就该存在，用来防止契约被静默破坏（哪怕两种实现在"最终解码
// 结果是否正确"这个意义上可能都不算错）。
TEST_CASE(d2_decode_branch_scans_tracks_in_ascending_order) {
    const std::string path = fixture("bframes.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    PipelineConfig cfg;
    cfg.max_frames_per_track  = 2;
    cfg.max_packets_per_track = 4;
    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, cfg, &err);
    REQUIRE(p != nullptr);

    int steps = 0;
    for (;;) {   // 只 step、不 pop，直到 Blocked——制造两轨同时满员的积压
        const StepOutcome o = p->step();
        if (o.kind == StepOutcome::Kind::Blocked) break;
        REQUIRE(o.kind == StepOutcome::Kind::DecodedFrame ||
                o.kind == StepOutcome::Kind::DemuxedPacket);
        REQUIRE(++steps < 10000);   // 空转防护：必须真的走到 Blocked
    }
    std::printf("  [D2] steps_to_blocked=%d\n", steps);

    REQUIRE(p->pop_frame(0).has_value());   // 视频腾一个空位
    REQUIRE(p->pop_frame(1).has_value());   // 音频腾一个空位

    const StepOutcome first = p->step();
    CHECK(first.kind == StepOutcome::Kind::DecodedFrame);
    CHECK_EQ(first.track_index, int32_t{0});   // 降序实现在这里必然给 1
    std::printf("  [D2] first.track_index=%d\n", first.track_index);
    (void)p->pop_frame(first.track_index);

    // 【修正】原来这里断言"再 step 一次就该是 track 1"——这条不
    // 是从升序仲裁契约推出来的，而是偶然依赖了这份素材在这一刻 track 0
    // 的 PacketQueue 恰好已经空了。跨 seed 实测证伪：换一颗 seed
    // （dur/fps/gop 参数不同），track 0 在这一步之后仍有不止一个待送包，
    // 契约允许（且要求）它连续拿到好几轮优先权——ascending 扫描每次都
    // 问"track 0 这一轮有事可做吗"，只要它的包队列没空，答案就是"有"，
    // 跟"这是不是刚给过它一次机会"无关。原断言在这类 seed 上会确定性
    // 变红，而且报错信息完全指不出"素材参数不同"这个真因（每个新 build
    // 目录的种子是 CMakeLists.txt 用 string(RANDOM) 现场抽的，干净 clone
    // 一次 configure 就有实打实的概率撞上）。
    //
    // 真正从契约推得出来的主张是"track 0 耗尽待送的包之后，下一帧必须
    // 来自次小的轨号"——这里把"耗尽"这个前提显式构造出来：不断把产出
    // 立刻让给 track 0（每次都腾出它刚满的位置），直到某一步的产出终于
    // 不是 track 0，那一步按契约必须是 track 1。track 0 待送的包数受
    // max_packets_per_track（=4）严格封顶，这个循环必然在有限步内结束——
    // demux 分支在 track 1 的包队列填满之前会持续给 track 0 补充新包，
    // 但一旦 track 1（我们全程没碰它）的包队列撞满 4 个，demux 分支的
    // "全体有空位才读"这条纪律（pipeline.cpp 里 all_have_room）会整体
    // 停摆，track 0 从此再等不到新包，才会真正耗尽——这就是循环保证
    // 终止的原因，不是"跑够多次总会撞上"这种碰运气。
    int32_t next_track = 0;
    int     guard       = 0;
    for (;;) {
        REQUIRE(++guard < 1000);   // 空转防护：见上方注释，理论上远用不到这么多
        const StepOutcome o = p->step();
        if (o.kind == StepOutcome::Kind::DemuxedPacket) continue;   // 偶尔夹杂的 demux 步，跳过
        REQUIRE(o.kind == StepOutcome::Kind::DecodedFrame);
        next_track = o.track_index;
        (void)p->pop_frame(next_track);
        if (next_track != 0) break;
    }
    CHECK_EQ(next_track, int32_t{1});
    std::printf("  [D2] track_0_exhausted_after=%d steps, next_track=%d\n", guard, next_track);
}

// =======================================================================
// 场景 E~H：背压与错误路径。详细设计取舍见文件顶部注释，
// 这里只放各场景专用的辅助代码。
// =======================================================================
namespace {

// ---------------------------------------------------------------------
// 场景 E 辅助：故意不在每次 DecodedFrame 后立刻 pop，让 FrameQueue 真的
// 堆到 capacity（PipelineConfig::max_frames_per_track 钉得很小），逼出
// 真实的 Blocked——只在 Blocked 时才把当前所有轨已经产出、还没取走的帧
// 一次性排空，然后继续 step()。
//
// 为什么最终比对按 track_index 分组、不整体跟 diff_frames(ref, ...) 比
// 全局交织顺序：跟 d2_ 用例上方长注释是同一个道理——一旦引入背压，两条
// 轨的 FrameQueue 可能同时堆到非空，Pipeline 按轨号升序的仲裁在那一刻
// 说了算，这是 pipeline.h 写死的实现契约，不是从 FFmpeg 参照推出的
// "正确"交织顺序，背压场景下两者不保证一致（不引入背压的 A~D 反而因为
// "产出即排空"从没触发过这个仲裁点，所以全局顺序天然跟参照一致）。E 要
// 验的主张是"帧不丢不重"，跟"全局交织顺序对不对"是两件不同的事——按轨
// 分组比对既不依赖那条不该依赖的顺序假设，又仍然是逐帧内容比对（含
// SHA-256），不是只数个数。
struct BlockedDrainResult {
    std::map<int32_t, std::vector<FrameDigest>> frames_by_track;
    int64_t     skipped_packets = 0;
    int64_t     blocked_count   = 0;
    std::string error_stage;   // 非空表示中途出了预期之外的问题
};

BlockedDrainResult decode_forcing_backpressure(Pipeline& p) {
    BlockedDrainResult out;

    auto drain_all_once = [&]() {
        for (int32_t ti = 0; ti < static_cast<int32_t>(p.tracks().size()); ++ti) {
            // 直接拿循环下标 ti 当 track_index 传给
            // pop_frame()/digest_of_frame()，依赖的假设是"Pipeline::
            // tracks() 的下标 == TrackInfo::index == StepOutcome::
            // track_index 同一套编号"。这是 demuxer.cpp::build_tracks()
            // 无条件按 0..nb_streams-1 顺序 push、且 t.index=i 带来的结构
            // 保证（不是凭空的假设），但此前只在注释里说过，没有被检查
            // 过——加一行成本为零的断言，把"结构保证"变成"被检查的结构
            // 保证"。
            REQUIRE(p.tracks()[static_cast<std::size_t>(ti)].index == ti);
            for (;;) {
                auto f = p.pop_frame(ti);
                if (!f.has_value()) break;
                FrameDigest d;
                std::string err;
                if (!syp::probe::detail::digest_of_frame(*f, ti, &d, &err)) {
                    out.error_stage = "digest_of_frame: " + err;
                    return;
                }
                out.frames_by_track[ti].push_back(std::move(d));
            }
        }
    };

    // 空转防护上限：跟 d2_ 同一条纪律，必须真的推进，不能靠一个足够大的
    // 数字掩盖潜在的死循环——一旦撞上限，报错而不是让用例静默超时。
    constexpr int64_t kStepCap = 2000000;
    for (int64_t steps = 0;; ++steps) {
        if (steps >= kStepCap) {
            out.error_stage = "decode_forcing_backpressure: 超过 " +
                std::to_string(kStepCap) + " 步仍未到 Eof——疑似死循环";
            return out;
        }
        const StepOutcome o = p.step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) continue;   // 故意不 pop
        if (o.kind == StepOutcome::Kind::DemuxedPacket) continue;
        if (o.kind == StepOutcome::Kind::Eof) break;
        if (o.kind == StepOutcome::Kind::Error) {
            out.error_stage = "非预期的 StepOutcome::Error, status=" +
                std::to_string(o.status);
            return out;
        }
        // Blocked：排空当前所有轨已经产出、还没取走的帧，解除积压再继续。
        ++out.blocked_count;
        drain_all_once();
        if (!out.error_stage.empty()) return out;
    }
    // Eof 的契约（pipeline.h/pipeline.cpp）本身要求每条被管理轨的
    // FrameQueue 在此刻已经空了，这里再排一次纯粹是防御性的——预期是
    // 空操作，不依赖它才能拿到完整结果。
    drain_all_once();
    if (!out.error_stage.empty()) return out;

    out.skipped_packets = p.skipped_packets();
    return out;
}

// ---------------------------------------------------------------------
// 场景 F 辅助：定位并损坏一个视频 packet。
//
// 跟 tests/test_packet_digest.cpp 的 find_mdat_range 逐字相同（那个是
// private 到那个 TU 的匿名 namespace，没有共享头可以直接复用）。
uint32_t f_read_be32(std::ifstream& f) {
    unsigned char b[4] = {};
    f.read(reinterpret_cast<char*>(b), 4);
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}
uint64_t f_read_be64(std::ifstream& f) {
    unsigned char b[8] = {};
    f.read(reinterpret_cast<char*>(b), 8);
    uint64_t v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return v;
}
std::optional<std::pair<uint64_t, uint64_t>> find_mdat_range(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(0, std::ios::end);
    const auto end_pos = f.tellg();
    if (end_pos < 0) return std::nullopt;
    const uint64_t file_size = static_cast<uint64_t>(end_pos);

    uint64_t pos = 0;
    while (pos + 8 <= file_size) {
        f.seekg(static_cast<std::streamoff>(pos));
        uint64_t box_size = f_read_be32(f);
        char     type[5]  = {};
        f.read(type, 4);
        uint64_t header_size = 8;
        if (box_size == 1) {
            box_size    = f_read_be64(f);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = file_size - pos;
        }
        if (box_size < header_size || pos + box_size > file_size) break;
        if (std::string(type) == "mdat") return std::make_pair(pos + header_size, pos + box_size);
        pos += box_size;
    }
    return std::nullopt;
}

bool copy_file_f(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return static_cast<bool>(out);
}

// 直接写入指定字节，不是 test_packet_digest.cpp 那种逐字节 XOR 翻转——
// 见文件顶部注释「F」那段：XOR 翻转在 H.264 上被验证过几乎总是被容错
// 成不同的像素，不产生真正的解码错误（实测记录）。
bool write_bytes_at(const std::string& path, uint64_t offset, const std::vector<uint8_t>& bytes) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return false;
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// 找第 want_index 个（0-based）视频 packet 在文件里的真实字节偏移
// （AVPacket::pos）。不经过本项目任何组件——纯 FFmpeg 原始 API，跟
// open_ref_seek() 系列一样只是"够用的最小参照实现"。跳过关键帧：损坏一
// 个 I 帧会连累它开的整个 GOP（实测记录），选一个
// 非关键帧才能让损坏效果局限在这一帧本身。
int64_t find_nth_non_key_video_packet_pos(const std::string& path, int64_t want_index) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return -1;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    int video_stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = static_cast<int>(i);
            break;
        }
    }
    int64_t   pos   = -1;
    int64_t   count = 0;
    AVPacket* pkt   = av_packet_alloc();
    while (video_stream >= 0 && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video_stream) {
            if (count >= want_index && (pkt->flags & AV_PKT_FLAG_KEY) == 0) {
                pos = pkt->pos;
                av_packet_unref(pkt);
                break;
            }
            ++count;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return pos;
}

}  // namespace

// 场景 E：极小队列逼真 Blocked，排空后继续，帧不丢不重（按轨分组、
// 含像素/采样 SHA-256 的逐帧比对，不是只数个数）——见文件顶部注释。
TEST_CASE(e_queue_full_triggers_blocked_and_drains_without_loss) {
    const std::string path = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    DecodeOptions opt;
    DecodeResult  ref = decode_reference(path, opt);
    REQUIRE(ref.error_stage.empty());
    REQUIRE(ref.frames.size() > 500);

    // max_frames_per_track=1：任何一条轨产出一帧不被立刻取走，这条轨
    // 立刻就"满"——比 d2_ 用的 2 还要紧，Blocked 应该会被频繁地压出来。
    PipelineConfig cfg;
    cfg.max_frames_per_track = 1;
    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, cfg, &err);
    REQUIRE(p != nullptr);

    BlockedDrainResult br;
    {
        // 全量解码 + 频繁 Blocked/排空循环，时间量级跟 A/D 类似，用同一套
        // 看门狗阈值。
        syp::test::Watchdog wd(
            "e_queue_full_triggers_blocked_and_drains_without_loss", /*soft_ms=*/60000,
            /*hard_ms=*/180000,
            "疑似卡在 Pipeline::step() 内部解码器，或本用例自己的排空逻辑有洞——"
            "见 decode_forcing_backpressure() 顶部注释。");
        br = decode_forcing_backpressure(*p);
    }

    REQUIRE(br.error_stage.empty());
    // 空转硬底线：Blocked 真的发生过，不是配置虽然极小但恰好一次都没撞上。
    REQUIRE(br.blocked_count > 0);

    std::map<int32_t, std::vector<FrameDigest>> ref_by_track;
    for (const FrameDigest& d : ref.frames) ref_by_track[d.track_index].push_back(d);

    std::printf("  [E] blocked_count=%lld ref_frames=%zu tracks_in_ref=%zu\n",
                static_cast<long long>(br.blocked_count), ref.frames.size(), ref_by_track.size());
    for (const auto& kv : br.frames_by_track) {
        std::printf("  [E] track=%d got_frames=%zu\n", kv.first, kv.second.size());
    }

    // 主断言：逐轨、逐帧（含内容 SHA-256）全等——不丢、不重、不错序（轨
    // 内顺序天然不受背压影响，见文件顶部注释；跨轨全局顺序不在这条断言
    // 范围内，理由同上）。
    CHECK(ref_by_track == br.frames_by_track);
    CHECK_EQ(br.skipped_packets, int64_t{0});   // 本场景素材未损坏

    // ===================================================================
    // 【曾经的盲区】上面这一段走的是**测试自己
    // 写的** decode_forcing_backpressure()/drain_all_once，只证明了
    // Pipeline::step() 本身的 Blocked 分支被真实走到过。库里
    // tools/syp_probe/frame_digest.cpp::decode_pipeline() 自己那条排空
    // 逻辑（drain_all_tracks，Blocked 分支专用）是完全独立的第二份代码，
    // 从未被这条用例（或本文件任何其它场景）验证过——实测：把
    // drain_all_tracks 整体短路成 `return true`，全量 ctest 16/16 依然
    // 全绿，插桩计数确认它在全部既有用例下命中 0 次。根因跟 d2_ 用例上方
    // 长注释是同一件事：decode_pipeline() 默认"产出即排空"（每次
    // DecodedFrame 都立刻 pop），FrameQueue 从不堆积，Blocked 分支的
    // if 判断这部分代码永远走不进去——A~D、以及本场景上半段用的都是这个
    // 默认驱动纪律（上半段绕开它是因为用了测试自己写的驱动循环，根本没
    // 调 decode_pipeline()）。
    //
    // 修法：DecodeOptions::lazy_pop（frame_digest.h，仅测试用，默认关）
    // ——为真时 decode_pipeline() 的 DecodedFrame 分支也不立刻 pop，逼着
    // 它自己走到 Blocked/drain_all_tracks。这里用同一份极小 cfg 跑第二个
    // 独立的 Pipeline 实例（不能复用上面已经跑到 Eof 的 p——Pipeline 没有
    // "倒带"接口），走库里的 decode_pipeline(lazy_opt) 而不是测试本地的
    // decode_forcing_backpressure()，判据仍然是按轨分组跟 ref_by_track
    // 比（理由同上：lazy_pop 下 drain_all_tracks 按轨号升序整条排空，
    // out.frames 的全局顺序不代表真实交织顺序，不能拿去跟 decode_
    // reference() 的输出做 diff_frames() 那种逐位比较）。
    syp_status err2 = SYP_OK;
    auto       p2   = Pipeline::create_file(path, cfg, &err2);
    REQUIRE(p2 != nullptr);

    DecodeOptions lazy_opt;
    lazy_opt.lazy_pop = true;
    DecodeResult lib_got;
    {
        syp::test::Watchdog wd2(
            "e_queue_full_triggers_blocked_and_drains_without_loss(lib_path)",
            /*soft_ms=*/60000, /*hard_ms=*/180000,
            "疑似卡在库里 decode_pipeline() 的 Blocked/drain_all_tracks 路径——"
            "见 frame_digest.h::DecodeOptions::lazy_pop 顶部注释。");
        lib_got = decode_pipeline(*p2, lazy_opt);
    }
    // error_stage 非空这里就是失败信号本身：可能是那条 track index
    // 映射断言不成立，也可能是 drain_all_tracks 内部真的出了别的问题——
    // 不管哪种，都不该被这条用例吞掉。
    REQUIRE(lib_got.error_stage.empty());

    std::map<int32_t, std::vector<FrameDigest>> lib_got_by_track;
    for (const FrameDigest& d : lib_got.frames) lib_got_by_track[d.track_index].push_back(d);

    std::printf("  [E] (lib decode_pipeline lazy_pop) got_frames=%zu tracks=%zu "
                "blocked_drain_rounds=%lld\n",
                lib_got.frames.size(), lib_got_by_track.size(),
                static_cast<long long>(lib_got.blocked_drain_rounds));
    for (const auto& kv : lib_got_by_track) {
        std::printf("  [E] (lib) track=%d got_frames=%zu\n", kv.first, kv.second.size());
    }

    CHECK(ref_by_track == lib_got_by_track);
    CHECK_EQ(lib_got.skipped_packets, int64_t{0});
    // 【变异体 2】ref_by_track == lib_got_by_track 这条
    // 对"是否真的走了 Blocked/drain_all_tracks"完全不敏感——把 lazy_pop
    // 为真时的行为悄悄改回立即 pop（开关形同虚设），这条照样全绿，因为
    // 两条驱动方式在"最终解出了哪些帧"这个层面本来就应该一样。
    // blocked_drain_rounds（DecodeResult 里专门为这件事加的字段，见它
    // 顶部注释）才是唯一守着"库路径这次调用是否真的经历过 Blocked 排空"
    // 这件事本身的可观测量。
    //
    // 不只断言 "> 0"（能精确就精确）：两个 Pipeline
    // 实例（p 与 p2）用同一份文件、同一份极小 cfg，且驱动它们的算法
    // 结构完全相同（"DecodedFrame 不立刻 pop、只在 Blocked 时才排空一
    // 整轮"）——Pipeline::step() 是纯同步、确定性的状态机，同样的输入
    // 加同样的驱动方式，Blocked 触发的次数必然逐位相等，不是"碰巧数值
    // 接近"。实测（54181493/11111111/99887766 三个 seed）都精确相等。
    CHECK_EQ(lib_got.blocked_drain_rounds, br.blocked_count);
}

// 场景 F：损坏包被跳过且计数，其余帧仍与参照全等。
TEST_CASE(f_corrupted_packet_is_skipped_and_counted) {
    const std::string src = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(src));   // 运行期守护，见文件顶部注释

    // 先数一下这份素材总共多少个视频 packet，挑中段一个非关键帧的
    // packet 下手——中段既远离首帧（首帧被打烂会连累整个第一个 GOP，
    // 实测记录），也远离尾部（避免恰好落在最后一个
    // GOP、样本量太小不足以证明"其余帧仍全等"）。
    const int64_t rough_mid = 200;   // 素材时长 40~100s、fps 24~30，
                                      // 200 个视频包足够落在中段附近。
    const int64_t pos = find_nth_non_key_video_packet_pos(src, rough_mid);
    REQUIRE(pos > 0);

    const auto mdat = find_mdat_range(src);
    REQUIRE(mdat.has_value());
    // 交叉验证：这个偏移真的落在 mdat 数据区里，不是瞎editing moov/元数据。
    REQUIRE(static_cast<uint64_t>(pos) >= mdat->first);
    REQUIRE(static_cast<uint64_t>(pos) < mdat->second);

    const std::string corrupt_path = fixture("bframes_faststart_corrupt_test.mp4");
    // RAII 兜底删除：跟 test_packet_digest.cpp::diff_report_catches_a_
    // corrupted_payload_byte 同一套，任何一条 REQUIRE 提前 return 都不该
    // 在共享的 fixture 目录里留一份孤儿副本。
    struct TempFileRemover {
        std::string path;
        ~TempFileRemover() { if (!path.empty()) std::remove(path.c_str()); }
    } temp_file{corrupt_path};

    REQUIRE(copy_file_f(src, corrupt_path));
    // AVCC 格式下，这个 packet 数据的开头 4 字节是它第一个 NAL 的大端
    // 长度前缀。改成一个远超实际剩余字节数的越界值——实测这会让 h264
    // parser 在 NAL 切分阶段直接判定整包不可解析
    // （"Invalid NAL unit size" / "Error splitting the input into NAL
    // units"），avcodec_send_packet 级别拒绝整包，而不是被"容错解码"
    // 悄悄吸收成不同的像素。
    REQUIRE(write_bytes_at(corrupt_path, static_cast<uint64_t>(pos), {0x7F, 0xFF, 0xFF, 0xFF}));

    // 判据是"Pipeline 在这份损坏文件上的表现 == FFmpeg 参照在同一份损坏
    // 文件上的表现"（跟 A~D 同一个口径，只是两边喂的都是损坏后的文件，
    // 不是跟"原始未损坏文件"比——那不是这条场景要验的东西：一旦文件真的
    // 损坏，"参照路径自己在这份文件上会解出什么"才是唯一站得住的真值）。
    DecodeOptions opt;
    DecodeResult  ref = decode_reference(corrupt_path, opt);
    REQUIRE(ref.error_stage.empty());
    REQUIRE(ref.frames.size() > 500);   // 空转硬底线
    // 精确 1，不是弱化成 "> 0"：本用例只损坏了一个非
    // 关键帧包的 4 字节 NAL 长度前缀，结构上必然且只能记一次坏包——
    // "> 0" 抓不住"某天计数器变成一个坏包记 2 次"或"损坏级联到了后继
    // 包"这类回归（两条路径同源退化，got==ref 也测不出来）。三个 seed
    // 同 seed 连跑均恒为 1，不是偶然。
    // 如果这个数字以后变了，那本身就是信息——先怀疑是不是级联，而不是
    // 顺手把断言放宽回去。
    REQUIRE(ref.skipped_packets == int64_t{1});

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(corrupt_path, PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    DecodeResult got;
    {
        syp::test::Watchdog wd("f_corrupted_packet_is_skipped_and_counted", /*soft_ms=*/60000,
                               /*hard_ms=*/180000,
                               "疑似卡在 Pipeline::step() 内部解码器——见 pipeline.h "
                               "顶部注释与 pipeline.cpp draining 那段（C-0 的教训）。");
        got = decode_pipeline(*p, opt);
    }

    std::printf("  [F] corrupt_pos=%lld ref_frames=%zu ref_skipped=%lld got_frames=%zu "
                "got_skipped=%lld\n",
                static_cast<long long>(pos), ref.frames.size(),
                static_cast<long long>(ref.skipped_packets), got.frames.size(),
                static_cast<long long>(got.skipped_packets));

    const std::string diff = diff_frames(ref, got);
    CHECK_EQ(diff, std::string());
    // 显式再断一遍（diff_frames 空串已经隐含这条，这里是为了让"skipped_
    // packets 被精确计数"在断言列表里独立可见，不必去读 diff_frames 的
    // 实现才知道它覆盖了这一维）。精确 1，理由见上面 ref.skipped_packets
    // 那条注释——同一份构造，同一条纪律。
    CHECK_EQ(got.skipped_packets, int64_t{1});
    CHECK_EQ(got.skipped_packets, ref.skipped_packets);
}

// 场景 G：一条轨的解码器打不开，该轨终止、其它轨继续，track_failed()
// 查得到。
TEST_CASE(g_decoder_init_failure_track_fails_others_continue) {
    // 本项目自己的 build-ffmpeg.sh 只 --enable-decoder=h264/aac（见文件
    // 顶部注释）——用系统 ffmpeg CLI（本仓库测试套件本身的硬依赖，
    // CMakeLists.txt 用 find_program(SYP_FFMPEG_CLI ffmpeg) 探测，找不到
    // 就整个跳过 decode_e2e 等测试目标）现场生成一份"音频轨编码成 flac"
    // 的小素材：flac 不在我们自己 SYFFmpeg.xcframework 的解码器清单里，
    // avcodec_find_decoder() 对它必然返回 nullptr。不进 gen-fixtures.sh/
    // 不进仓库——只有这一条用例用得到，现场生成、用完即删。
    //
    // 不调 PATH 里裸的 "ffmpeg"——configure 期
    // find_program 探测到的 ffmpeg 与运行期 ctest 进程实际的 PATH 不
    // 一定是同一个（沙箱、CI runner、ctest 被不同 shell 拉起都可能让两者
    // 失配），一旦失配，本用例的失败信息只会是"命令返回非 0"，指不出
    // "PATH 里没有 ffmpeg"这个真因。改用 tests/CMakeLists.txt 编译期烤
    // 进来的绝对路径 SYP_FFMPEG_CLI_PATH（跟 configure 期 gen-fixtures.sh
    // 用的是同一个 find_program 结果，两处不会漂移）。
    const std::string ffmpeg_cli = SYP_FFMPEG_CLI_PATH;
    if (std::system(("\"" + ffmpeg_cli + "\" -version > /dev/null 2>&1").c_str()) != 0) {
        tiny_test::fail(__FILE__, __LINE__,
                        ("ffmpeg CLI 不可用（路径: " + ffmpeg_cli +
                         "）——本用例需要现场生成素材，见用例顶部注释")
                            .c_str());
        return;
    }

    const TempCacheDir tmp("decode_g_unsupported_audio");
    const std::string  path = tmp.path + "/g_unsupported_audio.mp4";
    const std::string  cmd =
        "\"" + ffmpeg_cli + "\" -hide_banner -loglevel error -y "
        "-f lavfi -i testsrc2=size=320x240:rate=25:duration=3 "
        "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=3 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p "
        "-c:a flac "
        "\"" + path + "\" > /dev/null 2>&1";
    REQUIRE(std::system(cmd.c_str()) == 0);

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, PipelineConfig{}, &err);
    // 整体没失败——只有音频轨终止，不是整体终止（pipeline.h 顶部注释：
    // "单条轨的解码器初始化失败不会让这里返回 nullptr"）。
    REQUIRE(p != nullptr);

    const auto& tracks = p->tracks();
    REQUIRE(tracks.size() == 2);
    int32_t video_idx = -1, audio_idx = -1;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].is_video) video_idx = static_cast<int32_t>(i);
        else                    audio_idx = static_cast<int32_t>(i);
    }
    REQUIRE(video_idx >= 0);
    REQUIRE(audio_idx >= 0);

    CHECK(p->track_failed(audio_idx));    // 该轨终止：track_failed() 查得到
    CHECK(!p->track_failed(video_idx));   // 其它轨没有被连累

    // track_failed() 说对了不足以证明"其它轨继续"——pipeline.h 顶部
    // 警示过失效对调用方是静默的，必须真的驱动一遍确认存活轨还在出帧、
    // 失效轨没有帧（它的包在 demux 阶段被直接丢弃，见 pipeline.cpp）。
    DecodeOptions opt;
    DecodeResult  got;
    {
        syp::test::Watchdog wd("g_decoder_init_failure_track_fails_others_continue",
                               /*soft_ms=*/30000, /*hard_ms=*/90000,
                               "疑似卡在 Pipeline::step()——本素材只有 3 秒，不该跑这么久。");
        got = decode_pipeline(*p, opt);
    }
    REQUIRE(got.error_stage.empty());   // 该轨终止不等于整体终止

    int64_t video_frames = 0, audio_frames = 0;
    for (const FrameDigest& d : got.frames) {
        if (d.track_index == video_idx) ++video_frames;
        if (d.track_index == audio_idx) ++audio_frames;
    }
    std::printf("  [G] video_idx=%d audio_idx=%d video_frames=%lld audio_frames=%lld "
                "video_failed=%d audio_failed=%d\n",
                video_idx, audio_idx, static_cast<long long>(video_frames),
                static_cast<long long>(audio_frames), static_cast<int>(p->track_failed(video_idx)),
                static_cast<int>(p->track_failed(audio_idx)));

    CHECK(video_frames > 0);              // 存活轨真的还在出帧
    CHECK_EQ(audio_frames, int64_t{0});   // 失效轨的包被直接丢弃，没有帧
}

// 场景 H：dl 层报错冒上来，step() 报 Error 且不再推进。
TEST_CASE(h_dl_layer_error_propagates_and_step_stops_advancing) {
    const std::string path = fixture("bframes_faststart.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    REQUIRE(ensure_apple_backend());

    LoopbackConfig cfg;
    cfg.body_file = path;
    cfg.etag      = "\"fixture-v1\"";
    LoopbackServer srv(cfg);

    syp_config scfg;
    syp_config_init(&scfg);
    scfg.struct_size = sizeof(syp_config);
    const TempCacheDir cache_guard("decode_h_dl_error");
    scfg.cache_dir = cache_guard.path.c_str();

    syp_source* src = nullptr;
    const syp_status open_st =
        syp_source_open(&src, srv.url("/media.mp4").c_str(), nullptr, &scfg, nullptr);
    REQUIRE(open_st == SYP_OK);
    REQUIRE(src != nullptr);

    auto bridge = AvioBridge::create(src, 64 * 1024);
    REQUIRE(static_cast<bool>(bridge));

    syp_status perr = SYP_OK;
    auto       p    = Pipeline::create_avio(bridge->ctx(), PipelineConfig{}, &perr);
    REQUIRE(p != nullptr);

    int64_t     frames_before_error = 0;
    bool        switched_to_fatal   = false;
    bool        hit_error           = false;
    StepOutcome error_outcome;
    {
        // 正常下载 + 一次配置切换 + 等错误冒上来，量级跟其它场景类似。
        syp::test::Watchdog wd(
            "h_dl_layer_error_propagates_and_step_stops_advancing", /*soft_ms=*/60000,
            /*hard_ms=*/180000,
            "疑似卡在 SourceBridge::read() 的 cv_.wait——可能是 fatal_ 没有被真正置位"
            "（set_fatal_locked 没被调用/没 notify），或者 LoopbackServer 切换配置没生效。");

        constexpr int64_t kStepCap = 2000000;
        for (int64_t steps = 0; steps < kStepCap; ++steps) {
            const StepOutcome o = p->step();
            if (o.kind == StepOutcome::Kind::DecodedFrame) {
                ++frames_before_error;
                (void)p->pop_frame(o.track_index);
                // 先解出一批真正的帧（证明"中途失败"而不是"一开始就没
                // 打开"），再把服务端切到必然 fatal 的配置——404 落在
                // dl_task.cpp::classify_http_status 里"除 408/429/5xx
                // （Retryable）与 416（Unsatisfiable416）之外"整类都判
                // Fatal 的那一类（404 只是取其中最典型的代表），命中后
                // SourceBridge::fatal_ 一次置位、永久粘滞
                // （source_bridge.cpp:490/852）。
                if (!switched_to_fatal && frames_before_error >= 20) {
                    LoopbackConfig fatal_cfg = cfg;
                    fatal_cfg.status_code    = 404;
                    srv.set_config(fatal_cfg);
                    switched_to_fatal = true;
                }
                continue;
            }
            if (o.kind == StepOutcome::Kind::DemuxedPacket) continue;
            if (o.kind == StepOutcome::Kind::Blocked) continue;
            if (o.kind == StepOutcome::Kind::Eof) {
                // 不该走到这——说明整份文件在切到 fatal 之前就已经被
                // dl 层的预取窗口悄悄下完了，构造没达到目的。
                //
                // 真正的 flake 模式不是 frames_before_error
                // 本身的抖动（那只是切换那一刻已经飞行在途/已缓存的字节
                // 决定的，正常现象）——是"素材太小或预取太快，整份文件在
                // 切到 404 之前就被下完"，这条分支此前只有注释、一个不带
                // 任何痕迹的 break，将来真的撞上这条时,日志除了下面这条
                // REQUIRE(hit_error) 失败什么都看不出来。这里补一行诊断,
                // 让日志直接指到真因，不用现场重新推理。
                std::printf("  [H] 提前 Eof：frames=%lld switched=%d —— "
                            "素材太小/预取太快，构造失效\n",
                            static_cast<long long>(frames_before_error),
                            static_cast<int>(switched_to_fatal));
                break;
            }
            if (o.kind == StepOutcome::Kind::Error) {
                hit_error     = true;
                error_outcome = o;
                break;
            }
        }
    }

    std::printf("  [H] frames_before_error=%lld switched_to_fatal=%d hit_error=%d "
                "error_status=%d\n",
                static_cast<long long>(frames_before_error), static_cast<int>(switched_to_fatal),
                static_cast<int>(hit_error), static_cast<int>(error_outcome.status));

    REQUIRE(switched_to_fatal);            // 构造本身生效了：确实切到了 fatal 配置
    // 这条被上面 switched_to_fatal 蕴含——切换只在
    // frames_before_error >= 20 时才可能发生（见下面循环体里的判断），
    // 所以 switched_to_fatal 为真已经保证了这条，不是独立的检出力。留着
    // 是为了在断言列表里显式重申"切换点"这个语义（"确实解出过帧,不是
    // 一开始就失败"这条主张真正靠的是 switched_to_fatal），不是因为它
    // 自己能测出新东西。
    REQUIRE(frames_before_error >= 20);
    REQUIRE(hit_error);                    // 错误真的冒上来了，不是被吸收成 Eof
    CHECK(error_outcome.kind == StepOutcome::Kind::Error);

    // "不再推进"要真的验：拿到 Error 之后再调几次 step()，必须每次都还是
    // 同一个 Error，不会自己恢复、也不会推进出新的帧/包。
    for (int i = 0; i < 5; ++i) {
        const StepOutcome again = p->step();
        CHECK(again.kind == StepOutcome::Kind::Error);
        CHECK_EQ(again.status, error_outcome.status);
    }

    p.reset();
    bridge.reset();
    syp_source_close(src);
}

int main() { return tiny_test_main(); }
