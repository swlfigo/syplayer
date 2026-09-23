// hls_cases.h — dl 侧与 media 侧两份 HLS 判据的**唯一**一张共享用例表。
//
// 【为什么它必须是一个文件而不是两份字面量】dl 的
// syp::dl::is_playlist_url / PlaylistScan::encrypted 与 media 的
// hls::channel_for / hls::playlist_declares_encryption 判据相同、实现各一份
// （Preloader 在 dl 层，dl 不许依赖 media）。原来两个测试文件里各抄了一张
// 表——那只挡得住"改了代码忘了改用例"，挡不住"改了代码顺手把自己那张表也
// 改了"，而后者正是加密判据已经漂移了却没人发现的原因（16 份
// 播放列表中有 4 条分歧：没有 METHOD 属性、空 METHOD、小写标签、未闭合的引号）。
//
// 所以两边**include 同一个文件**：任何一侧的行为改变都会立刻在另一侧变红。
// 加用例只加在这里。
//
// 两张表钉住两对判据、一张表钉住 media 单边的一个函数：
//   kChannelCases    is_playlist_url           ↔ channel_for
//   kEncryptionCases scan_playlist().encrypted ↔ playlist_declares_encryption
//   kEndlistCases    playlist_declares_endlist（**只有 media 一侧**）
// kEndlistCases 曾经也是两侧对钉的，dl 那一侧是 PlaylistScan::has_endlist。
// 它后来删了：同一道 FFmpeg 闸的第二份
// 转写、`grep -rn has_endlist src/` 零消费点，而两份还漂了（NUL 算不算行
// 终止符）。表本身留着——media 侧的 playlist_declares_endlist 在 HlsSession
// 里有真实调用方，这张表是它唯一的守护。
// 离树探针（报告里的 fuzz）在**两对**判据上跑穷举 + 随机拼接，当前分歧类 0。
//
// 被包含方：
//   tests/test_preloader_hls.cpp   —— dl 侧
//   tests/test_hls_url_rewrite.cpp —— media 侧
#pragma once

namespace syp::test::hls_cases {

// ---- 通道判据：这个 URL 是不是播放列表 ----
// dl: is_playlist_url(url) == playlist
// media: channel_for(url) == (playlist ? Channel::Playlist : Channel::Segment)
struct ChannelCase {
    const char* url;
    bool        playlist;
};

inline constexpr ChannelCase kChannelCases[] = {
    {"http://h/a.m3u8", true},
    {"http://h/a.M3U8", true},           // CDN 上出现过大写后缀
    {"http://h/a.m3u", true},
    {"http://h/a.m3u8?token=1", true},   // 先剥 query 才看得见后缀
    {"http://h/a.m3u8#frag", true},
    {"http://h/seg.ts", false},
    {"http://h/seg.m4s", false},
    {"http://h/v/init.mp4", false},
    {"http://h/noext", false},           // 无后缀保守当分片，见 url_rewrite.h
    {"http://h/a.m3u8x", false},
    {"", false},
};

// ---- 加密判据：这份播放列表声明了本版本不支持的加密吗 ----
// dl: scan_playlist(text, base).encrypted == encrypted
// media: playlist_declares_encryption(text) == encrypted
//
// 【判据是"证明了 METHOD=NONE 才放行"】METHOD 缺失 / 为空 / 被未闭合的引号
// 吞掉，一律算加密。方向是单边的：多拦一条畸形播放列表只损失一次预加载，
// 漏拦一条真加密的就要去请求密钥（media 侧）或白下一批 media 层根本不会
// 打开的分片（dl 侧）。
struct EncryptionCase {
    const char* name;
    const char* text;
    bool        encrypted;
};

inline constexpr EncryptionCase kEncryptionCases[] = {
    {"no key tag at all",
     "#EXTM3U\n#EXTINF:4,\nseg0.ts\n#EXT-X-ENDLIST\n", false},
    {"METHOD=NONE",
     "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4,\nseg0.ts\n", false},
    {"METHOD=AES-128",
     "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\"\n#EXTINF:4,\nseg0.ts\n", true},
    {"METHOD=SAMPLE-AES",
     "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k.bin\"\n", true},
    // 值本身不该加引号，但畸形播放列表里见过；剥掉引号再比。
    {"quoted NONE",
     "#EXTM3U\n#EXT-X-KEY:METHOD=\"NONE\"\n", false},
    {"lowercase none value",
     "#EXTM3U\n#EXT-X-KEY:METHOD=none\n", false},
    // 属性名前后允许空白。
    {"space before METHOD",
     "#EXTM3U\n#EXT-X-KEY: METHOD=NONE\n", false},
    {"leading whitespace and CRLF",
     "#EXTM3U\r\n  #EXT-X-KEY:METHOD=AES-128\r\n", true},
    // 标签本身大小写不敏感：加密判定宁可多拦（其余标签仍然大小写敏感，
    // 与 FFmpeg 的 hls.c 一致）。
    {"lowercase tag with AES",
     "#EXTM3U\n#ext-x-key:METHOD=AES-128,URI=\"k.bin\"\n", true},
    {"lowercase tag with NONE",
     "#EXTM3U\n#ext-x-key:METHOD=NONE\n", false},
    // 属性列表切分：引号内的逗号/等号不参与切分，两个方向都要试。
    {"fake METHOD inside a quoted value",
     "#EXTM3U\n#EXT-X-KEY:METHOD=NONE,URI=\"a,METHOD=AES-128\"\n", false},
    {"real METHOD after a quoted decoy",
     "#EXTM3U\n#EXT-X-KEY:URI=\"x,METHOD=NONE\",METHOD=AES-128\n", true},
    // 缺 METHOD / 空 METHOD / 未闭合的引号：一律按加密处理。
    {"no METHOD attribute",
     "#EXTM3U\n#EXT-X-KEY:URI=\"k.bin\"\n#EXTINF:4,\nseg0.ts\n", true},
    {"empty METHOD value",
     "#EXTM3U\n#EXT-X-KEY:METHOD=\n", true},
    {"unterminated quote swallows METHOD",
     "#EXTM3U\n#EXT-X-KEY:URI=\"abc,METHOD=NONE\n", true},
    // master 上的 SESSION-KEY 两侧都不认（FFmpeg 8.1.2 里没有任何解析分支，
    // 见 url_rewrite.cpp 的论证）；真正的加密会在 media 播放列表上被拦下。
    {"session key is not the key tag",
     "#EXTM3U\n#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"k.bin\"\n", false},
    // 多行：任何一行声明了加密就算加密。
    {"second key line declares AES",
     "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4,\ns0.ts\n"
     "#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\"\n#EXTINF:4,\ns1.ts\n", true},
    // ---- 同一行里 METHOD 出现多次：取**最后一个** ----
    // FFmpeg 的 ff_parse_key_value（utils.c:507-559）对每一个 key= 都回调
    // handle_key_args（hls.c:401-414），后者把 dest 指回 info->method 的开头
    // 重写一遍 —— 后者完整覆盖前者。first-wins 的旧写法在下面第一条上判
    // "未加密"，而 FFmpeg 会置 KEY_AES_128 并**真的去请求 k.bin**。
    {"duplicate METHOD: NONE then AES-128 (last wins)",
     "#EXTM3U\n#EXT-X-KEY:METHOD=NONE,METHOD=AES-128,URI=\"k.bin\"\n"
     "#EXTINF:4,\nseg0.ts\n", true},
    {"duplicate METHOD: AES-128 then NONE (last wins)",
     "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\",METHOD=NONE\n"
     "#EXTINF:4,\nseg0.ts\n", false},
    // last-wins 只对**顶层**属性成立：引号里的 METHOD= 不是属性，不能覆盖
    // 前面那个真的。少了这条，"取最后一个"会退化成"找最后一个 METHOD= 子串"。
    {"later METHOD inside a quoted value does not override",
     "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k,METHOD=NONE,z\"\n", true},
    // ---- UTF-8 BOM：两侧都**不**剥 ----
    // FFmpeg 的 hls.c:849 是字面 strcmp(line, "#EXTM3U")，带 BOM 的 body 整份
    // 被拒收（AVERROR_INVALIDDATA），密钥当然也就不会被请求。所以这一条的
    // 期望值是 false —— 它同时钉住两个方向：dl 侧重新加回 BOM 剥离会变红
    // （它会把 BOM 之后的 #EXT-X-KEY 认出来 ⇒ true），media 侧新加 BOM 剥离
    // 同样变红。
    {"BOM before the key tag line is not stripped",
     "\xEF\xBB\xBF#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\"\n", false},
    // BOM 只挡住它**所在的那一行**：第二行起的 #EXT-X-KEY 照常认。两侧都
    // 不给 encrypted 加首行闸（加了会在这一条上分歧），理由见 m3u8_scan.cpp
    // 里 scan_playlist 的注释。
    {"BOM on line 1 does not hide a key tag on line 2",
     "\xEF\xBB\xBF#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\"\n", true},
};

// ---- ENDLIST 判据：这份播放列表在 FFmpeg 眼里"已经结束"了吗 ----
// media: playlist_declares_endlist(text) == endlist
// **单边表**（见文件头）：dl 侧不再有对应的判据。
//
// 【这张表同时是 #EXTM3U 首行闸的表】判定逐字照抄 FFmpeg 8.1.2：
//   · ff_get_chomp_line（aviobuf.c:772/789）：行由 '\n' 或 '\r' 切分，
//     "\r\n" 算一个行尾，只剥**行尾**的 av_isspace，行首一个字节都不剥；
//   · hls.c:849：首行必须恰好是 "#EXTM3U"，否则 parse_playlist 直接失败；
//   · hls.c:979：av_strstart(line, "#EXT-X-ENDLIST") 是大小写敏感的前缀匹配。
// 带 BOM 的那几条是这份判据的锚：media 侧一旦加回 BOM 剥离就会变红。
// （dl 侧同一件事由 test_preloader_hls.cpp 的
// scan_yields_nothing_preloadable_without_the_extm3u_header 守住。）
struct EndlistCase {
    const char* name;
    const char* text;
    bool        endlist;
};

inline constexpr EndlistCase kEndlistCases[] = {
    {"plain", "#EXTM3U\n#EXTINF:4,\ns0.ts\n#EXT-X-ENDLIST\n", true},
    {"no trailing newline", "#EXTM3U\n#EXT-X-ENDLIST", true},
    {"live playlist has none", "#EXTM3U\n#EXTINF:4,\ns0.ts\n", false},
    {"empty", "", false},
    {"CRLF", "#EXTM3U\r\n#EXT-X-ENDLIST\r\n", true},
    {"bare CR is a line break too", "#EXTM3U\rx\r#EXT-X-ENDLIST\r", true},
    {"trailing blanks on the header line are chomped",
     "#EXTM3U  \r\n#EXT-X-ENDLIST\r\n", true},
    // 【av_isspace 的六个字符一个不能少】ff_get_chomp_line 剥的是
    // { ' ', '\f', '\n', '\r', '\t', '\v' }。实测：把
    // '\v' / '\f' 从 chomp 集里删掉，51 条表 + 920 万次 fuzz 一条都杀
    // 不掉（它点名的是 dl 那份转写，该函数已连同一起删；
    // **media 这份有一模一样的洞**，这张表是它唯一的守护）。
    // 方向是错的：FFmpeg 在 strcmp 前已经把这两个字符剥干净了，漏剥等于
    // 把一份它认的播放列表判成"没结束"。
    {"vertical tab and form feed are chomped too（M13）",
     "#EXTM3U\v\f\r\n#EXT-X-ENDLIST\r\n", true},
    {"vertical tab alone on the header line", "#EXTM3U\v\n#EXT-X-ENDLIST\n", true},
    {"form feed alone on the header line", "#EXTM3U\f\n#EXT-X-ENDLIST\n", true},
    // 首行闸。
    {"header missing", "#EXT-X-ENDLIST\n", false},
    {"header not first", "\n#EXTM3U\n#EXT-X-ENDLIST\n", false},
    {"leading space before the header", " #EXTM3U\n#EXT-X-ENDLIST\n", false},
    {"lowercase header", "#extm3u\n#EXT-X-ENDLIST\n", false},
    {"html error page", "<html>#EXT-X-ENDLIST</html>\n", false},
    // BOM：FFmpeg 的 strcmp 认不出来 ⇒ 整份拒收 ⇒ 谈不上"结束"。
    // 任一侧剥了 BOM，这两条就会变红。
    {"BOM before the header", "\xEF\xBB\xBF#EXTM3U\n#EXT-X-ENDLIST\n", false},
    {"BOM before the header, CRLF", "\xEF\xBB\xBF#EXTM3U\r\n#EXT-X-ENDLIST\r\n", false},
    // 标签行本身的规则。
    {"lowercase tag", "#EXTM3U\n#ext-x-endlist\n", false},
    {"leading whitespace before the tag", "#EXTM3U\n  #EXT-X-ENDLIST\n", false},
    {"prefix match: ENDLISTX still counts", "#EXTM3U\n#EXT-X-ENDLISTX\n", true},
    {"not at line start", "#EXTM3U\nseg#EXT-X-ENDLIST\n", false},
};

}  // namespace syp::test::hls_cases
