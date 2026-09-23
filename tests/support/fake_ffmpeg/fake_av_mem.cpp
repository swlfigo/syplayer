// fake_av_mem.cpp — 假"接入方 FFmpeg"的一个归档成员，见 fake_ffmpeg_internal.h。
#include "support/fake_integrator_ffmpeg.h"
#include "support/fake_ffmpeg/fake_ffmpeg_internal.h"

#include <cstdlib>

using namespace fake_ffmpeg;

extern "C" {

void* av_malloc(size_t size) {
    fake_ffmpeg_hit(kAvMalloc);
    return std::malloc(size);
}

void av_free(void* ptr) {
    fake_ffmpeg_hit(kAvFree);
    std::free(ptr);
}

}  // extern "C"
