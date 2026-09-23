// fake_avformat_open_input.cpp — 假"接入方 FFmpeg"的一个归档成员，见 fake_ffmpeg_internal.h。
#include "support/fake_integrator_ffmpeg.h"
#include "support/fake_ffmpeg/fake_ffmpeg_internal.h"


using namespace fake_ffmpeg;

extern "C" {

int avformat_open_input(void** ps, const char*, const void*, void**) {
    fake_ffmpeg_hit(kAvformatOpenInput);
    if (ps) *ps = nullptr;
    return kFakeOpenInputResult;
}

}  // extern "C"
