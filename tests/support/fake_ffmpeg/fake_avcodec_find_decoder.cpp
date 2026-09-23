// fake_avcodec_find_decoder.cpp — 假"接入方 FFmpeg"的一个归档成员，见 fake_ffmpeg_internal.h。
#include "support/fake_integrator_ffmpeg.h"
#include "support/fake_ffmpeg/fake_ffmpeg_internal.h"


using namespace fake_ffmpeg;

extern "C" {

void* avcodec_find_decoder(int) {
    fake_ffmpeg_hit(kAvcodecFindDecoder);
    return nullptr;
}

}  // extern "C"
