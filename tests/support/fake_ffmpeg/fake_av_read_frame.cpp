// fake_av_read_frame.cpp — 假"接入方 FFmpeg"的一个归档成员，见 fake_ffmpeg_internal.h。
#include "support/fake_integrator_ffmpeg.h"
#include "support/fake_ffmpeg/fake_ffmpeg_internal.h"


using namespace fake_ffmpeg;

extern "C" {

int av_read_frame(void*, void*) {
    fake_ffmpeg_hit(kAvReadFrame);
    return kFakeOpenInputResult;
}

}  // extern "C"
