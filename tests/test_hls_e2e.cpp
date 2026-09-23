// test_hls_e2e.cpp —— HLS 端到端。
//
// 全部用例都打 127.0.0.1 上的 loopback server，**真实网络不可达**——
// 这是结构性验收的基础：如果 FFmpeg 自己开了 socket，
// 用例会失败而不是悄悄通过。
//
// 判据说明（诚实标注）：
//   · "帧数与直接播源素材一致" 是**内容等量**判据，不是逐帧内容比对。
//     fixture 的 source.mp4 与 HLS 分片是同一份编码（gen-fixtures.sh 用
//     `-c copy` remux，packet 逐字节相同），所以帧数相等是一条有意义的
//     必要条件；但它挡不住"帧内容错了而数量对"。逐帧比对留给后续任务
//     （HLS 走 Pipeline::create_hls 这条路，frame_digest.h 现有的
//     decode_pipeline() 只吃 create_file/create_avio，接上去要动
//     syp_probe_core 的公开接口，是一项独立的工作）。
//   · srv.requests_for() 的**正向**断言（`>= 1`，"master/media/seg0 被请求
//     过"）是一条**弱**的结构性判据，别高估它：LoopbackServer 只在
//     127.0.0.1 上听，FFmpeg 自己开 socket 打的也是同一个地址，所以正向
//     计数**对"谁开的 socket"零分辨率**。用一个变异场景实测过（把 io_open 与
//     protocol_whitelist 一起拿掉）：FFmpeg 自己开 socket，本用例这三条
//     正向断言全过。
//   · **但负向断言（`== 0`，"这个 URL 绝不该被请求"）不一样，它有牙**
//     ——以前这里一刀切地写成"requests_for()
//     零分辨率"，把这个区分抹平了。FFmpeg 自己开 socket 时会去要我们的
//     dl 层永远不会要的 URL，所以这个变异场景下这几条负向断言确实红了：
//     :464 明文 server 上的 /hls/master.m3u8（我们发的是 https，明文这边
//     该零请求）、:582 被 discard 那两档的 seg_high1/seg_low1、
//     :1212 加密流的 key.bin（我们在拉密钥之前就拒了）。
//   · 这个变异场景下的完整实测清单：
//     **6 红 8 绿**。红的是 vod_single_variant_plays_to_eof /
//     https_master_is_rewritten… / multi_variant_selects_by_bandwidth… /
//     encrypted_playlist_is_rejected… / segment_404_is_reported… /
//     every_byte_goes_through_our_dl_layer。
//   · 下面那两条缓存断言直接落在"缓存里有什么"上，但
//     **这个变异场景下只有第二条真的红**：`cached_a_segment`。第一条
//     `!ends_with(u, ".m3u8")` 在那一格里缓存目录是空的，**空转通过**
//     ——正是它们上方 :400 注释自己写明的那个理由。别把两条都当成有牙。
//   · 真正把"谁开的 socket"变成一条直接判据的是本文件最后一条
//     every_byte_goes_through_our_dl_layer：全局
//     syp_http_backend 换成内存桩 + 主机名用 DNS 上永不可解析的
//     fake.invalid，唯一的字节来源就是我们自己。
//     hls_session.h 那道 protocol_whitelist 护栏保证的是"绕过不会**静默**
//     发生"（没装 io_open 时当场失败而不是悄悄走 FFmpeg 自己的 IO），
//     它**不**让上面那些断言本身获得辨别力。
//
// 【跑起来会有固定的 FFmpeg 噪声，不是失败，别去"修"它】多码率那几条用例
// （vod_multi）稳定打印这三行：
//     [mov,mp4,...] Packet corrupt (stream = 1, dts = 11264).
//     [hls @ ...]   Packet corrupt (stream = 5, dts = 11264).
//     [aac @ ...]   decode_band_types: Input buffer exhausted before END element found
// 来源：选轨只能在 avformat_open_input **之后**做，而
// hls_read_header 在那之前已经为**每一档**开 demuxer 探测过首个分片
// （hls.c:2240 起）。被 discard 的那几档因此停在"读了半个分片就再也不续"
// 的状态上，留下一截不完整的包；FFmpeg 在拆解/排空时如实抱怨了一句。
// 它只发生在**已经不再被消费**的那几条流上，对选中档的输出没有任何影响
// ——用例的帧数/EOF/请求计数断言全部只看选中档。
// 唯一能让它消失的办法是别让 FFmpeg 去探测未选中档，那要么改 hls.c，
// 要么在 master 解析阶段就截断——两条都不在本项目的边界内。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>

#include "dl/source_bridge.h"
#include "media/avio_bridge.h"           // syp::dl::current_http_backend()
#include "media/hls/hls_session.h"
#include "platform/apple/apple_http_backend.h"  // C1 探针：syp_apple_http_backend()
#include "media/pipeline.h"
#include "media/track_player.h"
#include "scenarios.h"                  // ensure_apple_backend / TempCacheDir
#include "support/fake_audio_sink.h"
#include "support/fake_renderer.h"
#include "support/loopback_server.h"
#include "support/stub_backend.h"
#include "support/watchdog.h"
#include "tiny_test.h"

using namespace syp::media;
using syp::dl::test::LoopbackServer;

namespace {

std::string fixture_dir() {
    const char* dir = std::getenv("SYP_FIXTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        tiny_test::fail(__FILE__, __LINE__,
                        "SYP_FIXTURE_DIR 未设置——素材路径没配，不是 HLS 本身失败");
        return std::string();
    }
    return std::string(dir);
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        tiny_test::fail(__FILE__, __LINE__, ("读不到 fixture 文件：" + path).c_str());
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

const char* content_type_for(const std::string& name) {
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".m3u8") == 0)
        return "application/vnd.apple.mpegurl";
    return "video/mp4";
}

// 三套 HLS fixture 各自的文件清单（tools/gen-fixtures.sh 生成的实际文件名）。
//
// 不用 std::filesystem 遍历目录：文件名写死成显式清单，是为了让
// "fixture 少了一个文件" 立刻变成一条读文件失败的 FAIL，而不是变成
// "路由表里少一条 → 404 → HLS 打开失败" 这种绕一大圈的症状。
const std::vector<std::string> kSingleFiles = {
    "master.m3u8", "media.m3u8", "init.mp4",
    "seg0.m4s", "seg1.m4s", "seg2.m4s", "seg3.m4s",
};
const std::vector<std::string> kMultiFiles = {
    "master.m3u8",
    "media_high.m3u8", "init_high.mp4",
    "seg_high0.m4s", "seg_high1.m4s", "seg_high2.m4s", "seg_high3.m4s",
    "media_mid.m3u8", "init_mid.mp4",
    "seg_mid0.m4s", "seg_mid1.m4s", "seg_mid2.m4s", "seg_mid3.m4s",
    "media_low.m3u8", "init_low.mp4",
    "seg_low0.m4s", "seg_low1.m4s", "seg_low2.m4s", "seg_low3.m4s",
};
const std::vector<std::string> kDemuxedFiles = {
    "master.m3u8",
    "media_video.m3u8", "init_video.mp4",
    "seg_video_0.m4s", "seg_video_1.m4s", "seg_video_2.m4s", "seg_video_3.m4s",
    "media_audio.m3u8", "init_audio.mp4",
    "seg_audio_0.m4s", "seg_audio_1.m4s", "seg_audio_2.m4s",
    "seg_audio_3.m4s", "seg_audio_4.m4s",
};

// 把 fixture 目录下的一整棵 HLS 目录挂进 loopback server 的路由表。
// 返回 master.m3u8 的 URL。
std::string serve_hls_tree(LoopbackServer& srv, const std::string& dir,
                           const std::string& url_prefix,
                           const std::vector<std::string>& files) {
    for (const std::string& name : files) {
        srv.set_route(url_prefix + "/" + name, read_file(dir + "/" + name),
                      content_type_for(name));
    }
    return srv.url(url_prefix + "/master.m3u8");
}

// 把 cache_dir 里每一条缓存条目的原始 URL 读出来。
//
// 【为什么要直接读磁盘索引】两条通道的分流（播放列表零缓存 / 分片走缓存）
// 是整条 HLS 链路的核心不变量（hls_session.h 顶部、url_rewrite.h 都把它
// 写成第一性原理），但它失效后在 VOD 下**完全无症状**——用一个变异场景实测过：把
// channel_for 改成"一律走分片通道"，ctest 26/26 照样全绿，只有扫
// cache_dir 才看得见多出来的 master.m3u8 / media.m3u8 两条条目。真正的
// 症状要到直播刷新时才出现（"卡在某一刻不再前进"），那时离引入缺陷已经
// 隔了很远。所以判据必须直接落在"缓存里有什么"上。
//
// 磁盘格式见 src/dl/cache_index.cpp 顶部（v1，小端）：
//   [0,4) magic 'SYPI'  [4,8) version  [8,12) u32 url 长度  [12,…) url 字节
// 只解析到 url 就够——后面的字段本用例一个都不需要。
std::vector<std::string> cached_urls(const std::string& cache_dir) {
    namespace fs = std::filesystem;
    std::vector<std::string> urls;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cache_dir, ec)) {
        if (ec) break;
        if (e.path().extension() != ".idx") continue;   // .dat / .idx.tmp 跳过
        std::ifstream f(e.path(), std::ios::binary);
        if (!f) continue;
        unsigned char head[12];
        f.read(reinterpret_cast<char*>(head), sizeof(head));
        if (f.gcount() != static_cast<std::streamsize>(sizeof(head))) continue;
        if (head[0] != 'S' || head[1] != 'Y' || head[2] != 'P' || head[3] != 'I') continue;
        const uint32_t len = static_cast<uint32_t>(head[8]) |
                             (static_cast<uint32_t>(head[9]) << 8) |
                             (static_cast<uint32_t>(head[10]) << 16) |
                             (static_cast<uint32_t>(head[11]) << 24);
        if (len == 0 || len > (1u << 20)) continue;
        std::string url(len, '\0');
        f.read(url.data(), static_cast<std::streamsize>(len));
        if (f.gcount() != static_cast<std::streamsize>(len)) continue;
        urls.push_back(std::move(url));
    }
    return urls;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// 每一步之后把**所有**被管理轨排空，只给视频轨计数。
//
// 【只 pop 视频轨会挂死】pipeline.h 顶部写得
// 很清楚：背压是"联合"的——音频轨的 FrameQueue 一旦堆满（默认
// max_frames_per_track=8），demux 分支整体停摆，视频也跟着停，step()
// 从此恒返回 Blocked，永远到不了 EOF。实测症状正是"源素材没跑到 EOF"，
// 跟 HLS 一点关系都没有。所以这里对每条轨都排空，只是计数只数视频。
void drain_all_tracks(Pipeline& p, int64_t* video_frames) {
    for (const auto& t : p.tracks()) {
        while (p.pop_frame(t.index).has_value()) {
            if (t.is_video) ++(*video_frames);
        }
    }
}

// 对照组：同一份内容的非 HLS 单文件（vod_single/source.mp4），按
// demo/shared/bridge.mm:285 同样的方式从 tracks() 取最大 duration_us。
// 存在的唯一理由是给直播那条 `duration_us == 0` 当对照——证明这条判据在
// 别的输入上确实取得到非零值，不是恒真。
int64_t max_track_duration_us_of_source() {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture_dir() + "/hls/vod_single/source.mp4",
                                    PipelineConfig{}, &err);
    if (p == nullptr) {
        tiny_test::fail(__FILE__, __LINE__, "打不开 hls/vod_single/source.mp4");
        return -1;
    }
    int64_t d = 0;
    for (const auto& t : p->tracks()) {
        if (t.duration_us > d) d = t.duration_us;
    }
    return d;
}

// 直接播源素材数出来的视频帧数。**不写死数字**——写死的话 fixture 一变
// 这条断言就变成噪音（"为什么是 100" 谁也答不上来）。
int64_t expected_video_frames_of_source() {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture_dir() + "/hls/vod_single/source.mp4",
                                    PipelineConfig{}, &err);
    if (p == nullptr) {
        tiny_test::fail(__FILE__, __LINE__, "打不开 hls/vod_single/source.mp4");
        return -1;
    }
    int64_t frames  = 0;
    bool    saw_eof = false;
    for (int i = 0; i < 200000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        if (so.kind == StepOutcome::Kind::Error) {
            tiny_test::fail(__FILE__, __LINE__, "源素材解码报错");
            return -1;
        }
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        drain_all_tracks(*p, &frames);
    }
    if (!saw_eof) {
        tiny_test::fail(__FILE__, __LINE__, "源素材没跑到 EOF");
        return -1;
    }
    return frames;
}

// 在 vod_multi（800k/400k/200k 三档，流 0/1 = high、2/3 = mid、4/5 = low）
// 上按给定 HlsOptions 开一条管线，把"选轨之后没被 discard 的那条视频轨/音频轨
// 的流号"与被 discard 的流数报出来。-1 = 一条都没有。
//
// 抽成函数是因为 max_bandwidth_bps 的契约有三条分支（不限 / 有档满足 /
// 一档都不满足），每条都要在**多候选**的素材上各验一遍，而多次装配的代码
// 逐字相同——复制几份只会让下次改 fixture 时漏改其中一份。
struct SelectedTracks {
    int32_t video     = -1;
    int32_t audio     = -1;
    int32_t discarded = 0;
};

SelectedTracks select_on_vod_multi(const HlsOptions& opts, const char* cache_tag) {
    SelectedTracks r;
    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_multi", "/hls", kMultiFiles);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache(cache_tag);
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, opts, &err);
    if (p == nullptr) {
        tiny_test::fail(__FILE__, __LINE__, "Pipeline::create_hls 返回 nullptr");
        return r;
    }
    for (const auto& t : p->tracks()) {
        if (t.discard) { ++r.discarded; continue; }
        if (t.is_video)             r.video = t.index;
        else if (t.sample_rate > 0) r.audio = t.index;
    }
    return r;
}

// ---------------------------------------------------------------------------
// 直播 + 中止接线
// ---------------------------------------------------------------------------

// 【直播素材是现造的，不进 gen-fixtures.sh】直播的本质是"同一个 URL 在两次
// 请求之间内容变了"——这件事没有任何静态文件能表达，它只能由用例在跑的过程
// 中换路由（srv.set_route）来构造。分片字节复用 vod_single 的
// init.mp4 / seg0..seg3.m4s，变的只有 media playlist 本身。
std::vector<uint8_t> bytes_of(const std::string& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return std::vector<uint8_t>(p, p + s.size());
}

// 一份**直播** media playlist：没有 EXT-X-ENDLIST、没有 EXT-X-PLAYLIST-TYPE。
// 这两条缺席正是 FFmpeg 的 hls 判定"这是直播、要周期性重拉播放列表"的依据
// （hls.c 里 pls->finished 只有见到 ENDLIST 才置位）。
// seg_prefix 允许把分片指到另一台 server 上——request_abort_unblocks_a_hanging
// _playlist_fetch 靠它把"播放列表卡住"和"分片卡住"分成两台机器，否则
// LoopbackConfig::hang 是整机开关，分不出到底卡在哪条通道。
std::string live_media_playlist(const std::vector<int>& segs,
                                const std::string& seg_prefix) {
    std::string s = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n";
    s += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(segs.empty() ? 0 : segs.front()) + "\n";
    s += "#EXT-X-MAP:URI=\"" + seg_prefix + "init.mp4\"\n";
    for (const int n : segs) {
        s += "#EXTINF:1.000000,\n";
        s += seg_prefix + "seg" + std::to_string(n) + ".m4s\n";
    }
    return s;   // 故意不写 EXT-X-ENDLIST —— 那一行就是"点播/直播"的开关
}

// 只挂 vod_single 的媒体字节（init + 四片），不挂任何 .m3u8——播放列表由
// 上面的 live_media_playlist() 现造。
void serve_live_segments(LoopbackServer& srv, const std::string& prefix) {
    const std::string dir = fixture_dir() + "/hls/vod_single";
    for (const char* name : {"init.mp4", "seg0.m4s", "seg1.m4s", "seg2.m4s", "seg3.m4s"}) {
        srv.set_route(prefix + "/" + name, read_file(dir + "/" + name), "video/mp4");
    }
}

// 把一台已经挂好路由的 server 整机切成"读完请求后永不响应"。
// config() 返回的是 cfg_ 的拷贝（含 routes），所以这样改不会把路由表清掉。
void make_it_hang(LoopbackServer& srv) {
    syp::dl::test::LoopbackConfig cfg = srv.config();
    cfg.hang = true;
    srv.set_config(cfg);
}

}  // namespace

TEST_CASE(vod_single_variant_plays_to_eof) {
    // 看门狗：同 test_decode_e2e/test_sync_e2e——挂死时要能在日志里看出是
    // 哪条用例卡住，而不是把 ctest 拖到超时（超时的日志里连用例名都没有）。
    // HLS 这条路径比那两条更容易挂：分片打开是真的网络 IO，失败/超时的
    // 形态比本地文件多。
    syp::test::Watchdog wd("vod_single_variant_plays_to_eof", 60000, 180000,
                           "卡住多半在 HlsSession::open_segment/open_playlist 的阻塞 IO 上");

    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master = serve_hls_tree(srv, fixture_dir() + "/hls/vod_single", "/hls", kSingleFiles);

    const int64_t expected = expected_video_frames_of_source();
    REQUIRE(expected > 0);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_vod_single");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);
    CHECK_EQ(static_cast<int>(err), static_cast<int>(SYP_OK));

    // 跑到 EOF，数帧。帧数必须与直接播源素材一致——HLS 只是把同一份内容
    // 切成了分片，不该多也不该少。
    int64_t video_frames = 0;
    bool    saw_eof      = false;
    for (int i = 0; i < 200000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        REQUIRE(so.kind != StepOutcome::Kind::Error);
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        drain_all_tracks(*p, &video_frames);
    }
    CHECK(saw_eof);
    CHECK_EQ(video_frames, expected);

    // 结构性验收的第一半：master 与 media 播放列表真的被请求过，
    // 也就是说数据确实是经我们的 io_open 流过去的。
    CHECK(srv.requests_for("/hls/master.m3u8") >= 1);
    CHECK(srv.requests_for("/hls/media.m3u8")  >= 1);
    // 分片也要真的被请求过——只看播放列表的话，一个"播放列表解析成功、
    // 分片一个没取、直接 EOF"的实现会绿。
    CHECK(srv.requests_for("/hls/seg0.m4s") >= 1);

    // ---- 双通道分流的实证：缓存里该有什么、不该有什么 ----
    //
    // 这是本用例里唯一能抓住"分流失效"的断言。不加的话
    // channel_for 整个失效（一律走分片通道）在 VOD 下 26/26 全绿。
    // 必须在 Pipeline 析构**之后**扫——SourceBridge::close() 才会把索引
    // save() 落盘（source_bridge.cpp:1018 起）。
    p.reset();
    const std::vector<std::string> urls = cached_urls(cache.path);

    // 第一条：播放列表零缓存。任何 .m3u8 出现在缓存里，都说明它走错了通道。
    for (const std::string& u : urls) {
        CHECK(!ends_with(u, ".m3u8"));
    }
    // 第二条不能省：只有第一条的话，"一个字节都没缓存"（比如分片通道整个
    // 坏掉、或缓存目录写不进去）也会通过。分片必须**确实**走了缓存通道。
    bool cached_a_segment = false;
    for (const std::string& u : urls) {
        if (contains(u, "seg0.m4s")) cached_a_segment = true;
    }
    CHECK(cached_a_segment);

    CHECK(!wd.fired());
}

// 同一份 HLS 走线程模式（网络读在加载线程上）跑到 EOF，视频帧数与源素材一致。
TEST_CASE(vod_single_variant_plays_to_eof_with_demux_thread) {
    syp::test::Watchdog wd("vod_single_variant_plays_to_eof_with_demux_thread", 60000, 180000,
                           "卡住多半是加载线程阻塞在分片读里没返回，或终止标记没被吸收");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master = serve_hls_tree(srv, fixture_dir() + "/hls/vod_single", "/hls", kSingleFiles);
    const int64_t expected = expected_video_frames_of_source();
    REQUIRE(expected > 0);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_vod_single_thread");
    dl.cache_dir = cache.path.c_str();

    PipelineConfig pcfg;
    pcfg.demux_thread = true;
    auto p = Pipeline::create_hls(master, pcfg, dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);

    int64_t video_frames = 0;
    bool    saw_eof      = false;
    for (int64_t i = 0; i < 20000000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        REQUIRE(so.kind != StepOutcome::Kind::Error);
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        drain_all_tracks(*p, &video_frames);
        if (so.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(saw_eof);
    CHECK_EQ(video_frames, expected);
    CHECK(!wd.fired());
}

// 服务端扣住第 3 个分片（seg2，1 秒一片）→ 加载线程阻塞在它上面，已缓冲约 2 秒；
// 播放推进到缓冲尾部时 TrackPlayer 进入卡顿缓冲；此刻放行分片 → 读到文件尾 → 离开缓冲、
// 播完，帧一帧不少。用"扣住/放行"而不是固定延迟：卡顿的发生与结束都由用例决定，不赌时序。
// 时钟由 FakeAudioSink::advance() 推进（每步 2ms），缓冲中 sink 暂停、advance 不前进。
TEST_CASE(held_segment_makes_track_player_stall_then_resume_to_eof) {
    syp::test::Watchdog wd("held_segment_makes_track_player_stall_then_resume_to_eof", 60000, 180000,
                           "卡住多半是卡顿缓冲读到文件尾后没有离开，或 seg2 从未被请求");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master = serve_hls_tree(srv, fixture_dir() + "/hls/vod_single", "/hls", kSingleFiles);
    const int64_t expected = expected_video_frames_of_source();
    REQUIRE(expected > 0);
    srv.hold_route("/hls/seg2.m4s");

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_stall_hold");
    dl.cache_dir = cache.path.c_str();

    PipelineConfig pcfg;
    pcfg.demux_thread = true;
    auto p = Pipeline::create_hls(master, pcfg, dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    sink->set_capacity_frames(10'000'000);
    syp::test::FakeAudioSink* sink_raw = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    syp::test::FakeRenderer* renderer_raw = renderer.get();
    auto tp = TrackPlayer::create(std::move(p), std::move(sink), std::move(renderer), nullptr, &err);   // 默认策略：启用
    REQUIRE(tp != nullptr);
    CHECK(tp->buffering_reason() == BufferingReason::Startup);

    bool    released          = false;
    bool    reached_eof       = false;
    int64_t buffered_at_stall = -1;
    for (int64_t i = 0; i < 20000000; ++i) {
        const PlayOutcome o = tp->step();
        REQUIRE(o.kind != PlayOutcome::Kind::Error);
        if (o.kind == PlayOutcome::Kind::Eof) {
            reached_eof = true;
            break;
        }
        // 只在"正卡顿、且 seg2 确实被请求并扣住"时放行：证明这次卡顿就是扣住分片造成的。
        if (!released && tp->buffering_reason() == BufferingReason::Stall &&
            srv.requests_received_for("/hls/seg2.m4s") >= 1) {
            buffered_at_stall = tp->buffered_us();
            srv.release_route("/hls/seg2.m4s");
            released = true;
        }
        sink_raw->advance(2000);
        if (o.kind == PlayOutcome::Kind::Waiting || o.kind == PlayOutcome::Kind::Blocked) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    if (!released) srv.release_route("/hls/seg2.m4s");   // 失败路径也别把服务端线程晾着

    std::printf("  [stall-hold] released=%d eof=%d rebuffers=%lld buffered_at_stall=%lld "
                "rebuffer_total_us=%lld startup_us=%lld shown=%zu dropped=%lld expected=%lld\n",
                static_cast<int>(released), static_cast<int>(reached_eof),
                static_cast<long long>(tp->rebuffer_count()),
                static_cast<long long>(buffered_at_stall),
                static_cast<long long>(tp->rebuffer_total_us()),
                static_cast<long long>(tp->startup_us()), renderer_raw->shown().size(),
                static_cast<long long>(tp->dropped_frames()), static_cast<long long>(expected));
    CHECK(released);
    CHECK(reached_eof);
    CHECK(tp->rebuffer_count() >= 1);
    CHECK(!tp->buffering());
    CHECK(buffered_at_stall >= 0);
    CHECK(buffered_at_stall < 100000);
    CHECK(tp->rebuffer_total_us() > 0);
    CHECK(tp->startup_us() >= 0);
    CHECK_EQ(static_cast<int64_t>(renderer_raw->shown().size()) + tp->dropped_frames(), expected);
    CHECK(!wd.fired());
}

// https → http 的改写与还原这对接线，端到端只能钉到这个程度。
//
// 【为什么需要这条】用一个变异场景实测过（把 on_io_open 里的 to_real_url 还原去掉）
// 在 vod_single 那条用例下是**绿**的——fixture 全是 http:，还原是恒等变换，
// 去掉它什么都不变。to_real_url 本身在 test_hls_url_rewrite 里被穷举测过，
// 缺的是"HlsSession 真的把这两个函数接上了"。
//
// 【为什么只能到这个程度】loopback server 是明文 HTTP，起不了 TLS，
// 所以"https 真的能播"做不成用例。这条改用**可观测的副作用**来钉：
// 给一个 https:// 的 master URL，
//   · 递给 FFmpeg 的 URL 必须已经降级成 http://（to_ffmpeg_url 接上了）——
//     否则 hls.c:688 那道协议名检查就过不去（本项目没编 https 协议）；
//   · 而真正发出去的请求必须是 **https**，于是打在明文服务端上必然失败，
//     服务端**一条该路径的请求都不该记到**（to_real_url 接上了）。
// 去掉 to_real_url 的话，请求会以明文 http 发出去、被服务端正常受理，
// 下面那条 requests_for == 0 立刻变红。
TEST_CASE(https_master_is_rewritten_for_ffmpeg_and_restored_for_the_wire) {
    syp::test::Watchdog wd("https_master_round_trip", 60000, 180000,
                           "卡住多半在 https 连接的超时收敛上");

    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    // 只挂路由，不供任何内容——本用例不需要播成功，只看服务端记到了什么。
    srv.set_route("/hls/master.m3u8", read_file(fixture_dir() + "/hls/vod_single/master.m3u8"),
                  "application/vnd.apple.mpegurl");

    // srv.url() 给的是 http://127.0.0.1:<port>/...，把 scheme 换成 https。
    const std::string http_url  = srv.url("/hls/master.m3u8");
    REQUIRE(http_url.compare(0, 7, "http://") == 0);
    const std::string https_url = "https" + http_url.substr(4);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size         = sizeof(syp_config);
    // 短超时：本用例预期失败，不必陪着默认的 10s/15s 等。
    dl.connect_timeout_ms  = 1500;
    dl.read_timeout_ms     = 1500;
    const syp::probe::TempCacheDir cache("hls_https_round_trip");
    dl.cache_dir = cache.path.c_str();

    // 第一半：递给 FFmpeg 的 URL 已经降级成 http://。
    {
        syp_status cerr = SYP_OK;
        auto session = syp::media::hls::HlsSession::create(https_url, dl, HlsOptions{}, &cerr);
        REQUIRE(session != nullptr);
        CHECK_EQ(session->ffmpeg_url(), http_url);
    }

    // 第二半：真正发出去的是 https，明文服务端收不到这条路径的请求。
    auto p = Pipeline::create_hls(https_url, PipelineConfig{}, dl, HlsOptions{}, &err);
    CHECK(p == nullptr);            // 没有 TLS 服务端，本来就该失败
    CHECK_EQ(srv.requests_for("/hls/master.m3u8"), static_cast<int64_t>(0));

    CHECK(!wd.fired());
}

// ---------------------------------------------------------------------------
// 选轨 + 被 discard 的流不能被绑成主视频轨
// ---------------------------------------------------------------------------

// 三档 800k/400k/200k，限 500k ⇒ 选中 400k 那档（program[1]，流 2/3）。
//
// 【"未选中档的播放列表零请求"是错的】hls_read_header
// 无条件把**每一档**的播放列表 parse 一遍（hls.c:2178），还会为每一档开
// demuxer 探测首个分片（hls.c:2240 起）。而选轨只能在 open **之后**做
// （AVProgram 是 hls.c:2221 才建的），所以那些请求收不回来。实测（本机，
// FFmpeg 8.1.2）：未选中档的 media_*.m3u8 / init_*.mp4 / seg_*0.m4s 各 1 次。
//
// 真正省下来的、也是本用例的判据：**open 之后不再为未选中档取任何分片**。
// 选中档的 4 片全取；未选中档只有探测用的第 0 片，第 1~3 片一次都没取。
// 机制是 playlist_needed()（hls.c:1528-1570）+ recheck_discard_flags()
// （hls.c:2445）：一条播放列表的 main_streams 全被 AVDISCARD_ALL 之后
// pls->needed 变 0，read_data 那条路整个不再为它取分片。
// 这条断言证明选轨真的省掉了流量，不只是"解码时忽略"。
// max_bandwidth_bps 的另外两条分支。
//
// 【为什么必须单独钉，且必须在 vod_multi 上钉】hls_options.h 把契约写成三条：
//   0 = 不限 ⇒ 选最高档 / 有档满足 ⇒ 选满足里最高的 / 一档都不满足 ⇒ 选最低档。
// 第二条由下面那条用例（限 500k）覆盖。第一、三条此前**零覆盖**，两个
// 变异因此存活：
//   · M10：把 `opts_.max_bandwidth_bps <= 0 ||` 整个去掉（"0 = 不限"变成
//     "全都不满足"）⇒ 7/7 全绿；
//   · M11：把兜底的 `bw < smallest_bw` 反向（兜底改选**最大**档）⇒ 7/7 全绿。
// 原因是当时唯一走这两条分支的用例在 vod_demuxed 上，按"含视频流"过滤之后
// **只剩一个候选**——最大 = 最小 = 唯一，选谁都一样，判据零分辨力。必须在
// 有三个候选的 vod_multi 上才分得开。
//
// 而 `HlsOptions{}`（max_bandwidth_bps = 0）正是**默认值**，也是
// Pipeline::create_hls 调用方最常用的配置——本任务最常见的生产配置，此前在
// 多码率源上一条覆盖都没有。
TEST_CASE(bandwidth_policy_picks_highest_when_unlimited_and_lowest_when_nothing_fits) {
    syp::test::Watchdog wd("bandwidth_policy_highest_and_lowest", 60000, 180000,
                           "卡住多半在 HlsSession::open_segment/open_playlist 的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    // 一、默认值（0 = 不限）⇒ 选最高档 800k，即流 0（video）/ 1（audio）。
    // M10 杀手：去掉 `max_bandwidth_bps <= 0 ||` 之后三档都"不满足"，
    // 兜底选最低档 200k，这里会读到 4/5。
    const SelectedTracks unlimited = select_on_vod_multi(HlsOptions{}, "hls_bw_unlimited");
    CHECK_EQ(unlimited.discarded, 4);
    CHECK_EQ(unlimited.video, 0);
    CHECK_EQ(unlimited.audio, 1);

    // 二、100k 低于三档 ⇒ 兜底选**最低**档 200k，即流 4 / 5。
    // M11 杀手：兜底的比较反向之后会选最高档 800k，这里会读到 0/1。
    HlsOptions too_slow;
    too_slow.max_bandwidth_bps = 100000;      // 三档是 800k/400k/200k，一档都不满足
    const SelectedTracks fallback = select_on_vod_multi(too_slow, "hls_bw_fallback");
    CHECK_EQ(fallback.discarded, 4);
    CHECK_EQ(fallback.video, 4);
    CHECK_EQ(fallback.audio, 5);

    CHECK(!wd.fired());
}

TEST_CASE(multi_variant_selects_by_bandwidth_and_skips_the_others) {
    syp::test::Watchdog wd("multi_variant_selects_by_bandwidth", 60000, 180000,
                           "卡住多半在 HlsSession::open_segment/open_playlist 的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_multi", "/hls", kMultiFiles);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_vod_multi");
    dl.cache_dir = cache.path.c_str();

    HlsOptions opts;
    opts.max_bandwidth_bps = 500000;          // 三档是 800k/400k/200k
    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, opts, &err);
    REQUIRE(p != nullptr);

    // 选中的是 400k 那档：它的流（2=video / 3=audio）没被 discard，
    // 另外两档的四条流全被 discard。判据落在 tracks() 上而不是只看请求
    // 计数——后者分辨不出"选对了档"和"三档全没 discard 但只巧合取了一档"。
    int32_t kept_video = -1, kept_audio = -1, discarded = 0;
    for (const auto& t : p->tracks()) {
        if (t.discard) { ++discarded; continue; }
        if (t.is_video)             kept_video = t.index;
        else if (t.sample_rate > 0) kept_audio = t.index;
    }
    CHECK_EQ(discarded, 4);
    CHECK_EQ(kept_video, 2);
    CHECK_EQ(kept_audio, 3);

    bool saw_eof = false;
    int64_t frames = 0;
    for (int i = 0; i < 200000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        REQUIRE(so.kind != StepOutcome::Kind::Error);
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        drain_all_tracks(*p, &frames);
    }
    CHECK(saw_eof);

    // 选中那档的四片全取了。
    CHECK(srv.requests_for("/hls/media_mid.m3u8") >= 1);
    for (const char* seg : {"seg_mid0.m4s", "seg_mid1.m4s", "seg_mid2.m4s", "seg_mid3.m4s"}) {
        CHECK(srv.requests_for(std::string("/hls/") + seg) >= 1);
    }
    // 未选中两档：第 0 片是 read_header 的探测，收不回来；第 1~3 片
    // 一次都不该被取。把 select_variant() 里设 AVDISCARD_ALL 的那两层
    // 循环注掉，这六条立刻全红（自检 B 实测）。
    for (const char* seg : {"seg_high1.m4s", "seg_high2.m4s", "seg_high3.m4s",
                            "seg_low1.m4s",  "seg_low2.m4s",  "seg_low3.m4s"}) {
        CHECK_EQ(srv.requests_for(std::string("/hls/") + seg), static_cast<int64_t>(0));
    }

    CHECK(!wd.fired());
}

// AVDISCARD_ALL 不把流从 nb_streams 里移除，所以
// TrackPlayer 仍然看得见那两档的视频流。如果它认"第一条 is_video"，
// 就会绑上流 0（800k 那档的视频轨）——它在 read_header 的探测里还出了
// 几十帧，随后彻底干涸：画面播一下就永远冻住，无错误、无降级。
// 与一个此前出现过的封面图缺陷同一形状。
TEST_CASE(track_player_never_binds_a_discarded_variant_stream) {
    syp::test::Watchdog wd("track_player_never_binds_a_discarded_variant_stream",
                           60000, 180000, "卡住多半在分片的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_multi", "/hls", kMultiFiles);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_discard_bind");
    dl.cache_dir = cache.path.c_str();
    HlsOptions opts;
    opts.max_bandwidth_bps = 500000;

    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, opts, &err);
    REQUIRE(p != nullptr);

    // 先确认现场真的具备区分力：被 discard 的视频轨确实还在 tracks() 里，
    // 而且**排在选中那条之前**（否则这条用例测不到东西，会假绿）。
    int32_t first_video = -1, selected_video = -1;
    for (const auto& t : p->tracks()) {
        if (!t.is_video) continue;
        if (first_video < 0) first_video = t.index;
        if (!t.discard)      { selected_video = t.index; break; }
    }
    REQUIRE(first_video >= 0);
    REQUIRE(selected_video >= 0);
    REQUIRE(first_video != selected_video);   // 有区分力的现场

    auto sink     = std::make_unique<syp::test::FakeAudioSink>();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto tp = TrackPlayer::create(std::move(p), std::move(sink),
                                  std::move(renderer), nullptr, &err, BufferPolicy::disabled());
    REQUIRE(tp != nullptr);

    CHECK_EQ(tp->video_track_index(), selected_video);
    CHECK(tp->video_track_index() != first_video);

    CHECK(!wd.fired());
}

// 视频那一半有 video_track_index() 可以直接问；音频那一半没有对应的
// 访问器（TrackPlayer 只公开 video_track_index()，见 track_player.h 里
// 它上方"不是给测试开的口子"那段——加一个 audio_track_index() 纯粹
// 为了测试，不符合那条理由）。所以这一条走**可观测的副作用**：
// 真的驱动播放，看进了 sink 的音频总量。
//
// vod_multi 是 4 秒、1 秒一片。被 discard 的那条音轨（流 1，800k 那档）
// 只有 read_header 探测时取的第 0 片，最多 1 秒；选中那条（流 3）有完整
// 4 秒。断言"至少 3 秒音频进了 sink"因此能把两者分开：把
// track_player.cpp 音频分支里的 `if (t.discard)` 去掉，主音轨会绑上流 1，
// 音频时钟在 1 秒处干涸，这条立刻变红（自检 D 实测）。
TEST_CASE(track_player_never_binds_a_discarded_variant_audio_stream) {
    syp::test::Watchdog wd("track_player_never_binds_a_discarded_variant_audio_stream",
                           60000, 180000, "卡住多半在分片的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_multi", "/hls", kMultiFiles);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_discard_bind_audio");
    dl.cache_dir = cache.path.c_str();
    HlsOptions opts;
    opts.max_bandwidth_bps = 500000;

    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, opts, &err);
    REQUIRE(p != nullptr);

    // 现场的区分力：第一条受管音轨（流 1）确实是被 discard 的那条，
    // 也就是说"认第一条音轨"这个写法在这里确实会绑错。
    int32_t first_audio = -1, selected_audio = -1;
    for (const auto& t : p->tracks()) {
        if (t.is_video || t.sample_rate <= 0) continue;
        if (first_audio < 0) first_audio = t.index;
        if (!t.discard)      { selected_audio = t.index; break; }
    }
    REQUIRE(first_audio >= 0);
    REQUIRE(selected_audio >= 0);
    REQUIRE(first_audio != selected_audio);

    auto sink = std::make_unique<syp::test::FakeAudioSink>();
    syp::test::FakeAudioSink* sink_raw = sink.get();
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto tp = TrackPlayer::create(std::move(p), std::move(sink),
                                  std::move(renderer), nullptr, &err, BufferPolicy::disabled());
    REQUIRE(tp != nullptr);
    CHECK(tp->clock_kind() == ClockKind::Audio);

    // advance() 是 FakeAudioSink 模拟"实时回调消费了这么多音频"的注入面；
    // 不推它的话环形缓冲写满（默认容量 1 秒）就再也写不进去，
    // written_frames() 会停在容量上，这条用例就测不到东西了。
    tp->play();
    bool saw_eof = false;
    for (int i = 0; i < 400000 && !saw_eof; ++i) {
        const PlayOutcome po = tp->step();
        REQUIRE(po.kind != PlayOutcome::Kind::Error);
        if (po.kind == PlayOutcome::Kind::Eof) saw_eof = true;
        sink_raw->advance(1000);
    }
    CHECK(saw_eof);

    // 至少 3 秒音频进了 sink —— 被 discard 那条最多只能给出 1 秒。
    const int64_t written_us = sink_raw->written_frames() * 1000000 / 44100;
    CHECK(written_us >= 3000000);

    CHECK(!wd.fired());
}

// EXT-X-MEDIA 分轨：音视频在不同播放列表里。这是目标源（Twitter 的
// amplify_video）的形状，与单播放列表完全不同（两条 media playlist
// 并发拉分片），必须单独钉一条。
//
// "EXT-X-MEDIA 的分轨不参与 discard，FFmpeg 自己按
// program 归组"——实测**基本成立但有一处要命的细节**：ffmpeg 的 hls
// muxer 给 agroup 的音频轨额外写了一条自引用的 EXT-X-STREAM-INF，于是
// 有两个 program，而**那条真正被消费的音频流同时属于两个 program**
// （本机 FFmpeg 8.1.2 实测：nb_streams=3 / nb_programs=2，
//  program[0]=[0:audio,1:video]、program[1]=[0:audio,2:audio]）。
// 若遍历非选中 program 把它的流全设成 AVDISCARD_ALL"，
// 选中档的音频会被另一档连坐丢掉——分轨源当场变成没有声音。
// select_variant() 因此先收"选中档要保留的流"再去 discard。
//
// 【判据是 n_audio == 1，不是 af > 0——别把杀手归错】实测：去掉
// keep[] 连坐防护之后 `af > 0` **保持绿**。原因是下面的 drain 循环没有
// 跳过 t.discard，把 read_header 探测阶段（hls.c:2240 起为每一档开
// demuxer 探首片）已经解出来的那约 1 秒帧也算了进去——被 discard 的轨
// 不是"一帧都没有"，是"出一点点然后干涸"。
// vf/af 在这条用例里只是**活性检查**（"整条链路真的在出帧，不是空转"），
// 真正把缺陷钉住的是 n_audio == 1 / n_video == 1。
// 不去改 drain 循环让 vf/af 变成判据：那会改变用例语义，而现在的判据
// 已经够硬（自检 E 实测 n_audio 变 0）。
TEST_CASE(demuxed_source_binds_both_tracks_and_plays_both) {
    syp::test::Watchdog wd("demuxed_source_binds_both_tracks_and_plays_both",
                           60000, 180000, "卡住多半在两条播放列表的并发分片 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_demuxed", "/hls", kDemuxedFiles);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_vod_demuxed");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);

    // 两条轨都被托管、都没被 discard
    int32_t n_video = 0, n_audio = 0;
    for (const auto& t : p->tracks()) {
        if (t.discard) continue;
        if (t.is_video)             ++n_video;
        else if (t.sample_rate > 0) ++n_audio;
    }
    CHECK_EQ(n_video, 1);
    CHECK_EQ(n_audio, 1);

    // 两条轨都真的出帧——只数视频会让"音频播放列表压根没被拉"假绿。
    int64_t vf = 0, af = 0;
    bool saw_eof = false;
    for (int i = 0; i < 200000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        REQUIRE(so.kind != StepOutcome::Kind::Error);
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        for (const auto& t : p->tracks()) {
            while (p->pop_frame(t.index).has_value()) {
                if (t.is_video) ++vf; else ++af;
            }
        }
    }
    CHECK(saw_eof);
    CHECK(vf > 0);
    CHECK(af > 0);

    // 两条播放列表都被请求过 —— 服务端侧实证
    CHECK(srv.requests_for("/hls/media_video.m3u8") >= 1);
    CHECK(srv.requests_for("/hls/media_audio.m3u8") >= 1);

    CHECK(!wd.fired());
}

// 【选轨必须先过滤掉不含视频流的 program】
//
// vod_demuxed 有两个 program：program[0] BANDWIDTH=1445528（含视频），
// program[1] BANDWIDTH=131578（ffmpeg hls muxer 给 agroup 音频轨写的
// 自引用 EXT-X-STREAM-INF，纯音频）。这是标准行为，真实 CDN 上同样有。
//
// 把 max_bandwidth_bps 设成 100000（低于两档），"一档都不满足时选
// BANDWIDTH 最小的那档"这条兜底就会被触发。**不过滤的话它必然选中
// program[1]**，视频轨被 discard —— 表现是"有声音、没画面"，而且看起来
// 完全像一次正常的低码率降级，不像 bug。
//
// 把 select_variant() 里的 program_has_video 过滤去掉，下面 **n_video==1**
// 立刻变红（自检 C 实测：left=0）。
//
// 【杀手只有 n_video==1，vf > 0 不是】实测：同一个变异下 `vf > 0`
// **保持绿**——drain 循环没有跳过 t.discard，read_header 探测阶段解出来的
// 那约 1 秒视频帧被算了进去。vf/af 在这里只是活性检查，理由同上一条用例。
TEST_CASE(lowest_bandwidth_fallback_never_picks_an_audio_only_variant) {
    syp::test::Watchdog wd("lowest_bandwidth_fallback_never_picks_an_audio_only_variant",
                           60000, 180000, "卡住多半在分片的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_demuxed", "/hls", kDemuxedFiles);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_audio_only_variant");
    dl.cache_dir = cache.path.c_str();

    HlsOptions opts;
    opts.max_bandwidth_bps = 100000;   // 低于 131578 与 1445528，两档都不满足
    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, opts, &err);
    REQUIRE(p != nullptr);

    int32_t n_video = 0, n_audio = 0;
    for (const auto& t : p->tracks()) {
        if (t.discard) continue;
        if (t.is_video)             ++n_video;
        else if (t.sample_rate > 0) ++n_audio;
    }
    CHECK_EQ(n_video, 1);   // 兜底选的是含视频的那档，不是自引用的音频档
    CHECK_EQ(n_audio, 1);

    // 只看 tracks() 不够：视频轨"在 tracks() 里且没被 discard"跟"真的
    // 出帧"是两件事。选错档时 FFmpeg 那条播放列表整个不再取分片，
    // 视频轨会一帧都出不来。
    int64_t vf = 0, af = 0;
    bool saw_eof = false;
    for (int i = 0; i < 200000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        REQUIRE(so.kind != StepOutcome::Kind::Error);
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        for (const auto& t : p->tracks()) {
            while (p->pop_frame(t.index).has_value()) {
                if (t.is_video) ++vf; else ++af;
            }
        }
    }
    CHECK(saw_eof);
    CHECK(vf > 0);
    CHECK(af > 0);

    CHECK(!wd.fired());
}

// 直播语义：播放列表在两次请求之间变化，FFmpeg 的 hls 会
// 按 EXT-X-TARGETDURATION 周期性重拉——而**重拉能拿到新内容，完全取决于
// 播放列表通道真的零缓存**。走 syp_source 的话第二次会命中缓存、一个请求
// 都不发（source_bridge.cpp:911），直播就冻在第一份播放列表上。
//
// 【服务端侧的计数断言不能省】"内容变了"不足以证明"真的又发了一次请求"——
// 一个把播放列表整份缓存下来、只是碰巧第一次就拿到了新版本的实现，靠
// "看到 seg3 了"是分不出来的。唯一有辨别力的是
// srv.requests_for("/hls/media.m3u8") 在换路由之后**继续增长**。
TEST_CASE(live_playlist_is_refetched_and_new_segments_are_picked_up) {
    syp::test::Watchdog wd("live_playlist_is_refetched_and_new_segments_are_picked_up",
                           60000, 180000,
                           "卡住多半在 hls 的播放列表重拉等待（reload_interval）上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    serve_live_segments(srv, "/hls");
    srv.set_route("/hls/master.m3u8",
                  read_file(fixture_dir() + "/hls/vod_single/master.m3u8"),
                  "application/vnd.apple.mpegurl");
    // 第一版滑动窗口：只有 seg0/seg1，无 ENDLIST（= 直播）。
    srv.set_route("/hls/media.m3u8", bytes_of(live_media_playlist({0, 1}, "")),
                  "application/vnd.apple.mpegurl");

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_live_refresh");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(srv.url("/hls/master.m3u8"), PipelineConfig{},
                                  dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);

    // 直播没有总时长。
    // 【Pipeline 没有 duration_us() 这个方法】总时长是调用方从 tracks() 里
    // 取最大值算出来的（demo/shared/bridge.mm:285 就是这么做的）。这条用例
    // 照同样的方式算，不顺手给 Pipeline 加一个访问器。
    //
    // 【单看 == 0 是一条空断言，必须配对照组】用一个变异场景
    // （给直播播放列表补上 EXT-X-ENDLIST，即把它变回点播）实测：本用例另外
    // 三条断言全红，**唯独 == 0 照样绿**——因为这套 fixture 的分片是
    // fMP4，每片的 moof 里没有整体时长，点播下 tracks() 的 duration_us
    // 同样是 0。也就是说它当时并没有在测"直播没有总时长"，只是在复述一个
    // 跟直播与否无关的事实。
    //
    // 所以判据改成"直播 == 0 **且** 同一份内容做成的非 HLS 单文件 > 0"：
    // 对照组证明这条判据在别处确实能取到非零值（不是恒真），直播这边的 0
    // 才有意义。这是这套素材上能做到的最强形状；要让 HLS 点播本身给出
    // 非零 duration，得换一套带整体时长的素材，不在本任务范围内——如实
    // 标注在此，别把这条读成"HLS 点播有总时长而直播没有"。
    int64_t duration_us = 0;
    for (const auto& t : p->tracks()) {
        if (t.duration_us > duration_us) duration_us = t.duration_us;
    }
    CHECK_EQ(duration_us, static_cast<int64_t>(0));
    CHECK(max_track_duration_us_of_source() > 0);   // 对照组：判据不是恒真

    // 【换路由的时机必须由观测决定，不能"先跑 N 步再换"】step() 在直播下
    // 是会**阻塞**的：分片吃完之后 hls 会在 reload_interval 里等下一次
    // 重拉。写死步数的话，只要那几步恰好落进一次等待，用例就再也走不到
    // 换路由这一行。所以条件是"seg1 已经被服务端供过体"——那一刻播放列表
    // 里的两片都到手了，下一件事必然是重拉播放列表。
    int64_t reqs_before = 0;
    bool    swapped     = false;
    bool    saw_seg3    = false;
    bool    saw_eof     = false;
    bool    saw_error   = false;
    int64_t frames      = 0;
    for (int i = 0; i < 60000 && !saw_seg3; ++i) {
        if (!swapped && srv.requests_for("/hls/seg1.m4s") > 0) {
            reqs_before = srv.requests_for("/hls/media.m3u8");
            srv.set_route("/hls/media.m3u8", bytes_of(live_media_playlist({2, 3}, "")),
                          "application/vnd.apple.mpegurl");
            swapped = true;
        }
        const StepOutcome so = p->step();
        if (so.kind == StepOutcome::Kind::Error) { saw_error = true; break; }
        if (so.kind == StepOutcome::Kind::Eof)   { saw_eof   = true; break; }
        drain_all_tracks(*p, &frames);
        saw_seg3 = srv.requests_for("/hls/seg3.m4s") > 0;
    }

    REQUIRE(swapped);                 // 现场真的具备区分力
    CHECK(!saw_error);
    // 直播不该报 EOF —— 滑动窗口里还有内容。
    CHECK(!saw_eof);
    // 播放列表真的被重拉了（服务端侧实证，不是靠"内容变了"推断）。
    CHECK(srv.requests_for("/hls/media.m3u8") > reqs_before);
    // 新分片真的被取了。
    CHECK(saw_seg3);
    // 活性：整条链路确实在出帧，不是空转。
    CHECK(frames > 0);

    CHECK(!wd.fired());
}

// AvioBridge::request_abort() 此前没有接进任何一条 Pipeline
// 的打开路径，网络卡死时 av_read_frame() 会无限期阻塞。直播是长连接、持续
// 拉流，用户导航离开的概率远高于点播——这条在 HLS 下是必要条件，不是
// "顺带修"。
//
// 【超时必须调大，否则这条用例没有辨别力】dl 层默认 read_timeout_ms=15000，
// 而断言是"5 秒内返回"。要是把超时设成 1 秒，接不接线都绿。这里设 30 秒：
// 只有中止接线真的生效才可能在 5 秒内返回。
TEST_CASE(request_abort_unblocks_a_hanging_segment_read) {
    syp::test::Watchdog wd("request_abort_unblocks_a_hanging_segment_read",
                           60000, 180000,
                           "卡住说明 request_abort() 没能打断分片的阻塞读");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_single", "/hls", kSingleFiles);

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size        = sizeof(syp_config);
    dl.connect_timeout_ms = 30000;
    dl.read_timeout_ms    = 30000;
    const syp::probe::TempCacheDir cache("hls_abort_segment");
    dl.cache_dir = cache.path.c_str();

    // 源素材的完整帧数——下面用它来证明"这次播放确实被打断了、没播完"。
    const int64_t expected = expected_video_frames_of_source();
    REQUIRE(expected > 0);

    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);

    // 打开成功之后才把服务端切成"永不响应"——open 阶段（read_header +
    // find_stream_info）要能正常拿到 master/media/init/seg0，否则
    // create_hls 自己就失败了，测不到 step() 里的那条阻塞路径。
    make_it_hang(srv);

    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        p->request_abort();
    });

    const auto t0 = std::chrono::steady_clock::now();
    bool       saw_error    = false;
    bool       saw_eof      = false;
    syp_status abort_status = SYP_OK;
    int64_t    frames       = 0;
    for (int i = 0; i < 100000; ++i) {
        const StepOutcome so = p->step();
        if (so.kind == StepOutcome::Kind::Error) {
            saw_error    = true;
            abort_status = so.status;
            break;
        }
        if (so.kind == StepOutcome::Kind::Eof)   { saw_eof   = true; break; }
        // 不排空的话队列很快堆满，step() 恒返回 Blocked，循环在几微秒里
        // 跑完——那样它压根没碰到阻塞 IO，这条用例会假绿。
        drain_all_tracks(*p, &frames);
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    killer.join();

    // 【saw_error 是本条的区分力所在，不能省】只断言"5 秒内返回"的话，
    // 一个"open 阶段就把四片全读完了、step() 一次网络 IO 都没发"的现场
    // 也会绿。必须确认它**真的**撞上了那条永不响应的连接、并且是被中止
    // 打断的（EOF 不算：那说明它根本没走到 hang 上）。
    // 【中止必须能在返回值上分辨出来】FFmpeg 那边天然给不出这个区分：
    // AvioBridge 的读回调返回 AVERROR_EXIT，嵌套的 mov 解封装器把它当成
    // 这一片没了，hls 随后判定播放列表放完、在顶层返回 AVERROR_EOF，于是
    // Demuxer::read() 走的是 Eof 分支。照抄的话调用方拿到的是"正常播完
    // 了"——demo 壳的 bridge.mm 正是把 Kind::Eof 转成 onEof（「播放完成」），
    // 用户导航离开会显示成播完了。Pipeline 因此自己记住 aborted_ 并在
    // step() 的 Eof 出口改写成 Error/SYP_ERR_CANCELED。
    // 把 pipeline.cpp 里那段改写注掉，下面两条立刻变红（反向自检可验证）。
    CHECK(saw_error);
    CHECK(!saw_eof);
    CHECK_EQ(static_cast<int>(abort_status), static_cast<int>(SYP_ERR_CANCELED));

    // 【"它真的撞上了那条永不响应的连接"——本条的结构性区分力】
    // 只断言耗时的话，一个"open 阶段就把四片全读完、step() 一次网络 IO 都
    // 没发"的现场也会绿。源素材共 expected 帧；这里只可能拿到 open 阶段
    // 已经落袋的那一小截（实测 25 帧 ≈ 1 片），远少于 expected。
    CHECK(frames > 0);
    CHECK(frames < expected);

    // 关键断言：它真的返回了，而且是在几秒内，不是挂到 30 秒的读超时
    // 或者看门狗上。300ms（killer 开火）对 30000ms（读超时）有 100 倍余量，
    // 5 秒这条线两边都碰不到。
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 5000);

    CHECK(!wd.fired());
}

// 【播放列表那条通道的中止是单独一个洞，必须单独钉】
// open_playlist() 走的是 fetch_playlist()，它只受自身 read_timeout_ms 约束，
// **不看 abort_**——看门狗在播放列表抓取期间开火完全没有效果。分片那条在
// HlsSession::open_segment/AvioBridge 里有传播，播放列表那条没有。
//
// 【为什么要两台 server】LoopbackConfig::hang 是**整机**开关，一台机器上
// 分不出"卡在播放列表"还是"卡在分片"。这里把播放列表（master/media）放在
// srv_pl、分片放在 srv_seg，只让 srv_pl 卡住：此时唯一能阻塞的就是一次
// 播放列表重拉，分片通道始终健康。没有这个分离，用例会被分片那条已经接好
// 的路径顺带弄绿，测不到本条要测的东西。
TEST_CASE(request_abort_unblocks_a_hanging_playlist_fetch) {
    syp::test::Watchdog wd("request_abort_unblocks_a_hanging_playlist_fetch",
                           60000, 180000,
                           "卡住说明 request_abort() 没能打断 fetch_playlist");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv_seg;                       // 分片，始终健康
    serve_live_segments(srv_seg, "/media");
    const std::string seg_prefix = srv_seg.url("/media") + "/";

    LoopbackServer srv_pl;                        // 播放列表，稍后整机卡住
    srv_pl.set_route("/hls/master.m3u8",
                     bytes_of("#EXTM3U\n#EXT-X-VERSION:7\n"
                              "#EXT-X-STREAM-INF:BANDWIDTH=800000\n"
                              "media.m3u8\n"),
                     "application/vnd.apple.mpegurl");
    srv_pl.set_route("/hls/media.m3u8",
                     bytes_of(live_media_playlist({0, 1}, seg_prefix)),
                     "application/vnd.apple.mpegurl");

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size        = sizeof(syp_config);
    dl.connect_timeout_ms = 30000;
    dl.read_timeout_ms    = 30000;
    const syp::probe::TempCacheDir cache("hls_abort_playlist");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(srv_pl.url("/hls/master.m3u8"), PipelineConfig{},
                                  dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);

    // 等两片都到手，再把播放列表那台切成永不响应——下一件事必然是重拉
    // 播放列表，而且只可能卡在那里。
    //
    // 【killer 睡 3 秒而不是 300 毫秒，这是实测校正过的，别调回去】
    // hls.c 的重拉等待循环自己每 100ms 就 ff_check_interrupt() 一次
    // （等到 reload_interval 到点才去 io_open）。看门狗要是在那个**等待
    // 窗口**里开火，hls 当场返回 AVERROR_EXIT——压根没走进
    // open_playlist/fetch_playlist，本条要钉的那段轮询一次都没被用到。
    // 实测：睡 300ms 时，把 fetch_playlist 的中止轮询整段注掉，这条用例
    // **照样绿**（反向自检 B 第一轮）。3 秒稳稳越过 reload_interval
    // （TARGETDURATION=1，hls 最多等 1 秒），确保开火时确实卡在
    // fetch_playlist 里面。
    // 门限相应放到 10 秒：绿的现场 ~3 秒，没接线的现场要等满 30 秒读超时。
    bool       hung            = false;
    bool       saw_error       = false;
    bool       saw_eof         = false;
    syp_status abort_status    = SYP_OK;
    int64_t    pl_reqs_at_hang = 0;
    int64_t    frames          = 0;
    std::thread killer;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        if (!hung && srv_seg.requests_for("/media/seg1.m4s") > 0) {
            // 切 hang 的那一刻服务端**收到过**的播放列表请求数——末尾要
            // 断言它之后又涨过。用 requests_received_for
            // 而不是 requests_for：后者在供完体之后才计数，而我们要数的
            // 恰恰是那一次被晾住、永远供不完的请求。
            pl_reqs_at_hang = srv_pl.requests_received_for("/hls/media.m3u8");
            make_it_hang(srv_pl);
            hung = true;
            t0   = std::chrono::steady_clock::now();
            killer = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                p->request_abort();
            });
        }
        const StepOutcome so = p->step();
        if (so.kind == StepOutcome::Kind::Error) {
            saw_error    = true;
            abort_status = so.status;
            break;
        }
        if (so.kind == StepOutcome::Kind::Eof)   { saw_eof   = true; break; }
        drain_all_tracks(*p, &frames);
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    if (killer.joinable()) killer.join();

    REQUIRE(hung);                    // 现场真的具备区分力
    // 中止在返回值上必须分辨得出来，同上一条用例。
    CHECK(saw_error);
    CHECK(!saw_eof);
    CHECK_EQ(static_cast<int>(abort_status), static_cast<int>(SYP_ERR_CANCELED));
    //   · 播放列表通道压根没卡住 ⇒ 一直重拉，既不 Eof 也不 Error，循环跑满
    //     10 万步撞软看门狗 ⇒ 红；
    //   · 卡住了但中止没接线 ⇒ 等满 30 秒读超时才终止 ⇒ 耗时断言红
    //     （反向自检 B 第二轮实测 30028ms）；
    //   · 卡住了且中止接线生效 ⇒ ~3 秒（killer 的睡眠时间）终止 ⇒ 绿。
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 10000);

    // 【这条才是"卡住之后真的进过 fetch_playlist"的结构性
    // 保证，别退回成 `>= 1`】`>= 1` 被**开局那次**请求就满足了，证明不了
    // 卡住之后又发生过一次。没有这条的话，将来 reload_interval 或网络时序
    // 一变、开火重新落回 hls 自己的等待窗口（那里每 100ms 自己
    // ff_check_interrupt），用例会**静默退回假绿而不变红**——就是自检 B
    // 第一轮抓到的那个形态换个触发条件。
    //
    // 必须用 requests_received_for（收到即计数）而不是 requests_for
    // （供完体才计数）：被 hang 晾住的那次请求永远供不完，用后者写出来的
    // 是一条**恒不成立**的断言（第一版就是这么写的，实测直接红）。
    // 涨过 ⇒ 切 hang 之后服务端确实又收到了一次 /hls/media.m3u8 ⇒ hls 真的
    // 走进了 io_open → open_playlist → fetch_playlist，并且卡在了那里面。
    CHECK(srv_pl.requests_received_for("/hls/media.m3u8") > pl_reqs_at_hang);
    // 分片通道全程健康：两片都取到了，说明卡住的只可能是播放列表那条。
    CHECK(srv_seg.requests_for("/media/seg1.m4s") >= 1);
    CHECK(frames > 0);

    CHECK(!wd.fired());
}

// ---------------------------------------------------------------------------
// 错误分级 —— 拒绝加密流（拉密钥之前）+ 分片 404 不被当成 EOF
// ---------------------------------------------------------------------------

// 本版本不支持加密 HLS。要的是**明确报错**，不是崩、不是
// 静默播出噪音、不是"看起来在播但没画面"。而且要在**拉密钥之前**就拒——
// key.bin 故意不挂路由，requests_for 断言它一次都没被请求到，证明
// playlist_declares_encryption() 在把 body 交给 FFmpeg 之前就已经生效，
// 不是靠 FFmpeg 打开 key.bin 失败才连带失败。
TEST_CASE(encrypted_playlist_is_rejected_with_a_clear_error) {
    syp::test::Watchdog wd("encrypted_playlist_is_rejected_with_a_clear_error",
                           60000, 180000,
                           "卡住不该发生——加密判定在拿到 body 之后是纯内存扫描");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    srv.set_route("/hls/master.m3u8",
                 bytes_of("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n"
                          "#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS=\"avc1.64001f,mp4a.40.2\"\n"
                          "media.m3u8\n"),
                 "application/vnd.apple.mpegurl");
    srv.set_route("/hls/media.m3u8",
                 bytes_of("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:1\n"
                          "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
                          "#EXTINF:1.0,\nseg0.m4s\n#EXT-X-ENDLIST\n"),
                 "application/vnd.apple.mpegurl");
    // key.bin 与 seg0.m4s 都故意不挂路由——加密判定必须先于两者都失败。

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_encrypted_rejected");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(srv.url("/hls/master.m3u8"), PipelineConfig{}, dl,
                                  HlsOptions{}, &err);
    CHECK(p == nullptr);
    CHECK_EQ(static_cast<int>(err), static_cast<int>(SYP_ERR_NOT_IMPLEMENTED));
    // 密钥一次都没被请求——我们在拉密钥之前就拒了。
    CHECK_EQ(srv.requests_for("/hls/key.bin"), static_cast<int64_t>(0));

    CHECK(!wd.fired());
}

// 分片 404。最糟的表现是被当成 EOF——用户看到"播完了"，而其实是一半内容
// 没下下来。seg1.m4s 故意不挂路由：LoopbackServer 在路由模式下未命中的
// 路径自动 404。
TEST_CASE(segment_404_is_reported_not_silently_treated_as_eof) {
    syp::test::Watchdog wd("segment_404_is_reported_not_silently_treated_as_eof",
                           60000, 180000,
                           "卡住多半在 open_segment 的阻塞 IO 上，不该跟 404 判定有关");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string dir = fixture_dir() + "/hls/vod_single";
    srv.set_route("/hls/master.m3u8", read_file(dir + "/master.m3u8"),
                 "application/vnd.apple.mpegurl");
    srv.set_route("/hls/init.mp4", read_file(dir + "/init.mp4"), "video/mp4");
    srv.set_route("/hls/seg0.m4s", read_file(dir + "/seg0.m4s"), "video/mp4");
    // media.m3u8 现造：只挂 seg0（真实字节），seg1 不挂路由 ⇒ 404。
    // seg2/seg3 同样不挂——404 一旦被正确报出，压根不该再往后取。
    srv.set_route("/hls/media.m3u8",
                 bytes_of("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n"
                          "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                          "#EXT-X-MAP:URI=\"init.mp4\"\n"
                          "#EXTINF:1.000000,\nseg0.m4s\n"
                          "#EXTINF:1.000000,\nseg1.m4s\n"
                          "#EXT-X-ENDLIST\n"),
                 "application/vnd.apple.mpegurl");

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_segment_404");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(srv.url("/hls/master.m3u8"), PipelineConfig{}, dl,
                                  HlsOptions{}, &err);
    REQUIRE(p != nullptr);
    CHECK_EQ(static_cast<int>(err), static_cast<int>(SYP_OK));

    // request_abort() 不在本用例的路径上——404 必须自己走出 Error，不能靠
    // 那条"中止时把 Eof 改写成 Error"的机制来产生这个结果，否则测的
    // 是中止机制而不是 404 的错误传播。
    bool       saw_error   = false;
    bool       saw_eof     = false;
    syp_status err_status  = SYP_OK;
    int64_t    frames      = 0;
    for (int i = 0; i < 20000 && !saw_error && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        if (so.kind == StepOutcome::Kind::Error) { saw_error = true; err_status = so.status; }
        if (so.kind == StepOutcome::Kind::Eof)   saw_eof   = true;
        drain_all_tracks(*p, &frames);
    }
    CHECK(saw_error);
    CHECK(!saw_eof);
    // 报出去的错误码要分级到 SYP_ERR_HTTP_STATUS（服务端
    // 真的返回了 404），不能是一个叫"错误分级"的任务却把 HTTP 404 分级成
    // 通用的 SYP_ERR_IO——HlsSession::on_source_error() 挂在
    // syp_source_open() 的 on_error 回调上，拿到的正是这个精确值。
    CHECK_EQ(static_cast<int>(err_status), static_cast<int>(SYP_ERR_HTTP_STATUS));
    // seg0 的一片（25 帧）应该已经播出来了——404 命中的是 seg1，不该连累
    // seg0 已经解出来的帧。
    CHECK(frames > 0);

    CHECK(!wd.fired());
}

// C7：aborted_ 与 pending_segment_error_ 的**优先级**本身要被守卫。
//
// step() 的 Eof 出口上串着两条改写（pipeline.cpp:436 起）：先看
// aborted_（报 SYP_ERR_CANCELED），再看 hls_->pending_segment_error()
// （报那次分片失败的精确码）。两条单独都有用例覆盖
// （request_abort_unblocks_a_hanging_segment_read /
// segment_404_is_reported_not_silently_treated_as_eof），但**谁在前**
// 一直零覆盖：用一个变异场景把这两条对调，hls_e2e 14/14 + track_player
// 35/35 全绿。
//
// 顺序不是随意的。它是下面这三处"排除中止"防御分支的**唯一**理由：
//   · hls_session.cpp on_io_close 的 last != AVERROR_EXIT
//   · hls_session.cpp on_source_error 的 if (status == SYP_ERR_CANCELED) return;
//   · hls_session.cpp open_segment 的 effective != SYP_ERR_CANCELED
// 它们各自的注释都说"再记一遍会把用户主动中止误标成内容缺失"——那句话
// 只有在**反过来**的优先级下才成立；在正确的优先级下它们是冗余防御。
// 优先级一旦被悄悄对调，这三条就从"冗余"变成"唯一防线"，而它们全都
// 只拦 CANCELED、拦不住这里这种"先真 404、后中止"的叠加现场。
//
// 本用例把这个顺序钉死：同一条管线上先制造一次真实的分片 404（拿到
// Error(-21 HTTP_STATUS)），再 request_abort()，之后必须报
// Error(-3 CANCELED)——也就是 aborted_ 赢。把优先级对调，第二段断言变红。
TEST_CASE(abort_outranks_a_pending_segment_error_at_the_eof_gate) {
    syp::test::Watchdog wd("abort_outranks_a_pending_segment_error_at_the_eof_gate",
                           60000, 180000,
                           "卡住多半在 open_segment 的阻塞 IO 上，不该跟优先级判定有关");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string dir = fixture_dir() + "/hls/vod_single";
    srv.set_route("/hls/master.m3u8", read_file(dir + "/master.m3u8"),
                 "application/vnd.apple.mpegurl");
    srv.set_route("/hls/init.mp4", read_file(dir + "/init.mp4"), "video/mp4");
    srv.set_route("/hls/seg0.m4s", read_file(dir + "/seg0.m4s"), "video/mp4");
    // 与 segment_404_… 同一份现造播放列表：seg0 真实、seg1 不挂路由 ⇒ 404。
    srv.set_route("/hls/media.m3u8",
                 bytes_of("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n"
                          "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                          "#EXT-X-MAP:URI=\"init.mp4\"\n"
                          "#EXTINF:1.000000,\nseg0.m4s\n"
                          "#EXTINF:1.000000,\nseg1.m4s\n"
                          "#EXT-X-ENDLIST\n"),
                 "application/vnd.apple.mpegurl");

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_abort_vs_404");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(srv.url("/hls/master.m3u8"), PipelineConfig{}, dl,
                                  HlsOptions{}, &err);
    REQUIRE(p != nullptr);
    CHECK_EQ(static_cast<int>(err), static_cast<int>(SYP_OK));

    // 第一段：还没中止过，分片错误必须原样报出来（这是 pending_segment_error_
    // 那一条改写；它自己已有专门用例，这里只是把现场造到位）。
    bool       saw_error  = false;
    syp_status err_status = SYP_OK;
    int64_t    frames     = 0;
    for (int i = 0; i < 20000 && !saw_error; ++i) {
        const StepOutcome so = p->step();
        if (so.kind == StepOutcome::Kind::Error) { saw_error = true; err_status = so.status; }
        if (so.kind == StepOutcome::Kind::Eof)   break;   // 报了 Eof 就是那条改写整个没生效
        drain_all_tracks(*p, &frames);
    }
    REQUIRE(saw_error);
    CHECK_EQ(static_cast<int>(err_status), static_cast<int>(SYP_ERR_HTTP_STATUS));

    // 第二段：同一条管线上叠加一次用户主动中止。此后 step() 必须改口报
    // CANCELED——"你按了停止"优先于"有一片没下下来"。
    //
    // 【为什么这里不需要再有阻塞 IO】request_abort() 的另一半作用（打断
    // 正卡着的读）由 request_abort_unblocks_a_hanging_segment_read 覆盖；
    // 本用例只问 Eof 出口那两条改写的先后，现场必须是"两个条件同时成立"，
    // 所以刻意让管线停在已经出过错、也已经没有 IO 在跑的状态上。
    p->request_abort();
    bool       saw_second = false;
    syp_status second     = SYP_OK;
    for (int i = 0; i < 100 && !saw_second; ++i) {
        const StepOutcome so = p->step();
        if (so.kind == StepOutcome::Kind::Error) { saw_second = true; second = so.status; }
        if (so.kind == StepOutcome::Kind::Eof)   { saw_second = true; second = SYP_OK; }
        drain_all_tracks(*p, &frames);
    }
    REQUIRE(saw_second);
    CHECK_EQ(static_cast<int>(second), static_cast<int>(SYP_ERR_CANCELED));

    CHECK(!wd.fired());
}

// ---------------------------------------------------------------------------
// 播放阶段的播放列表失败 + seek 之后的错误状态
// ---------------------------------------------------------------------------

namespace {

// 一次 step() 循环的终态。frames 只数视频帧。
struct Terminal {
    bool       saw_error = false;
    bool       saw_eof   = false;
    syp_status status    = SYP_OK;
    int64_t    frames    = 0;
};

// 跑到 Error 或 Eof 为止（或步数耗尽，两者都为 false）。on_step 在每步
// step() 之前调一次，给直播用例按观测时机换路由。
template <typename F>
Terminal run_until_terminal(Pipeline& p, int max_steps, F&& on_step) {
    Terminal t;
    for (int i = 0; i < max_steps; ++i) {
        on_step();
        const StepOutcome so = p.step();
        if (so.kind == StepOutcome::Kind::Error) { t.saw_error = true; t.status = so.status; break; }
        if (so.kind == StepOutcome::Kind::Eof)   { t.saw_eof   = true; break; }
        drain_all_tracks(p, &t.frames);
    }
    return t;
}

Terminal run_until_terminal(Pipeline& p, int max_steps) {
    return run_until_terminal(p, max_steps, [] {});
}

// 直播现场：master + 只含 seg0/seg1 的直播 media playlist + 分片字节。
std::unique_ptr<Pipeline> open_live(LoopbackServer& srv, const syp::probe::TempCacheDir& cache) {
    serve_live_segments(srv, "/hls");
    srv.set_route("/hls/master.m3u8",
                  read_file(fixture_dir() + "/hls/vod_single/master.m3u8"),
                  "application/vnd.apple.mpegurl");
    srv.set_route("/hls/media.m3u8", bytes_of(live_media_playlist({0, 1}, "")),
                  "application/vnd.apple.mpegurl");
    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    dl.cache_dir   = cache.path.c_str();
    return Pipeline::create_hls(srv.url("/hls/master.m3u8"), PipelineConfig{}, dl,
                                HlsOptions{}, &err);
}

}  // namespace

// #47：直播重拉播放列表 404。hls.c:1607-1611 对"播放期间重拉失败"不重试，
// 直接结束这一路播放列表——所以停播本身已经发生，唯一的问题是报不报。
// 不报的话用户看到的是"播放完成"。
//
// 换路由的时机同 live_playlist_is_refetched…：seg1 已经供过体 ⇒ 下一件事
// 必然是重拉播放列表。
TEST_CASE(live_playlist_refetch_404_is_reported_not_silently_treated_as_eof) {
    syp::test::Watchdog wd("live_playlist_refetch_404_is_reported_not_silently_treated_as_eof",
                           60000, 180000,
                           "卡住多半在 hls 的播放列表重拉等待（reload_interval）上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const syp::probe::TempCacheDir cache("hls_live_refetch_404");
    auto p = open_live(srv, cache);
    REQUIRE(p != nullptr);

    bool removed = false;
    const Terminal t = run_until_terminal(*p, 60000, [&] {
        if (!removed && srv.requests_for("/hls/seg1.m4s") > 0) {
            srv.remove_route("/hls/media.m3u8");
            removed = true;
        }
    });

    REQUIRE(removed);                 // 现场真的具备区分力
    CHECK(t.saw_error);
    CHECK(!t.saw_eof);
    CHECK_EQ(static_cast<int>(t.status), static_cast<int>(SYP_ERR_HTTP_STATUS));
    CHECK(t.frames > 0);              // seg0/seg1 照常出过帧

    CHECK(!wd.fired());
}

// #46：直播中途播放列表变加密。安全那一半（不取密钥）今天已经成立，这条
// 同时守住报错那一半与安全那一半。
TEST_CASE(live_playlist_turning_encrypted_mid_stream_is_reported) {
    syp::test::Watchdog wd("live_playlist_turning_encrypted_mid_stream_is_reported",
                           60000, 180000,
                           "卡住多半在 hls 的播放列表重拉等待（reload_interval）上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const syp::probe::TempCacheDir cache("hls_live_turns_encrypted");
    auto p = open_live(srv, cache);
    REQUIRE(p != nullptr);

    const std::string encrypted =
        "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:2\n#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:1.000000,\nseg2.m4s\n#EXTINF:1.000000,\nseg3.m4s\n";
    bool swapped = false;
    const Terminal t = run_until_terminal(*p, 60000, [&] {
        if (!swapped && srv.requests_for("/hls/seg1.m4s") > 0) {
            srv.set_route("/hls/media.m3u8", bytes_of(encrypted),
                          "application/vnd.apple.mpegurl");
            swapped = true;
        }
    });

    REQUIRE(swapped);
    CHECK(t.saw_error);
    CHECK(!t.saw_eof);
    CHECK_EQ(static_cast<int>(t.status), static_cast<int>(SYP_ERR_NOT_IMPLEMENTED));
    CHECK_EQ(srv.requests_for("/hls/key.bin"), static_cast<int64_t>(0));

    CHECK(!wd.fired());
}

// 重拉拿到 200，但 body 不是播放列表（CDN/代理的 HTML
// 错误页）。取回本身"成功"，失败发生在 FFmpeg 的 parse_playlist（首行不是
// #EXTM3U ⇒ AVERROR_INVALIDDATA，hls.c:849），结局与 404 完全一样：那一路
// 列表结束、干净 Eof。取回层面看不见这次失败，判据必须落在"直播的最后
// 一次取回有没有 ENDLIST"上。
TEST_CASE(live_playlist_refetch_returning_an_unparsable_body_is_reported) {
    syp::test::Watchdog wd("live_playlist_refetch_returning_an_unparsable_body_is_reported",
                           60000, 180000,
                           "卡住多半在 hls 的播放列表重拉等待（reload_interval）上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const syp::probe::TempCacheDir cache("hls_live_unparsable_refetch");
    auto p = open_live(srv, cache);
    REQUIRE(p != nullptr);

    bool swapped = false;
    const Terminal t = run_until_terminal(*p, 60000, [&] {
        if (!swapped && srv.requests_for("/hls/seg1.m4s") > 0) {
            srv.set_route("/hls/media.m3u8", bytes_of("<html>502 Bad Gateway</html>\n"),
                          "text/html");
            swapped = true;
        }
    });

    REQUIRE(swapped);
    CHECK(t.saw_error);
    CHECK(!t.saw_eof);
    CHECK_EQ(static_cast<int>(t.status), static_cast<int>(SYP_ERR_IO));

    CHECK(!wd.fired());
}

// 反方向的守卫：直播正常收尾（重拉时追加 EXT-X-ENDLIST）必须报干净的
// Eof。"最后一次取回没有 ENDLIST ⇒ 异常结束"这条判据一旦把 ENDLIST 认漏，
// 所有正常播完的直播都会被报成错误。
TEST_CASE(live_stream_ending_with_endlist_reports_a_clean_eof) {
    syp::test::Watchdog wd("live_stream_ending_with_endlist_reports_a_clean_eof",
                           60000, 180000,
                           "卡住多半在 hls 的播放列表重拉等待（reload_interval）上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const syp::probe::TempCacheDir cache("hls_live_ends_with_endlist");
    auto p = open_live(srv, cache);
    REQUIRE(p != nullptr);

    bool swapped = false;
    const Terminal t = run_until_terminal(*p, 60000, [&] {
        if (!swapped && srv.requests_for("/hls/seg1.m4s") > 0) {
            srv.set_route("/hls/media.m3u8",
                          bytes_of(live_media_playlist({2, 3}, "") + "#EXT-X-ENDLIST\n"),
                          "application/vnd.apple.mpegurl");
            swapped = true;
        }
    });

    REQUIRE(swapped);
    CHECK(t.saw_eof);
    CHECK(!t.saw_error);
    CHECK_EQ(static_cast<int>(t.status), static_cast<int>(SYP_OK));
    CHECK(srv.requests_for("/hls/seg3.m4s") > 0);   // 真的播到了追加的最后一片

    CHECK(!wd.fired());
}

// 打开阶段的播放列表失败**不能**被记成播放期错误。hls.c:2178-2183：
// master 下某一档播放列表拉取失败只会被标成 broken、跳过，其余档照常播。
// 这里挂两档坏的（一档 404、一档加密）加一档好的，必须干净地播到 Eof。
//
// 【它守的是 HlsSession 的"已进入播放阶段"判断】把那个判断去掉，本用例
// 会把一次完整的播放报成 Error。
TEST_CASE(variant_broken_at_open_does_not_poison_eof) {
    syp::test::Watchdog wd("variant_broken_at_open_does_not_poison_eof",
                           60000, 180000,
                           "卡住多半在 open_segment/open_playlist 的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string dir = fixture_dir() + "/hls/vod_single";
    for (const char* name : {"media.m3u8", "init.mp4", "seg0.m4s", "seg1.m4s", "seg2.m4s", "seg3.m4s"}) {
        srv.set_route(std::string("/hls/") + name, read_file(dir + "/" + name),
                      content_type_for(name));
    }
    srv.set_route("/hls/master.m3u8",
                  bytes_of("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS=\"avc1.64001f,mp4a.40.2\"\n"
                           "media.m3u8\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=600000,CODECS=\"avc1.64001f,mp4a.40.2\"\n"
                           "broken.m3u8\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=400000,CODECS=\"avc1.64001f,mp4a.40.2\"\n"
                           "enc.m3u8\n"),
                  "application/vnd.apple.mpegurl");
    // broken.m3u8 不挂路由 ⇒ 404。
    srv.set_route("/hls/enc.m3u8",
                  bytes_of("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:1\n"
                           "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
                           "#EXTINF:1.0,\nseg0.m4s\n#EXT-X-ENDLIST\n"),
                  "application/vnd.apple.mpegurl");

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_variant_broken_at_open");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(srv.url("/hls/master.m3u8"), PipelineConfig{}, dl,
                                  HlsOptions{}, &err);
    REQUIRE(p != nullptr);
    // 现场具备区分力：两档坏的在 open 阶段真的被请求过、真的失败过。
    CHECK(srv.requests_received_for("/hls/broken.m3u8") > 0);
    CHECK(srv.requests_for("/hls/enc.m3u8") > 0);

    const Terminal t = run_until_terminal(*p, 200000);
    CHECK(t.saw_eof);
    CHECK(!t.saw_error);
    CHECK_EQ(static_cast<int>(t.status), static_cast<int>(SYP_OK));
    CHECK_EQ(t.frames, expected_video_frames_of_source());

    CHECK(!wd.fired());
}

// 与 abort_outranks_a_pending_segment_error_… 同形：播放列表错误那条改写
// 同样必须让位于中止。
TEST_CASE(abort_outranks_a_pending_playlist_error_at_the_eof_gate) {
    syp::test::Watchdog wd("abort_outranks_a_pending_playlist_error_at_the_eof_gate",
                           60000, 180000,
                           "卡住多半在 hls 的播放列表重拉等待（reload_interval）上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const syp::probe::TempCacheDir cache("hls_abort_vs_playlist_404");
    auto p = open_live(srv, cache);
    REQUIRE(p != nullptr);

    bool removed = false;
    const Terminal first = run_until_terminal(*p, 60000, [&] {
        if (!removed && srv.requests_for("/hls/seg1.m4s") > 0) {
            srv.remove_route("/hls/media.m3u8");
            removed = true;
        }
    });
    REQUIRE(first.saw_error);
    CHECK_EQ(static_cast<int>(first.status), static_cast<int>(SYP_ERR_HTTP_STATUS));

    p->request_abort();
    const Terminal second = run_until_terminal(*p, 100);
    REQUIRE(second.saw_error || second.saw_eof);
    CHECK_EQ(static_cast<int>(second.status), static_cast<int>(SYP_ERR_CANCELED));

    CHECK(!wd.fired());
}

// #48：错误状态回答的是"**从上一次定位起**这一轮播放有没有漏内容"。
// 分片 404 → 把资源补回来 → seek 回去重播 → 这一轮内容完整，必须报 Eof。
// 旧行为（错误跨 seek 粘住）让有进度条的 demo 拖回去重看仍然报错，调用方
// 唯一的出路是销毁重建。中止（aborted_）不在此列，仍不随 seek 复位。
TEST_CASE(successful_seek_clears_a_pending_segment_error) {
    syp::test::Watchdog wd("successful_seek_clears_a_pending_segment_error",
                           60000, 180000,
                           "卡住多半在 open_segment 的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string dir = fixture_dir() + "/hls/vod_single";
    srv.set_route("/hls/master.m3u8", read_file(dir + "/master.m3u8"),
                  "application/vnd.apple.mpegurl");
    srv.set_route("/hls/init.mp4", read_file(dir + "/init.mp4"), "video/mp4");
    srv.set_route("/hls/seg0.m4s", read_file(dir + "/seg0.m4s"), "video/mp4");
    srv.set_route("/hls/media.m3u8",
                  bytes_of("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n"
                           "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                           "#EXT-X-MAP:URI=\"init.mp4\"\n"
                           "#EXTINF:1.000000,\nseg0.m4s\n"
                           "#EXTINF:1.000000,\nseg1.m4s\n"
                           "#EXT-X-ENDLIST\n"),
                  "application/vnd.apple.mpegurl");

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_seek_clears_segment_error");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(srv.url("/hls/master.m3u8"), PipelineConfig{}, dl,
                                  HlsOptions{}, &err);
    REQUIRE(p != nullptr);

    const Terminal first = run_until_terminal(*p, 20000);
    REQUIRE(first.saw_error);
    CHECK_EQ(static_cast<int>(first.status), static_cast<int>(SYP_ERR_HTTP_STATUS));

    srv.set_route("/hls/seg1.m4s", read_file(dir + "/seg1.m4s"), "video/mp4");
    REQUIRE(p->seek(1000000) == SYP_OK);

    const Terminal second = run_until_terminal(*p, 20000);
    CHECK(second.saw_eof);
    CHECK(!second.saw_error);
    CHECK_EQ(static_cast<int>(second.status), static_cast<int>(SYP_OK));
    CHECK(second.frames > 0);         // 这一轮真的重新播出了 seg1

    CHECK(!wd.fired());
}

// ---------------------------------------------------------------------------
// 非 HLS 的 URL 播放（demo 壳 -openURLString: 的形状）
// ---------------------------------------------------------------------------

// 放在本文件是因为这里有整套真实网络现场（LoopbackServer + Apple 后端 +
// TrackPlayer 夹具），不是因为它跟 HLS 有关。
//
// 形状逐字照 demo/shared/bridge.mm：syp_source → AvioBridge →
// Pipeline::create_avio(io_abort = AvioBridge::request_abort) → TrackPlayer，
// 一条泵线程反复 step()，另一条线程（demo 里是主线程上的 -close）
// TrackPlayer::request_abort()。服务端在每条响应发满 1MB 后停 60 秒，
// 泵线程最终会卡在 syp_source_read 里。
//
// 【判据有牙】dl 读超时设成 30 秒，断言"中止后 5 秒内泵线程退出且报
// CANCELED"。不交 io_abort 钩子时（修复前 demo 的写法）这一步要等满读
// 超时——已用变异测试验证过。
TEST_CASE(url_playback_request_abort_unblocks_a_pump_stuck_in_a_network_read) {
    syp::test::Watchdog wd("url_playback_request_abort_unblocks_a_pump_stuck_in_a_network_read",
                           60000, 180000, "卡住说明 io_abort 钩子没有打断 syp_source_read");
    REQUIRE(syp::probe::ensure_apple_backend());

    syp::dl::test::LoopbackConfig lc;
    lc.body_file         = fixture_dir() + "/faststart.mp4";
    lc.pause_after_bytes = 1 << 20;
    lc.pause_ms          = 60000;
    LoopbackServer srv(lc);

    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size     = sizeof(syp_config);
    dl.read_timeout_ms = 30000;
    const syp::probe::TempCacheDir cache("url_abort_stuck_pump");
    dl.cache_dir = cache.path.c_str();

    syp_source* src = nullptr;
    REQUIRE(syp_source_open(&src, srv.url("/media.mp4").c_str(), nullptr, &dl, nullptr) == SYP_OK);
    auto avio = AvioBridge::create(src, 0);
    REQUIRE(avio != nullptr);
    AvioBridge* avio_raw = avio.get();

    syp_status err = SYP_OK;
    auto p = Pipeline::create_avio(avio->ctx(), PipelineConfig{}, &err,
                                   [avio_raw] { avio_raw->request_abort(); });
    REQUIRE(p != nullptr);
    auto renderer = std::make_unique<syp::test::FakeRenderer>(nullptr);
    auto tp = TrackPlayer::create(std::move(p), nullptr, std::move(renderer), nullptr, &err, BufferPolicy::disabled());
    REQUIRE(tp != nullptr);
    tp->play();

    using clock = std::chrono::steady_clock;
    const auto ms_since = [](clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t).count();
    };
    std::atomic<int64_t> last_return_ms{0};
    const auto t0 = clock::now();
    PlayOutcome last{};
    std::thread pump([&] {
        for (;;) {
            last = tp->step();
            last_return_ms.store(ms_since(t0));
            if (last.kind == PlayOutcome::Kind::Error || last.kind == PlayOutcome::Kind::Eof) break;
            if (last.kind == PlayOutcome::Kind::Waiting || last.kind == PlayOutcome::Kind::Blocked) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    // 等到泵线程在 step() 里停了 1.5 秒以上：step() 本身从不 sleep，这么长
    // 的沉默只能是卡在读里。
    bool stuck = false;
    for (int i = 0; i < 40000 && !stuck; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stuck = last_return_ms.load() > 0 && ms_since(t0) - last_return_ms.load() > 1500;
    }
    REQUIRE(stuck);   // 现场具备区分力

    const auto t_abort = clock::now();
    tp->request_abort();
    pump.join();
    const int64_t unblock_ms = ms_since(t_abort);
    std::printf("  [url-abort] pump returned %lld ms after request_abort()\n",
                static_cast<long long>(unblock_ms));

    CHECK(unblock_ms < 5000);
    CHECK(last.kind == PlayOutcome::Kind::Error);
    CHECK_EQ(static_cast<int>(last.status), static_cast<int>(SYP_ERR_CANCELED));

    tp.reset();
    avio.reset();
    syp_source_close(src);
    CHECK(!wd.fired());
}

// ---------------------------------------------------------------------------
// 结构性验收 —— 证明 FFmpeg 没有自己联网
// ---------------------------------------------------------------------------

// 把全局 syp_http_backend 换掉，析构时换**回原来那一个**。
//
// 【为什么必须是 RAII，而不是在用例末尾写一行 syp_set_http_backend(...)】
// tiny_test 的 REQUIRE 失败会提前结束用例，写在末尾的还原就不执行；全局
// 后端从此是 stub，**后续每一条用例**都会拿着 stub 去打 loopback——表现是
// "改了 A 用例导致 B 用例红"，本仓库里最难查的那一类。tests/
// test_source_bridge.cpp:186 的 Env 是同形状的先例。
//
// 【为什么还原成 prev 而不是 nullptr】ensure_apple_backend()
// （tools/syp_probe/scenarios.cpp:23）把"已经装好"记在函数局部 static 里，
// 第二次调用直接返回缓存的 true，**不会再 set 一次**。还原成 nullptr 的话，
// 本用例之后所有用例的 ensure_apple_backend() 都变成空操作，全局后端一直
// 是 nullptr——同样是"改了 A 用例导致 B 用例红"。
//
// 【为什么旧后端必须**按值**存下来，存指针是一次自赋值】这个陷阱曾经真实发生过：
// syp::dl::current_http_backend()（src/dl/source_bridge.h:188）返回的**不是**
// 调用方当初传进去的那个 syp_http_backend 的地址，而是 dl 层内部那块唯一的
// 按值存储 g_backend 的地址（source_bridge.cpp:155 `syp_http_backend g_backend{}`，
// :276 `return g_backend_set ? &g_backend : nullptr`）。而
// syp_set_http_backend(next) 做的是 `g_backend = *next`——**就地覆盖同一块存储**。
// 所以 `prev = current_http_backend()` 拿到的是 &g_backend；构造函数里
// syp_set_http_backend(next) 当场把 Apple 后端的内容抹掉换成 stub；析构时
// syp_set_http_backend(prev) 执行的是 `g_backend = g_backend`——一次自赋值，
// 什么也没还原。全局后端从此一直指着**已经析构的、住在本用例栈帧上的**
// StubBackend，后续任何一次 HTTP 都是穿进已释放内存。
//
// 这个错误今天零症状，唯一原因是本用例恰好排在 TEST_CASE 列表的最后一条；
// 下一个往文件末尾追加用例的人会撞上一次指不到任何真因的崩溃（实测：
// 追加一条与 segment_404_… 逐字同构的用例，SIGSEGV 于
// StubBackend::trampoline_create → pthread_mutex_lock，栈上那把 std::mutex
// 早已不在）。
//
// 修法只有一条：**拷贝一份 syp_http_backend 的值**，还原时把这份副本装回去。
// 副本的生存期由 guard 自己保证，与 dl 层那块存储无关。
struct HttpBackendGuard {
    syp_http_backend prev_{};            // 按值——不能是 const syp_http_backend*
    bool             had_prev_ = false;
    explicit HttpBackendGuard(const syp_http_backend* next) {
        if (const syp_http_backend* p = syp::dl::current_http_backend()) {
            prev_     = *p;              // 必须在 set 之前拷贝：set 会就地覆盖 *p
            had_prev_ = true;
        }
        syp_set_http_backend(next);
    }
    ~HttpBackendGuard() { syp_set_http_backend(had_prev_ ? &prev_ : nullptr); }
    HttpBackendGuard(const HttpBackendGuard&)            = delete;
    HttpBackendGuard& operator=(const HttpBackendGuard&) = delete;
};

TEST_CASE(every_byte_goes_through_our_dl_layer) {
    // 【最重要的一条验收】"绕过了 dl 层"是**无现象**
    // 的：缓存、预加载、统计全部失效，但视频照播、测试照绿。
    //
    // 【本文件其它用例为什么抓不住它】那些用例打的是 127.0.0.1 上的
    // LoopbackServer，而 FFmpeg 自己开 socket 打的**也是同一个地址**——
    // srv.requests_for() 对"谁开的 socket"零分辨率。用一个变异场景
    // 实测过：把 io_open 覆盖与 protocol_whitelist 一起拿掉，FFmpeg 自己
    // 开 socket，那三条 requests_for 断言全过、用例全绿。
    //
    // 【本用例怎么把这个洞堵上】两件事一起做：
    //   1. 主机名用 fake.invalid。`.invalid` 是 RFC 2606 保留的顶级域，
    //      **DNS 上永远不可解析**——任何真的 socket 都连不出去，这不是
    //      靠 whitelist 拦，是物理上走不通。
    //   2. 把全局 syp_http_backend 整个换成纯内存的 StubBackend，并按 URL
    //      挂上真实的 fixture 字节。dl 层的两条通道（播放列表走
    //      fetch_playlist(current_http_backend())、分片走 syp_source）都只
    //      经这一个后端出流量，所以"数据来自 stub" ≡ "数据经过了我们"。
    //
    // 于是两个方向都不会静默通过：
    //   · FFmpeg 自己开 socket ⇒ 解析不了 fake.invalid ⇒ 拿不到字节 ⇒ 红；
    //   · 拿到了字节 ⇒ 只可能来自 stub ⇒ 证明经过了 dl 层。
    syp::test::Watchdog wd("every_byte_goes_through_our_dl_layer", 60000, 180000,
                           "卡住多半在 StubBackend 的 worker 线程或 HlsSession 的阻塞 IO 上");
    // 先让 ensure_apple_backend() 的 static 落定，再装 stub：这样
    // HttpBackendGuard 存下的 prev 是 Apple 后端，用例结束后原样还回去。
    REQUIRE(syp::probe::ensure_apple_backend());

    // 对照基准在装 stub **之前**取——它走 create_file 读本地文件，与 HTTP
    // 无关，但放在前面能保证 stub 的请求计数里只有 HLS 这一条链路的请求。
    const int64_t expected = expected_video_frames_of_source();
    REQUIRE(expected > 0);

    const std::string dir  = fixture_dir() + "/hls/vod_single";
    const std::string base = "https://fake.invalid/hls/";

    syp::dl::test::StubBackend stub;
    // Async：分片通道是 FFmpeg 线程上的阻塞读，必须有真的 worker 线程投递
    // on_data/on_complete，Sync 模式要用例自己 pump()，这里没人 pump。
    stub.set_mode(syp::dl::test::StubBackend::Mode::Async);
    for (const std::string& name : kSingleFiles) {
        stub.add_resource(base + name, read_file(dir + "/" + name));
    }
    const HttpBackendGuard backend_guard(stub.backend());

    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_no_bypass");
    dl.cache_dir = cache.path.c_str();

    auto p = Pipeline::create_hls(base + "master.m3u8", PipelineConfig{}, dl,
                                  HlsOptions{}, &err);
    // 主机名根本不存在；能打开就只能是因为字节来自 stub。
    REQUIRE(p != nullptr);
    CHECK_EQ(static_cast<int>(err), static_cast<int>(SYP_OK));

    int64_t video_frames = 0;
    bool    saw_eof      = false;
    for (int i = 0; i < 200000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        REQUIRE(so.kind != StepOutcome::Kind::Error);
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        drain_all_tracks(*p, &video_frames);
    }
    CHECK(saw_eof);
    // 帧数与直接播源素材一致：不只是"打开成功"，是整条内容都从 stub 流过来
    // 并解出了同样多的帧。只看 frames > 0 的话，"取到首片就断"也会过。
    CHECK_EQ(video_frames, expected);

    // stub 侧实证：每一个资源都真的被问过。这里的 requests_for 与本文件
    // 上方那几条同名断言性质完全不同——stub 是**唯一**的字节来源，计数为 0
    // 就意味着这份字节根本没出现过。
    for (const std::string& name : kSingleFiles) {
        CHECK(stub.requests_for(base + name) >= 1);
    }

    // 没有任何一条请求落到资源表之外的 URL 上。落到表外 ⇒ stub 回 404 ⇒
    // 上面的帧数断言早就红了；这条是把它说成一句直接的话，顺带挡住
    // "多发了一条谁也没注意的请求"。
    const int total = stub.request_count();
    int in_table = 0;
    for (const std::string& name : kSingleFiles) in_table += stub.requests_for(base + name);
    CHECK_EQ(in_table, total);

    // 后端契约自查（on_complete 之后再回调 / handle 泄漏 / cancel 后没
    // complete）。dl 层是被测对象，它对后端的用法本身也得合规。
    // 必须在 Pipeline 析构之后查——句柄要等 SourceBridge::close() 才还。
    p.reset();
    std::string why;
    CHECK(stub.contract_ok(&why));
    if (!why.empty()) std::fprintf(stderr, "stub 契约违约：%s\n", why.c_str());

    // 双通道分流：跟 vod_single_variant_plays_to_eof 同一条判据，在"真实
    // 网络物理不可达"这个更强的前提下再钉一遍。实测：这个变异场景
    // 下只有这条缓存断言真的红了。
    const std::vector<std::string> urls = cached_urls(cache.path);
    for (const std::string& u : urls) {
        CHECK(!ends_with(u, ".m3u8"));
    }
    bool cached_a_segment = false;
    for (const std::string& u : urls) {
        if (contains(u, "seg0.m4s")) cached_a_segment = true;
    }
    CHECK(cached_a_segment);
    // 缓存里记的 URL 必须是 base 那一份（stub 的地址），顺带证明进缓存的
    // 就是 stub 发出来的那份字节。
    //
    // 【从 base 派生，不重写一遍主机名】实测：把 base 改掉而这里
    // 还写着旧主机名时，只有这一条单独红——那是一条"改了 A 却红在 B"的假
    // 失败，浪费的是下一个人的时间。
    for (const std::string& u : urls) {
        CHECK(contains(u, base));
    }

    CHECK(!wd.fired());
}

// ---------------------------------------------------------------------------
// 【守卫的守卫】HttpBackendGuard 本身是不是真的还原了？
//
// 上一条用例把全局 syp_http_backend 换成了一个**住在它栈帧上**的
// StubBackend。还原一旦失效（实测过的形态：按指针保存 ⇒ 析构时是一次
// 自赋值，见 HttpBackendGuard 上方长注释），全局后端就一直指着那块已经
// 释放的栈内存——而这件事**在本文件里零症状**，因为上一条恰好排在最后。
// 下一个往末尾追加用例的人才会撞上崩溃，且现场指不到任何真因。
//
// 这条用例花不了 1ms，作用是把那个「零症状」窗口堵死：它必须排在
// every_byte_goes_through_our_dl_layer **之后**（tiny_test 按注册顺序跑，
// 即源文件里的出现顺序）。**往本文件追加新用例时，把新用例加在这条之前。**
// ---------------------------------------------------------------------------
TEST_CASE(http_backend_guard_restored_the_apple_backend) {
    REQUIRE(syp::probe::ensure_apple_backend());
    const syp_http_backend* cur   = syp::dl::current_http_backend();
    const syp_http_backend* apple = syp_apple_http_backend();
    REQUIRE(cur != nullptr);
    REQUIRE(apple != nullptr);
    // 比函数指针而不是比 syp_http_backend* 本身：current_http_backend()
    // 返回的永远是 dl 层内部那块存储的地址，跟 syp_apple_http_backend()
    // 返回的地址**本来就不相等**，能分辨"装的是谁"的只有内容。
    CHECK(cur->create == apple->create);
    if (cur->create != apple->create) {
        std::fprintf(stderr,
                     "全局 HTTP 后端没有被还原成 Apple 后端："
                     "current->create=%p apple->create=%p —— "
                     "多半是某条用例的 HttpBackendGuard 还原失效了\n",
                     reinterpret_cast<const void*>(cur->create),
                     reinterpret_cast<const void*>(apple->create));
    }
}

// ---------------------------------------------------------------- 容量/TTL
//
// 【HLS 播放这一支的容量三件套】
// 播放侧的读回缝拍照点在 SYPBridge.mm 的 -openURLString:，发生在
// HlsSession::create() **之前**；create() 把 dl_cfg 整份拷进 dl_cfg_ 之后
// 还能再改一手，而 HLS 的**每一条媒体分片源**都是从这份拷贝开出去的
// （hls_session.cpp 的 open_segment()）。此前这条路径上一条断言都没有：
// 一个变异场景（在 cache_dir 改指之后把三个字段清零）实测
// `ctest 33/33` + `xcodebuild 82 tests, 0 failures` 双绿存活。后果不是预加载而是 **HLS 播放**——每条分片都不看上限、
// 不过期、不看可用空间，永久累积。
//
// 这一条是读回缝断言，落点在"配置长什么样"；下一条落在后果上。两条都要：
// 单靠读回缝挡不住"配置对了但这条路径根本不走 enforce_capacity"，单靠
// 行为用例则要跑完整条 FFmpeg 播放，诊断信息远不如这条直接。
TEST_CASE(hls_session_carries_capacity_and_ttl_into_its_segment_config) {
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size           = sizeof(syp_config);
    const syp::probe::TempCacheDir cache("hls_cap_seam");
    dl.cache_dir             = cache.path.c_str();
    dl.max_cache_bytes       = 12345678;
    dl.min_free_space_bytes  = 2345678;
    dl.cache_ttl_ms          = 1500;

    syp_status err = SYP_OK;
    // create() 只装配不 open，所以这条用例不碰网络、不需要 fixture。
    auto s = hls::HlsSession::create("http://127.0.0.1:9/m.m3u8", dl,
                                     HlsOptions{}, &err);
    REQUIRE(s != nullptr);
    const syp_config& got = s->dl_config_for_test();
    CHECK_EQ(got.max_cache_bytes, static_cast<int64_t>(12345678));
    CHECK_EQ(got.min_free_space_bytes, static_cast<int64_t>(2345678));
    CHECK_EQ(got.cache_ttl_ms, static_cast<int64_t>(1500));
    // cache_dir 已经改指本对象自己那份字符串，但内容必须逐字节相同——
    // 指针换了、内容也跟着换了的话，分片会落进另一份缓存。
    REQUIRE(got.cache_dir != nullptr);
    CHECK_EQ(std::string(got.cache_dir), cache.path);
    CHECK(got.cache_dir != dl.cache_dir);
}

// 行为用例：HLS 播放真的会按 max_cache_bytes 淘汰无人引用的旧条目。
//
// 判据取"victim 的 .idx 没了"而不是"目录总字节 <= 上限"：后者在分片自己
// 还开着时恒不成立（enforce_capacity 跳过被引用的 key），会把一条真用例
// 写成恒红的用例——与 preloader_capacity_actually_evicts_an_unreferenced_
// entry 同一条理由。
TEST_CASE(hls_playback_evicts_an_unreferenced_entry_per_max_cache_bytes) {
    syp::test::Watchdog wd("hls_playback_evicts_an_unreferenced_entry_per_max_cache_bytes",
                           60000, 180000, "卡住多半在分片的阻塞 IO 上");
    REQUIRE(syp::probe::ensure_apple_backend());

    LoopbackServer srv;
    const std::string master =
        serve_hls_tree(srv, fixture_dir() + "/hls/vod_single", "/hls", kSingleFiles);
    // victim 是一条与 HLS 无关的资源，暖完就没人引用了。
    constexpr size_t kVictimBytes = 64 * 1024;
    srv.set_route("/victim.bin", std::vector<uint8_t>(kVictimBytes, 0x77),
                  "application/octet-stream");

    const syp::probe::TempCacheDir cache("hls_cap_evict");
    const std::string victim = srv.url("/victim.bin");
    const std::filesystem::path victim_idx =
        std::filesystem::path(cache.path) /
        (syp::dl::CacheIndex::key_for_url(victim) + ".idx");

    // 1) 暖 victim（不限容量），关掉，磁盘上留着。
    {
        syp_config vdl{};
        syp_config_init(&vdl);
        vdl.struct_size      = sizeof(syp_config);
        vdl.cache_dir        = cache.path.c_str();
        vdl.max_cache_bytes  = 0;
        vdl.cache_ttl_ms     = 0;
        auto opened = syp::dl::SourceBridge::open(victim, nullptr, vdl, nullptr,
                                                  syp::dl::current_http_backend(),
                                                  syp::dl::system_clock());
        REQUIRE(opened.has_value());
        std::unique_ptr<syp::dl::SourceBridge> src = std::move(*opened);
        std::vector<uint8_t> buf(kVictimBytes);
        int32_t got = 0;
        while (got < static_cast<int32_t>(kVictimBytes)) {
            const int32_t n = src->read(buf.data() + got,
                                        static_cast<int32_t>(kVictimBytes) - got);
            if (n <= 0) break;
            got += n;
        }
        CHECK_EQ(got, static_cast<int32_t>(kVictimBytes));
        src->close();
    }
    REQUIRE(std::filesystem::exists(victim_idx));

    // 2) 上限远低于 victim 的体积；分片源落盘并关闭时必须把它淘汰掉。
    syp_status err = SYP_OK;
    syp_config dl{};
    syp_config_init(&dl);
    dl.struct_size     = sizeof(syp_config);
    dl.cache_dir       = cache.path.c_str();
    dl.max_cache_bytes = 4096;
    dl.cache_ttl_ms    = 0;

    auto p = Pipeline::create_hls(master, PipelineConfig{}, dl, HlsOptions{}, &err);
    REQUIRE(p != nullptr);
    int64_t video_frames = 0;
    bool    saw_eof      = false;
    for (int i = 0; i < 200000 && !saw_eof; ++i) {
        const StepOutcome so = p->step();
        REQUIRE(so.kind != StepOutcome::Kind::Error);
        if (so.kind == StepOutcome::Kind::Eof) saw_eof = true;
        drain_all_tracks(*p, &video_frames);
    }
    CHECK(saw_eof);
    p.reset();

    CHECK(!std::filesystem::exists(victim_idx));
    CHECK(!wd.fired());
}

int main() { return tiny_test_main(); }
