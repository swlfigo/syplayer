// url_rewrite.h —— HLS 的纯函数：URL 改写 + 播放列表文本扫描。
//
// 这个文件**不碰任何 IO**，也不 include 任何 FFmpeg / dl 层的头。
// 它是整条 HLS 链路上唯一能被穷举测的部分，所以逻辑尽量都挤到这里。
#pragma once

#include <string>
#include <string_view>

namespace syp::media::hls {

// 一个子资源该走哪条 IO 通道。
//
// 两条通道的**缓存语义相反**：播放列表在直播下必须永不
// 缓存（同一 URL 内容每几秒变一次），分片是不可变内容、URL 唯一、缓存它
// 永远正确。
enum class Channel {
    Playlist,   // syp_http_backend 直接取，零缓存
    Segment,    // syp_source，带缓存/预加载/洞调度
};

// 按 URL **路径部分**的后缀判定（先剥掉 ?query 与 #fragment）。
//
// 【局限，必须知道】后缀是启发式，不是协议保证——HLS 规范不要求播放列表
// 以 .m3u8 结尾。判错的后果是**可观测但不会自动报警**的：
//   · 把播放列表当分片 → 它会被缓存 → 直播刷新失效（表现为直播卡在某一
//     刻不再前进）；
//   · 把分片当播放列表 → 它会被整个读进内存且不缓存（表现为内存占用异常
//     + 重复下载）。
// 前者更严重，所以**没有后缀时默认当分片**这个选择是反的——不，正好相反：
// 没有后缀的 URL 绝大多数是分片（CDN 常用无后缀的对象键），而把一个真的
// 播放列表误判成分片只会发生在无后缀播放列表上，那种源本身就罕见。
// 这条默认值是有意识的取舍，不是随手写的。
//
// 【为什么不按 Content-Type 判】那要求先发请求才能决定走哪条通道，而两条
// 通道的请求发法不同 —— 循环依赖。
//
// 【同一判据在 dl 层还有一份：syp::dl::is_playlist_url（src/dl/m3u8_scan.h）】
// Preloader 在 dl 层，而 dl 不许依赖 media（syp_dl 必须零 FFmpeg、零平台
// 符号），所以那边只能另写一份。
//
// 【两边的判据表只有一张：tests/support/hls_cases.h】两个测试文件
// （tests/test_hls_url_rewrite.cpp 与 tests/test_preloader_hls.cpp）
// **include 同一个文件**：kChannelCases（本函数 ↔ is_playlist_url）、
// kEncryptionCases（playlist_declares_encryption ↔ PlaylistScan::encrypted）。
// kEndlistCases 只钉本文件的 playlist_declares_endlist —— dl 侧那份转写
// 已经删掉了（零消费点、两份已漂），所以它是单边表。
// 早先是两份同名字面量各抄一张，那挡不住"改了代码顺手把自己那张表也改了"。
// **改任何一侧都要在那个文件里加用例**。
Channel channel_for(std::string_view url) noexcept;

// 取 URL 的 scheme，**小写**，不含 "://"。没有 scheme 返回空串。
std::string scheme_of(std::string_view url);

// 递给 FFmpeg 之前的改写：https → http，其余原样。
//
// 为什么：hls.c 的 open_url() 在调 io_open **之前**先做
// avio_find_protocol_name()，查的是**编译进去的协议表**，且要求名字是
// file/http*/data 之一。而 --enable-protocol=https 会拉进整套 TLS
// （configure:4027），--enable-protocol=http 只拉 tcp（configure:4023）。
// scheme 对 FFmpeg 只是个标签——真正的连接由 HlsSession::io_open 发起，
// 所以降级成 http 是安全的，换来的是不引入 TLS 依赖。
std::string to_ffmpeg_url(std::string_view real_url);

// io_open 里的还原：把 http 换回会话记录的真实 scheme。
//
// 【局限】混合 scheme 的流（https 播放列表 + http 分片）会被整体当成
// real_scheme。这是**降级失败而非降级成功**：把 http 资源当 https 请求会
// 失败并报错，不会静默地明文传输。方向是安全的那一侧。
std::string to_real_url(std::string_view ffmpeg_url, std::string_view real_scheme);

// 播放列表文本里是否声明了本版本不支持的加密。
//
// 扫 #EXT-X-KEY 行（标签本身大小写不敏感），按 HLS 属性列表语法切分属性
// （逗号分隔，**双引号内的逗号不算分隔符**），找 METHOD 的值。
// METHOD=NONE 是合法的"这一段不加密"声明，不能一见 EXT-X-KEY 就拒。
// 准确判据是"**证明了 METHOD=NONE 才放行**"，不是"没证明加密就放行"：
//   · 同一行里 METHOD 出现多次 ⇒ 取**最后一个**（FFmpeg 的
//     ff_parse_key_value + handle_key_args 是 last-wins，见实现处注释）；
//   · METHOD 缺失 / 值为空串 / 被未闭合的引号吞掉 ⇒ **返回 true**；
//   · 只有剥掉可能的包裹引号之后大小写不敏感地等于 "NONE" 才算不加密。
// 任意一行声明了加密，整份就返回 true。
//
// 【故意**没有** #EXTM3U 首行闸】与下面的 playlist_declares_endlist 不同：
// 这是阻止密钥被请求的最后一道关，方向必须是"宁可多拦"。加一道闸等于给
// 它开一个新口子（闸的任何一点误差都变成一次真实的密钥请求）。
//
// 调用点在 open_playlist() 取回 body 之后、建 AVIOContext 之前：必须在
// 把播放列表交给 FFmpeg（进而可能去拉 EXT-X-KEY 的 URI，即密钥本身）之前
// 就分辨完，密钥 URL 一次都不该被请求。
//
// 【dl 层的那一份：PlaylistScan::encrypted（src/dl/m3u8_scan.h）】判据逐条
// 相同（连 last-wins 都一样）、时点相同（body 到手之后、发出任何子请求之
// 前），实现另一份，理由同 channel_for 上面那段；共享用例表同上。
bool playlist_declares_encryption(std::string_view playlist_text) noexcept;

// 这份播放列表在 FFmpeg 眼里是否"已经结束"（EXT-X-ENDLIST）——判定规则与
// FFmpeg 8.1.2 逐字一致，见实现处注释。首行不是 "#EXTM3U" 时返回 false：
// 那份 body 在 FFmpeg 那里会解析失败，谈不上"结束"。
//
// 用途：HlsSession 判定直播的一次 Eof 是否正常。直播只有在最后一次取回的
// 播放列表带 ENDLIST 时才会正常播完；否则一定是重拉失败、解析失败或停滞
// 这类异常结束。
//
// 【dl 层没有对应的那一份】曾经有过（PlaylistScan::has_endlist，本函数的
// 逐条转写），但 `grep -rn has_endlist src/` 零消费点、两份转写还漂了
// （NUL 是不是行终止符），后来删掉。本函数在 HlsSession
// 里有真实调用方，用例表 kEndlistCases 现在只钉它一个。
bool playlist_declares_endlist(std::string_view playlist_text) noexcept;

// 去掉 master 播放列表里的字幕 rendition（#EXT-X-MEDIA 且属性 TYPE=SUBTITLES 的整行，
// 连同行尾换行），其余字节原样保留。
//
// 为什么：本播放器不支持字幕，FFmpeg 构建也没开 webvtt demuxer。hls.c 的
// hls_read_header 会为**每个** rendition 建子 demuxer 并探测首段；字幕组的探测
// 必然失败，而这一失败会让整个 avformat_open_input 失败（"Error when loading first
// segment '...vtt'"，上层看到 SYP_ERR_IO）——一个用不上的字幕组拖垮整条流（twimg
// 的 HLS 都带自动生成字幕）。变体行里的 SUBTITLES="组名" 属性保留：hls.c 找不到对应
// 组只是不挂 rendition，无害。
//
// 属性按 HLS 属性列表语法切分（逗号分隔、双引号内的逗号不算），只认 TYPE 属性本身，
// NAME="...TYPE=SUBTITLES.." 这类引号内的字样不会误删。
std::string strip_subtitle_renditions(std::string_view playlist_text);

}  // namespace syp::media::hls
