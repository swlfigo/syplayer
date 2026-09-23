// syp_probe — headless 媒体源探查工具。
//
// 两套模式：
//   1. packet 层（默认）：不解码不渲染，开流、读包、seek、再读，把流信息、
//      packet 统计与 dl 层的下载统计打出来。
//   2. `--decode`：走到帧这一层，判据同
//      tools/syp_probe/frame_digest.h——`--diff` 做机器判定的逐帧比对，
//      `--stats` 只出统计（含 pix_fmt/sample_fmt）。
// 都能直接打真网址，用来排查线上资源。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "frame_digest.h"
#include "media/avio_bridge.h"
#include "media/pipeline.h"
#include "scenarios.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_source.h>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

using namespace syp::probe;
using syp::media::AvioBridge;
using syp::media::Pipeline;
using syp::media::PipelineConfig;
using syp::media::TrackInfo;

namespace {

// 退出码约定（--help 里原样列一份，两处必须保持同步）：
//   0  成功——没做 --diff 就是"跑通了"；做了 --diff 就是"全等"
//   1  --diff 判定不等（机器判定：两条路径都跑通了，内容不同）
//   2  用法错误（参数不对）
//   3  工具自身跑不下去（打开失败、URL 不通、参照路径打不开、解码器
//      初始化失败……——跟"比对不等"是两件事，脚本必须能分辨）
//   70 看门狗硬超时强制退出（--decode 路径没有可打断的中止点，见下面
//      CliWatchdog 的注释；跟 tests/support/watchdog.h 用的硬超时码一致）
constexpr int kExitOk              = 0;
constexpr int kExitDiffMismatch    = 1;
constexpr int kExitUsage           = 2;
constexpr int kExitToolFailure     = 3;
constexpr int kExitWatchdogHardExit = 70;

void usage() {
    std::fprintf(stderr,
        "用法:\n"
        "  syp_probe [选项] <url>                        packet 层探查（默认）\n"
        "  syp_probe --decode --diff <file> <url>        帧层差分比对，机器判定\n"
        "  syp_probe --decode --stats <url>               帧层统计（含 pix_fmt/sample_fmt）\n"
        "\n"
        "选项:\n"
        "  --cache-dir DIR   缓存目录（默认建临时目录，退出时删）\n"
        "  --seek MS         seek 到该毫秒并再读 60 个包；可重复多次（仅 packet 层，"
                            "与 --decode 不兼容）\n"
        "  --timeout MS      看门狗超时，默认 120000。packet 层用于打断 FFmpeg 的\n"
        "                    interrupt_callback 轮询点；--decode 下没有可打断的中止点\n"
        "                    （Pipeline::create_avio 没有接 interrupt_callback），只能\n"
        "                    在硬超时时整个进程 _exit(70)\n"
        "  --diff FILE       与本地 FILE 的结果做比对：不带 --decode 是逐 packet\n"
        "                    （走 file: 协议），带 --decode 是逐帧（直接用\n"
        "                    libavformat+libavcodec 解码，不经本项目任何组件）\n"
        "  --decode          切到帧层判据（frame_digest.h），须搭配 --diff 或 --stats\n"
        "  --stats           只出帧层统计，不比对；须搭配 --decode\n"
        "\n"
        "退出码:\n"
        "  0   成功（--diff 下表示全等）\n"
        "  1   --diff 判定不等（两条路径都跑通了，内容不同）\n"
        "  2   用法错误\n"
        "  3   工具自身跑不下去（打开/连接/参照文件失败，跟"
                "「比对不等」是两件事）\n"
        "  70  看门狗硬超时强制退出\n");
}

// ---------------------------------------------------------------------
// --decode 路径专用的看门狗。
//
// packet 层（scenarios.cpp::WatchdogGuard）到点只置一个 abort 标志，靠
// AvioBridge::request_abort() 打断 FFmpeg 会轮询 interrupt_callback 的
// 阻塞点——那套机制要求 AVFormatContext::interrupt_callback 真的接上了
// bridge->interrupt_cb()。demuxer.cpp/pipeline.cpp 通读一遍：
// Pipeline::create_avio() 当时没有接这根线。后来已修：create_avio() 现在
// 接受 io_abort 钩子、request_abort() 在三条
// 打开路径上都生效。本工具的 decode_pipeline()（syp_probe_core）还没有
// 把这个口子接出来，所以下面这套硬退出仍是 --decode 唯一的兜底——接上
// 它是本工具自己的后续工作，不是 Pipeline 缺口。decode_pipeline() 一旦卡在内部解码器或 dl 层的同步读，
// 唯一兜得住的只有硬退出——跟 tests/support/watchdog.h 同一套两级设计
// （软超时打诊断、硬超时 _exit），硬退出码沿用它的 70。tools/ 不链
// tests/ 下的头，这里本地一份而不是新增共享头（跟
// test_decode_e2e.cpp/test_pipeline.cpp 里 material_has_b_frames 各自
// 本地一份同一个理由：为一个几十行的类新增共享头不值得）。
class CliWatchdog {
public:
    CliWatchdog(const char* name, int soft_ms, int hard_ms)
        : name_(name), soft_ms_(soft_ms), hard_ms_(hard_ms), th_([this] { run(); }) {}

    ~CliWatchdog() {
        {
            std::lock_guard<std::mutex> g(mu_);
            done_ = true;
        }
        cv_.notify_all();
        th_.join();
    }

    CliWatchdog(const CliWatchdog&)            = delete;
    CliWatchdog& operator=(const CliWatchdog&) = delete;

    bool fired() const { return fired_.load(std::memory_order_acquire); }

private:
    void run() {
        std::unique_lock<std::mutex> lk(mu_);
        if (cv_.wait_for(lk, std::chrono::milliseconds(soft_ms_), [this] { return done_; })) {
            return;
        }
        fired_.store(true, std::memory_order_release);
        std::fprintf(stderr,
            "WATCHDOG %s: 卡住超过 %d ms——--decode 路径没有可打断的中止点，"
            "只能等硬超时。\n", name_, soft_ms_);
        std::fflush(stdout);
        std::fflush(stderr);
        if (cv_.wait_for(lk, std::chrono::milliseconds(hard_ms_ - soft_ms_),
                         [this] { return done_; })) {
            return;
        }
        std::fprintf(stderr, "WATCHDOG %s: 硬超时 %d ms，强制退出（exit=%d）。\n",
                     name_, hard_ms_, kExitWatchdogHardExit);
        std::fflush(stdout);
        std::fflush(stderr);
        _exit(kExitWatchdogHardExit);
    }

    const char*             name_;
    int                     soft_ms_;
    int                     hard_ms_;
    bool                    done_ = false;
    std::atomic<bool>       fired_{false};
    std::mutex              mu_;
    std::condition_variable cv_;
    std::thread             th_;
};

// 经 dl 层解码。跟 tests/test_decode_e2e.cpp 场景 B
// （decode_via_source）同一条搭法：syp_source → AvioBridge →
// Pipeline::create_avio；这里另起一份而不是把测试那份挪出来共享，
// 理由跟 CliWatchdog 一样——tools/ 不链 tests/。
struct SourceDecodeOutcome {
    DecodeResult          decode;
    std::vector<TrackInfo> tracks;    // 供 --stats 报 codec/is_video；
                                       // pipeline 在函数返回前就析构了，
                                       // 必须在那之前拷出来
    std::string           open_err;   // 非空 = open/bridge/pipeline 建立失败
    bool                  watchdog_fired = false;
};

SourceDecodeOutcome decode_via_source(const std::string& url, const std::string& cache_dir,
                                       const DecodeOptions& opt, int watchdog_ms) {
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
        const int soft_ms = watchdog_ms / 2 > 0 ? watchdog_ms / 2 : 1;
        CliWatchdog wd("decode_via_source", soft_ms, watchdog_ms);
        // DecodeOptions::lazy_pop 恒为其默认值（false）——这是「工具要尽快
        // 解完、给出判定」这条要求本身，不是漏配：lazy_pop=true 只是测试
        // 专用的开关（frame_digest.h 顶部注释），逼 decode_pipeline() 走
        // Blocked/drain_all_tracks 那条排空路径；在这个配置下每个
        // DecodedFrame 一产出就立刻 pop_frame()，FrameQueue 结构性堆不到
        // capacity，Blocked 不可能发生（已证明），syp_probe
        // --decode 走的正是这条路径。
        out.decode        = decode_pipeline(*pipeline, opt);
        out.watchdog_fired = wd.fired();
    }

    out.tracks = pipeline->tracks();

    pipeline.reset();
    bridge.reset();
    syp_source_close(src);
    return out;
}

std::string pix_fmt_name(int32_t fmt) {
    const char* n = av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt));
    return n != nullptr ? n : ("?(" + std::to_string(fmt) + ")");
}

std::string sample_fmt_name(int32_t fmt) {
    const char* n = av_get_sample_fmt_name(static_cast<AVSampleFormat>(fmt));
    return n != nullptr ? n : ("?(" + std::to_string(fmt) + ")");
}

// 按 track_index 分组，取该轨第一帧的 fmt/尺寸/采样参数——同一条轨每帧
// 的这些字段本就不该变化（分辨率变化不在本工具考虑范围内）。
void print_decode_stats(const std::vector<TrackInfo>& tracks, const DecodeResult& r) {
    std::map<int32_t, const FrameDigest*> first_by_track;
    std::map<int32_t, int64_t>            count_by_track;
    for (const FrameDigest& d : r.frames) {
        ++count_by_track[d.track_index];
        if (first_by_track.find(d.track_index) == first_by_track.end()) {
            first_by_track[d.track_index] = &d;
        }
    }

    std::printf("tracks: %zu\n", tracks.size());
    for (const TrackInfo& t : tracks) {
        const int64_t frames = count_by_track.count(t.index) ? count_by_track[t.index] : 0;
        std::printf("  track #%d %-5s frames=%lld", t.index, t.is_video ? "video" : "audio",
                    static_cast<long long>(frames));
        const auto it = first_by_track.find(t.index);
        if (it == first_by_track.end()) {
            std::printf("  (未解出任何帧，pix_fmt/sample_fmt 未知)\n");
            continue;
        }
        const FrameDigest& d = *it->second;
        if (t.is_video) {
            std::printf("  pix_fmt=%s  size=%dx%d\n", pix_fmt_name(d.fmt).c_str(), d.width,
                        d.height);
        } else {
            std::printf("  sample_fmt=%s  sample_rate=%d  channels=%d\n",
                        sample_fmt_name(d.fmt).c_str(), d.sample_rate, d.channels);
        }
    }
    std::printf("frames_total: %zu\n", r.frames.size());
    std::printf("skipped_packets: %lld\n", static_cast<long long>(r.skipped_packets));
}

int run_decode_stats(const std::string& url, const std::string& cache_dir, int timeout_ms) {
    const DecodeOptions opt;   // lazy_pop 恒默认值——见 decode_via_source 注释
    const SourceDecodeOutcome so = decode_via_source(url, cache_dir, opt, timeout_ms);

    std::printf("url: %s\n", url.c_str());
    if (!so.open_err.empty()) {
        std::printf("FAILURE: %s\n", so.open_err.c_str());
        return kExitToolFailure;
    }
    if (!so.decode.error_stage.empty()) {
        std::printf("FAILURE: decode 错误于「%s」(averror=%d)\n",
                    so.decode.error_stage.c_str(), so.decode.averror);
        return kExitToolFailure;
    }
    if (so.watchdog_fired) {
        std::printf("注意: 看门狗曾软触发（跑得比预期慢，但最终跑完了）\n");
    }

    print_decode_stats(so.tracks, so.decode);
    return kExitOk;
}

int run_decode_diff(const std::string& diff_file, const std::string& url,
                    const std::string& cache_dir, int timeout_ms) {
    const DecodeOptions opt;   // 两条路径必须用同一份 probesize/analyzeduration
                                // 取值来源（frame_digest.h 顶部注释），否则
                                // 探测深度不一致会让逐帧比对假红

    // 本地参照解码这一段此前完全没有看门狗
    // 覆盖——CliWatchdog 之前只包住了下面的 decode_via_source() 那一半，
    // 「--decode 一旦真卡在内部解码器/dl 层同步读，唯一兜得住的只有硬
    // _exit」这句话对本地参照解码这一半并不成立：真卡住就是永久挂，连
    // 诊断都没有。跟 decode_via_source() 内部同一套两级设计（软超时打
    // 诊断、硬超时 _exit 直接终止进程——真的卡死时下面这行
    // `ref_watchdog_fired` 根本读不到，硬超时靠的是进程退出码本身，不是
    // 这个标志；它只用来记录"跑得比预期慢但最终跑完了"，跟
    // decode_via_source() 那一半的 out.watchdog_fired 同一个语义，不是
    // 额外发明一套新的判定），沿用同一个 timeout_ms。
    bool ref_watchdog_fired = false;
    DecodeResult ref;
    {
        const int soft_ms = timeout_ms / 2 > 0 ? timeout_ms / 2 : 1;
        CliWatchdog wd("decode_reference", soft_ms, timeout_ms);
        ref                 = decode_reference(diff_file, opt);
        ref_watchdog_fired  = wd.fired();
    }
    std::printf("file: %s\n", diff_file.c_str());
    if (ref_watchdog_fired) {
        std::printf("注意: 参照解码的看门狗曾软触发（跑得比预期慢，但最终跑完了）\n");
    }
    if (!ref.error_stage.empty()) {
        // 「参照路径打不开」——工具自己跑不下去，不是比对判定，退出码必须
        // 跟「比对不等」区分。
        std::printf("FAILURE: 参照路径解码失败于「%s」(averror=%d)\n",
                    ref.error_stage.c_str(), ref.averror);
        return kExitToolFailure;
    }

    const SourceDecodeOutcome so = decode_via_source(url, cache_dir, opt, timeout_ms);
    std::printf("url: %s\n", url.c_str());
    if (!so.open_err.empty()) {
        std::printf("FAILURE: %s\n", so.open_err.c_str());
        return kExitToolFailure;
    }
    if (!so.decode.error_stage.empty()) {
        // 「URL 不通/解码不下去」同样是工具跑不下去，不是内容比对——
        // 跟上面 ref 那支同一个道理，不能落进 diff 不等这条码。
        std::printf("FAILURE: 被测路径解码失败于「%s」(averror=%d)\n",
                    so.decode.error_stage.c_str(), so.decode.averror);
        return kExitToolFailure;
    }
    if (so.watchdog_fired) {
        std::printf("注意: 看门狗曾软触发（跑得比预期慢，但最终跑完了）\n");
    }

    std::printf("ref_frames=%zu got_frames=%zu skipped_packets=%lld\n", ref.frames.size(),
                so.decode.frames.size(), static_cast<long long>(so.decode.skipped_packets));

    const std::string diff = diff_frames(ref, so.decode);
    if (diff.empty()) {
        std::printf("diff: 全等\n");
        return kExitOk;
    }
    std::printf("diff: %s\n", diff.c_str());
    return kExitDiffMismatch;
}

}  // namespace

int main(int argc, char** argv) {
    std::string url, cache_dir, diff_file;
    std::vector<SeekPlan> seeks;
    int  timeout_ms = 120000;
    bool decode_mode = false;
    bool stats_mode  = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); std::exit(kExitUsage); }
            return argv[++i];
        };
        if (a == "--cache-dir")    cache_dir = next();
        else if (a == "--seek")    seeks.push_back(SeekPlan{std::atoll(next()) * 1000, 60});
        else if (a == "--timeout") timeout_ms = std::atoi(next());
        else if (a == "--diff")    diff_file = next();
        else if (a == "--decode")  decode_mode = true;
        else if (a == "--stats")   stats_mode = true;
        else if (a == "-h" || a == "--help") { usage(); return kExitOk; }
        else if (!a.empty() && a[0] == '-') { usage(); return kExitUsage; }
        else url = a;
    }
    if (url.empty()) { usage(); return kExitUsage; }

    if (decode_mode) {
        const bool has_diff = !diff_file.empty();
        if (stats_mode == has_diff) {
            // 恰好要二选一：都没给、或都给了，都是用法错误。
            std::fprintf(stderr, "--decode 必须搭配且只能搭配 --stats 或 --diff 之一\n");
            usage();
            return kExitUsage;
        }
        if (!seeks.empty()) {
            std::fprintf(stderr, "--decode 目前不支持 --seek（frame_digest.h 的判据不带 "
                                 "seek；scenario C 那种 seek+解码没有暴露成可复用接口）\n");
            usage();
            return kExitUsage;
        }
    } else if (stats_mode) {
        std::fprintf(stderr, "--stats 须搭配 --decode\n");
        usage();
        return kExitUsage;
    }

    // 冒烟程序（smoke.cpp）已删——探查工具本身
    // 报一下链上的 FFmpeg 版本更有用，让 ffmpeg_version_string() 保持存活。
    std::printf("ffmpeg: %s\n", syp::media::ffmpeg_version_string().c_str());

    bool temp_cache = cache_dir.empty();
    if (temp_cache) cache_dir = make_temp_cache_dir("cli");

    int rc;
    if (decode_mode) {
        rc = stats_mode ? run_decode_stats(url, cache_dir, timeout_ms)
                         : run_decode_diff(diff_file, url, cache_dir, timeout_ms);
    } else {
        RunSpec spec;
        spec.url         = url;
        spec.cache_dir   = cache_dir;
        spec.seeks       = seeks;
        spec.watchdog_ms = timeout_ms;

        const RunResult r = run_through_source(spec);

        std::printf("url: %s\n", url.c_str());
        if (!r.failure.empty()) std::printf("FAILURE: %s\n", r.failure.c_str());
        if (!r.demux.error_stage.empty()) std::printf("demux 错误: %s\n", r.demux.error_stage.c_str());

        std::printf("duration: %.2fs  streams: %zu\n",
                    static_cast<double>(r.demux.duration_us) / 1000000.0,
                    r.demux.streams.size());
        for (const StreamSummary& s : r.demux.streams) {
            std::printf("  #%d codec_id=%d %dx%d\n", s.index, s.codec_id, s.width, s.height);
        }
        std::printf("packets: %zu\n", r.demux.packets.size());
        std::printf("downloaded=%lld cache_hit=%lld cached=%lld failed_tasks=%d redirects=%d\n",
                    (long long)r.metrics.downloaded_bytes,
                    (long long)r.metrics.cache_hit_bytes,
                    (long long)r.metrics.cached_bytes,
                    r.metrics.failed_tasks, r.metrics.redirect_count);

        // 「工具跑不下去」（open/demux 失败，含看门狗触发）跟「diff 判定
        // 不等」是两件事，退出码分开——同一条纪律，跟 --decode 那两个
        // run_decode_* 函数保持一致，不只管 --decode。
        if (!r.failure.empty() || !r.demux.error_stage.empty()) {
            rc = kExitToolFailure;
        } else {
            rc = kExitOk;
        }

        if (!diff_file.empty()) {
            const DemuxResult expected = demux_file(diff_file, spec.opt, seeks);
            if (!expected.error_stage.empty()) {
                std::printf("FAILURE: 参照路径 demux 失败于「%s」(averror=%d)\n",
                            expected.error_stage.c_str(), expected.averror);
                rc = kExitToolFailure;
            } else if (rc == kExitOk) {
                const std::string d = diff_report(expected, r.demux);
                if (d.empty()) {
                    std::printf("diff: 全等\n");
                } else {
                    std::printf("diff: %s\n", d.c_str());
                    rc = kExitDiffMismatch;
                }
            }
        }
    }

    if (temp_cache) remove_dir_recursive(cache_dir);
    return rc;
}
