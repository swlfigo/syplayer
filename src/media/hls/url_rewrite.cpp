#include "media/hls/url_rewrite.h"

#include <algorithm>
#include <cctype>

namespace syp::media::hls {
namespace {

char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// 剥掉 ?query 与 #fragment，只留路径（含 scheme 与 host，够判后缀了）。
std::string_view path_part(std::string_view url) noexcept {
    const std::size_t cut = url.find_first_of("?#");
    return cut == std::string_view::npos ? url : url.substr(0, cut);
}

bool ends_with_ci(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() < suffix.size()) return false;
    const std::size_t off = s.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (lower(s[off + i]) != suffix[i]) return false;
    }
    return true;
}

}  // namespace

Channel channel_for(std::string_view url) noexcept {
    const std::string_view p = path_part(url);
    if (ends_with_ci(p, ".m3u8") || ends_with_ci(p, ".m3u")) return Channel::Playlist;
    return Channel::Segment;
}

std::string scheme_of(std::string_view url) {
    const std::size_t pos = url.find("://");
    if (pos == std::string_view::npos || pos == 0) return std::string();
    std::string s(url.substr(0, pos));
    std::transform(s.begin(), s.end(), s.begin(), lower);
    return s;
}

std::string to_ffmpeg_url(std::string_view real_url) {
    if (scheme_of(real_url) != "https") return std::string(real_url);
    // "https" 是 5 个字符，换成 "http"（4 个），"://" 及之后原样保留。
    std::string out("http");
    out.append(real_url.substr(5));
    return out;
}

std::string to_real_url(std::string_view ffmpeg_url, std::string_view real_scheme) {
    if (real_scheme.empty()) return std::string(ffmpeg_url);
    if (scheme_of(ffmpeg_url) != "http") return std::string(ffmpeg_url);
    std::string out(real_scheme);
    out.append(ffmpeg_url.substr(4));   // "http" 之后的 "://..." 原样接上
    return out;
}

namespace {

bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) return false;
    return ieq(s.substr(0, prefix.size()), prefix);
}

// 去掉两端的一对双引号（有就去，没有原样返回）。METHOD 的值本身按 HLS
// spec 从不加引号（enumerated-string），这里纯防御——真实世界见过写成
// METHOD="NONE" 的畸形播放列表，不认引号
// 就会把它误判成"值是 `"NONE"` 不是 `NONE`，字符串不相等"。
std::string_view strip_quotes(std::string_view s) noexcept {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// 在一份属性列表（逗号分隔的 NAME=VALUE，值可能被双引号包住）里找
// METHOD 属性。
//
// 【必须这样写：不能只找"METHOD="子串】旧实现是
// `line.find("METHOD=")`，不认引号——`URI="...METHOD=NONE...",
// METHOD=AES-128` 会命中 URI 值**里面**那个假的 METHOD=NONE，把一条真的
// 加密声明判成未加密，直接推翻本任务存在的理由（"密钥 URL 一次都不该
// 被请求"）。这里改成正经的属性列表扫描：逐字符走，用 in_quotes 盯住
// 是否在双引号内，**只在顶层（不在引号内）的逗号处切分属性**——URI 值
// 内部的逗号/等号一律不当分隔符看。
//
// 返回值：*found 是有没有 METHOD 属性（区分"没有"和"有但值是空串"这类
// 边角）；*value 是它的值（未剥引号，调用方自己剥）。同名属性出现多次
// （畸形）时保留**最后一次**命中的值。
//
// 【为什么是 last-wins】逐字照抄 FFmpeg 8.1.2：
// ff_parse_key_value（libavformat/utils.c:507-559）是个无限 for，每遇到一个
// `key=` 就回调一次 callback_get_buf；hls.c:401-414 的 handle_key_args 对
// `METHOD=` 一律 `*dest = info->method`（指回**缓冲开头**），然后
// ff_parse_key_value 从 dest 开始重写值并在末尾补 '\0'。于是同一行里第二个
// METHOD 会把第一个整个覆盖掉。旧的 first-wins 写法在
// `#EXT-X-KEY:METHOD=NONE,METHOD=AES-128,URI="k.bin"` 上返回"未加密"，而
// FFmpeg 会置 key_type = KEY_AES_128 并**真的去请求 k.bin**——那是这条判据
// 唯一还漏得掉密钥请求的路径，直接推翻"密钥 URL 一次都不该被请求"。
// dl 侧 src/dl/m3u8_scan.cpp 的同名函数是本函数的逐条转写，两边一起改。
//
// 【未对齐之处，记在这里】FFmpeg 只在每个 pair 的**开头**跳过逗号，找 '='
// 用的是 strchr（会跨过逗号），所以 `FOO,METHOD=AES-128` 在它眼里 key 是
// "FOO,METHOD=" ⇒ 认不出 METHOD ⇒ 判未加密；本实现按逗号切分会认出来 ⇒
// 判加密。方向是**更保守**的那一侧（多拦），与本函数的既定取舍一致。
void find_method_attr(std::string_view attrs, bool* found, std::string_view* value) noexcept {
    *found = false;
    bool        in_quotes = false;
    std::size_t start     = 0;
    for (std::size_t i = 0; i <= attrs.size(); ++i) {
        const bool at_end = (i == attrs.size());
        // 走到字符串末尾时，当成"还有一个逗号"来flush 最后一个属性——
        // 属性列表末尾没有尾随逗号，不这样处理会漏掉最后一条属性。
        const char c = at_end ? ',' : attrs[i];
        if (!at_end && c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (in_quotes) continue;
        if (c == ',') {
            std::string_view attr = attrs.substr(start, i - start);
            // HLS 属性列表语法允许逗号周围有空白；防御性剥掉。
            while (!attr.empty() && (attr.front() == ' ' || attr.front() == '\t')) {
                attr.remove_prefix(1);
            }
            if (istarts_with(attr, "METHOD=")) {
                *found = true;
                *value = attr.substr(7);   // "METHOD=" 长 7；后者覆盖前者
            }
            start = i + 1;
        }
    }
}

// 一行 EXT-X-KEY 是否声明了加密：METHOD 属性存在且不是 NONE。attrs 是
// "#EXT-X-KEY:" 之后的部分（属性列表本身，不含标签）。
// METHOD 属性本身缺失（畸形属性列表）也按"声明了加密"处理——判据是
// "证明了 METHOD=NONE 才放行"，不是"没证明加密就放行"。
bool key_line_is_encrypted(std::string_view attrs) noexcept {
    bool             found = false;
    std::string_view value;
    find_method_attr(attrs, &found, &value);
    if (!found) return true;
    return !ieq(strip_quotes(value), "NONE");
}

// "#EXT-X-KEY:" 本身，11 个字符。playlist_declares_encryption() 用它判
// 一行是不是这个标签，key_line_is_encrypted() 只需要看它后面的属性列表——
// 两处共用同一个字面量，避免长度算错这种低级错误各写一遍。
constexpr std::string_view kKeyTagPrefix = "#EXT-X-KEY:";

}  // namespace

bool playlist_declares_encryption(std::string_view playlist_text) noexcept {
    // 【这份安全性依赖 FFmpeg 当前的实现，不是协议保证，必须写死在这里】
    // 实测过 FFmpeg 8.1.2 的 libavformat/hls.c：`grep -n "EXT-X-.*KEY"
    // hls.c` 只有一处命中——hls.c:871 的
    // `av_strstart(line, "#EXT-X-KEY:", &ptr)`。`#EXT-X-SESSION-KEY:`
    // （HLS spec 里 master 播放列表上的对应标签）在整个文件里一次都没
    // 出现——FFmpeg 压根不认它，没有任何解析分支。这意味着即使 master
    // 播放列表上只在 EXT-X-SESSION-KEY 里声明了加密，FFmpeg 也不会因此
    // 去拉密钥；而本函数只扫 media 播放列表（真正被 hls.c 消费、进而可能
    // 触发取密钥的那份 body），加密仍然会在那一层被本函数拦下。这条结论
    // **依赖 FFmpeg 8.1.2 的这个具体行为**，换一个把 EXT-X-SESSION-KEY
    // 也解析了的 FFmpeg 版本，这个论证就要重新核实——所以写在这里而不是
    // 只记在脑子里。
    //
    // 【我们的匹配比 FFmpeg 更宽，方向是安全的】`av_strstart`（非
    // `av_stristart`）逐字符 `==` 比较，大小写敏感；喂给它的 line 来自
    // `ff_get_chomp_line`（aviobuf.c），只从**行尾**剥 `av_isspace` 字符，
    // 不剥行首——也就是说 FFmpeg 那行匹配既大小写敏感、也不容忍行首空白。
    // 这里的匹配（大小写不敏感 + 容忍行首空白）是它的**严格超集**——凡是
    // FFmpeg 会认成密钥标签而去处理的行，本函数一定也会看见并拦下；反过来
    // 不成立（本函数可能拦下一些 FFmpeg 其实不认的畸形写法），但那只会让
    // 判定更保守，不会漏判。
    std::size_t pos = 0;
    while (pos <= playlist_text.size()) {
        const std::size_t nl   = playlist_text.find('\n', pos);
        std::string_view  line = (nl == std::string_view::npos)
                                     ? playlist_text.substr(pos)
                                     : playlist_text.substr(pos, nl - pos);
        // 行尾 \r（CRLF 播放列表）与行首空白都不算数据的一部分。
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        if (istarts_with(line, kKeyTagPrefix) &&
            key_line_is_encrypted(line.substr(kKeyTagPrefix.size()))) {
            return true;
        }

        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return false;
}

bool playlist_declares_endlist(std::string_view playlist_text) noexcept {
    // 【逐字照抄 FFmpeg 8.1.2 的读法，别"改进"】与上面加密判定不同，这里
    // 没有"更宽才安全"的方向：说有而 FFmpeg 没认，直播的异常结束会被放过
    // 成 Eof；说没有而 FFmpeg 认了，正常播完会被报成错误。所以：
    //   · 行由 '\n' 或 '\r' 切分，只剥行尾空白、不剥行首
    //     （aviobuf.c ff_get_line / ff_get_chomp_line）；
    //   · 首行必须恰好是 "#EXTM3U"（hls.c:849，否则 parse_playlist 失败）；
    //   · 标签是大小写敏感的前缀匹配 av_strstart(line, "#EXT-X-ENDLIST")
    //     （hls.c:979）。
    // 已知不对齐之处：FFmpeg 的行缓冲有长度上限、遇 NUL 截断，以及首行之后
    // 的其他解析失败（属性畸形、ENOMEM 等）——这些情况下本函数可能说"有"
    // 而 FFmpeg 其实解析失败。真实播放列表里罕见。
    static constexpr std::string_view kEndlist = "#EXT-X-ENDLIST";
    bool first = true;
    std::size_t pos = 0;
    while (pos < playlist_text.size()) {
        const std::size_t eol = playlist_text.find_first_of("\r\n", pos);
        std::string_view line = (eol == std::string_view::npos)
                                    ? playlist_text.substr(pos)
                                    : playlist_text.substr(pos, eol - pos);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                 line.back() == '\v' || line.back() == '\f')) {
            line.remove_suffix(1);
        }
        if (first) {
            if (line != "#EXTM3U") return false;
            first = false;
        } else if (line.substr(0, kEndlist.size()) == kEndlist) {
            return true;
        }
        if (eol == std::string_view::npos) break;
        // "\r\n" 算一个行尾（ff_get_line 在 '\r' 后吞掉紧跟的 '\n'）。
        pos = eol + 1;
        if (playlist_text[eol] == '\r' && pos < playlist_text.size() &&
            playlist_text[pos] == '\n') {
            ++pos;
        }
    }
    return false;
}

namespace {

// 一行 #EXT-X-MEDIA 的属性列表里 TYPE 是否为 SUBTITLES。按 HLS 属性列表语法切分：
// 逗号分隔，双引号内的逗号不算；属性名大小写敏感（与 hls.c 的 ff_parse_key_value 一致）。
bool media_line_is_subtitles(std::string_view attrs) noexcept {
    std::size_t start = 0;
    bool        in_quotes = false;
    for (std::size_t i = 0; i <= attrs.size(); ++i) {
        const bool end = (i == attrs.size());
        if (!end && attrs[i] == '"') in_quotes = !in_quotes;
        if (end || (attrs[i] == ',' && !in_quotes)) {
            std::string_view kv = attrs.substr(start, i - start);
            while (!kv.empty() && (kv.front() == ' ' || kv.front() == '\t')) kv.remove_prefix(1);
            static constexpr std::string_view kType = "TYPE=";
            if (kv.substr(0, kType.size()) == kType) {
                return strip_quotes(kv.substr(kType.size())) == "SUBTITLES";
            }
            start = i + 1;
        }
    }
    return false;
}

}  // namespace

std::string strip_subtitle_renditions(std::string_view playlist_text) {
    static constexpr std::string_view kMedia = "#EXT-X-MEDIA:";
    std::string out;
    out.reserve(playlist_text.size());
    std::size_t pos = 0;
    while (pos < playlist_text.size()) {
        std::size_t eol = playlist_text.find_first_of("\r\n", pos);
        std::size_t next = playlist_text.size();
        std::string_view line = playlist_text.substr(pos);
        if (eol != std::string_view::npos) {
            line = playlist_text.substr(pos, eol - pos);
            next = eol + 1;
            if (playlist_text[eol] == '\r' && next < playlist_text.size() && playlist_text[next] == '\n') {
                ++next;
            }
        }
        const bool drop = line.substr(0, kMedia.size()) == kMedia &&
                          media_line_is_subtitles(line.substr(kMedia.size()));
        if (!drop) out.append(playlist_text.substr(pos, next - pos));
        pos = next;
    }
    return out;
}

}  // namespace syp::media::hls
