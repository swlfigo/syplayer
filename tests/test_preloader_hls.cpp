// test_preloader_hls.cpp — HLS 预加载（最小扫描器 + master→variant→分片）。
//
// 【扫描器的范围】只认 URI 与 EXTINF，不解析属性、不处理加密。它只用于
// 预加载：扫错的代价是白下或少下几个分片，**不影响播放**（播放走的是
// FFmpeg 的 hls 解封装器 + HlsSession 接管 IO）。所以这里的用例钉的是
// "遇到看不懂的东西不会乱来"，不是"解析得跟 FFmpeg 一致"。
//
// 【确定性】断言用 requests_received_for()（收到即计数，对被扣住的请求同样
// 有效）+ 谓词式等待；超时只把挂死变成失败，不承担判据。
#include "tiny_test.h"
#include "support/hls_cases.h"
#include "support/loopback_server.h"
#include "support/watchdog.h"

#include <dl/clock.h>
#include <dl/cache_index.h>
#include <dl/m3u8_scan.h>
#include <dl/preloader.h>
#include <dl/source_bridge.h>
#include <platform/apple/apple_http_backend.h>

#include <syplayer/syp_config.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <vector>

using syp::dl::PlaylistScan;
using syp::dl::PreloadConfig;
using syp::dl::PreloadPriority;
using syp::dl::PreloadState;
using syp::dl::Preloader;
using syp::dl::is_playlist_url;
using syp::dl::resolve_url;
using syp::dl::scan_playlist;
using syp::dl::segments_for_ms;
using syp::dl::test::LoopbackConfig;
using syp::dl::test::LoopbackServer;

namespace {

std::vector<uint8_t> text_body(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

}  // namespace

// 判据表在 tests/support/hls_cases.h，media 侧的 test_hls_url_rewrite.cpp
// include 的是**同一个文件**。dl 与 media 各有一份实现（dl 不许依赖 media），
// 共享这一张表是它们不漂移的唯一保障：两份字面量只挡得住"改了代码忘了改
// 用例"，挡不住"改了代码顺手把自己那张表也改了"——加密判据就是这么漂的。
TEST_CASE(is_playlist_url_matches_the_shared_table) {
    for (const auto& c : syp::test::hls_cases::kChannelCases) {
        if (is_playlist_url(c.url) != c.playlist) {
            tiny_test::fail(__FILE__, __LINE__, "is_playlist_url", c.url);
        }
    }
}

// 加密判据同样共享一张表。**这条判据带着安全后果**：漏判的代价在 media 侧
// 是去请求密钥，在 dl 侧是白暖一批 media 层根本不会打开的分片。
TEST_CASE(scan_encrypted_matches_the_shared_table) {
    for (const auto& c : syp::test::hls_cases::kEncryptionCases) {
        const bool got = scan_playlist(c.text, "http://h/m.m3u8").encrypted;
        if (got != c.encrypted) {
            tiny_test::fail(__FILE__, __LINE__, "scan_playlist(...).encrypted", c.name);
        }
    }
}

// 【首行闸的结构性后果，dl 独有】media 侧没有对应的函数（它不展开播放列表），
// 所以这条只能在 dl 侧测：首行不是 "#EXTM3U" 的 body，FFmpeg 在
// parse_playlist 的第一行就整份拒收（hls.c:849），我们连一个可暖的东西都
// 不产出——不认 master、不认变体、不认 #EXT-X-MAP、不收分片。
TEST_CASE(scan_yields_nothing_preloadable_without_the_extm3u_header) {
    // 带 BOM 的 media 播放列表：上一轮会剥掉 BOM 照常展开，这一轮不展开。
    const std::string bom =
        "\xEF\xBB\xBF#EXTM3U\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:4,\nseg0.m4s\n#EXTINF:4,\nseg1.m4s\n#EXT-X-ENDLIST\n";
    const auto b = scan_playlist(bom, "http://h/v/media.m3u8");
    CHECK(!b.is_master);
    CHECK(b.map_uri.empty());
    CHECK_EQ(b.segments.size(), static_cast<size_t>(0));
    CHECK(!b.encrypted);   // 没有 EXT-X-KEY；闸不改 encrypted，见 m3u8_scan.cpp

    // 带 BOM 的 master：不能认出 is_master，否则会去抓那条变体。
    const std::string bom_master =
        "\xEF\xBB\xBF#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nlow/media.m3u8\n";
    const auto m = scan_playlist(bom_master, "http://h/master.m3u8");
    CHECK(!m.is_master);
    CHECK(m.first_variant.empty());

    // 其它不过闸的形状：HTML 错误页、首行前有空行/空格、小写首行。
    for (const char* text : {
             "<html>not a playlist</html>\n#EXTINF:4,\nseg0.ts\n",
             "\n#EXTM3U\n#EXTINF:4,\nseg0.ts\n",
             " #EXTM3U\n#EXTINF:4,\nseg0.ts\n",
             "#extm3u\n#EXTINF:4,\nseg0.ts\n",
         }) {
        const auto s = scan_playlist(text, "http://h/v/media.m3u8");
        if (!s.segments.empty()) {
            tiny_test::fail(__FILE__, __LINE__, "gated playlist yielded segments", text);
        }
    }

    // 闸**只**看首行，不是"整份拒收"：正常首行后面的一切照常认。
    const auto ok = scan_playlist(
        "#EXTM3U\n#EXTINF:4,\nseg0.ts\n#EXT-X-ENDLIST\n", "http://h/v/media.m3u8");
    REQUIRE(ok.segments.size() == 1);

    // 【ff_isspace 的六个字符必须一个不少】
    // ff_get_chomp_line 剥的是 av_isspace = { ' ', '\f', '\n', '\r', '\t', '\v' }。
    // 实测：把 '\v' / '\f' 从我们的集合里删掉，51 条共享表 + 920 万次
    // fuzz **一条都杀不掉**，可行为变化是真的、而且方向是错的——我们会拦下
    // FFmpeg 其实认的播放列表（它 strcmp 前已经把这两个字符剥干净了）。
    // 这条用例就是钉那两个字符的，直接用 gatecmp 探针实测过的形状。
    CHECK_EQ(scan_playlist("#EXTM3U\v\f\n#EXTINF:4,\nseg0.ts\n",
                           "http://h/v/media.m3u8").segments.size(),
             static_cast<size_t>(1));
    CHECK_EQ(scan_playlist("#EXTM3U\v\n#EXTINF:4,\nseg0.ts\n",
                           "http://h/v/media.m3u8").segments.size(),
             static_cast<size_t>(1));
    CHECK_EQ(scan_playlist("#EXTM3U\f\n#EXTINF:4,\nseg0.ts\n",
                           "http://h/v/media.m3u8").segments.size(),
             static_cast<size_t>(1));
    // 另外四个也一并钉住，省得将来只补这两个。
    CHECK_EQ(scan_playlist("#EXTM3U \t\n#EXTINF:4,\nseg0.ts\n",
                           "http://h/v/media.m3u8").segments.size(),
             static_cast<size_t>(1));

    // NUL 与 FFmpeg 一致地终止首行（ff_get_line 遇 0 就收行），所以
    // "#EXTM3U\0..." 是**过闸**的——这个方向必须对，闸多拦一条 FFmpeg 其实
    // 认得的播放列表，就是白白少暖一份。
    const std::string nul_line(
        "#EXTM3U\0junk\n#EXTINF:4,\nseg0.ts\n", sizeof("#EXTM3U\0junk\n#EXTINF:4,\nseg0.ts\n") - 1);
    CHECK_EQ(scan_playlist(nul_line, "http://h/v/media.m3u8").segments.size(),
             static_cast<size_t>(1));
}

TEST_CASE(resolve_url_handles_the_four_shapes) {
    const std::string base = "http://h/a/b/media.m3u8?v=2";
    CHECK_EQ(resolve_url(base, "https://other/x.ts"), std::string("https://other/x.ts"));
    CHECK_EQ(resolve_url(base, "//cdn/x.ts"), std::string("http://cdn/x.ts"));
    CHECK_EQ(resolve_url(base, "/root.ts"), std::string("http://h/root.ts"));
    CHECK_EQ(resolve_url(base, "seg0.ts"), std::string("http://h/a/b/seg0.ts"));
    CHECK_EQ(resolve_url(base, "sub/seg0.ts"), std::string("http://h/a/b/sub/seg0.ts"));
    // base 自带 query：解析相对路径时必须先剥掉它。
    CHECK_EQ(resolve_url(base, "seg1.ts?k=1"), std::string("http://h/a/b/seg1.ts?k=1"));
    // 空 ref / 空 base：返回空串，不崩。
    CHECK_EQ(resolve_url(base, ""), std::string());
    CHECK_EQ(resolve_url("", "seg.ts"), std::string());
    // 【只有 authority、没有路径段的 base】"最后一段"不存在，不能把 host
    // 当成最后一段切掉——切了会得出 http://seg.ts，把请求发到另一台主机上。
    // 曾经的 off-by-one：scheme:// 的第二个斜杠正好落在 bsep+2。
    CHECK_EQ(resolve_url("http://h", "seg.ts"), std::string("http://h/seg.ts"));
    CHECK_EQ(resolve_url("http://h/", "seg.ts"), std::string("http://h/seg.ts"));
    CHECK_EQ(resolve_url("http://h?x=1", "seg.ts"), std::string("http://h/seg.ts"));
    CHECK_EQ(resolve_url("http://h:8080", "seg.ts"),
             std::string("http://h:8080/seg.ts"));
    // 【authority 必须从剥过 query/fragment 的 base 里取】旧写法只有
    // 相对路径那一支走 path_part()，绝对路径支直接用 base 的 "://" 之后，于是
    // 没有路径段的 base 会把 ?query / #fragment 当成 authority 的一部分。
    // 两条支 × query/fragment 四种组合都钉一遍。
    CHECK_EQ(resolve_url("http://h?x=1", "/root.ts"), std::string("http://h/root.ts"));
    CHECK_EQ(resolve_url("http://h#f",   "/root.ts"), std::string("http://h/root.ts"));
    CHECK_EQ(resolve_url("http://h#f",   "seg.ts"),   std::string("http://h/seg.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8?v=2", "/root.ts"),
             std::string("http://h/root.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8#f", "/root.ts"),
             std::string("http://h/root.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8#f", "seg.ts"),
             std::string("http://h/a/b/seg.ts"));
    // "//host" 支同样不许把 query 带进去（它只用 scheme，但 scheme 也来自
    // 同一次拆分）。
    CHECK_EQ(resolve_url("http://h?x=1", "//cdn/x.ts"), std::string("http://cdn/x.ts"));
    // "://" 落在 fragment 里 ⇒ base 根本没有 scheme ⇒ 空串，而不是把
    // "h#ttp" 当成 scheme。
    CHECK_EQ(resolve_url("h#ttp://x", "seg.ts"), std::string());
}

// 【RFC 3986 §5.2.4 的点段归一】
//
// 每一条期望值都是**直链 build-ffmpeg 的 libavformat.a、调真的
// ff_make_absolute_url 跑出来的**（hls.c:1045 解析分片 URI 用的就是它），
// 不是照 RFC 推的。探针在 29 base × 43 ref 的全矩阵 + 198 万次随机 URL 上
// 与它逐字节比过，在"扫描器真能产出的形状"里零分歧。
//
// 为什么这条判据必须是"与 ff_make_absolute_url 一致"而不是"能请求成功"：
// CacheStore::make_key 哈希的是 URL **原串、不归一**，播放侧拿到的是
// FFmpeg 归一后的串。差一个字节 ⇒ 两个 key ⇒ 暖进去的字节永远命中不了。
// 变体落在 …/v1/index.m3u8、分片写 ../segs/0.ts 是真实 CDN 的常见布局，
// 所以这不是理论上的洁癖，是"这条播放列表每一个分片都白下"。
TEST_CASE(resolve_url_removes_dot_segments_like_ff_make_absolute_url) {
    // ---- 相对路径支 ----
    CHECK_EQ(resolve_url("http://h/a/b/media.m3u8", "../seg.ts"),
             std::string("http://h/a/seg.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/media.m3u8", "./seg.ts"),
             std::string("http://h/a/b/seg.ts"));
    CHECK_EQ(resolve_url("http://h/a/m.m3u8", "../../seg.ts"),
             std::string("http://h/seg.ts"));
    // 真实 CDN 布局：变体在 v1/ 下，分片写在兄弟目录里。
    CHECK_EQ(resolve_url("http://h/v1/index.m3u8", "../segs/0.ts"),
             std::string("http://h/segs/0.ts"));
    // 中段的 ".."：弹掉前一段，落点在某个 '/' 之后。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "x/../y.ts"),
             std::string("http://h/a/b/y.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "x/./y.ts"),
             std::string("http://h/a/b/x/y.ts"));
    // 弹过头：到根就停，不会弹出 authority（append_path 的 out-root>1 那道闸）。
    CHECK_EQ(resolve_url("http://h/a/m.m3u8", "../../../seg.ts"),
             std::string("http://h/seg.ts"));
    CHECK_EQ(resolve_url("http://h/", "../seg.ts"), std::string("http://h/seg.ts"));
    CHECK_EQ(resolve_url("http://h", "../seg.ts"), std::string("http://h/seg.ts"));
    // 末尾的 ".." / "."：留下一个目录形状的 URL（注意结尾那个 '/'）。
    CHECK_EQ(resolve_url("http://h/a/b/media.m3u8", ".."), std::string("http://h/a/"));
    CHECK_EQ(resolve_url("http://h/a/b/media.m3u8", "."), std::string("http://h/a/b/"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "x/.."), std::string("http://h/a/b/"));
    // base 自己带点段：一起归一（FFmpeg 的 append_path 对 base 段与 ref 段
    // 是同一遍，不是两套规则）。
    CHECK_EQ(resolve_url("http://h/a/./b/m.m3u8", "seg.ts"),
             std::string("http://h/a/b/seg.ts"));
    CHECK_EQ(resolve_url("http://h/a/../b/m.m3u8", "seg.ts"),
             std::string("http://h/b/seg.ts"));

    // ---- "/path" 绝对路径支：同样归一 ----
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "/a/../b.ts"),
             std::string("http://h/b.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "/./x.ts"), std::string("http://h/x.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "/../x.ts"), std::string("http://h/x.ts"));
    // authority 从剥过 query 的 base 里取这件事不能被归一化带坏。
    CHECK_EQ(resolve_url("http://h?x=1", "/a/../root.ts"), std::string("http://h/root.ts"));

    // ---- "//host" 支：ref 自带 authority 时 FFmpeg 也归一（simplify_path=1） ----
    CHECK_EQ(resolve_url("http://h/a/m.m3u8", "//cdn/x/../y.ts"),
             std::string("http://cdn/y.ts"));

    // ---- 带 scheme 的 ref：整条取代 base，**但点段照样归一** ----
    // ff_make_absolute_url2 里 `HAVE(uc,scheme) ⇒ simplify_path=0` 的**下一行**
    // 是 `HAVE(uc,authority) ⇒ simplify_path=1`，后者覆盖前者。抄漏那一行就
    // 会以为带 scheme 的 ref 原样返回——本轮实现第一版就是这么写的，是这条
    // 用例的期望值（直接从 ff_make_absolute_url 跑出来的）把它揪出来的。
    CHECK_EQ(resolve_url("http://h/a/m.m3u8", "https://o/x/../y.ts"),
             std::string("https://o/y.ts"));
    CHECK_EQ(resolve_url("http://h/a/m.m3u8", "https://o/x/./y.ts"),
             std::string("https://o/x/y.ts"));
    // 没有点段的绝对 ref 一个字节都不变。
    CHECK_EQ(resolve_url("http://h/a/m.m3u8", "https://other/x.ts"),
             std::string("https://other/x.ts"));

    // ---- ".." 的判据是**两个字符都要是点** ----
    // 覆盖洞：`seg == 2 && url[in] == '.' && url[in+1] == '.'` 去掉**前半**，
    // 上面这一整张表 14/0 仍然全绿。它不是等价变异——拿 ff_make_absolute_url
    // 对跑变异体，焦点矩阵 2,940 格里 35 条差异、198 万条 fuzz 里 279,919 条：
    // 任何**两字符且以点结尾**的路径段（"a."、"x." …）会被当成 ".." 弹栈。
    // 表里钉过 "..a" / "a..b" / "..seg.ts" / "..." / "%2e%2e"，唯独没有这一类。
    // 发布的代码是对的，缺的只是这一条断言。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "a./seg.ts"),
             std::string("http://h/a/b/a./seg.ts"));
    // 变异体给出的那个错值（"http://h/seg.ts"）来自这一行，一并钉住。
    CHECK_EQ(resolve_url("http://h/a/../b/m.m3u8", "a./seg.ts"),
             std::string("http://h/b/a./seg.ts"));
    // 对称的另一半（第一个字符是点、第二个不是）本来就被 "..a" 那类盖着，
    // 但两字符的形状单独钉一条，省得将来只补一半。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", ".x/seg.ts"),
             std::string("http://h/a/b/.x/seg.ts"));

    // ---- append_path 的 `out - root > 1` 那道闸：空段（"//"）不许被弹穿 ----
    // 路径 "//a/" 上连着两个 ".."：第一个弹掉 "a/"，第二个时 out-root==1
    // （只剩那个空段留下的 '/'），FFmpeg **不弹**。写成 >=1 会得出
    // "http://h/x.ts"，少一个斜杠 ⇒ 又是一个永远命中不了的 key。
    CHECK_EQ(resolve_url("http://h//a/m.m3u8", "../../x.ts"),
             std::string("http://h//x.ts"));
    CHECK_EQ(resolve_url("http://h//a/m.m3u8", "../x.ts"),
             std::string("http://h//x.ts"));

    // ---- query / fragment 不参与归一，只动路径 ----
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "../seg.ts?k=1#f"),
             std::string("http://h/a/seg.ts?k=1#f"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8?v=2", "../seg.ts"),
             std::string("http://h/a/seg.ts"));
    // base 的 query 里有 "../" 也不许被当成路径段。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8?q=../x", "seg.ts"),
             std::string("http://h/a/b/seg.ts"));
    // ref 的 query / fragment 里有整段的 "/../" 同样不许被归一掉
    // （归一的上界必须停在第一个 '?' / '#'）。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "seg.ts?a=/../b"),
             std::string("http://h/a/b/seg.ts?a=/../b"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "seg.ts#/../b"),
             std::string("http://h/a/b/seg.ts#/../b"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "../seg.ts?k=/./z"),
             std::string("http://h/a/seg.ts?k=/./z"));
    // authority 后面直接跟 query、根本没有路径段时，query 里的斜杠不许被
    // 当成"路径的开头"（找路径起点必须先剥 query/fragment）。
    CHECK_EQ(resolve_url("http://h/a/m.m3u8", "//cdn?x=/a/../b"),
             std::string("http://cdn?x=/a/../b"));

    // ---- 不是点段的东西不许被误伤 ----
    // 百分号编码不解码（FFmpeg 也不解），"%2e%2e" 是一个普通段。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "%2e%2e/seg.ts"),
             std::string("http://h/a/b/%2e%2e/seg.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "..."),
             std::string("http://h/a/b/..."));
    // 空段（"//"）原样保留：append_path 的 else 支把段尾那个 '/' 一起搬走，
    // 零长度段搬的就是一个孤立的 '/'。实测 ff_make_absolute_url 同样保留。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "....//seg.ts"),
             std::string("http://h/a/b/....//seg.ts"));
    // 连续弹栈与 "./.." 混写。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "a/b/../../../c.ts"),
             std::string("http://h/a/c.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "./../seg.ts"),
             std::string("http://h/a/seg.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "../a/../../b.ts"),
             std::string("http://h/b.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "a..b/seg.ts"),
             std::string("http://h/a/b/a..b/seg.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "..seg.ts"),
             std::string("http://h/a/b/..seg.ts"));
    // 普通相对路径一个字节都不能变（归一化最容易伤到的就是这条主路径）。
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "seg0.ts"),
             std::string("http://h/a/b/seg0.ts"));
    CHECK_EQ(resolve_url("http://h/a/b/m.m3u8", "sub/dir/seg0.ts"),
             std::string("http://h/a/b/sub/dir/seg0.ts"));
}

TEST_CASE(scan_master_takes_first_variant) {
    const std::string text =
        "#EXTM3U\n"
        "## 一行注释，不是标签\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\n"
        "low/media.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000\n"
        "high/media.m3u8\n";
    const auto s = scan_playlist(text, "http://h/master.m3u8");
    CHECK(s.is_master);
    CHECK(!s.encrypted);
    CHECK_EQ(s.first_variant, std::string("http://h/low/media.m3u8"));
    CHECK_EQ(s.segments.size(), static_cast<size_t>(0));
}

TEST_CASE(scan_media_takes_map_and_segments) {
    // CRLF 播放列表。**这里曾经带一个 UTF-8 BOM**——上一轮扫描器会剥掉它，
    // 而 FFmpeg 的 hls.c:849 是字面 strcmp，带 BOM 的整份拒收。剥 BOM 等于
    // 去暖一条 media 层根本打不开的流，已删。BOM 的行为由
    // scan_yields_nothing_preloadable_without_the_extm3u_header 覆盖。
    const std::string text =
        "#EXTM3U\r\n"
        "#EXT-X-TARGETDURATION:4\r\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\r\n"
        "#EXTINF:4.000,\r\n"
        "seg0.m4s\r\n"
        "#EXTINF:4.000,title\r\n"
        "seg1.m4s\r\n"
        "#EXTINF:2.5,\r\n"
        "seg2.m4s\r\n"
        "#EXT-X-ENDLIST\r\n";
    const auto s = scan_playlist(text, "http://h/v/media.m3u8");
    CHECK(!s.is_master);
    CHECK_EQ(s.map_uri, std::string("http://h/v/init.mp4"));
    REQUIRE(s.segments.size() == 3);
    CHECK_EQ(s.segments[0].url, std::string("http://h/v/seg0.m4s"));
    CHECK(s.segments[0].duration_s > 3.9 && s.segments[0].duration_s < 4.1);
    CHECK_EQ(s.segments[2].url, std::string("http://h/v/seg2.m4s"));
    CHECK(s.segments[2].duration_s > 2.4 && s.segments[2].duration_s < 2.6);
}

TEST_CASE(scan_detects_encryption_but_not_method_none) {
    const std::string none =
        "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4,\nseg0.ts\n";
    CHECK(!scan_playlist(none, "http://h/m.m3u8").encrypted);
    const std::string aes =
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n#EXTINF:4,\nseg0.ts\n";
    CHECK(scan_playlist(aes, "http://h/m.m3u8").encrypted);
    // NAME 属性里出现 METHOD= 字样不能误判（引号内的逗号/等号不参与切分）。
    const std::string tricky =
        "#EXTM3U\n#EXT-X-KEY:METHOD=NONE,URI=\"a,METHOD=AES-128\"\n";
    CHECK(!scan_playlist(tricky, "http://h/m.m3u8").encrypted);
}

TEST_CASE(segments_for_ms_counts_cumulative_duration) {
    std::vector<PlaylistScan::Segment> segs;
    segs.push_back({"a", 4.0});
    segs.push_back({"b", 4.0});
    segs.push_back({"c", 4.0});
    CHECK_EQ(segments_for_ms(segs, 3000, 16), static_cast<size_t>(1));
    CHECK_EQ(segments_for_ms(segs, 4000, 16), static_cast<size_t>(1));
    CHECK_EQ(segments_for_ms(segs, 5000, 16), static_cast<size_t>(2));
    CHECK_EQ(segments_for_ms(segs, 60000, 16), static_cast<size_t>(3));  // 不超过总数
    CHECK_EQ(segments_for_ms(segs, 60000, 2), static_cast<size_t>(2));   // 受 cap 约束
    CHECK_EQ(segments_for_ms({}, 3000, 16), static_cast<size_t>(0));
    std::vector<PlaylistScan::Segment> zero;
    zero.push_back({"a", 0.0});
    zero.push_back({"b", 0.0});
    // 时长未知：数不出来就一直数到 cap（宁可多暖一个，不会少于 1）。
    CHECK_EQ(segments_for_ms(zero, 3000, 16), static_cast<size_t>(2));
}

// ------------------------------------------------------------------ 端到端

namespace {

struct TempDir {
    std::filesystem::path p;
    TempDir() {
        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed);
        p = std::filesystem::temp_directory_path(ec)
            / ("syp-prehls-" + std::to_string(::getpid()) + "-" + std::to_string(n));
        std::filesystem::create_directories(p, ec);
    }
    ~TempDir() {
        std::error_code ec;
        if (!p.empty()) std::filesystem::remove_all(p, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

bool wait_received(const LoopbackServer& s, const std::string& path, int64_t n,
                   int timeout_ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (s.requests_received_for(path) >= n) return true;
        std::this_thread::yield();
    }
    return s.requests_received_for(path) >= n;
}

constexpr int kSoftMs = 6000;
constexpr int kHardMs = 90000;
constexpr const char* kHint = "HLS 预加载会串行抓两次播放列表再展开分片条目。";

}  // namespace

TEST_CASE(hls_warms_master_variant_and_first_k_segments) {
    syp::test::Watchdog wd("hls_warms_master_variant_and_first_k_segments",
                           kSoftMs, kHardMs, kHint);
    TempDir td;
    LoopbackConfig lc;
    lc.routes["/master.m3u8"] = text_body(
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nv/media.m3u8\n");
    lc.routes["/v/media.m3u8"] = text_body(
        "#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:4,\nseg0.m4s\n#EXTINF:4,\nseg1.m4s\n#EXTINF:4,\nseg2.m4s\n"
        "#EXT-X-ENDLIST\n");
    lc.routes["/v/init.mp4"] = std::vector<uint8_t>(2048, 0x11);
    lc.routes["/v/seg0.m4s"] = std::vector<uint8_t>(4096, 0x22);
    lc.routes["/v/seg1.m4s"] = std::vector<uint8_t>(4096, 0x33);
    lc.routes["/v/seg2.m4s"] = std::vector<uint8_t>(4096, 0x44);
    LoopbackServer server(std::move(lc));

    const std::string dir = td.p.string();
    syp_config dl{};
    syp_config_init(&dl);
    dl.cache_dir            = dir.c_str();
    dl.max_concurrent_tasks = 1;
    dl.max_cache_bytes      = 0;
    dl.min_free_space_bytes = 0;
    dl.cache_ttl_ms         = 0;
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);

    PreloadConfig pc;
    pc.default_preload_ms = 5000;        // 4s + 4s ⇒ K = 2
    syp_status err = SYP_OK;
    auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);

    const std::string master = server.url("/master.m3u8");
    CHECK_EQ(p->add(master, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->wait_terminal_for_test(master, 20000),
             static_cast<int32_t>(PreloadState::Done));

    // 两份播放列表都被取过。
    CHECK(wait_received(server, "/master.m3u8", 1));
    CHECK(wait_received(server, "/v/media.m3u8", 1));
    // init 段 + 前 2 个分片被暖，第 3 个不碰。
    CHECK(wait_received(server, "/v/init.mp4", 1));
    CHECK(wait_received(server, "/v/seg0.m4s", 1));
    CHECK(wait_received(server, "/v/seg1.m4s", 1));
    CHECK_EQ(server.requests_received_for("/v/seg2.m4s"), static_cast<int64_t>(0));

    syp_set_http_backend(nullptr);
}

TEST_CASE(hls_encrypted_playlist_requests_no_segment_and_no_key) {
    syp::test::Watchdog wd("hls_encrypted_playlist_requests_no_segment_and_no_key",
                           kSoftMs, kHardMs, kHint);
    TempDir td;
    LoopbackConfig lc;
    lc.routes["/e.m3u8"] = text_body(
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:4,\nseg0.ts\n#EXT-X-ENDLIST\n");
    lc.routes["/key.bin"]  = std::vector<uint8_t>(16, 0x55);
    lc.routes["/seg0.ts"]  = std::vector<uint8_t>(4096, 0x66);
    LoopbackServer server(std::move(lc));

    const std::string dir = td.p.string();
    syp_config dl{};
    syp_config_init(&dl);
    dl.cache_dir    = dir.c_str();
    dl.cache_ttl_ms = 0;
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);

    PreloadConfig pc;
    syp_status err = SYP_OK;
    auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);

    const std::string u = server.url("/e.m3u8");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->wait_terminal_for_test(u, 20000),
             static_cast<int32_t>(PreloadState::Failed));
    p->wait_settled_for_test();
    // 密钥一次都不请求，分片一个都不暖。
    CHECK_EQ(server.requests_received_for("/key.bin"), static_cast<int64_t>(0));
    CHECK_EQ(server.requests_received_for("/seg0.ts"), static_cast<int64_t>(0));
    // 【为什么还要这一条】上面两条是"还没请求"，它对一条**刚建出来、下一轮
    // 才会开源**的子条目同样成立——也就是说光靠它们，"加密时不建子条目"这件
    // 事可能只是被计时侥幸掩盖。条目数才是静止的判据：只剩播放列表自己，
    // 一个分片条目都没展开出来。
    syp::dl::PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.entries, static_cast<int64_t>(1));

    syp_set_http_backend(nullptr);
}

TEST_CASE(hls_live_playlist_is_warmed_once_only) {
    syp::test::Watchdog wd("hls_live_playlist_is_warmed_once_only",
                           kSoftMs, kHardMs, kHint);
    TempDir td;
    LoopbackConfig lc;
    // 没有 ENDLIST = 直播。预加载只暖一轮，绝不续暖（前几个分片很快过期）。
    lc.routes["/live.m3u8"] = text_body(
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:2,\ns100.ts\n#EXTINF:2,\ns101.ts\n");
    lc.routes["/s100.ts"] = std::vector<uint8_t>(2048, 0x77);
    lc.routes["/s101.ts"] = std::vector<uint8_t>(2048, 0x88);
    LoopbackServer server(std::move(lc));

    const std::string dir = td.p.string();
    syp_config dl{};
    syp_config_init(&dl);
    dl.cache_dir    = dir.c_str();
    dl.cache_ttl_ms = 0;
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);

    PreloadConfig pc;
    pc.default_preload_ms = 3000;      // 2s + 2s ⇒ K = 2
    syp_status err = SYP_OK;
    auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);

    const std::string u = server.url("/live.m3u8");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    CHECK_EQ(p->wait_terminal_for_test(u, 20000),
             static_cast<int32_t>(PreloadState::Done));
    p->wait_settled_for_test();
    CHECK_EQ(server.requests_received_for("/live.m3u8"), static_cast<int64_t>(1));
    CHECK(wait_received(server, "/s100.ts", 1));
    CHECK(wait_received(server, "/s101.ts", 1));

    // remove 父条目：子条目一并消失。
    p->remove(u);
    p->wait_settled_for_test();
    syp::dl::PreloadStats st{};
    p->get_stats(&st);
    CHECK_EQ(st.entries, static_cast<int64_t>(0));

    syp_set_http_backend(nullptr);
}

// 【remove 要打断在途下载】播放列表抓取是唯一跑在驱动线程上的
// 阻塞下载——驱动线程卡在 read() 里的时候没法自己响应 removing 标志，所以
// remove() 必须（在锁外）把它打断。实测未打断时条目在 5,819ms 后才消失。
//
// 【这条用例为什么可以有一个墙钟判据】路由被 hold_route 扣住且**从不放行**，
// 于是"抓取自己结束"只有一条路：kPlaylistReadTimeoutMs（5s）的读超时。判据
// 取 2.5s——它落在"被打断"（毫秒级）与"等超时"（≥5s）中间，两侧各有一倍
// 余量，不是在量性能。
TEST_CASE(remove_interrupts_an_in_flight_playlist_fetch) {
    syp::test::Watchdog wd("remove_interrupts_an_in_flight_playlist_fetch",
                           kSoftMs, kHardMs, kHint);
    TempDir td;
    LoopbackConfig lc;
    lc.routes["/held.m3u8"] = text_body(
        "#EXTM3U\n#EXTINF:4,\ns0.ts\n#EXT-X-ENDLIST\n");
    LoopbackServer server(std::move(lc));
    server.hold_route("/held.m3u8");     // 收到请求行之后原地扣住，永不放行

    const std::string dir = td.p.string();
    syp_config dl{};
    syp_config_init(&dl);
    dl.cache_dir    = dir.c_str();
    dl.cache_ttl_ms = 0;
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);

    PreloadConfig pc;
    syp_status err = SYP_OK;
    auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);

    const std::string u = server.url("/held.m3u8");
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    // 谓词式等待：服务端**收到**了请求 ⇒ 驱动线程一定已经卡在这次抓取里。
    REQUIRE(wait_received(server, "/held.m3u8", 1));

    const auto t0 = std::chrono::steady_clock::now();
    p->remove(u);
    // 条目消失（驱动线程从 read 里被打断、走完这一轮）才算 remove 生效。
    syp::dl::PreloadStats st{};
    const auto deadline = t0 + std::chrono::milliseconds(2500);
    do {
        p->get_stats(&st);
        if (st.entries == 0) break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("  [remove] entry gone after %lld ms\n", static_cast<long long>(ms));
    CHECK_EQ(st.entries, static_cast<int64_t>(0));

    server.release_route("/held.m3u8");
    syp_set_http_backend(nullptr);
}

// 【钉住 kPlaylistMaxRetries】播放列表抓取是**同步**跑在
// 驱动线程上的：它卡多久，add / set_priority / remove / destroy 就跟着卡多久
// （destroy 还要 join 那条线程）。preloader.h 里那段"max_retries 不能写 0"的
// 论证此前一条用例都没有——SourceBridge 把 max_retries 分两路用，scheduler
// 那一路走 clamp_i32_pos(max_retries, 5)，**0 被当成"没填"、退回默认的 5**，
// 于是写 0 反而更慢。
//
// 判据是墙钟，但方向是单边的，且门槛是量出来的：
//   kPlaylistMaxRetries = 1 ⇒ 实测 6,00x ms（5s 读超时 + 一次重试的开销）；
//   kPlaylistMaxRetries = 0 ⇒ 实测 15,0xx ms（scheduler 起满 5 轮）。
// 门槛取 10,000ms：比实测值宽 1.66 倍（扛得住被压的机器），又比退化值紧
// 5,000ms（常数改回 0 必定变红）。
TEST_CASE(a_stalled_playlist_fetch_gives_up_within_the_retry_bound) {
    syp::test::Watchdog wd("a_stalled_playlist_fetch_gives_up_within_the_retry_bound",
                           20000, 120000,
                           "路由被扣住且永不放行，只能靠读超时结束。");
    TempDir td;
    LoopbackConfig lc;
    lc.routes["/stall.m3u8"] = text_body("#EXTM3U\n#EXTINF:4,\ns0.ts\n#EXT-X-ENDLIST\n");
    LoopbackServer server(std::move(lc));
    server.hold_route("/stall.m3u8");    // 收到请求行之后原地扣住，永不放行

    const std::string dir = td.p.string();
    syp_config dl{};
    syp_config_init(&dl);
    dl.cache_dir    = dir.c_str();
    dl.cache_ttl_ms = 0;
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);

    PreloadConfig pc;
    syp_status err = SYP_OK;
    auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);

    const std::string u = server.url("/stall.m3u8");
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
    // 超时给到 60s：它只把"挂死"变成"失败"，不承担判据。
    CHECK_EQ(p->wait_terminal_for_test(u, 60000),
             static_cast<int32_t>(PreloadState::Failed));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("  [playlist-stall] gave up after %lld ms\n", static_cast<long long>(ms));
    CHECK(ms < 10000);

    server.release_route("/stall.m3u8");
    syp_set_http_backend(nullptr);
}

// 【播放列表抓取没有墙钟上界，把让路（peer-yield）整个按死】
//
// connect/read/max_retries 三个超时都是**每次 read** 的，整次抓取原先只有
// kMaxPlaylistBytes = 8 MiB 这一个**字节**上界。于是一个"每 4000ms 给 4 个
// 字节"的服务端既永远踩不到 5s 的单次读超时，又要走 `播放列表长度 ÷ 滴流
// 速率` 那么久；理论天花板是 `8 MiB ÷ 滴流速率`。
//
// 真正的后果不是这条 HLS 预加载慢，而是**让路在这段时间里整个失效**——
// 让路正是用来保护在播流带宽的机制，preloader.h:53-56
// 承诺的"让路有一个 50ms 的探测延迟"在这条路径上不成立：抓取是**同步**跑在
// 驱动线程上的，驱动线程卡在 read() 里就不会再去问 open_count()，而那条
// 预加载的 SourceBridge **一直开着**，和真正在播的流在同一个 cache key 上抢
// 带宽。实测：正常让路 14ms，驱动线程扎在滴流播放列表里时 116,547ms
// （8,300×）。本机在本用例这个形状下实测见下面的 printf。
//
// 修法是给 fetch_text 一条墙钟 deadline（kPlaylistFetchDeadlineMs =
// 2 × kPlaylistReadTimeoutMs），在读循环里与 stop_/removing 一起查。
//
// 【判据怎么取的】对照组（没有在抓播放列表）与实验组（驱动线程扎在滴流里）
// 各量一次让路延迟，门槛取 30,000ms：比修复后的上界（deadline 10s + 最多
// 一次 5s 读超时 + 一轮 50ms 轮询）宽 2 倍，比修复前的实测值（≈160s）紧
// 5 倍以上，常数被删掉必定变红。
//
// 【为什么要两台服务器】body_chunk_bytes / body_chunk_delay_ms 是
// LoopbackConfig 的**全局**字段，不是按路由配的：滴流加在同一台服务器上会
// 把 /big.bin 那条也一起拖慢，于是"驱动线程被播放列表按住"与"下载本来就慢"
// 两件事分不开，用例就没有鉴别力了。
TEST_CASE(a_trickling_playlist_cannot_park_peer_yield) {
    syp::test::Watchdog wd("a_trickling_playlist_cannot_park_peer_yield",
                           40000, 240000,
                           "滴流服务端 4 B / 4000ms；修复前这条会跑满约 160 秒。");
    TempDir td;
    const std::string dir = td.p.string();
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);

    LoopbackConfig fast;
    fast.routes["/big.bin"] = std::vector<uint8_t>(512 * 1024, 0x11);
    LoopbackServer fast_srv(std::move(fast));

    // 正好 160 字节的 media 播放列表：内容合法（会真的展开），长度是量出来的。
    std::string pl = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\ns0.ts\n"
                     "#EXTINF:4,\ns1.ts\n#EXT-X-ENDLIST\n";
    while (pl.size() < 160) pl.insert(pl.size() - 17, "#\n");   // 在 ENDLIST 之前垫注释行
    LoopbackConfig slow;
    slow.routes["/trickle.m3u8"] = text_body(pl);
    slow.body_chunk_bytes    = 4;
    slow.body_chunk_delay_ms = 4000;     // 160 B ÷ (4 B / 4s) ≈ 160 s
    LoopbackServer slow_srv(std::move(slow));

    syp_config dl{};
    syp_config_init(&dl);
    dl.cache_dir            = dir.c_str();
    dl.cache_ttl_ms         = 0;
    dl.max_cache_bytes      = 0;
    dl.min_free_space_bytes = 0;
    dl.max_concurrent_tasks = 1;     // per_max = 1 ⇒ 两个条目各拿到 1 份额度

    const std::string big = fast_srv.url("/big.bin");
    const std::string tri = slow_srv.url("/trickle.m3u8");

    // 让路延迟：从"对等源真的开起来"到"条目被标成让路中"。
    const auto measure_yield = [&](Preloader& p) -> int64_t {
        syp_config peer_cfg = dl;
        peer_cfg.cache_dir  = dir.c_str();
        const auto t0 = std::chrono::steady_clock::now();
        auto peer = syp::dl::SourceBridge::open(big, nullptr, peer_cfg, nullptr,
                                                syp::dl::current_http_backend(),
                                                syp::dl::system_clock());
        CHECK(peer.has_value());
        if (!peer.has_value()) return -1;
        const auto limit = t0 + std::chrono::milliseconds(200000);
        while (std::chrono::steady_clock::now() < limit) {
            if (p.peer_yield_for_test(big) == 1) break;
            // 【这里睡 1ms 而不是 yield()】量测窗口最长 200 秒，纯自旋会把一
            // 整个核吃满 200 秒（实测 176s user），而这条用例量的正是时间：
            // 自旋本身就会扰动被量的对象。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
        const int32_t got = p.peer_yield_for_test(big);
        (*peer)->interrupt();
        (*peer)->close();
        peer->reset();
        return got == 1 ? ms : -1;
    };

    PreloadConfig pc;
    pc.max_total_tasks       = 2;
    pc.reserved_for_playing  = 0;
    pc.default_preload_bytes = 256 * 1024;

    // ---- 对照组：驱动线程空闲，只有一条按字节的条目。
    int64_t control_ms = -1;
    {
        fast_srv.hold_route("/big.bin");   // 一个字节都不落地，状态静止
        syp_status err = SYP_OK;
        auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                                   syp::dl::system_clock(), &err);
        REQUIRE(p != nullptr);
        CHECK_EQ(p->add(big, PreloadPriority::Next, 0), SYP_OK);
        REQUIRE(wait_received(fast_srv, "/big.bin", 1));
        CHECK_EQ(p->peer_yield_for_test(big), 0);
        control_ms = measure_yield(*p);
        fast_srv.release_route("/big.bin");
    }

    // ---- 实验组：同一条字节条目 + 一条滴流播放列表，驱动线程扎在 fetch_text。
    int64_t parked_ms = -1;
    {
        fast_srv.hold_route("/big.bin");
        syp_status err = SYP_OK;
        auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                                   syp::dl::system_clock(), &err);
        REQUIRE(p != nullptr);
        CHECK_EQ(p->add(big, PreloadPriority::Next, 0), SYP_OK);   // seq 小，先开源
        CHECK_EQ(p->add(tri, PreloadPriority::Next, 0), SYP_OK);
        // 两个前提都要成立才算构造成功：字节条目的源已经开出去了，
        // 且驱动线程已经**进到**播放列表抓取里（服务端收到了请求行）。
        REQUIRE(wait_received(fast_srv, "/big.bin", 1, 20000));
        REQUIRE(wait_received(slow_srv, "/trickle.m3u8", 1, 20000));
        parked_ms = measure_yield(*p);
        fast_srv.release_route("/big.bin");
    }

    std::printf("  [peer-yield] control=%lld ms  parked-in-trickle=%lld ms\n",
                static_cast<long long>(control_ms), static_cast<long long>(parked_ms));
    CHECK(control_ms >= 0);
    CHECK(parked_ms >= 0);
    CHECK(parked_ms < 30000);
    syp_set_http_backend(nullptr);
}

// ---------------------------------------------------------------- 容量/TTL
//
// 【播放列表这一支的容量三件套】
// fetch_text() 为每一张播放列表另开一条一次性的 SourceBridge。它和条目源
// 一样会在关闭时走 enforce_capacity，所以 max_cache_bytes /
// min_free_space_bytes / cache_ttl_ms 必须原样穿过 playlist_config()。
//
// 此前这条路径上**一条断言都没有**：这个变异（在 fetch_text 里
// base_config() 之后把三个字段清零）实测 `ctest 33/33` +
// `xcodebuild 82 tests, 0 failures` 双绿存活。
TEST_CASE(hls_playlist_fetch_carries_capacity_and_ttl) {
    TempDir td;
    const std::string dir = td.p.string();
    syp_config dl{};
    syp_config_init(&dl);
    dl.cache_dir            = dir.c_str();
    dl.max_cache_bytes      = 12345678;
    dl.min_free_space_bytes = 2345678;
    dl.cache_ttl_ms         = 1500;
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);
    syp_status err = SYP_OK;
    auto p = Preloader::create(dl, PreloadConfig{}, nullptr,
                               syp::dl::current_http_backend(),
                               syp::dl::system_clock(), &err);
    REQUIRE(p != nullptr);
    const syp_config c = p->playlist_config_for_test();
    CHECK_EQ(c.max_cache_bytes, static_cast<int64_t>(12345678));
    CHECK_EQ(c.min_free_space_bytes, static_cast<int64_t>(2345678));
    CHECK_EQ(c.cache_ttl_ms, static_cast<int64_t>(1500));
    // 并发/超时/窗口这几类才是 fetch_text 该覆盖的，一并钉住，免得将来
    // "把容量穿过去"被实现成"什么都不覆盖"。
    CHECK_EQ(c.max_concurrent_tasks, 1);
    CHECK_EQ(c.first_buffer_ms, 0);
    CHECK_EQ(c.target_buffer_ms, 0);
    syp_set_http_backend(nullptr);
}

// 行为用例：播放列表那条源真的会按 max_cache_bytes 淘汰无人引用的旧条目。
//
// 【为什么不能只靠上面那条读回缝】读回缝断的是"配置长什么样"，它挡不住
// "配置对了但这条路径根本没走 enforce_capacity"。更要紧的是它可以被绕过：
// 把清零挪到 playlist_config() 的**调用点之后**（也就是上面那个变异字面上的
// 那个位置），读回缝就看不见了。这条用例落在后果上，两个位置都盖得住。
//
// 【判据为什么能鉴别】故意用一张**加密**的播放列表：加密时展开会失败，
// 一个分片条目都不产生（见 hls_encrypted_playlist_requests_no_segment_and_
// no_key），而播放列表条目本身从不开 SourceBridge。于是整轮里**唯一**开过
// 的源就是 fetch_text 那一条——victim 被不被淘汰，只取决于它手里那份配置。
TEST_CASE(hls_playlist_fetch_carries_capacity_and_evicts_an_unreferenced_entry) {
    syp::test::Watchdog wd("hls_playlist_fetch_carries_capacity_and_evicts_an_unreferenced_entry",
                           kSoftMs, kHardMs, kHint);
    constexpr int64_t kVictimBytes = 64 * 1024;
    TempDir td;
    LoopbackConfig lc;
    lc.routes["/victim.mp4"] = std::vector<uint8_t>(
        static_cast<size_t>(kVictimBytes), 0x77);
    lc.routes["/e.m3u8"] = text_body(
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:4,\nseg0.ts\n#EXT-X-ENDLIST\n");
    LoopbackServer server(std::move(lc));
    REQUIRE(syp_set_http_backend(syp_apple_http_backend()) == SYP_OK);

    const std::string dir    = td.p.string();
    const std::string victim = server.url("/victim.mp4");
    const std::filesystem::path victim_idx =
        td.p / (syp::dl::CacheIndex::key_for_url(victim) + ".idx");

    // 1) 先把 victim 整份暖下来，然后让它的 Preloader 析构（引用计数归零，
    //    磁盘上留着）。这一轮不限容量。
    {
        syp_config dl{};
        syp_config_init(&dl);
        dl.cache_dir       = dir.c_str();
        dl.max_cache_bytes = 0;
        dl.cache_ttl_ms    = 0;
        PreloadConfig pc;
        pc.default_preload_bytes = kVictimBytes;
        syp_status err = SYP_OK;
        auto p = Preloader::create(dl, pc, nullptr, syp::dl::current_http_backend(),
                                   syp::dl::system_clock(), &err);
        REQUIRE(p != nullptr);
        CHECK_EQ(p->add(victim, PreloadPriority::Next, 0), SYP_OK);
        CHECK_EQ(p->wait_terminal_for_test(victim, 20000),
                 static_cast<int32_t>(PreloadState::Done));
        p->wait_settled_for_test();
    }
    REQUIRE(std::filesystem::exists(victim_idx));

    // 2) 上限远低于 victim 的体积。这一轮只抓一张加密播放列表——唯一开过的
    //    源是 fetch_text 那一条，它落盘并关闭时必须把 victim 淘汰掉。
    {
        syp_config dl{};
        syp_config_init(&dl);
        dl.cache_dir       = dir.c_str();
        dl.max_cache_bytes = 4096;
        dl.cache_ttl_ms    = 0;
        syp_status err = SYP_OK;
        auto p = Preloader::create(dl, PreloadConfig{}, nullptr,
                                   syp::dl::current_http_backend(),
                                   syp::dl::system_clock(), &err);
        REQUIRE(p != nullptr);
        const std::string u = server.url("/e.m3u8");
        CHECK_EQ(p->add(u, PreloadPriority::Next, 0), SYP_OK);
        CHECK_EQ(p->wait_terminal_for_test(u, 20000),
                 static_cast<int32_t>(PreloadState::Failed));
        p->wait_settled_for_test();
        // 前提复核：真的一个分片条目都没展开出来（否则"谁淘汰的"就不唯一了）。
        syp::dl::PreloadStats st{};
        p->get_stats(&st);
        CHECK_EQ(st.entries, static_cast<int64_t>(1));
    }
    CHECK(!std::filesystem::exists(victim_idx));

    syp_set_http_backend(nullptr);
}

int main() { return tiny_test_main(); }
