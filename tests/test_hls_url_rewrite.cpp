// test_hls_url_rewrite.cpp —— HLS 的 URL 纯函数。
//
// 这几个函数不碰任何 IO，是整条 HLS 链路上唯一能被穷举测的部分，
// 所以这里测得细一点：后面 io_open 的分流判错是**静默**的
// （把播放列表当分片 → 它会被缓存 → 直播冻住），没有断言会响。
#include <string>
#include "media/hls/url_rewrite.h"
#include "support/hls_cases.h"
#include "tiny_test.h"

using namespace syp::media::hls;

TEST_CASE(scheme_of_basics) {
    CHECK_EQ(scheme_of("https://h/p"), std::string("https"));
    CHECK_EQ(scheme_of("http://h/p"),  std::string("http"));
    CHECK_EQ(scheme_of("HTTPS://h/p"), std::string("https"));   // 小写化
    CHECK_EQ(scheme_of("/no/scheme"),  std::string(""));
    CHECK_EQ(scheme_of(""),            std::string(""));
}

// 判据表在 tests/support/hls_cases.h —— dl 侧的 test_preloader_hls.cpp
// include 的是**同一个文件**。dl（syp::dl::is_playlist_url /
// PlaylistScan::encrypted）与 media（channel_for /
// playlist_declares_encryption）判据相同、实现各一份（dl 不许依赖 media），
// 共享这张表是它们不漂移的唯一保障。
TEST_CASE(channel_for_matches_the_shared_table) {
    for (const auto& c : syp::test::hls_cases::kChannelCases) {
        const bool playlist = channel_for(c.url) == Channel::Playlist;
        if (playlist != c.playlist) {
            tiny_test::fail(__FILE__, __LINE__, "channel_for", c.url);
        }
    }
}

// 加密判据同样共享一张表：**这条带着安全后果**（漏判 = 去请求密钥）。
TEST_CASE(playlist_declares_encryption_matches_the_shared_table) {
    for (const auto& c : syp::test::hls_cases::kEncryptionCases) {
        if (playlist_declares_encryption(c.text) != c.encrypted) {
            tiny_test::fail(__FILE__, __LINE__, "playlist_declares_encryption", c.name);
        }
    }
}

// ENDLIST 判据的表是**单边**的：dl 侧曾经有一份
// PlaylistScan::has_endlist 与本函数对钉，但那是同一道 FFmpeg 闸的第二份
// 转写、在 src/ 里零消费点、而且两份已经漂了（NUL 算不算行终止符），
// 后来删掉了它。本函数在 HlsSession 里有真实调用方，
// kEndlistCases 现在是它唯一的守护——表里带 BOM 的用例是那道锚，
// 这一侧一旦加回 BOM 剥离就会在这里变红。
TEST_CASE(playlist_declares_endlist_matches_the_shared_table) {
    for (const auto& c : syp::test::hls_cases::kEndlistCases) {
        if (playlist_declares_endlist(c.text) != c.endlist) {
            tiny_test::fail(__FILE__, __LINE__, "playlist_declares_endlist", c.name);
        }
    }
}

TEST_CASE(channel_for_uses_path_suffix_not_query) {
    CHECK(channel_for("https://h/a/master.m3u8") == Channel::Playlist);
    CHECK(channel_for("https://h/a/master.m3u")  == Channel::Playlist);
    // 目标源的真实形状：后缀后面带一长串 query。剥 query 之后才看得见 .m3u8
    CHECK(channel_for("https://h/pl/x.m3u8?tag=14&v=cfc") == Channel::Playlist);
    CHECK(channel_for("https://h/pl/x.m3u8#frag")         == Channel::Playlist);
    CHECK(channel_for("https://h/v/seg0.m4s")             == Channel::Segment);
    CHECK(channel_for("https://h/v/init.mp4")             == Channel::Segment);
    CHECK(channel_for("https://h/v/seg0.ts")              == Channel::Segment);
    // 大小写不敏感：CDN 上 .M3U8 出现过
    CHECK(channel_for("https://h/a/MASTER.M3U8") == Channel::Playlist);
    // 没有后缀 → 当分片（保守：分片通道会缓存，而误缓存一个播放列表的
    // 后果比误不缓存一个分片更严重，所以这条默认值的选择要有意识——
    // 见 url_rewrite.h 的注释）
    CHECK(channel_for("https://h/a/playlist") == Channel::Segment);
}

TEST_CASE(to_ffmpeg_url_downgrades_https_only) {
    CHECK_EQ(to_ffmpeg_url("https://h/p?q=1"), std::string("http://h/p?q=1"));
    CHECK_EQ(to_ffmpeg_url("http://h/p"),      std::string("http://h/p"));
    // 非 http(s) 原样返回：不是我们负责改写的东西
    CHECK_EQ(to_ffmpeg_url("file:///tmp/a"),   std::string("file:///tmp/a"));
    CHECK_EQ(to_ffmpeg_url("/rel/path"),       std::string("/rel/path"));
}

TEST_CASE(to_real_url_restores_session_scheme) {
    CHECK_EQ(to_real_url("http://h/p?q=1", "https"), std::string("https://h/p?q=1"));
    CHECK_EQ(to_real_url("http://h/p",     "http"),  std::string("http://h/p"));
    // 端口、userinfo、query 一律保留原样
    CHECK_EQ(to_real_url("http://h:8080/p", "https"), std::string("https://h:8080/p"));

    // 以下几条守护头文件里的"降级失败而非降级成功"安全承诺：
    // ffmpeg_url 不是 http 时原样返回，防止畸形 scheme（如 httpss://）
    CHECK_EQ(to_real_url("https://h/p", "https"), std::string("https://h/p"));
    CHECK_EQ(to_real_url("https://h/p", "http"),  std::string("https://h/p"));
    CHECK_EQ(to_real_url("file:///tmp/a", "https"), std::string("file:///tmp/a"));
    // real_scheme 为空时原样返回（不下沉 scheme 改写）
    CHECK_EQ(to_real_url("http://h/p", ""), std::string("http://h/p"));
}

TEST_CASE(round_trip_is_identity_for_http_and_https) {
    for (const char* u : {"https://a.b/c/d.m3u8?x=1", "http://a.b:9/c.m4s"}) {
        const std::string real_scheme = scheme_of(u);
        CHECK_EQ(to_real_url(to_ffmpeg_url(u), real_scheme), std::string(u));
    }
}

// playlist_declares_encryption：穷举判据。本版本不支持加密 HLS，判据是
// "有 EXT-X-KEY 且 METHOD 不是 NONE"——METHOD=NONE 是合法的"这一段不
// 加密"声明，不能一见 EXT-X-KEY 就拒。
TEST_CASE(playlist_declares_encryption_method_none_does_not_count) {
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-KEY:METHOD=NONE\n"
        "#EXTINF:1.0,\nseg0.m4s\n#EXT-X-ENDLIST\n"));
}

TEST_CASE(playlist_declares_encryption_aes128_counts) {
    CHECK(playlist_declares_encryption(
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:1.0,\nseg0.m4s\n"));
}

TEST_CASE(playlist_declares_encryption_sample_aes_counts) {
    CHECK(playlist_declares_encryption(
        "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"key.bin\"\n"
        "#EXTINF:1.0,\nseg0.m4s\n"));
}

TEST_CASE(playlist_declares_encryption_no_key_tag_does_not_count) {
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1.0,\nseg0.m4s\n#EXT-X-ENDLIST\n"));
    CHECK(!playlist_declares_encryption(""));
}

TEST_CASE(playlist_declares_encryption_is_case_insensitive) {
    // 标签名、METHOD 值大小写都不该影响判定——真实 CDN 上遇到过混大小写。
    CHECK(playlist_declares_encryption(
        "#extm3u\n#ext-x-key:method=aes-128,uri=\"key.bin\"\n"));
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n#Ext-X-Key:Method=none\n"));
}

TEST_CASE(playlist_declares_encryption_tolerates_leading_whitespace) {
    // 行首空白（有些生成器/代理会缩进 tag 行）不该让判定失效。
    CHECK(playlist_declares_encryption(
        "#EXTM3U\n   #EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"));
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n\t#EXT-X-KEY:METHOD=NONE\n"));
}

TEST_CASE(playlist_declares_encryption_ignores_key_mentioned_in_a_comment) {
    // EXT-X-KEY 出现在别的注释行文本里（不是这一行的 tag 本身）不算数——
    // 判据看的是"这一行是不是 EXT-X-KEY tag"，不是"这一行含不含这个子串"。
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n# 备注：源本该有 #EXT-X-KEY:METHOD=AES-128 但这版没有\n"
        "#EXTINF:1.0,\nseg0.m4s\n"));
}

// 真正的 METHOD 属性必须在引号外找，不能靠
// 朴素子串匹配——URI 属性的值里完全可能含 "METHOD=NONE" 这样的文本
// （比如查询参数），朴素子串匹配会在那里假阳命中，把一条真实的加密声明
// 判成未加密，直接推翻"密钥 URL 一次都不该被请求"这条保证。
TEST_CASE(playlist_declares_encryption_ignores_method_substring_inside_uri_value) {
    // URI 的引号值里塞了一个假的 "METHOD=NONE"，真正的 METHOD 属性在
    // 逗号之后、引号之外，是 AES-128——必须判定为加密。
    CHECK(playlist_declares_encryption(
        "#EXTM3U\n"
        "#EXT-X-KEY:URI=\"http://x/k?a=METHOD=NONE,b\",METHOD=AES-128\n"
        "#EXTINF:1.0,\nseg0.m4s\n"));
}

TEST_CASE(playlist_declares_encryption_finds_real_method_none_past_a_uri_decoy) {
    // 反过来：URI 引号值里塞了假的 "METHOD=AES-128"，真正的 METHOD 属性
    // 在逗号之后、引号之外，是 NONE——必须判定为未加密。方向调换过来测，
    // 证明修法不是"见到任何 METHOD= 子串就判真"这种只对一个方向安全的
    // 半吊子写法。
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n"
        "#EXT-X-KEY:URI=\"http://x/k?a=METHOD=AES-128,b\",METHOD=NONE\n"
        "#EXTINF:1.0,\nseg0.m4s\n"));
}

// METHOD 的值本身按规范从不加引号，但真实世界见过写成
// METHOD="NONE" 的畸形播放列表——值要先剥引号再比较，否则 `"NONE"` 永远
// 不等于 `NONE`，被误判成加密。
TEST_CASE(playlist_declares_encryption_strips_quotes_around_method_value) {
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n#EXT-X-KEY:METHOD=\"NONE\",URI=\"key.bin\"\n"));
    CHECK(!playlist_declares_encryption(
        "#EXTM3U\n#EXT-X-KEY:method=\"none\",URI=\"key.bin\"\n"));
}

// playlist_declares_endlist：必须与 FFmpeg 的判定**逐字一致**，两个方向
// 都不能偏——说有而 FFmpeg 没认，会把直播的异常结束放过成 Eof；说没有而
// FFmpeg 认了，会把正常播完报成错误。FFmpeg 8.1.2 的读法：
//   · 行由 '\n' 或 '\r' 切分，行尾空白剥掉，行首不剥（aviobuf.c ff_get_chomp_line）；
//   · 首行必须恰好是 "#EXTM3U"，否则 parse_playlist 直接失败（hls.c:849）；
//   · av_strstart(line, "#EXT-X-ENDLIST")：大小写敏感的前缀匹配（hls.c:979）。
TEST_CASE(playlist_declares_endlist_basic) {
    CHECK(playlist_declares_endlist("#EXTM3U\n#EXTINF:1.0,\nseg0.m4s\n#EXT-X-ENDLIST\n"));
    CHECK(playlist_declares_endlist("#EXTM3U\n#EXTINF:1.0,\nseg0.m4s\n#EXT-X-ENDLIST"));
    CHECK(!playlist_declares_endlist("#EXTM3U\n#EXTINF:1.0,\nseg0.m4s\n"));
    CHECK(!playlist_declares_endlist(""));
}

TEST_CASE(playlist_declares_endlist_requires_the_extm3u_header_like_ffmpeg) {
    // 200 回来的是 HTML 错误页 / 首行不对：FFmpeg 解析失败，不能算"正常结束"。
    CHECK(!playlist_declares_endlist("<html>#EXT-X-ENDLIST</html>\n"));
    CHECK(!playlist_declares_endlist("\n#EXTM3U\n#EXT-X-ENDLIST\n"));
    CHECK(!playlist_declares_endlist(" #EXTM3U\n#EXT-X-ENDLIST\n"));
    CHECK(playlist_declares_endlist("#EXTM3U  \r\n#EXT-X-ENDLIST\r\n"));
}

TEST_CASE(playlist_declares_endlist_matches_ffmpeg_line_rules) {
    CHECK(!playlist_declares_endlist("#EXTM3U\n#ext-x-endlist\n"));     // 大小写敏感
    CHECK(!playlist_declares_endlist("#EXTM3U\n  #EXT-X-ENDLIST\n"));   // 行首空白不剥
    CHECK(playlist_declares_endlist("#EXTM3U\r#EXT-X-ENDLIST\r"));      // 单独 '\r' 也是行尾
    CHECK(playlist_declares_endlist("#EXTM3U\n#EXT-X-ENDLISTX\n"));     // av_strstart 是前缀匹配
    CHECK(!playlist_declares_endlist("#EXTM3U\nseg#EXT-X-ENDLIST\n"));  // 不在行首
}

TEST_CASE(strip_subtitle_renditions_removes_only_subtitle_media_lines) {
    // twimg 真实 master 的形状（节选）：字幕组 + 音频组 + 引用两者的变体。
    const std::string in =
        "#EXTM3U\n"
        "#EXT-X-VERSION:6\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"en\",URI=\"/s0/a.m3u8\",LANGUAGE=\"en\"\r\n"
        "#EXT-X-MEDIA:NAME=\"Audio\",TYPE=AUDIO,GROUP-ID=\"audio-128000\",URI=\"/mp4a/128000/b.m3u8\"\n"
        "#EXT-X-MEDIA:NAME=\"x,TYPE=SUBTITLES\",TYPE=AUDIO,GROUP-ID=\"decoy\",URI=\"/c.m3u8\"\n"
        "#EXT-X-MEDIA:GROUP-ID=\"s2\",TYPE=SUBTITLES,NAME=\"fr\",URI=\"/s1/d.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"mp4a.40.2,avc1.640028\",SUBTITLES=\"subs\",AUDIO=\"audio-128000\"\n"
        "/avc1/v.m3u8\n";
    const std::string out = strip_subtitle_renditions(in);
    CHECK_EQ(out,
             std::string("#EXTM3U\n"
                         "#EXT-X-VERSION:6\n"
                         "#EXT-X-MEDIA:NAME=\"Audio\",TYPE=AUDIO,GROUP-ID=\"audio-128000\",URI=\"/mp4a/128000/b.m3u8\"\n"
                         "#EXT-X-MEDIA:NAME=\"x,TYPE=SUBTITLES\",TYPE=AUDIO,GROUP-ID=\"decoy\",URI=\"/c.m3u8\"\n"
                         "#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"mp4a.40.2,avc1.640028\",SUBTITLES=\"subs\",AUDIO=\"audio-128000\"\n"
                         "/avc1/v.m3u8\n"));
}

TEST_CASE(strip_subtitle_renditions_leaves_other_playlists_untouched) {
    const std::string media = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\nseg0.m4s\n#EXT-X-ENDLIST";
    CHECK_EQ(strip_subtitle_renditions(media), media);   // 无尾换行也原样
    CHECK_EQ(strip_subtitle_renditions(""), std::string());
    // 没有字幕行时不改一个字节（包括 CRLF）。
    const std::string crlf = "#EXTM3U\r\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"a\"\r\n";
    CHECK_EQ(strip_subtitle_renditions(crlf), crlf);
}

int main() { return tiny_test_main(); }
