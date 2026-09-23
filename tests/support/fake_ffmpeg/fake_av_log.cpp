// fake_av_log.cpp — 假"接入方 FFmpeg"的一个归档成员，见 fake_ffmpeg_internal.h。
#include "support/fake_integrator_ffmpeg.h"
#include "support/fake_ffmpeg/fake_ffmpeg_internal.h"

#include <atomic>

using namespace fake_ffmpeg;

namespace {
std::atomic<int> g_log_level{-12345};
}  // namespace

extern "C" {

void av_log_set_level(int level) {
    fake_ffmpeg_hit(kAvLogSetLevel);
    g_log_level.store(level, std::memory_order_relaxed);
}

int av_log_get_level(void) {
    fake_ffmpeg_hit(kAvLogGetLevel);
    return g_log_level.load(std::memory_order_relaxed);
}

}  // extern "C"
