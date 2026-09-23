// test_decoder.cpp — 两个解码器。
//
// send/receive 模型的三个返回值不是错误：send 的 EAGAIN 表示"先取走输出"，
// receive 的 EAGAIN 表示"要更多输入"、EOF 表示"已排空"。把它们当错误处理
// 是这类代码最常见的写法错误。
//
// 挑轨道一律用 codec_type，不用 is_video/!is_video：demuxer.h 的
// TrackInfo 上方已经写了警示——is_video==false 不等于「是音频轨」，字幕/
// 数据轨同样是 is_video==false 但媒体字段全 0。这里视频轨判据也统一走
// codec_type，两处判据一致，不给以后加字幕/数据轨留坑。
#include "media/demuxer.h"
#include "media/ffmpeg_audio_decoder.h"
#include "media/ffmpeg_video_decoder.h"
#include "support/fake_hw_backend.h"
#include "tiny_test.h"

#include <cstdlib>
#include <memory>
#include <string>

using namespace syp::media;

namespace {
std::string fixture(const char* name) {
    const char* d = std::getenv("SYP_FIXTURE_DIR");
    return std::string(d ? d : "") + "/" + name;
}

int32_t find_track(const Demuxer& d, AVMediaType type) {
    for (unsigned i = 0; i < d.raw()->nb_streams; ++i) {
        if (d.raw()->streams[i]->codecpar->codec_type == type) return static_cast<int32_t>(i);
    }
    return -1;
}
}  // namespace

TEST_CASE(video_decoder_produces_frames_with_monotonic_pts) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);

    int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    REQUIRE(vidx >= 0);

    FFmpegVideoDecoder dec;
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar,
                     d->raw()->streams[vidx]->time_base) == SYP_OK);

    int64_t frames = 0;
    int64_t last_pts = INT64_MIN;
    for (;;) {
        AVPacket* p = nullptr;
        int32_t   ti = -1;
        auto r = d->read(&p, &ti);
        if (r == Demuxer::ReadResult::Eof) break;
        REQUIRE(r == Demuxer::ReadResult::Packet);
        if (ti != vidx) { av_packet_free(&p); continue; }

        REQUIRE(dec.send(p) == SYP_OK);
        av_packet_free(&p);

        for (;;) {
            Frame f;
            auto rr = dec.receive(&f);
            if (rr == IVideoDecoder::Receive::NeedInput) break;
            REQUIRE(rr == IVideoDecoder::Receive::Frame);
            CHECK(f.is_video());
            CHECK(f.width() > 0);
            CHECK(f.height() > 0);
            if (f.pts_us() != AV_NOPTS_VALUE) {
                CHECK(f.pts_us() >= last_pts);   // 显示顺序单调
                last_pts = f.pts_us();
            }
            ++frames;
        }
    }

    // flush/EOF 信号（send(nullptr)）之前从未被执行过——如果流里有需要靠
    // drain 才释放的缓冲帧（典型场景：B 帧重排），不给这条信号就会被静默
    // 丢弃，而 frames > 50 这条下限断言根本发现不了少几帧。
    //
    // 用独立 probe 在完整素材上验证过（scratchpad 下，不提交）：本
    // fixture 由 tools/gen-fixtures.sh 用 `-preset ultrafast` 编码，该
    // preset 硬关掉 B 帧（ffprobe 确认 has_b_frames=0，帧类型序列全是
    // I/P，无 B），所以 drain 阶段预期吐出 0 帧——这不是没测，是测了、且
    // 如实反映了这份素材没有可暴露的重排缓冲。下面仍然执行这条路径并
    // 断言其正确终止于 Eof（不是 Error/不是继续吐 NeedInput 之外的怪
    // 状态），一旦将来素材/编码参数改成带 B 帧，这条路径已经在跑，能
    // 立刻暴露被静默丢弃的尾部帧。
    REQUIRE(dec.send(nullptr) == SYP_OK);
    int64_t drain_frames = 0;
    for (;;) {
        Frame f;
        auto rr = dec.receive(&f);
        if (rr == IVideoDecoder::Receive::Eof) break;
        REQUIRE(rr == IVideoDecoder::Receive::Frame);
        CHECK(f.is_video());
        if (f.pts_us() != AV_NOPTS_VALUE) {
            CHECK(f.pts_us() >= last_pts);
            last_pts = f.pts_us();
        }
        ++frames;
        ++drain_frames;
    }
    CHECK_EQ(drain_frames, int64_t{0});   // 本 fixture 编码参数决定的经验值，见上方注释

    CHECK(frames > 50);
    CHECK_EQ(dec.skipped_packets(), int64_t{0});

    // 只验单调性抓不住「time_base 传错一个数量级」这类错误：av_rescale_q
    // 是线性缩放，只要原始 pts 在流时基下单调，换算后无论时基对不对都
    // 还是单调的。补一条绝对量的下限/上限，容差取 2 倍（素材几十秒、帧
    // 间隔几十毫秒，时基错一个数量级会让这个值直接偏出几个数量级，2 倍
    // 容差足够宽松不会误报，又足够窄能抓住数量级错误）。
    REQUIRE(d->duration_us() > 0);
    CHECK(last_pts > d->duration_us() / 2);
    CHECK(last_pts < d->duration_us() * 2);
}

TEST_CASE(audio_decoder_produces_frames) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);

    int32_t aidx = find_track(*d, AVMEDIA_TYPE_AUDIO);
    REQUIRE(aidx >= 0);

    FFmpegAudioDecoder dec;
    REQUIRE(dec.open(d->raw()->streams[aidx]->codecpar,
                     d->raw()->streams[aidx]->time_base) == SYP_OK);

    int64_t frames = 0;
    int64_t last_pts = INT64_MIN;
    for (;;) {
        AVPacket* p = nullptr;
        int32_t   ti = -1;
        auto r = d->read(&p, &ti);
        if (r == Demuxer::ReadResult::Eof) break;
        if (ti != aidx) { av_packet_free(&p); continue; }
        REQUIRE(dec.send(p) == SYP_OK);
        av_packet_free(&p);
        for (;;) {
            Frame f;
            auto rr = dec.receive(&f);
            if (rr != IVideoDecoder::Receive::Frame) break;
            CHECK(!f.is_video());
            CHECK(f.sample_rate() > 0);
            CHECK(f.channels() > 0);      // 走 ch_layout.nb_channels
            CHECK(f.nb_samples() > 0);
            if (f.pts_us() != AV_NOPTS_VALUE) {
                CHECK(f.pts_us() >= last_pts);   // 与视频对称：显示顺序单调
                last_pts = f.pts_us();
            }
            ++frames;
        }
    }

    // 同视频一样补 drain。AAC 有恒定 1 帧的 codec 级 priming 差值（送了
    // N 个包只吐出 N-1 帧），用独立 probe 在完整素材上验证过：这个差值
    // 在真正 EOF 也不会被放出来（drain 阶段仍是 0 帧）——它是编码器起始
    // 的先验样本损耗，FFmpeg 的 aac 解码器内部直接丢弃，不是「延后释
    // 放」，不是本任务要处理的缓冲语义。仍然执行这条路径，理由同视频
    // 用例：现在测了，将来编码参数一变就能立刻发现问题。
    REQUIRE(dec.send(nullptr) == SYP_OK);
    int64_t drain_frames = 0;
    for (;;) {
        Frame f;
        auto rr = dec.receive(&f);
        if (rr == IVideoDecoder::Receive::Eof) break;
        REQUIRE(rr == IVideoDecoder::Receive::Frame);
        CHECK(!f.is_video());
        if (f.pts_us() != AV_NOPTS_VALUE) {
            CHECK(f.pts_us() >= last_pts);
            last_pts = f.pts_us();
        }
        ++frames;
        ++drain_frames;
    }
    CHECK_EQ(drain_frames, int64_t{0});

    CHECK(frames > 50);

    // 与视频对称补绝对量断言，理由同视频那条：单调性抓不住「time_base
    // 传错一个数量级」，音频走的是完全对称的 ctx->pkt_timebase = time_base
    // 路径，同样可能踩这个坑。
    REQUIRE(d->duration_us() > 0);
    CHECK(last_pts > d->duration_us() / 2);
    CHECK(last_pts < d->duration_us() * 2);
}

TEST_CASE(decoder_open_with_unsupported_codec_fails_cleanly) {
    // 该轨终止（不是整体终止）：open 失败要如实返回错误、不崩、可重复调用
    FFmpegVideoDecoder dec;
    AVCodecParameters* par = avcodec_parameters_alloc();
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id   = AV_CODEC_ID_VP9;   // 未编入本次 FFmpeg 构建
    CHECK(dec.open(par, AVRational{1, 1000}) != SYP_OK);
    avcodec_parameters_free(&par);
}

TEST_CASE(decoder_open_avcodec_open2_failure_cleans_up) {
    // decoder_open_with_unsupported_codec_fails_cleanly 覆盖的是
    // avcodec_find_decoder 失败这条分支（VP9 没编进本次 FFmpeg 构建）。
    // 这里覆盖更深一层：avcodec_find_decoder / avcodec_parameters_to_context
    // 都成功，失败发生在 avcodec_open2——AAC 解码器在 open 时会解析
    // AudioSpecificConfig extradata，塞非法字节（采样率索引越界）会让
    // avcodec_open2 直接返回负值，触发 open() 内部「avcodec_open2 失败
    // 也要 avcodec_free_context 清理、不留半开状态」这条此前从未被执行
    // 过的路径。
    //
    // 这条用例依赖的是 FFmpeg 内部实现细节（不是公开 API 契约），但触发
    // 条件本身绑定在 MPEG-4 Audio 标准的采样率表结构上：该表固定 13 个
    // 标准采样率条目，索引 13/14 保留未用、15 是「显式采样率」转义值，
    // 这是标准冻结的结构，不是随版本调整的启发式阈值，跨 FFmpeg 版本
    // 变动概率低。
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    int32_t aidx = find_track(*d, AVMEDIA_TYPE_AUDIO);
    REQUIRE(aidx >= 0);

    AVCodecParameters* par = avcodec_parameters_alloc();
    REQUIRE(par != nullptr);
    REQUIRE(avcodec_parameters_copy(par, d->raw()->streams[aidx]->codecpar) >= 0);
    REQUIRE(par->extradata_size > 0);
    for (int i = 0; i < par->extradata_size; ++i) {
        par->extradata[i] = static_cast<uint8_t>(0xFF);
    }

    FFmpegAudioDecoder dec;
    CHECK(dec.open(par, d->raw()->streams[aidx]->time_base) != SYP_OK);
    // 该轨终止不是整体终止：失败后必须能不崩、可重复调用。
    CHECK(dec.open(par, d->raw()->streams[aidx]->time_base) != SYP_OK);
    avcodec_parameters_free(&par);
}

TEST_CASE(decoder_open_rejects_mismatched_codec_type) {
    // Pipeline 传错轨时（把音频轨的 codecpar 递给视频解码器，反之亦然）
    // 必须在 open() 就报错，不能让底层解码器「照样能跑」、产出一个
    // is_video() 与实际内容矛盾的 Frame。
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    int32_t aidx = find_track(*d, AVMEDIA_TYPE_AUDIO);
    REQUIRE(vidx >= 0);
    REQUIRE(aidx >= 0);

    FFmpegVideoDecoder vdec;
    CHECK(vdec.open(d->raw()->streams[aidx]->codecpar,
                     d->raw()->streams[aidx]->time_base) != SYP_OK);

    FFmpegAudioDecoder adec;
    CHECK(adec.open(d->raw()->streams[vidx]->codecpar,
                     d->raw()->streams[vidx]->time_base) != SYP_OK);
}

TEST_CASE(decoder_open_is_reentrant) {
    // 接口契约（video_decoder.h）：open() 可以在同一实例上重复调用，
    // 等价于先释放旧状态再重新初始化——不只是「失败后能重开」，「成功
    // 之后再开一次」也要继续正常工作。这是给 VTDecoder 立的规矩，
    // 这里先在软解上钉一个回归用例。
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    REQUIRE(vidx >= 0);

    FFmpegVideoDecoder dec;
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar,
                     d->raw()->streams[vidx]->time_base) == SYP_OK);
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar,
                     d->raw()->streams[vidx]->time_base) == SYP_OK);
    CHECK_EQ(dec.skipped_packets(), int64_t{0});   // 重新 open 要清零

    AVPacket* p = nullptr;
    int32_t   ti = -1;
    for (;;) {
        REQUIRE(d->read(&p, &ti) == Demuxer::ReadResult::Packet);
        if (ti == vidx) break;
        av_packet_free(&p);
    }
    REQUIRE(dec.send(p) == SYP_OK);
    av_packet_free(&p);
    Frame f;
    CHECK(dec.receive(&f) == IVideoDecoder::Receive::Frame);
}

TEST_CASE(decoder_send_skips_corrupt_packet) {
    // 三级错误分类里最轻的一级：单包解码失败计入 skipped_packets()、
    // 返回 SYP_OK，不终止整条解码。不需要真的损坏素材（场景 F
    // 才会有那种素材）——手写一个非法 NAL 数据的包喂给已 open() 的真实
    // H264 解码器就能稳定触发：avcodec_send_packet 内部的 NAL 切分校验
    // 会返回负值（非 EAGAIN/EOF）。
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    REQUIRE(vidx >= 0);

    FFmpegVideoDecoder dec;
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar,
                     d->raw()->streams[vidx]->time_base) == SYP_OK);

    AVPacket* garbage = av_packet_alloc();
    REQUIRE(garbage != nullptr);
    REQUIRE(av_new_packet(garbage, 64) == 0);
    for (int i = 0; i < 64; ++i) {
        garbage->data[i] = static_cast<uint8_t>(0xAA ^ i);
    }
    garbage->stream_index = vidx;

    CHECK_EQ(dec.send(garbage), SYP_OK);   // 跳过不是错误，返回 SYP_OK
    CHECK_EQ(dec.skipped_packets(), int64_t{1});
    av_packet_free(&garbage);

    // 排空一下：不该有帧吐出来，也不该是 Error。
    Frame f;
    CHECK(dec.receive(&f) == IVideoDecoder::Receive::NeedInput);
}

TEST_CASE(decoder_flush_allows_reuse_after_seek) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    REQUIRE(vidx >= 0);

    FFmpegVideoDecoder dec;
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar,
                     d->raw()->streams[vidx]->time_base) == SYP_OK);

    auto decode_one = [&]() -> bool {
        for (;;) {
            AVPacket* p = nullptr; int32_t ti = -1;
            if (d->read(&p, &ti) != Demuxer::ReadResult::Packet) return false;
            if (ti != vidx) { av_packet_free(&p); continue; }
            dec.send(p);
            av_packet_free(&p);
            Frame f;
            if (dec.receive(&f) == IVideoDecoder::Receive::Frame) return true;
        }
    };

    CHECK(decode_one());
    REQUIRE(d->seek(3000000) == SYP_OK);
    dec.flush();
    CHECK(decode_one());   // flush 之后能继续解，不带旧状态

    // 局限性（如实记录）：本用例只验证了「flush 后还能继续解出一帧」，
    // 验不出「flush 真的丢弃了内部缓冲状态」——因为 seek 落点是关键帧
    // （demuxer 保证首个视频包带 AV_PKT_FLAG_KEY），IDR 帧按 H.264 规范
    // 自带「清空参考帧/DPB」的语义，不依赖外部 avcodec_flush_buffers
    // 调用也能独立解码成功，flush 与否结果一样。真正判别性的用例见下面
    // 的 video_decoder_flush_discards_pending_buffered_frame /
    // audio_decoder_flush_discards_pending_buffered_frame。
}

// flush() 的判别性用例。
//
// 上一轮曾判定"构造不出能证明 flush() 有效的判别性用例"，结论下早了：
// 把"构造不出带 B 帧的 drain 测试"错误地等同于"构造不出任何 flush
// 判别性测试"。三条排除理由（本 fixture 视频轨 has_b_frames=0、audio
// 轨 1 帧差值是 priming 损耗、vendored FFmpeg 没有 encoder）全都只回答了
// "drain 时能不能多吐出帧"这一个问题，而 flush 要丢的不止是 B 帧重排
// 缓冲。
//
// 真正的机制在 avcodec_send_packet 内部（libavcodec/decode.c）：只要
// avci->buffer_frame 当前为空且没有进入 draining，send() 会在内部就
// 乐观地尝试解一帧存进 buffer_frame；receive() 优先检查 buffer_frame，
// 非空就直接把它交出去，不会再触发一次真正解码。这条链路和 B 帧、
// priming 完全无关，是 send/receive 状态机的通用优化，任何编解码器都
// 有——"send 之后、receive 之前"天然存在一个窗口，一帧已经躺在
// buffer_frame 里等着被取走。avcodec_flush_buffers 明确
// av_frame_unref(avci->buffer_frame)，这正是 flush() 该丢弃、也是这两
// 条用例要验证的状态。
//
// A/B 对照，两个独立解码器实例喂相同的包序列（先热身几个包让解码器过了
// 启动阶段，再送一个测试包）：
//   A：send(测试包) 之后不 receive，直接 flush()，receive() 应为
//      NeedInput——缓冲帧被丢弃。
//   B（对照，证明"不 flush 时那帧真的取得出来"这个前提成立）：同样
//      send(测试包) 不 receive，不 flush，receive() 应为 Frame。
//   两组都要有：只有 A 组的话，如果哪天 FFmpeg 改了乐观解码的行为
//   （send 之后不再预解一帧），A 组会因为"本来就没帧可取"而变成恒真，
//   用例会静默失效而不会报错。

TEST_CASE(video_decoder_flush_discards_pending_buffered_frame) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    REQUIRE(vidx >= 0);

    FFmpegVideoDecoder decA;
    FFmpegVideoDecoder decB;
    REQUIRE(decA.open(d->raw()->streams[vidx]->codecpar,
                       d->raw()->streams[vidx]->time_base) == SYP_OK);
    REQUIRE(decB.open(d->raw()->streams[vidx]->codecpar,
                       d->raw()->streams[vidx]->time_base) == SYP_OK);

    // 热身：给 A/B 喂相同的前几个视频包，严格遵守 send 前先排空的契约。
    constexpr int kWarm = 3;
    int warmed = 0;
    AVPacket* test_pkt = nullptr;
    for (;;) {
        AVPacket* p = nullptr; int32_t ti = -1;
        REQUIRE(d->read(&p, &ti) == Demuxer::ReadResult::Packet);
        if (ti != vidx) { av_packet_free(&p); continue; }
        if (warmed < kWarm) {
            REQUIRE(decA.send(p) == SYP_OK);
            REQUIRE(decB.send(p) == SYP_OK);
            Frame fa, fb;
            while (decA.receive(&fa) == IVideoDecoder::Receive::Frame) {}
            while (decB.receive(&fb) == IVideoDecoder::Receive::Frame) {}
            av_packet_free(&p);
            ++warmed;
            continue;
        }
        test_pkt = p;
        break;
    }
    REQUIRE(test_pkt != nullptr);

    // 测试包：送进 A/B，都不 receive。
    REQUIRE(decA.send(test_pkt) == SYP_OK);
    REQUIRE(decB.send(test_pkt) == SYP_OK);
    av_packet_free(&test_pkt);

    decA.flush();
    Frame fa;
    CHECK(decA.receive(&fa) == IVideoDecoder::Receive::NeedInput);   // A：缓冲帧被 flush 丢弃

    Frame fb;
    CHECK(decB.receive(&fb) == IVideoDecoder::Receive::Frame);       // B（对照）：不 flush 那帧确实取得出来
}

TEST_CASE(audio_decoder_flush_discards_pending_buffered_frame) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("faststart.mp4"), &err);
    REQUIRE(d != nullptr);
    int32_t aidx = find_track(*d, AVMEDIA_TYPE_AUDIO);
    REQUIRE(aidx >= 0);

    FFmpegAudioDecoder decA;
    FFmpegAudioDecoder decB;
    REQUIRE(decA.open(d->raw()->streams[aidx]->codecpar,
                       d->raw()->streams[aidx]->time_base) == SYP_OK);
    REQUIRE(decB.open(d->raw()->streams[aidx]->codecpar,
                       d->raw()->streams[aidx]->time_base) == SYP_OK);

    constexpr int kWarm = 3;
    int warmed = 0;
    AVPacket* test_pkt = nullptr;
    for (;;) {
        AVPacket* p = nullptr; int32_t ti = -1;
        REQUIRE(d->read(&p, &ti) == Demuxer::ReadResult::Packet);
        if (ti != aidx) { av_packet_free(&p); continue; }
        if (warmed < kWarm) {
            REQUIRE(decA.send(p) == SYP_OK);
            REQUIRE(decB.send(p) == SYP_OK);
            Frame fa, fb;
            while (decA.receive(&fa) == IVideoDecoder::Receive::Frame) {}
            while (decB.receive(&fb) == IVideoDecoder::Receive::Frame) {}
            av_packet_free(&p);
            ++warmed;
            continue;
        }
        test_pkt = p;
        break;
    }
    REQUIRE(test_pkt != nullptr);

    REQUIRE(decA.send(test_pkt) == SYP_OK);
    REQUIRE(decB.send(test_pkt) == SYP_OK);
    av_packet_free(&test_pkt);

    decA.flush();
    Frame fa;
    CHECK(decA.receive(&fa) == IVideoDecoder::Receive::NeedInput);

    Frame fb;
    CHECK(decB.receive(&fb) == IVideoDecoder::Receive::Frame);
}

// HEVC 软解。曾经 FFmpeg 没编 hevc decoder 时，这条会在 open 处报
// SYP_ERR_NOT_IMPLEMENTED；素材缺失会在 open_file 处失败。
TEST_CASE(video_decoder_decodes_hevc_software) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("hevc.mp4"), &err);
    REQUIRE(d != nullptr);
    const int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    REQUIRE(vidx >= 0);
    CHECK_EQ(d->raw()->streams[vidx]->codecpar->codec_id, AV_CODEC_ID_HEVC);

    FFmpegVideoDecoder dec;
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar, d->raw()->streams[vidx]->time_base) == SYP_OK);

    int64_t frames = 0;
    for (;;) {
        AVPacket* p = nullptr;
        int32_t ti = -1;
        const auto r = d->read(&p, &ti);
        if (r == Demuxer::ReadResult::Eof) break;
        REQUIRE(r == Demuxer::ReadResult::Packet);
        if (ti == vidx) REQUIRE(dec.send(p) == SYP_OK);
        av_packet_free(&p);
        Frame f;
        while (dec.receive(&f) == IVideoDecoder::Receive::Frame) {
            CHECK_EQ(f.pix_fmt(), static_cast<int32_t>(AV_PIX_FMT_YUV420P));
            ++frames;
        }
    }
    REQUIRE(dec.send(nullptr) == SYP_OK);
    Frame f;
    while (dec.receive(&f) == IVideoDecoder::Receive::Frame) ++frames;
    CHECK(frames > 0);
}

// ---- 硬解模式的平台无关闸门（假后端）----

namespace {
struct VideoStream {
    std::unique_ptr<Demuxer> d;
    const AVStream*          st = nullptr;
};
VideoStream open_video(const char* name) {
    VideoStream v;
    syp_status err = SYP_OK;
    v.d = Demuxer::open_file(fixture(name), &err);
    if (v.d == nullptr) return v;
    const int32_t idx = find_track(*v.d, AVMEDIA_TYPE_VIDEO);
    if (idx >= 0) v.st = v.d->raw()->streams[idx];
    return v;
}
}  // namespace

TEST_CASE(hardware_mode_without_backend_is_not_implemented) {
    auto v = open_video("faststart.mp4");
    REQUIRE(v.st != nullptr);
    FFmpegVideoDecoder dec(VideoDecodeMode::Hardware, nullptr);
    CHECK_EQ(dec.open(v.st->codecpar, v.st->time_base), SYP_ERR_NOT_IMPLEMENTED);
    CHECK(!dec.hardware());
}

TEST_CASE(hardware_mode_unsupported_backend_is_not_implemented_and_never_prepares) {
    auto v = open_video("faststart.mp4");
    REQUIRE(v.st != nullptr);
    syp::test::FakeHwBackend be;
    be.supports_result = false;
    FFmpegVideoDecoder dec(VideoDecodeMode::Hardware, &be);
    CHECK_EQ(dec.open(v.st->codecpar, v.st->time_base), SYP_ERR_NOT_IMPLEMENTED);
    CHECK_EQ(be.supports_calls, 1);
    CHECK_EQ(be.prepare_calls, 0);
}

TEST_CASE(hardware_mode_prepare_failure_is_not_implemented_and_reopen_works) {
    auto v = open_video("faststart.mp4");
    REQUIRE(v.st != nullptr);
    syp::test::FakeHwBackend be;
    be.prepare_result = SYP_ERR_IO;
    FFmpegVideoDecoder dec(VideoDecodeMode::Hardware, &be);
    CHECK_EQ(dec.open(v.st->codecpar, v.st->time_base), SYP_ERR_NOT_IMPLEMENTED);
    // 接口契约 2：同一实例可再次 open；失败不留半开状态。
    be.prepare_result = SYP_OK;
    CHECK_EQ(dec.open(v.st->codecpar, v.st->time_base), SYP_OK);
    CHECK(dec.hardware());
}

TEST_CASE(software_mode_ignores_backend_entirely) {
    auto v = open_video("faststart.mp4");
    REQUIRE(v.st != nullptr);
    syp::test::FakeHwBackend be;
    FFmpegVideoDecoder dec(VideoDecodeMode::Software, &be);
    CHECK_EQ(dec.open(v.st->codecpar, v.st->time_base), SYP_OK);
    CHECK(!dec.hardware());
    CHECK_EQ(be.supports_calls, 0);
    CHECK_EQ(be.prepare_calls, 0);
}

// get_format 闸门：候选里没有硬件格式时必须返回 NONE，绝不挑软件格式（不兜底）。
TEST_CASE(choose_hw_format_never_falls_back_to_software) {
    const AVPixelFormat with_hw[] = {AV_PIX_FMT_VIDEOTOOLBOX, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    const AVPixelFormat sw_only[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_NONE};
    const AVPixelFormat hw_last[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_VIDEOTOOLBOX, AV_PIX_FMT_NONE};
    CHECK_EQ(choose_hw_format(with_hw, AV_PIX_FMT_VIDEOTOOLBOX), AV_PIX_FMT_VIDEOTOOLBOX);
    CHECK_EQ(choose_hw_format(hw_last, AV_PIX_FMT_VIDEOTOOLBOX), AV_PIX_FMT_VIDEOTOOLBOX);
    CHECK_EQ(choose_hw_format(sw_only, AV_PIX_FMT_VIDEOTOOLBOX), AV_PIX_FMT_NONE);
    CHECK_EQ(choose_hw_format(nullptr, AV_PIX_FMT_VIDEOTOOLBOX), AV_PIX_FMT_NONE);
}

// 硬解模式下 hwaccel 初始化失败（假后端 prepare() 不挂任何设备，
// 等价于 VT 会话创建失败）绝不能静默软解。H.264 原来会在首个 slice 报错后靠
// avctx->pix_fmt=yuv420p 绕过 get_format 继续软解出帧；HEVC 原来会把每个包计成
// "可跳过"直到 Eof、不报任何错。修复后：零个非硬件帧，且解码器必报错（send 非 OK
// 或 receive Error）。
namespace {
struct HwFailDecodeResult {
    int64_t non_hw_frames = 0;
    bool    errored       = false;
};
void decode_hw_mode_without_device(const char* name, HwFailDecodeResult& res) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture(name), &err);
    REQUIRE(d != nullptr);
    const int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    REQUIRE(vidx >= 0);
    const AVStream* st = d->raw()->streams[vidx];
    syp::test::FakeHwBackend be;   // prepare() 是 no-op：不挂 hw_device_ctx
    FFmpegVideoDecoder dec(VideoDecodeMode::Hardware, &be);
    REQUIRE(dec.open(st->codecpar, st->time_base) == SYP_OK);

    auto drain = [&] {
        for (;;) {
            Frame f;
            const auto r = dec.receive(&f);
            if (r == IVideoDecoder::Receive::Frame) {
                if (f.pix_fmt() != static_cast<int32_t>(AV_PIX_FMT_VIDEOTOOLBOX)) ++res.non_hw_frames;
                continue;
            }
            if (r == IVideoDecoder::Receive::Error) res.errored = true;
            return;
        }
    };
    for (;;) {
        AVPacket* p = nullptr;
        int32_t ti = -1;
        const auto r = d->read(&p, &ti);
        if (r == Demuxer::ReadResult::Eof) break;
        REQUIRE(r == Demuxer::ReadResult::Packet);
        if (ti == vidx) {
            if (dec.send(p) != SYP_OK) res.errored = true;
        }
        av_packet_free(&p);
        if (res.errored) break;
        drain();
        if (res.errored) break;
    }
    if (!res.errored) {
        if (dec.send(nullptr) != SYP_OK) res.errored = true;
        else drain();
    }
}
}  // namespace

TEST_CASE(hardware_mode_hwaccel_init_failure_h264_never_yields_software_frames) {
    HwFailDecodeResult r;
    decode_hw_mode_without_device("faststart.mp4", r);
    CHECK_EQ(r.non_hw_frames, int64_t{0});
    CHECK(r.errored);
}

TEST_CASE(hardware_mode_hwaccel_init_failure_hevc_reports_error) {
    HwFailDecodeResult r;
    decode_hw_mode_without_device("hevc.mp4", r);
    CHECK_EQ(r.non_hw_frames, int64_t{0});
    CHECK(r.errored);
}

// 闩住的拒绝状态随 open() 复位：同一实例 re-open 后再次处于可用状态。
TEST_CASE(hardware_mode_latched_rejection_resets_on_reopen) {
    auto v = open_video("faststart.mp4");
    REQUIRE(v.st != nullptr);
    syp::test::FakeHwBackend be;
    FFmpegVideoDecoder dec(VideoDecodeMode::Hardware, &be);
    REQUIRE(dec.open(v.st->codecpar, v.st->time_base) == SYP_OK);
    bool errored = false;
    for (int i = 0; i < 400 && !errored; ++i) {
        AVPacket* p = nullptr;
        int32_t ti = -1;
        const auto r = v.d->read(&p, &ti);
        if (r != Demuxer::ReadResult::Packet) break;
        if (ti == v.st->index && dec.send(p) != SYP_OK) errored = true;
        av_packet_free(&p);
        Frame f;
        if (!errored && dec.receive(&f) == IVideoDecoder::Receive::Error) errored = true;
    }
    REQUIRE(errored);
    REQUIRE(dec.open(v.st->codecpar, v.st->time_base) == SYP_OK);
    Frame f;
    CHECK(dec.receive(&f) == IVideoDecoder::Receive::NeedInput);
}

namespace {
int64_t count_decoded_video_frames(const char* name, bool skip_nonref) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture(name), &err);
    if (d == nullptr) return -1;
    const int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    if (vidx < 0) return -1;
    FFmpegVideoDecoder dec;
    if (dec.open(d->raw()->streams[vidx]->codecpar, d->raw()->streams[vidx]->time_base) != SYP_OK) return -1;
    dec.set_skip_nonref(skip_nonref);
    int64_t frames = 0;
    for (;;) {
        AVPacket* p = nullptr;
        int32_t ti = -1;
        const auto r = d->read(&p, &ti);
        if (r != Demuxer::ReadResult::Packet) break;
        if (ti == vidx) dec.send(p);
        av_packet_free(&p);
        Frame f;
        while (dec.receive(&f) == IVideoDecoder::Receive::Frame) ++frames;
    }
    dec.send(nullptr);
    Frame f;
    while (dec.receive(&f) == IVideoDecoder::Receive::Frame) ++frames;
    return frames;
}
}  // namespace

// 追帧：跳过非参考帧后解出的帧严格变少，但不为 0（参考帧照常出）。
TEST_CASE(video_decoder_skip_nonref_drops_nonreference_frames) {
    const int64_t all  = count_decoded_video_frames("bframes.mp4", false);
    const int64_t some = count_decoded_video_frames("bframes.mp4", true);
    REQUIRE(all > 0);
    CHECK(some > 0);
    CHECK(some < all);
}

// open() 复位为关闭：同一实例重开后解出全部帧。
TEST_CASE(video_decoder_skip_nonref_resets_on_open) {
    syp_status err = SYP_OK;
    auto d = Demuxer::open_file(fixture("bframes.mp4"), &err);
    REQUIRE(d != nullptr);
    const int32_t vidx = find_track(*d, AVMEDIA_TYPE_VIDEO);
    FFmpegVideoDecoder dec;
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar, d->raw()->streams[vidx]->time_base) == SYP_OK);
    dec.set_skip_nonref(true);
    REQUIRE(dec.open(d->raw()->streams[vidx]->codecpar, d->raw()->streams[vidx]->time_base) == SYP_OK);
    CHECK(!dec.skip_nonref());
}

int main() { return tiny_test_main(); }
