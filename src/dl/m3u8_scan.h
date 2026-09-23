// m3u8_scan.h — 预加载专用的**最小**播放列表扫描器：只认 URI 与 EXTINF。
//
// 【范围，必须知道】它不是 m3u8 解析器：不做 ABR 选择（master 取第一条
// #EXT-X-STREAM-INF）、不处理加密（只判"这份声明了加密吗"，判是就由调用方
// 放弃整个条目，见下面 encrypted 的准确判据）、不处理
// EXT-X-DISCONTINUITY / BYTERANGE / DATERANGE。
// 首行不是 "#EXTM3U" 的 body 一律当作"没有可预加载的东西"——FFmpeg 的
// hls.c:849 是字面 strcmp，那份 body 它整份拒收，暖它的分片纯属白下。
//
// 【为什么可以这么糙】它只服务预加载。扫错的代价是**白下或少下几个分片**，
// 播放路径一个字节都不受影响——播放走的是 FFmpeg 自带的 hls 解封装器 +
// HlsSession 接管 IO。
//
// 【与 src/media/hls/url_rewrite.h 的重复，是有意的】那边的 channel_for()
// 与 playlist_declares_encryption() 跟这里的 is_playlist_url()、
// PlaylistScan::encrypted 判据相同、实现另一份。dl 层不许依赖 media 层
// （syp_dl 必须零 FFmpeg、零平台符号），所以只能各写一份。
// **只剩这两对**：dl 侧曾经还有一份 PlaylistScan::has_endlist（media 的
// playlist_declares_endlist 的第二份转写），`grep -rn has_endlist src/` 零
// 消费点、两份转写却已经漂了，删掉了（见 m3u8_scan.cpp 里
// scan_playlist 主循环处的注释）。
//
// 【两边的判据表只有一张：tests/support/hls_cases.h】两个测试文件 include
// **同一个文件**（kChannelCases / kEncryptionCases 钉两侧；kEndlistCases 现在
// 退化成 media 单边表，只有 test_hls_url_rewrite.cpp 消费）。
// 早先是两份同名字面量各抄一张，那只挡得住"改了代码忘了改用例"，挡不住
// "改了代码顺手把自己那张表也改了"——加密判据就是这么漂的。**改任何一侧
// 都要在那个文件里加用例**。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace syp::dl {

// URL 的**路径部分**（先剥 ?query 与 #fragment）是否以 .m3u8 / .m3u 结尾，
// 大小写不敏感。空串返回 false。判据与 hls::channel_for 的 Playlist 分支一致。
bool is_playlist_url(std::string_view url) noexcept;

// 把 ref 解析成绝对 URL。base 必须是带 scheme 的绝对 URL。
// base 的 ?query 与 #fragment 在**所有分支**里都先剥掉（绝对路径
// 那一支曾经漏剥过，于是 resolve_url("http://h?x=1", "/root.ts") 得出
// "http://h?x=1/root.ts"）。
//   带 scheme 的 ref（"xxx://…"）→ ref 整条取代 base
//   "//host/path"           → base 的 scheme + ref
//   "/path"                 → base 的 scheme://authority + ref
//   其它                    → base 去掉 query/fragment 与最后一段之后 + ref
// **四条支的结果都要再做一遍** RFC 3986 §5.2.4 的 remove_dot_segments
// （只动路径，query/fragment 不动），实现是 libavformat/url.c:166 append_path
// 的逐条转写。
//
// 【第一条支也归一，别按"带 scheme 就原样返回"去改】`ff_make_absolute_url2` 里
//     if (URL_COMPONENT_HAVE(uc, scheme))    simplify_path = 0;
//     if (URL_COMPONENT_HAVE(uc, authority)) simplify_path = 1;   // 覆盖上一行
// 只读第一行就会得出"不归一"。而我们这一支的判据是 ref 里有 "://"，
// scheme 与 authority **都在** ⇒ 落在第二行 ⇒ simplify_path = 1。实测
// ff_make_absolute_url("http://h/a/m.m3u8", "https://o/x/../y.ts")
// = "https://o/y.ts"，不是原样。
// base 或 ref 为空 → 返回空串。
//
// 【判据是"与 ff_make_absolute_url 逐字节一致"，不是 RFC 的字面】播放侧
// 解析分片 URI 用的就是 ff_make_absolute_url（hls.c:1045 → hls_session.cpp），
// 而 CacheStore::make_key 哈希 URL **原串**、不归一，所以我们只要和它差一个
// 字节，暖进去的分片就永远命中不了。判据由离树探针**直链 libavformat.a 调
// 真的 ff_make_absolute_url** 校验。
//
// 【已知且故意保留的分歧，共 4 类，全部落在扫描器产不出的输入上】
//   A base 或 ref 为空       → 我们返回空串；扫描器跳过空行，到不了。
//   B base 没有 "scheme://" → 我们返回空串（FFmpeg 会拼出相对结果）。
//     expand_playlist 对空串是 `if (u.empty()) continue;`，无害。
//   C ref 形如 "data:foo" / "x:y"（有 scheme 冒号但没有 "//"）→ FFmpeg 认它
//     是绝对 URL，我们当相对路径拼。这类 URI 在 media 层同样打不开，那条流
//     本来就播不了。
//   D ref 只有 query/fragment（"?q=1" / "#z"）→ FFmpeg 保留 base 的文件名，
//     我们拼在目录后面。没有任何打包器会这么写分片 URI。
std::string resolve_url(std::string_view base, std::string_view ref);

struct PlaylistScan {
    bool is_master   = false;   // 见过 #EXT-X-STREAM-INF（过闸后才算）
    bool encrypted   = false;   // 见下面的判据；**不受首行闸影响**
    std::string first_variant;
    std::string map_uri;
    struct Segment {
        std::string url;
        double      duration_s = 0.0;
    };
    std::vector<Segment> segments;
};

// 扫描规则（逐条，**实现以此为准**）：
//
//   · **首行闸**：首行必须恰好是 "#EXTM3U"，逐字照抄 FFmpeg 8.1.2 的
//     hls.c:849（`ff_get_chomp_line` 之后 `strcmp(line, "#EXTM3U")`）。
//     "首行"是 body 开头到第一个 '\n' / '\r' / NUL 为止、再剥掉行尾
//     av_isspace 字符的那一段；**行首一个字节都不剥，也不区分大小写地放宽**。
//     所以 UTF-8 BOM、前导空格、小写 #extm3u 一律**不过闸**。
//     不过闸 ⇒ is_master / first_variant / map_uri / segments 全部为空
//     —— "没有任何可预加载的东西"。理由：FFmpeg 那边 parse_playlist
//     第一行就整份拒收，那条流无论如何也播不了。
//     **一个例外**：超长首行。FFmpeg 先截到 4095 字节再剥行尾空白，两步
//     合起来能从 5008 字节的行里剥出恰好 "#EXTM3U"；我们不模拟截断，于是
//     在这一类上比 FFmpeg 更严（实测两条，见 m3u8_scan.cpp 里
//     first_line_is_extm3u 的注释）。方向单边：只会少暖，不会暖错。
//     **encrypted 是唯一不受这道闸管的字段**，见下。
//
//   · 行尾的 \r 被剥掉；行首尾的空白被剥掉；空行跳过。（这比 FFmpeg 宽，
//     方向无害：多认几行畸形写法。**不剥 BOM**。）
//   · "#EXT-X-STREAM-INF" 开头的行 ⇒ is_master = true，其后**第一条**非空
//     非注释行是变体 URI（只记第一条）。
//   · "#EXTINF:" 开头的行 ⇒ 取冒号之后到第一个逗号之前的浮点数作为时长
//     （解析失败记 0），其后第一条非空非注释行是分片 URI。
//   · "#EXT-X-MAP:" 开头的行 ⇒ 取属性里 URI="..." 的值。
//   · 其它 '#' 开头的行忽略；不在上述两种标签之后的非 '#' 行也忽略。
//   · 所有 URI 都用 base_url 绝对化。
//
//   · **encrypted**（"#EXT-X-KEY:" 开头的行，标签本身大小写不敏感）：
//     属性按 HLS 属性列表语法切分（逗号分隔，**双引号内的逗号不算分隔符**），
//     所以 URI="a,METHOD=AES-128" 这类引号内的字样不参与判定。
//     判据是"**证明了 METHOD=NONE 才放行**"，不是"没证明加密就放行"：
//       - 同一行里 METHOD 出现多次 ⇒ 取**最后一个**（FFmpeg 的
//         ff_parse_key_value + handle_key_args 是 last-wins，utils.c:507-559
//         / hls.c:401-414）；
//       - METHOD 缺失 / 值为空串 / 被未闭合的引号吞掉 ⇒ **算加密**；
//       - 只有剥掉可能的包裹引号之后大小写不敏感地等于 "NONE" 才算不加密。
//     任意一行声明了加密，整份就算加密。
//     **它不受首行闸影响**：这是 dl 与 media 两侧唯一要求逐比特一致的判据
//     （media 侧 playlist_declares_encryption 没有、也不该有首行闸——那是
//     防止密钥被请求的最后一道关），给它加闸会当场造出新的分歧类。
//     闸住的条目 encrypted 取两个值
//     **不是**"对 Preloader 同一个结果"——true ⇒ Failed，false ⇒ Done 且
//     ++completed_（preloader.cpp:629-648 实测）。决定不变，依据是上面那条
//     "不造新分歧类"；细节见 m3u8_scan.cpp 里 scan_playlist 的注释。
//
//   · **#EXT-X-ENDLIST 一个字都不认**：预加载不区分直播与点播，
//     直播同样只暖一轮。曾经有过的 has_endlist 字段是 media 侧
//     playlist_declares_endlist 的第二份转写、零消费点、且两份已经漂了
//     （NUL 是不是行终止符），已删。
PlaylistScan scan_playlist(std::string_view text, std::string_view base_url);

// 挑前 K 个分片：最小的 K 使 sum(EXTINF) >= want_ms / 1000。
// segs 为空返回 0；否则至少 1；至多 min(segs.size(), cap)。
// 时长为 0（解析不出）的分片按"贡献 0 秒"处理，于是会一直数到 cap ——
// 宁可多暖一个，也不要因为一个解析不出的时长就只暖一个分片。
size_t segments_for_ms(const std::vector<PlaylistScan::Segment>& segs,
                       int64_t want_ms, size_t cap) noexcept;

}  // namespace syp::dl
