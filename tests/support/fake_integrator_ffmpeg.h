// fake_integrator_ffmpeg.h — 模拟"接入方自己带的那份 FFmpeg"。
//
// 这里声明的是真实 FFmpeg 的函数名（不加任何前缀），签名做了简化：接入方
// 的代码只按名字链接，参数类型与真实头文件是否一致不影响符号解析，而本头
// 故意不包含任何 FFmpeg 头，免得和 SYFFmpeg 的头文件混在一个编译单元里。
//
// 每个函数都返回可辨认的假值，并把调用次数记在库内；测试据此区分"这次
// 调用落到了假库"还是"落到了 SYFFmpeg"。
#pragma once

#include <cstddef>

// 假库的版本号：与任何真实 FFmpeg 版本（AV_VERSION_INT(大, 小, 微)）都
// 不可能相等——真实的主版本号不会到 0xFA。
inline constexpr unsigned kFakeAvformatVersion = 0xFA0001u;
inline constexpr unsigned kFakeAvcodecVersion  = 0xFA0002u;
// 假 avformat_open_input 的返回值：一个明显不是 FFmpeg 错误码的负数。
inline constexpr int kFakeOpenInputResult = -0x5AFE;

extern "C" {

unsigned avformat_version(void);
unsigned avcodec_version(void);
void*    av_malloc(size_t size);
void     av_free(void* ptr);
void     av_log_set_level(int level);
int      av_log_get_level(void);
int      avformat_open_input(void** ps, const char* url, const void* fmt, void** options);
int      avformat_find_stream_info(void* ic, void** options);
void     avformat_close_input(void** ps);
int      av_read_frame(void* s, void* pkt);
void*    avcodec_find_decoder(int id);

// 假库自己的记账接口。name 取上面任一函数名；未知名字返回 -1。
int  fake_ffmpeg_call_count(const char* name);
// 所有函数被调用的总次数。
int  fake_ffmpeg_total_calls(void);
void fake_ffmpeg_reset_counts(void);

}  // extern "C"
