// ffmpeg_audio_decoder.h — 音频解码器，FFmpeg 软解，唯一实现。
//
// 为什么不像视频那样先抽一个 IAudioDecoder 接口：架构文档的「缝 ②」只
// 要求抽象*音频输出*（各平台的音频渲染/输出路径不同——AudioQueue /
// AudioUnit / AAudio 等），不要求抽象*音频解码*。音频解码在可见未来只有
// FFmpeg 软解这一种实现，没有第二种平台特定路径需要切换，此刻抽一层纯
// 虚接口不会被第二个实现消费，纯粹是空中楼阁。等哪天真的出现第二种音频
// 解码实现，再按需抽接口不迟。
//
// 方法集与 IVideoDecoder 完全一致（open/send/receive/flush/
// skipped_packets），因此复用 IVideoDecoder::Receive 这个枚举，而不是
// 再定义一个字段完全相同的 AudioReceive：两边语义相同，定义两份只会让
// 调用方多写一份几乎一样的 switch，维护时容易漏改一份。如果将来觉得
// 「音频类复用视频接口里的枚举」这个耦合别扭，可以把 Receive 提到一个
// 独立的公共头（比如改名 DecodeReceive），但那次改动必须视频/音频两处
// 一起改，不能留下两个语义相同的枚举各管一半调用方。
#pragma once

#include <cstdint>

#include "media/video_decoder.h"  // 复用 IVideoDecoder::Receive，见上方注释

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace syp::media {

class FFmpegAudioDecoder final {
public:
    using Receive = IVideoDecoder::Receive;

    FFmpegAudioDecoder() = default;
    ~FFmpegAudioDecoder();

    FFmpegAudioDecoder(const FFmpegAudioDecoder&)            = delete;
    FFmpegAudioDecoder& operator=(const FFmpegAudioDecoder&) = delete;

    // 契约与 IVideoDecoder::open() 相同（见 video_decoder.h 里的完整说明，
    // 不重复贴一遍）：par->codec_type 必须是 AVMEDIA_TYPE_AUDIO，不匹配
    // 返回 SYP_ERR_INVALID_ARG；可以在同一实例上重复调用，等价于先释放
    // 旧状态再重新初始化。
    syp_status open(const AVCodecParameters* par, AVRational time_base);
    syp_status send(const AVPacket* pkt);
    Receive    receive(Frame* out);
    void       flush();
    int64_t    skipped_packets() const { return skipped_packets_; }

private:
    void close() noexcept;

    AVCodecContext* ctx_             = nullptr;
    AVRational      time_base_       = {0, 1};
    int64_t         skipped_packets_ = 0;
};

}  // namespace syp::media
