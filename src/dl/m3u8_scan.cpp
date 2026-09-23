// m3u8_scan.cpp — 纯文本扫描，零 IO、零依赖。
#include "m3u8_scan.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace syp::dl {
namespace {

char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool ends_with_ci(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() < suffix.size()) return false;
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (lower(s[off + i]) != lower(suffix[i])) return false;
    }
    return true;
}

// 剥掉 ?query 与 #fragment，返回 scheme://host/path 那一段。
std::string_view path_part(std::string_view url) noexcept {
    const size_t q = url.find_first_of("?#");
    return q == std::string_view::npos ? url : url.substr(0, q);
}

// 就地对 url 的**路径部分**做 RFC 3986 §5.2.4 的 remove_dot_segments。
// path_begin 是路径那个前导 '/' 的下标；?query 与 #fragment 不参与
// （FFmpeg 也只归一路径，见 ff_make_absolute_url2 里 simplify_path 的分支）。
//
// 【这是 libavformat/url.c:166 的 append_path 的逐条转写，不是自己发明的】
// 原文（ffmpeg-8.1.2）：
//     if (in < in_end && *in == '/') in++;            /* already taken care of */
//     while (in < in_end) {
//         d = find_delim("/", in, in_end);
//         next = d + (d < in_end && *d == '/');
//         if (d - in == 1 && in[0] == '.') { /* skip */ }
//         else if (d - in == 2 && in[0] == '.' && in[1] == '.') {
//             if (out - root > 1) while (out > root && (--out)[-1] != '/');
//         } else { memmove(out, in, next - in); out += next - in; }
//         in = next;
//     }
// 三处容易抄错、都特地保留：
//   · `out - root > 1` 这道闸——root 是前导 '/' **之后**的位置，所以 ".." 在
//     "/"（out==root）和 "//"（out==root+1）上是**不弹的**，不是"弹到根为止"；
//   · 弹栈是"先减再看 out[-1]"，落点总在某个 '/' 之后，所以 "/a/b/" 弹一次
//     得 "/a/"，不是 "/a"；
//   · 段的判定是**逐字节**的 "." / ".."，不解百分号编码——"%2e%2e" 是一个
//     普通段，FFmpeg 也是这么看的。
//
// 【为什么必须做】变体播放列表落在
// `…/v1/index.m3u8`、分片写成 `../segs/0.ts` 是真实 CDN 的常见布局。不归一
// 的话我们去 GET `http://h/v1/../segs/0.ts`——CFNetwork 多半会在发包前归一，
// 字节可能真的下下来，**但永远命中不了**：CacheStore::make_key
// （src/dl/cache_store.cpp:207-214）哈希的是 URL 的**原串、不归一**，而播放
// 走 FFmpeg 的 ff_make_absolute_url（hls.c:1045）开的是
// `http://h/segs/0.ts`（src/media/hls/hls_session.cpp:502-507）⇒ 两个 key ⇒
// 这条播放列表下**每一个分片都是确定性 100% 白下**。
void remove_dot_segments(std::string& url, size_t path_begin) {
    if (path_begin == std::string::npos || path_begin >= url.size()) return;
    if (url[path_begin] != '/') return;
    size_t path_end = url.find_first_of("?#", path_begin);
    if (path_end == std::string::npos) path_end = url.size();

    const size_t root = path_begin + 1;   // 对应 append_path 的 root（前导 '/' 之后）
    size_t out = root;
    size_t in  = root;
    while (in < path_end) {
        size_t d = url.find('/', in);
        if (d == std::string::npos || d > path_end) d = path_end;
        const size_t next = (d < path_end) ? d + 1 : d;   // 段尾那个 '/' 归本段
        const size_t seg  = d - in;
        if (seg == 1 && url[in] == '.') {
            // "." 段：整段丢掉（连同它的 '/'）
        } else if (seg == 2 && url[in] == '.' && url[in + 1] == '.') {
            if (out - root > 1) {
                while (out > root) {
                    --out;
                    if (url[out - 1] == '/') break;
                }
            }
        } else {
            // out <= in 恒成立 ⇒ 正向逐字节搬运等价于 memmove
            for (size_t k = 0; k < next - in; ++k) url[out + k] = url[in + k];
            out += next - in;
        }
        in = next;
    }
    url.erase(out, path_end - out);       // 抹掉搬运后留下的空洞，query/fragment 原样保留
}

// 在一个已经绝对化的 URL 里找到路径那个前导 '/' 的下标；没有路径段返回 npos。
// 只在 ?query / #fragment **之前**找，否则 "http://h?x=/y" 会把 query 里的
// 斜杠当成路径开头（FFmpeg 的 ff_url_decompose 同样先切 authority 再切 path）。
size_t path_slash_offset(const std::string& url) noexcept {
    const size_t sep = url.find("://");
    if (sep == std::string::npos) return std::string::npos;
    const std::string_view p = path_part(url);
    const size_t slash = p.find('/', sep + 3);
    return slash == std::string_view::npos ? std::string::npos : slash;
}

std::string_view trim(std::string_view s) noexcept {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) return false;
    return equals_ci(s.substr(0, prefix.size()), prefix);
}

// 去掉两端的一对双引号（有就去，没有原样返回）。METHOD 的值按 HLS spec
// 从不加引号，这里纯防御——真实世界见过 METHOD="NONE" 的畸形播放列表。
std::string_view strip_quotes(std::string_view s) noexcept {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// 在一份属性列表里找 METHOD 属性。**这是 media 侧 find_method_attr 的逐条
// 转写**（src/media/hls/url_rewrite.cpp）：逐字符走，in_quotes 盯住是否在
// 双引号内，只在顶层逗号处切分；属性名前面的空白剥掉；同名属性出现多次
// 时保留**最后一次**。*found 区分"没有 METHOD"与"有但值是空串"——这个区分
// 是判据的一半，不能省。
//
// 【为什么是 last-wins 而不是 first-wins】FFmpeg 的
// ff_parse_key_value（libavformat/utils.c:507-559）对**每一次** key=value
// 都回调一次 handle_key_args（hls.c:401-414），而后者对 METHOD= 一律把
// dest 指回 info->method 的**开头**、从头写一遍再补 '\0'——也就是后一个
// METHOD 完整覆盖前一个。旧的 first-wins 在 `METHOD=NONE,METHOD=AES-128`
// 上判"未加密"，FFmpeg 却判 KEY_AES_128 并**真的去请求 URI 指的密钥**，
// 正好推翻本模块存在的理由。
void find_method_attr(std::string_view attrs, bool* found,
                      std::string_view* value) noexcept {
    *found = false;
    bool   in_quotes = false;
    size_t start     = 0;
    for (size_t i = 0; i <= attrs.size(); ++i) {
        const bool at_end = (i == attrs.size());
        const char c = at_end ? ',' : attrs[i];
        if (!at_end && c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (in_quotes) continue;
        if (c == ',') {
            std::string_view attr = attrs.substr(start, i - start);
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

// 一行 EXT-X-KEY 是否声明了加密。attrs 是 "#EXT-X-KEY:" 之后的属性列表。
//
// 【判据是"证明了 METHOD=NONE 才放行"，不是"没证明加密就放行"】METHOD
// 属性缺失（畸形属性列表、未闭合的引号把它吞掉）一律按加密处理。media 侧
// 曾经有过更弱的一版判据，后来被否掉，这里必须是同一条：两侧一旦漂移，dl 这边就会
// 去暖一条 media 层根本不会打开的流——那些字节 100% 是白下的。
bool key_line_is_encrypted(std::string_view attrs) noexcept {
    bool             found = false;
    std::string_view value;
    find_method_attr(attrs, &found, &value);
    if (!found) return true;
    return !equals_ci(strip_quotes(value), "NONE");
}

// "#EXT-X-KEY:" 本身，11 个字符。
constexpr std::string_view kKeyTagPrefix = "#EXT-X-KEY:";

// 按 HLS 属性列表语法取某个属性的值：逗号分隔，双引号内的逗号不算分隔符。
// 找不到返回空串。返回值已剥掉包裹的双引号。
std::string attribute_value(std::string_view line, std::string_view name) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) return {};
    std::string_view attrs = line.substr(colon + 1);
    size_t i = 0;
    while (i < attrs.size()) {
        // 一个属性：KEY=VALUE，VALUE 可能被双引号包裹
        const size_t eq = attrs.find('=', i);
        if (eq == std::string_view::npos) break;
        const std::string_view key = trim(attrs.substr(i, eq - i));
        size_t vb = eq + 1;
        size_t ve = vb;
        bool quoted = false;
        if (vb < attrs.size() && attrs[vb] == '"') {
            quoted = true;
            ++vb;
            ve = attrs.find('"', vb);
            if (ve == std::string_view::npos) ve = attrs.size();
        } else {
            ve = attrs.find(',', vb);
            if (ve == std::string_view::npos) ve = attrs.size();
        }
        const std::string_view val = attrs.substr(vb, ve - vb);
        if (equals_ci(key, name)) return std::string(trim(val));
        // 前进到下一个属性
        size_t next = quoted ? (ve < attrs.size() ? ve + 1 : ve) : ve;
        next = attrs.find(',', next);
        if (next == std::string_view::npos) break;
        i = next + 1;
    }
    return {};
}

// av_isspace（libavutil/avstring.h）认的那六个字符。
bool ff_isspace(char c) noexcept {
    return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
}

// 首行是不是恰好 "#EXTM3U" —— **逐字照抄 FFmpeg 8.1.2 的 hls.c:849**：
//     ff_get_chomp_line(in, line, sizeof(line));
//     if (strcmp(line, "#EXTM3U")) { ret = AVERROR_INVALIDDATA; goto fail; }
// ff_get_line（aviobuf.c:772）一直读到第一个 '\n' / '\r' / NUL 为止（NUL
// 既不入缓冲也终止本行）；ff_get_chomp_line（:789）再把行尾的 av_isspace
// 字符逐个剥掉。然后是**大小写敏感的 strcmp**，行首一个字节都不剥。
//
// 【为什么扫描器需要这道闸】不过闸的播放列表
// FFmpeg 在 parse_playlist 的第一行就整份拒收，那条流无论如何也播不了，
// 暖它的分片纯属白下。这正是本文件下面"其余标签保持大小写敏感"用的同一
// 条论证——BOM 剥离曾违反过它：`\xEF\xBB\xBF#EXTM3U` 在
// strcmp 眼里不等于 "#EXTM3U"，FFmpeg 拒收，我们却照样去暖。
//
// 【行缓冲截断：我们**不**模拟，分歧是单边的】
// 此前这里的论证是"截断不可能截出 #EXTM3U，要 maxlen==8"。**那个论证是错的**：
// 截断与剥空白是**两步**——ff_get_line 先把行截到前 maxlen-1 = 4095 字节，
// ff_get_chomp_line **再**剥行尾的 av_isspace。两步合起来完全可以剥出恰好
// "#EXTM3U"。用 avio_alloc_context 喂内存缓冲、直接调真的 ff_get_chomp_line
// 实测到两条（27 种首行形状里就这两条分歧）：
//     "#EXTM3U" + 5000 个空格 + "x"（长 5008）  FFmpeg 过闸，我们不过
//     "#EXTM3U" + 4088 个空格 + "x"（长 4096）  FFmpeg 过闸，我们不过
// 第一条：截到 4095 字节 = "#EXTM3U" + 4088 个空格（那个 'x' 被截掉），
// 再剥掉行尾空白 ⇒ 恰好 "#EXTM3U"。
//
// **结论仍然成立，但理由换成方向**：我们不模拟 4095 字节截断，于是只会在
// "超长首行"这一类上比 FFmpeg **更严**（它认、我们不认）。代价是少暖一份
// 播放列表；危险方向（我们认而 FFmpeg 不认 ⇒ 去暖一条播不了的流）不存在。
// 真要对齐得把 MAX_URL_SIZE 这个常数也抄过来，那是把 FFmpeg 的实现细节
// 焊进 dl 层，收益（一份首行长于 4095 字节的播放列表）远不抵成本。
bool first_line_is_extm3u(std::string_view text) noexcept {
    static constexpr char kDelims[] = {'\r', '\n', '\0'};
    const size_t end = text.find_first_of(std::string_view(kDelims, 3));
    std::string_view line = (end == std::string_view::npos) ? text : text.substr(0, end);
    while (!line.empty() && ff_isspace(line.back())) line.remove_suffix(1);
    return line == "#EXTM3U";
}

double parse_extinf_duration(std::string_view line) noexcept {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) return 0.0;
    std::string_view rest = line.substr(colon + 1);
    const size_t comma = rest.find(',');
    if (comma != std::string_view::npos) rest = rest.substr(0, comma);
    const std::string s(trim(rest));
    if (s.empty()) return 0.0;
    char* endp = nullptr;
    const double v = std::strtod(s.c_str(), &endp);
    if (endp == s.c_str() || v < 0.0) return 0.0;
    return v;
}

}  // namespace

bool is_playlist_url(std::string_view url) noexcept {
    if (url.empty()) return false;
    const std::string_view p = path_part(url);
    return ends_with_ci(p, ".m3u8") || ends_with_ci(p, ".m3u");
}

std::string resolve_url(std::string_view base, std::string_view ref) {
    if (base.empty() || ref.empty()) return {};
    // 带 scheme（形如 "xxx://"）的 ref 原样返回。
    const size_t sep = ref.find("://");
    if (sep != std::string_view::npos && sep > 0) {
        bool all_scheme_chars = true;
        for (size_t i = 0; i < sep; ++i) {
            const char c = ref[i];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-'
                  || c == '.')) {
                all_scheme_chars = false;
                break;
            }
        }
        // 【带 scheme 的 ref 整条取代 base，但**点段仍然要归一**】
        // ff_make_absolute_url2 里那三行的**顺序**是判据，抄漏一行就错：
        //     if (URL_COMPONENT_HAVE(uc, scheme))    simplify_path = 0;
        //     if (URL_COMPONENT_HAVE(uc, authority)) simplify_path = 1;   // 后置，覆盖上一行
        // 我们这一支的判据是 ref 里有 "://"，也就是 scheme **和** authority
        // 都在 ⇒ 落在第二行 ⇒ simplify_path = 1。实测
        // ff_make_absolute_url("http://h/a/m.m3u8", "https://o/x/../y.ts")
        // = "https://o/y.ts"，**不是**原样返回。
        // （只有 scheme 没有 authority 的 "x:y" / "data:foo" 才是 0——那种
        // 形状我们根本不当绝对 URL，见头文件。）
        if (all_scheme_chars) {
            std::string absolute(ref);
            remove_dot_segments(absolute, path_slash_offset(absolute));
            return absolute;
        }
    }
    // 【先剥 ?query 与 #fragment 再拆】authority 从**剥过**的 base
    // 里取：旧写法在 base 上取 after，于是没有路径段的 base 会把 query 或
    // fragment 当成 authority 的一部分——resolve_url("http://h?x=1","/root.ts")
    // 得出 "http://h?x=1/root.ts"（请求发到一个不存在的路径上）。相对路径
    // 那一支一直走的是 path_part()，所以只有绝对路径支有这个洞。
    // 顺带一个副作用是对的：base = "h#ttp://x" 这种 "://" 落在 fragment 里
    // 的串现在返回空串，而不是被当成 scheme 为 "h#ttp" 的 URL。
    const std::string_view bp = path_part(base);
    const size_t bsep = bp.find("://");
    if (bsep == std::string_view::npos) return {};
    const std::string_view scheme = bp.substr(0, bsep);     // 不含 "://"
    const std::string_view after  = bp.substr(bsep + 3);    // host[/path]
    const size_t slash = after.find('/');
    const std::string_view authority = slash == std::string_view::npos
                                           ? after : after.substr(0, slash);

    std::string out;
    if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/') {
        out = std::string(scheme) + ":" + std::string(ref);
    } else if (ref[0] == '/') {
        out = std::string(scheme) + "://" + std::string(authority) + std::string(ref);
    } else {
        // 相对路径：base 去掉 query/fragment（就是上面的 bp），再去掉最后一段。
        const size_t last = bp.rfind('/');
        // "scheme://" 里那两个斜杠在 bsep+1 与 bsep+2。找到的最后一个斜杠**就是**
        // bsep+2 时，base 只有 authority、没有路径段（"http://h"）——那时不能切，
        // 切了会把 host 也当成"最后一段"扔掉，得出 "http://seg.ts"。所以是 <=。
        const std::string_view dir =
            (last == std::string_view::npos || last <= bsep + 2) ? bp : bp.substr(0, last + 1);
        out.assign(dir);
        if (!out.empty() && out.back() != '/') out.push_back('/');
        out.append(ref);
    }
    // 【三条支都要归一】不止相对路径支：FFmpeg 的
    // simplify_path 在"保留了 base 的 authority"或"ref 自带 authority"时都为 1，
    // 所以 "//cdn/x/../y.ts" 与 "/a/../b.ts" 同样归一（实测 ff_make_absolute_url
    // 分别给出 "http://cdn/y.ts" 与 "http://h/b.ts"）。
    remove_dot_segments(out, path_slash_offset(out));
    return out;
}

PlaylistScan scan_playlist(std::string_view text, std::string_view base_url) {
    PlaylistScan out;
    // 首行闸：不是 "#EXTM3U" 的 body，FFmpeg 整份拒收（hls.c:849），
    // 于是**一个可预加载的东西都不产出**——不认 master、不认变体、不认
    // #EXT-X-MAP、不收分片。
    //
    // 【encrypted 故意**不**受这道闸管】
    // 它是 dl 与 media 两侧唯一要求逐比特一致的判据（共享表
    // tests/support/hls_cases.h 与离树 fuzz 都按"两者恒等"来校验），而 media
    // 侧的 playlist_declares_encryption 没有、也不该有这道闸——那个函数是
    // 防止密钥被请求的最后一道关，给它加闸等于给唯一的安全网开一个新口子。
    // 让 encrypted 跟着闸走会当场造出一个新的分歧类（带 BOM 且声明了 AES
    // 的播放列表：dl=false、media=true），把上一轮消灭的东西换个方向再造。
    //
    // 【更正一条写错的理由】这里曾经写的是
    // "闸住的条目 encrypted 取什么值对 Preloader 没有任何影响"。**那是假的**，
    // 实测 src/dl/preloader.cpp:629-648：
    //     闸住 + encrypted == true  → finish_expand_failed ⇒ state **Failed**、
    //                                 last_error = SYP_ERR_NOT_IMPLEMENTED
    //     闸住 + encrypted == false → k == 0 那一支 ⇒ state **Done**、++completed_
    // 两条路都"一个分片都没暖"，但对外的 state 与 get_stats().completed 不同。
    // 决定不变（理由是上面那条"不造新分歧类"，不是"行为等价"）；这条 Failed
    // 与 Done 的分野本身是**期望行为**：带 BOM 又声明了 AES 的播放列表，
    // 报 Failed/NOT_IMPLEMENTED 比报 Done 更诚实——调用方从 Done 读不出
    // "我一个字节都没给你暖"，从 Failed 能。反过来"闸住但没声明加密"报 Done
    // 则是既有取舍：首行之外的解析失败我们一律看不见，这里不改变这个取舍。
    const bool parseable = first_line_is_extm3u(text);

    bool want_variant = false;
    bool want_segment = false;
    double pending_duration = 0.0;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string_view raw =
            nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        const std::string_view line = trim(raw);
        if (line.empty()) continue;

        if (!parseable) {
            // 不过闸：只把 encrypted 算完（理由见上），其余一概不产出。
            if (line[0] == '#' && istarts_with(line, kKeyTagPrefix)
                && key_line_is_encrypted(line.substr(kKeyTagPrefix.size()))) {
                out.encrypted = true;
            }
            continue;
        }

        if (line[0] == '#') {
            if (line.starts_with("#EXT-X-STREAM-INF")) {
                out.is_master = true;
                want_variant  = out.first_variant.empty();
                want_segment  = false;
            } else if (line.starts_with("#EXTINF:")) {
                pending_duration = parse_extinf_duration(line);
                want_segment = true;
                want_variant = false;
            } else if (line.starts_with("#EXT-X-MAP:")) {
                const std::string uri = attribute_value(line, "URI");
                if (!uri.empty()) out.map_uri = resolve_url(base_url, uri);
            } else if (istarts_with(line, kKeyTagPrefix)) {
                // 【只有这一个标签用大小写不敏感匹配】其余标签保持大小写
                // 敏感，与 FFmpeg 的 hls.c 一致：一份小写标签的播放列表
                // FFmpeg 根本解析不了，把它的分片暖下来纯属白下。加密判定
                // 的方向相反——宁可多拦，所以它比 FFmpeg 宽（与 media 侧
                // playlist_declares_encryption 同一条论证）。
                if (key_line_is_encrypted(line.substr(kKeyTagPrefix.size()))) {
                    out.encrypted = true;
                }
            }
            // #EXT-X-ENDLIST 一个字都不认：预加载不区分直播与点播（直播同样
            // 只暖一轮、绝不续暖，见 preloader.cpp 的 expand_playlist）。
            // dl 侧曾有一个 has_endlist 字段 + playlist_has_endlist()，是
            // media 侧 playlist_declares_endlist 的**第二份转写**；两份已经
            // 漂了（first_line_is_extm3u 认 NUL 是行终止符、与 FFmpeg 一致，
            // 那份的 find_first_of("\r\n") 不认），而 `grep -rn has_endlist src/`
            // 从头到尾零消费点。两份转写、零消费点 ⇒ 纯成本，已删除。
            // 将来真需要"这份是不是直播"，连着调用方一起加。
            continue;
        }
        if (want_variant) {
            out.first_variant = resolve_url(base_url, line);
            want_variant = false;
            continue;
        }
        if (want_segment) {
            PlaylistScan::Segment s;
            s.url = resolve_url(base_url, line);
            s.duration_s = pending_duration;
            if (!s.url.empty()) out.segments.push_back(std::move(s));
            want_segment = false;
            pending_duration = 0.0;
            continue;
        }
        // 既不在 STREAM-INF 也不在 EXTINF 之后的裸 URI：忽略。
    }
    return out;
}

size_t segments_for_ms(const std::vector<PlaylistScan::Segment>& segs,
                       int64_t want_ms, size_t cap) noexcept {
    if (segs.empty() || cap == 0) return 0;
    const size_t limit = std::min(segs.size(), cap);
    if (want_ms <= 0) return 1;
    const double want_s = static_cast<double>(want_ms) / 1000.0;
    double acc = 0.0;
    for (size_t i = 0; i < limit; ++i) {
        acc += segs[i].duration_s;
        if (acc >= want_s) return i + 1;
    }
    return limit;
}

}  // namespace syp::dl
