// fake_ffmpeg_counts.cpp — 记账成员：只有计数与查询，不定义任何 FFmpeg 名字。
#include "support/fake_integrator_ffmpeg.h"
#include "support/fake_ffmpeg/fake_ffmpeg_internal.h"

#include <atomic>
#include <cstring>

namespace {

using namespace fake_ffmpeg;

constexpr const char* kNames[kFnCount] = {
    "avformat_version",
    "avcodec_version",
    "av_malloc",
    "av_free",
    "av_log_set_level",
    "av_log_get_level",
    "avformat_open_input",
    "avformat_find_stream_info",
    "avformat_close_input",
    "av_read_frame",
    "avcodec_find_decoder",
};

// 原子计数：若真有串用，调用可能来自解码/加载线程。
std::atomic<int> g_counts[kFnCount];

}  // namespace

extern "C" {

void fake_ffmpeg_hit(int fn) {
    if (fn >= 0 && fn < kFnCount) g_counts[fn].fetch_add(1, std::memory_order_relaxed);
}

int fake_ffmpeg_call_count(const char* name) {
    for (int i = 0; i < kFnCount; ++i) {
        if (std::strcmp(kNames[i], name) == 0) {
            return g_counts[i].load(std::memory_order_relaxed);
        }
    }
    return -1;
}

int fake_ffmpeg_total_calls(void) {
    int total = 0;
    for (const auto& c : g_counts) total += c.load(std::memory_order_relaxed);
    return total;
}

void fake_ffmpeg_reset_counts(void) {
    for (auto& c : g_counts) c.store(0, std::memory_order_relaxed);
}

}  // extern "C"
