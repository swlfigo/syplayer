// audio_unit_sink.mm — AudioUnitSink 实现。
//
// 平台分叉：macOS 用 kAudioUnitSubType_HALOutput，iOS 用
// kAudioUnitSubType_RemoteIO——CoreAudio 的设备级 HAL 属性
// （AudioObjectGetPropertyData 等）在 iOS SDK 上根本不存在（不是权限
// 问题，是连 CoreAudio/CoreAudio.h 这条头文件路径都没有），所以设备延迟
// 三分量里 safety offset / buffer frame size 两项在 iOS 上留 0，只有
// AudioUnit 自身的 kAudioUnitProperty_Latency 是两个平台共有的。
// tools/check-deploy-target.sh 按 arm64-apple-ios13.0 对本文件做语法
// 检查，TARGET_OS_OSX/否则分支必须两边都能编译通过——这是本文件在没有
// iOS 真机的情况下唯一能验证的部分。这台开发机是 macOS；
// iOS 路径的功能正确性未经真机验证，是已知的一个限制：具体后果是 played_us()
// 系统性高报 10~40ms，吃掉 kPresentWindowUs 25%~100%，且恒偏同一侧。
//
// 本文件里唯一在实时音频线程上跑的代码是 render_cb（下面第一个函数）。
// 它之外的一切——open()/write()/flush()/set_speed()/析构——都跑在调用方
// 线程（TrackPlayer 所在线程），允许分配内存、调 FFmpeg、打日志、加锁。

#include "platform/apple/audio_unit_sink.h"

#include <syplayer/syp_types.h>

#import <AudioToolbox/AudioToolbox.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <CoreAudio/CoreAudio.h>
#else
// AVAudioSession 只在 iOS / tvOS / Mac Catalyst 上存在，纯 macOS
// （TARGET_OS_OSX）没有这个类——即使链了 AVFoundation.framework，纯 macOS
// SDK 的 AVFoundation 头里也没有 AVAudioSession 的声明。这条 #else 跟
// query_and_log_device_latency() / open() / 析构里其它三处 `#if
// TARGET_OS_OSX` ... `#else` 分叉用同一个判据，保持文件内一致。
//
// 只导入 AVAudioSession.h 这一个子头，不导入 <AVFoundation/AVFoundation.h>
// 这个总头：tools/check-deploy-target.sh 用 -std=c++23 objective-c++
// 严格模式（-Wpedantic -Werror 等）对 iOS SDK 做 -fsyntax-only 检查时，
// 总头间接拉进的 AVCaptureDevice.h/AVCaptureInput.h 在这套编译选项下会
// 报 SDK 自身的头文件错误（"nullability specifier 'nullable' cannot be
// applied to non-pointer type 'AVMediaType'"）——跟本文件的改动无关，是
// 这几个 Capture 头在这套严格标志下的既有问题，本类完全不需要 AVCapture
// 家族的任何声明，只导入真正用到的子头即可绕开。
#import <AVFoundation/AVAudioSession.h>
#endif

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

// dl 层的全局日志汇（唯一的 syp_set_log_callback 注册点在
// src/dl/syp_source_api.cpp）转发到这里；只前置声明签名，不
// #include "dl/source_bridge.h"——那个头拖着 cache_file.h/cache_index.h/
// scheduler.h 一整套 dl 层内部类型，本文件只要这一个函数。签名必须跟
// source_bridge.h 里的声明逐字匹配。
namespace syp::dl {
void log_msg(syp_log_level lvl, const char* tag, const char* msg);
}  // namespace syp::dl

namespace syp::platform {

namespace {

// 输出（设备）格式恒定为交错 Float32——不管源内容是什么采样格式，都由
// swresample 转成这个格式再喂给 AudioUnit。选它不是巧合：CoreAudio 的
// canonical float 格式，且与源 sample_fmt 无关，天然契合"open() 时还不
// 知道真实 sample_fmt"这个约束——设备格式在 open() 就能
// 定死，源格式留到第一帧 Frame 到达时才需要。
constexpr AVSampleFormat kDeviceSampleFmt = AV_SAMPLE_FMT_FLT;

inline AudioUnit as_audio_unit(void* p) noexcept { return static_cast<AudioUnit>(p); }

// 实时线程。这个函数体里出现的每一个符号都必须是"不会阻塞、不会分配"
// 的。inRefCon 指向的 RenderCtx——它拿不到 AudioUnitSink 本身，更拿不到
// TrackPlayer / Pipeline。这不是纪律要求，是结构事实：RenderCtx 只声明了
// 一个环、若干原子量指针（underrun/underrun_count/armed/underrun_edge_reset/
// gain_target）与若干渲染线程私有字段（in_underrun、
// gain_current/channels/sample_rate），没有第 N+1 个字段能塞下更多东西
// （见 audio_unit_sink.h RenderCtx 定义）。
//
// 这里补了两处需要注意的边界（下面分别标注），逻辑主干不变——这仍然是
// 整个音频链路最危险的一段代码，改动后用 objdump 逐条核对过反汇编（此前
// 版本这里写的"3 条 bl"数字不准，已改成实测值，分平台两套数字）：
//
// 【重测】数的是
// `syp::platform::(anonymous namespace)::render_cb(void*, unsigned int*,
// AudioTimeStamp const*, unsigned int, unsigned int, AudioBufferList*)`
// 这个符号（匿名命名空间里的符号名会被修饰，用
// `otool -tV audio_unit_sink.mm.o | c++filt` 还原可读签名之后按函数边界
// 数，不是靠 awk 猜区间）：
//   - Debug（build/，-O0）：10 条 bl。此前的旧注释写的是 4 条——
//     这里新增的增益/斜坡逻辑在 -O0 下没有被内联（std::atomic<float>
//     ::load、AudioUnitSink::apply_gain 本身、std::atomic<bool>::exchange
//     等各自多出一条 bl），4 → 10 是真实增长，不是测错。
//   - Release（build-rel/，-O2）：仍是 2 条 bl（AudioRing::read /
//     _bzero），与此前记的数字**巧合一致**——不是没测，是测过了发现
//     没变：apply_gain()/advance_gain() 都是 static 纯函数、跟 render_cb
//     同一个 TU，-O2 下被完整内联进 render_cb 本体，一条调用指令都不剩；
//     std::min 内联成 csel、atomic<bool>::store(relaxed) 内联成一条普通
//     strb，跟旧注释描述的一致。
// 两种情况下都零分配、零锁、零 ObjC 消息、零 FFmpeg、零日志——这条护栏没
// 因为这次改动被打破。
OSStatus render_cb(void* inRefCon, AudioUnitRenderActionFlags* ioActionFlags,
                    const AudioTimeStamp* /*inTimeStamp*/, UInt32 /*inBusNumber*/,
                    UInt32 inNumberFrames, AudioBufferList* ioData) {
    // ioData->mBuffers[0] 在 mNumberBuffers == 0 时是越界访问。
    // ASBD 是我们自己在 open() 里设的、正常情况下恒有 1 个 buffer，概率
    // 极低，但这是本文件里唯一一处「没有任何断言/日志/测试」的实时线程，
    // 值得一个分支的代价。
    if (ioData->mNumberBuffers == 0) return noErr;

    auto* ctx = static_cast<AudioUnitSink::RenderCtx*>(inRefCon);

    // want 必须钳在 mDataByteSize 之内——inNumberFrames *
    // bytes_per_frame 理论上应该恒等于 mDataByteSize（我们自己设的
    // ASBD），但不做这个钳位的话，一旦哪天不等，AudioRing::read 会往
    // ioData 提供的缓冲区外 memcpy，在实时线程上越界写。std::min 在
    // Release 下内联成 csel、不引入函数调用；Debug（-O0）下不内联、
    // 会多一条 bl——跟上面那段"Debug 10 条/Release 2 条"的反汇编实测数字
    // 一致，不是矛盾。
    const int32_t want = std::min<int32_t>(
        static_cast<int32_t>(inNumberFrames) * ctx->bytes_per_frame,
        static_cast<int32_t>(ioData->mBuffers[0].mDataByteSize));
    auto* dst = static_cast<uint8_t*>(ioData->mBuffers[0].mData);

    const int32_t got = ctx->ring->read(dst, want);

    // 增益 + 每帧线性斜坡。只作用在真实样本（前 got 字节）上——
    // 补出来的静音本来就是 0，乘任何数都是 0，没必要碰。斜坡按**实际输出的
    // 帧数**推进：欠载时少推进，跟听到的声音一致，不会在欠载段里凭空把斜坡
    // 走完——got == 0（完全欠载）时 nfloats == 0，apply_gain() 按订正后的逻辑
    // 返回 current 而不是 target，这里不会因为"没有真实输出"就让斜坡自己
    // 瞬间走完。current == 1.0f && target == 1.0f（绝大多数时间）时完全跳过，
    // 不调 apply_gain、不碰样本——这是本函数在"没人调过 set_gain()"这条
    // 最常见路径上唯一新增的分支判断，零额外开销（这条快速
    // 跳过留在这里而不是塞进 apply_gain 内部，好处是 apply_gain 本身可以
    // 被单测直接摆一个 current=target=1 的用例验证"恒等"这条行为，不用
    // 靠这条跳过去猜它"本该"是恒等）。
    // 输出 ASBD 是 float32 packed（open() 里设的，见 kDeviceSampleFmt），
    // 所以这里 got 字节数对应 got / 4 个 float——4 是 sizeof(float)，不是
    // 随手写的魔数。
    {
        const float target = ctx->gain_target->load(std::memory_order_relaxed);
        if (ctx->gain_current != 1.0f || target != 1.0f) {
            const int32_t nfloats = got / static_cast<int32_t>(sizeof(float));
            ctx->gain_current = AudioUnitSink::apply_gain(
                reinterpret_cast<float*>(dst), nfloats, ctx->channels, ctx->gain_current,
                target, ctx->sample_rate);
        }
    }

    // flush() 需要让 in_underrun（渲染线程私有）在下一段
    // 播放开始前清零，但 in_underrun 本身绝不能被调用方线程直接写——那是跨
    // 线程无同步访问，即便 AudioOutputUnitStop() 已经同步停了硬件回调，
    // CoreAudio 内部的停止时序也不提供 TSan 能识别的 happens-before 边（实测
    // 会报数据竞争）。真正的写者恒是这条渲染线程自己：flush() 只
    // store 一个独立的原子"请求位"，这里 exchange 消费掉它、由渲染线程自己
    // 把私有标志清零。
    if (ctx->underrun_edge_reset->exchange(false, std::memory_order_relaxed)) {
        ctx->in_underrun = false;
    }

    const bool starved = got < want;
    if (starved) {
        // 欠载：补静音。不打日志、不加锁、不通知任何人——只置原子标志与计数，主侧自己去读。
        std::memset(dst + got, 0, static_cast<size_t>(want - got));
        ctx->underrun->store(true, std::memory_order_relaxed);
    }
    // 欠载段计数。underrun_edge 头文件内联，fetch_add(relaxed) 是一条原子指令，仍零
    // 分配零锁零日志（本函数上方的 bl 条数见文件头 render_cb
    // 上方注释，Release 下仍是 2 条——这条护栏没被后续任何一次改动打破）。
    if (AudioUnitSink::underrun_edge(starved, ctx->armed->load(std::memory_order_relaxed),
                                     &ctx->in_underrun)) {
        ctx->underrun_count->fetch_add(1, std::memory_order_relaxed);
    }
    // kAudioUnitRenderAction_OutputIsSilence 的语义是"整个
    // buffer 都是静音"，宿主可能据此直接跳过/清零整块——只有 got == 0
    // （一个字节真实音频都没有）时才能置这个标志。got 在 (0, want) 之间
    // 时前 got 字节是真实音频、且 consumed_bytes 已经因它们真的前进了：
    // 若这时仍置 OutputIsSilence，宿主清零整块的话就变成"时钟前进了但
    // 那段音频没播出去"——跟"补的静音不能计进
    // consumed_bytes"这条纪律是同一类错误，只是从另一扇门（宿主对这个标志的
    // 解读）进来。最初的实现是 `got < want` 就置位，这一条同时
    // 是对那个实现的修正。
    if (got == 0) *ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

}  // namespace

// ---------------------------------------------------------------------
// 可脱离硬件独立测试的两段算术
// ---------------------------------------------------------------------

bool AudioUnitSink::commit_frame(syp::media::AudioRing& ring, const uint8_t* data,
                                  int32_t bytes) noexcept {
    if (data == nullptr || bytes <= 0) return false;
    if (ring.writable() < bytes) return false;   // 背压：空间不够整帧，一个字节都不写
    const int32_t n = ring.write(data, bytes);
    // 到这里已经确认 writable() >= bytes；本类是唯一的生产者（write() 只
    // 从调用方线程调用一次一次串行进来），消费者（render_cb）只会让
    // writable() 变大、不会变小，所以这次 write 必然整帧成功。下面这条
    // assert 只是把这条不变量摆在明面上，不是防御性代码——真触发了说明
    // "唯一生产者"这条假设被打破了。返回值仍然如实取自 n==bytes 而不是
    // 硬编码 true。
    assert(n == bytes);
    return n == bytes;
}

int64_t AudioUnitSink::compute_played_us(int64_t consumed_bytes, int64_t base_consumed_bytes,
                                          int64_t device_latency_us, int32_t sample_rate,
                                          int32_t bytes_per_frame, double speed,
                                          int64_t base_us) noexcept {
    if (sample_rate <= 0 || bytes_per_frame <= 0) return base_us;

    // 快照相减：AudioRing::consumed_bytes() 单调递增、reset() 不清它，
    // 直接拿当前值当"自上次 flush 以来"的消费量用是一个陷阱——
    // 每次 flush 之后会把 flush 之前的全部历史消费量
    // 也算进来，表现为时钟突然跳到远超预期的值。
    const int64_t net_consumed_bytes = consumed_bytes - base_consumed_bytes;
    const int64_t consumed_frames    = net_consumed_bytes / bytes_per_frame;
    const int64_t latency_frames =
        device_latency_us * static_cast<int64_t>(sample_rate) / 1'000'000;
    const int64_t effective = consumed_frames - latency_frames;
    if (effective <= 0) return base_us;
    return base_us + static_cast<int64_t>(static_cast<double>(effective) * 1'000'000.0 /
                                          static_cast<double>(sample_rate) * speed);
}

int64_t AudioUnitSink::seconds_to_us_clamped(double seconds) noexcept {
    // !(seconds > 0.0) 一并吃掉 NaN（NaN 参与的任何比较都是 false，
    // !false = true）、负数、0——都视为 0，不往下走乘法。
    if (!(seconds > 0.0)) return 0;
    // >= 0.5s（含 +inf）直接钳到上限，换算成 us 之前就拦掉——避免
    // seconds * 1'000'000.0 产出一个 static_cast<int64_t> 会溢出/UB 的
    // 超范围浮点数（1e300 * 1e6 早就超过 int64_t 能表示的范围）。
    if (seconds >= 0.5) return 500000;
    // 到这里 seconds 在 (0, 0.5) 内，是有限值（NaN/inf 都已经在上面被
    // 拦截），乘 1e6 之后落在 (0, 500000) 内，static_cast<int64_t> 安全。
    return static_cast<int64_t>(seconds * 1'000'000.0);
}

int64_t AudioUnitSink::compose_device_latency_us(int64_t au_latency_us, int64_t safety_us,
                                                  int64_t buffer_us, int64_t session_output_us,
                                                  int64_t session_io_buffer_us) noexcept {
    constexpr int64_t kMax = 500000;
    int64_t sum = 0;
    for (int64_t v : {au_latency_us, safety_us, buffer_us, session_output_us, session_io_buffer_us}) {
        if (v <= 0) continue;
        if (v >= kMax || sum >= kMax - v) return kMax;
        sum += v;
    }
    return sum;
}

int64_t AudioUnitSink::slew_latency_us(int64_t applied_us, int64_t target_us,
                                       int64_t elapsed_us) noexcept {
    if (elapsed_us <= 0 || applied_us == target_us) return applied_us;
    const int64_t max_step = static_cast<int64_t>(static_cast<double>(elapsed_us) * kLatencySlewRate);
    if (target_us > applied_us) {
        return (target_us - applied_us <= max_step) ? target_us : applied_us + max_step;
    }
    return (applied_us - target_us <= max_step) ? target_us : applied_us - max_step;
}

// ---------------------------------------------------------------------
// 增益与斜坡：可脱离硬件独立测试的纯算术
// ---------------------------------------------------------------------

float AudioUnitSink::advance_gain(float current, float target,
                                  int32_t frames, int32_t sample_rate) noexcept {
    // frames <= 0 与 sample_rate <= 0 是两件不同的事，
    // 早先版本把它们揉进同一条 "return target"，是错的：
    //   - frames <= 0：这次回调**没有真实输出**（0 帧 = 0 时间流逝）。
    //     斜坡只按实际输出的帧数推进（欠载时少推进）
    //     ——0 帧就是"推进 0"，必须停在 current，不能凭空跳到 target
    //     （那等价于"完全欠载时斜坡自己走完"：听不到声音的间隙里音量
    //     状态却已经变了，下次真正出声时不连续）。
    //   - sample_rate <= 0：真的有输出（frames > 0），但采样率非法，
    //     算不出"多少帧对应多少毫秒"这个步长——这时才没有"半吊子过渡"
    //     可言，只能直接落点到 target。
    if (frames <= 0) return current;
    if (sample_rate <= 0) return target;
    const float ramp_frames = static_cast<float>(sample_rate) *
                              static_cast<float>(kGainRampMs) / 1000.0f;
    if (!(ramp_frames > 0.0f)) return target;
    const float step = static_cast<float>(frames) / ramp_frames;
    if (target > current) return std::min(target, current + step);
    if (target < current) return std::max(target, current - step);
    return target;
}

float AudioUnitSink::apply_gain(float* samples, int32_t nfloats, int32_t channels,
                                float current, float target, int32_t sample_rate) noexcept {
    if (nfloats <= 0 || channels <= 0) {
        // 没有样本可乘——不碰缓冲区。斜坡状态委托给 advance_gain(…, 0, …)，
        // 按上面的约定返回 current（不前进），不是 target。
        return advance_gain(current, target, 0, sample_rate);
    }
    // 只处理整帧；nfloats 不是 channels 的整数倍时，落在末尾的零头样本
    // 在下面单独处理（见后）。
    const int32_t nframes = nfloats / channels;
    const float   g1      = advance_gain(current, target, nframes, sample_rate);
    // 按帧插值，不按样本插值：同一帧内所有声道乘同一个增益值。
    // dg 用 (nframes - 1) 做分母（而不是正文参考实现的 nfloats - 1），
    // 这样最后一帧的增益恰好落在 g1 上，块间连续；nframes <= 1 时没有
    // "块内"可言，整块直接用 current（下面 dg = 0）。
    const float dg = (nframes > 1) ? (g1 - current) / static_cast<float>(nframes - 1) : 0.0f;
    for (int32_t frame = 0; frame < nframes; ++frame) {
        const float g = current + dg * static_cast<float>(frame);
        for (int32_t ch = 0; ch < channels; ++ch) {
            samples[frame * channels + ch] *= g;
        }
    }
    // 尾部不满一帧的零头样本（nfloats 不是 channels 的
    // 整数倍时才会有——理论上 ring 里只会整帧写入/读出，但 render_cb 里
    // `want` 会被 ioData 的 mDataByteSize 截断，理论上可能切在半帧中间）：
    // 早先版本让它们维持原样不动，静音时这几个样本会以**全音量**漏出——
    // 全部按本块结束时的增益 g1 处理，跟"块间连续"的口径一致（下一块
    // 从 g1 继续）。
    for (int32_t i = nframes * channels; i < nfloats; ++i) {
        samples[i] *= g1;
    }
    return g1;
}

// ---------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------

AudioUnitSink::AudioUnitSink(int32_t ring_ms) : ring_ms_(ring_ms > 0 ? ring_ms : 200) {}

AudioUnitSink::~AudioUnitSink() {
    // 析构顺序是硬要求：必须先停止真正的硬件回调，才能
    // 安全释放 ring_/swr_。AudioRing::reset() 的前置条件是"消费者已
    // 停止"（audio_ring.h：这是该类唯一一处靠调用方纪律而非结构保证
    // 的地方）；stop_and_dispose_unit() 里的 AudioOutputUnitStop 是
    // 同步调用，返回时 render_cb 保证不会再被调用，之后碰 ring_/swr_
    // 才安全。ring_（unique_ptr）在本函数体结束后的隐式成员析构里释放，
    // 那时回调已经停了，天然满足这条前置条件。
    stop_and_dispose_unit();
    if (swr_ != nullptr) swr_free(&swr_);
}

void AudioUnitSink::stop_and_dispose_unit() noexcept {
#if !TARGET_OS_OSX
    // 路由变化观察者跟 audio_unit_ 的生命周期绑在一起注册/注销：无论是
    // 析构还是 open() 重复调用（open() 一进来就先调本函数收干净旧状态，
    // 见 open()），都要在这里注销，否则重复 open() 会注册第二个观察者，
    // 之后每次路由变化会触发两次重算——不算错误但是浪费。
    //
    // 这里**不能**指望"注销之后 block 就不会再跑"来保证并发
    // 安全——`removeObserver:` 只保证以后不会再投递新的调用，不保证等待
    // 一个此刻已经在其它线程上执行中的 block 结束（这是原设计的
    // 缺陷：旧版本靠"注销发生在析构/重新 open() 之前"这条时序去论证安全，
    // 站不住）。真正的并发安全靠 open() 里注册 block 时只捕获
    // LatencyState 的 shared_ptr、不捕获 `this`/`audio_unit_`——block 就算
    // 在这里注销之后仍在飞、甚至在 ~AudioUnitSink() 跑完之后才真正执行，
    // 它手里的 shared_ptr 副本也只会安全地写进一个可能已经没人再读、但
    // 依然合法存活的 LatencyState 对象，不是 UAF、也不会污染新状态
    // （详见 audio_unit_sink.h 里 LatencyState/state_ 的注释）。这里的
    // 注销纯粹是为了不重复注册、不泄漏观察者，不是并发安全的防线。
    if (route_observer_ != nullptr) {
        id token = CFBridgingRelease(route_observer_);
        [[NSNotificationCenter defaultCenter] removeObserver:token];
        route_observer_ = nullptr;
    }
#endif
    if (audio_unit_ == nullptr) return;
    AudioUnit au = as_audio_unit(audio_unit_);
    AudioOutputUnitStop(au);
    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(au);
    audio_unit_ = nullptr;
}

// ---------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------

// 这个函数**只在 open() 里被调用一次**，是调用方线程上的
// 单线程代码，没有并发问题——跟 open() 其它逻辑同一条既有纪律。它不再
// 像此前那样被路由变化 block 直接调用（旧设计里 block 捕获
// `self_ptr` 再调回这个成员函数，能碰到 `this`/`audio_unit_`，这样
// 不安全：`removeObserver:` 不等待在飞的 block，
// `this` 可能已经析构、`audio_unit_` 可能已经被新的 open() 换掉）。
// 路由变化触发的后续重算改为内联在 open() 注册观察者的那个 block 里，
// 只重算 session 两个分量、只捕获 LatencyState 的 shared_ptr——不碰
// `this`，因此也不需要这里再谈并发安全。
void AudioUnitSink::query_and_log_device_latency() {
    AudioUnit au = as_audio_unit(audio_unit_);

    Float64 au_latency_s = 0;
    UInt32  sz            = static_cast<UInt32>(sizeof(au_latency_s));
    AudioUnitGetProperty(au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
                         &au_latency_s, &sz);
    const int64_t au_latency_us = seconds_to_us_clamped(static_cast<double>(au_latency_s));

    int64_t safety_us = 0;
    int64_t buffer_us  = 0;
    int64_t session_output_us    = 0;
    int64_t session_io_buffer_us = 0;
#if TARGET_OS_OSX
    AudioObjectID dev = 0;
    UInt32 dev_sz = static_cast<UInt32>(sizeof(dev));
    AudioObjectPropertyAddress dev_addr{kAudioHardwarePropertyDefaultOutputDevice,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &dev_addr, 0, nullptr, &dev_sz,
                                    &dev) == noErr) {
        UInt32 safety_frames = 0;
        UInt32 fsz            = static_cast<UInt32>(sizeof(safety_frames));
        AudioObjectPropertyAddress safety_addr{kAudioDevicePropertySafetyOffset,
                                                kAudioObjectPropertyScopeOutput,
                                                kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(dev, &safety_addr, 0, nullptr, &fsz, &safety_frames) ==
            noErr) {
            safety_us = static_cast<int64_t>(safety_frames) * 1'000'000 /
                        static_cast<int64_t>(sample_rate_);
        }

        UInt32 buf_frames = 0;
        fsz                = static_cast<UInt32>(sizeof(buf_frames));
        AudioObjectPropertyAddress buf_addr{kAudioDevicePropertyBufferFrameSize,
                                             kAudioObjectPropertyScopeOutput,
                                             kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(dev, &buf_addr, 0, nullptr, &fsz, &buf_frames) == noErr) {
            buffer_us = static_cast<int64_t>(buf_frames) * 1'000'000 /
                        static_cast<int64_t>(sample_rate_);
        }
    }
#else
    // iOS/tvOS/Mac Catalyst：CoreAudio 的设备级 HAL 属性不存在（见文件头
    // 注释），safety_us/buffer_us 两项留 0；改用 AVAudioSession 暴露的
    // 等价量（本机是 macOS，这条路径没有真机验证）。
    AVAudioSession* session = [AVAudioSession sharedInstance];
    session_output_us    = seconds_to_us_clamped(static_cast<double>(session.outputLatency));
    session_io_buffer_us = seconds_to_us_clamped(static_cast<double>(session.IOBufferDuration));
#endif

    // 这里建一个全新的 LatencyState 换掉 state_（不是往旧
    // 对象里写）——重复 open() 时，旧的路由变化 block（如果还在飞）手里
    // 攥着的是旧 shared_ptr 副本，只会继续写那个旧对象，绝不会碰这个新
    // 建的 state_，见 audio_unit_sink.h 里 LatencyState 的注释。
    // au_latency_us/safety_us/buffer_us 这三个分量在构造之后当常量存
    // （路由变化不改变它们），只有 device_latency_us 会被以后的路由变化
    // block 反复覆写。
    auto state          = std::make_shared<LatencyState>();
    state->au_latency_us = au_latency_us;
    state->safety_us     = safety_us;
    state->buffer_us     = buffer_us;
    const int64_t total = compose_device_latency_us(au_latency_us, safety_us, buffer_us,
                                                      session_output_us, session_io_buffer_us);
    state->device_latency_us.store(total, std::memory_order_relaxed);
    state_ = std::move(state);

    // 五个分量分别打日志——真机上漂移读数不对时，第一件事就是看这几个数。
    // 直接打刚算出来的本地
    // `total`，不重新 load 原子量——同一线程内这次 store 之前算出来的值
    // 跟 load 回来的值必然相同，重新 load 只是多一次没必要的原子操作。
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "device latency components: au=%lldus safety=%lldus buffer=%lldus session_output=%lldus "
        "session_io_buffer=%lldus total=%lldus (sample_rate=%d channels=%d)",
        static_cast<long long>(au_latency_us), static_cast<long long>(safety_us),
        static_cast<long long>(buffer_us), static_cast<long long>(session_output_us),
        static_cast<long long>(session_io_buffer_us), static_cast<long long>(total), sample_rate_,
        channels_);
    dl::log_msg(SYP_LOG_INFO, "audio_unit_sink", buf);
}

syp_status AudioUnitSink::open(int32_t sample_rate, int32_t channels, int32_t /*sample_fmt*/) {
    // sample_fmt 参数按 IAudioSink 接口约定接收，但 TrackPlayer::create()
    // 那边恒传 AV_SAMPLE_FMT_NONE（sample_fmt 不在
    // TrackInfo 里，格式来源改用 Frame）——本实现故意不使用这个参数，
    // 真实源格式在第一帧 Frame 到达 write() 时才知道，见 ensure_swr_for()。
    gain_at_open_ = std::numeric_limits<float>::quiet_NaN();   // 见 gain_at_open_for_test()
    if (sample_rate <= 0 || channels <= 0) return SYP_ERR_INVALID_ARG;

    // 允许重复 open()：先按析构同一套顺序把旧的硬件状态收干净，再重新
    // 来一遍。TrackPlayer::create() 目前只会调一次，这里做成幂等只是
    // 不给调用方挖坑。
    stop_and_dispose_unit();
    if (swr_ != nullptr) { swr_free(&swr_); swr_ = nullptr; }
    swr_in_rate_ = 0; swr_in_channels_ = 0; swr_in_sample_fmt_ = -1; swr_speed_ = 0.0;
    opened_ = false;
    failed_ = false;
    // 重复 open() 之前这两个字段没有被重置——注释
    // 承诺了幂等，但会继承上一轮的 flush 基准、且一律重新 Start（不管
    // 上一轮是不是被 pause() 过）。开一条新设备实例，语义上就是全新
    // 状态：基准回到 0（从未 flush 过），暂停状态也重来。
    base_us_ = 0;
    latency_applied_valid_ = false;
    last_played_us_        = 0;
    paused_  = false;

    channels_        = channels;
    sample_rate_      = sample_rate;
    bytes_per_frame_  = channels_ * static_cast<int32_t>(av_get_bytes_per_sample(kDeviceSampleFmt));

    // 环容量按时长配置（默认 200ms，构造时经 ring_ms_ 定死；
    // 要够大以吸收泵线程的调度抖动）。约定是"capacity_bytes <= 0
    // 静默兜底成 1 字节，风险转移到调用侧"——这里必须自己断言 > 0，不能
    // 依赖那个兜底当错误处理。sample_rate_/channels_ 都已经在上面校验过
    // > 0，bytes_per_frame_ 因此 > 0，ring_ms_ 在构造函数里已经钳到 > 0，
    // 乘积只要不溢出 int32_t 就必然 > 0（常见采样率×声道数×4 字节×几百
    // 毫秒远小于 int32_t 上限，不做额外的溢出检查）。
    const int32_t ring_capacity_bytes =
        static_cast<int32_t>(static_cast<int64_t>(bytes_per_frame_) * sample_rate_ * ring_ms_ /
                              1000);
    assert(ring_capacity_bytes > 0);
    ring_ = std::make_unique<syp::media::AudioRing>(ring_capacity_bytes);
    base_consumed_bytes_ = ring_->consumed_bytes();   // 恒为 0，但让"每次 open 都重新对齐基准"这条不变量显式化
    underrun_.store(false, std::memory_order_relaxed);
    underrun_count_.store(0, std::memory_order_relaxed);
    armed_.store(false, std::memory_order_relaxed);
    underrun_edge_reset_.store(false, std::memory_order_relaxed);
    // gain_current 在这里（构造 render_ctx_ 的同一条语句）就地
    // 取 gain_target_ 的当前值，不是留到"open() 末尾"再单独赋值——render_cb
    // 在下面 AudioOutputUnitStart() 成功之后随时可能开始在渲染线程上运行，
    // 如果 gain_current/channels/sample_rate 这三个渲染线程私有字段是在
    // Start() 之后才写，就是对正在跑的回调线程做无同步的跨线程写，与
    // in_underrun 那条"绝不能被调用方线程直接写"的纪律是同一类错误。
    // 首次 open() 不淡入：这里取的是"此刻"的 gain_target_，
    // 不是硬编码 1.0f——如果调用方在 open() 之前已经调过 set_gain()，
    // 新打开的设备应该直接从那个目标值起播，不应该先经历一段无意义的
    // 静音→目标斜坡。
    render_ctx_ = RenderCtx{ring_.get(),       &underrun_, &underrun_count_, &armed_,
                            &underrun_edge_reset_, bytes_per_frame_, false,
                            &gain_target_, gain_target_.load(std::memory_order_relaxed),
                            channels_, sample_rate_};
    // 记下渲染线程将要从哪个增益起播——读的是刚构造好的
    // render_ctx_.gain_current 本身（不是再 load 一次 gain_target_），这样
    // "上面那句快照写错了"也能被 gain_at_open_for_test() 看见。此刻还没
    // AudioOutputUnitStart()，渲染线程不存在，读它不构成跨线程访问。
    gain_at_open_ = render_ctx_.gain_current;

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_OSX
    desc.componentSubType = kAudioUnitSubType_HALOutput;
#else
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) { failed_ = true; return SYP_ERR_IO; }

    AudioUnit au = nullptr;
    if (AudioComponentInstanceNew(comp, &au) != noErr || au == nullptr) {
        failed_ = true;
        return SYP_ERR_IO;
    }

    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate       = static_cast<Float64>(sample_rate_);
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel   = 32;
    asbd.mChannelsPerFrame = static_cast<UInt32>(channels_);
    asbd.mBytesPerFrame    = static_cast<UInt32>(bytes_per_frame_);
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame * asbd.mFramesPerPacket;

    if (AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd,
                              static_cast<UInt32>(sizeof(asbd))) != noErr) {
        AudioComponentInstanceDispose(au);
        failed_ = true;
        return SYP_ERR_IO;
    }

    AURenderCallbackStruct cb{};
    cb.inputProc       = &render_cb;
    cb.inputProcRefCon = &render_ctx_;
    if (AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                              &cb, static_cast<UInt32>(sizeof(cb))) != noErr) {
        AudioComponentInstanceDispose(au);
        failed_ = true;
        return SYP_ERR_IO;
    }

    if (AudioUnitInitialize(au) != noErr) {
        AudioComponentInstanceDispose(au);
        failed_ = true;
        return SYP_ERR_IO;
    }

    audio_unit_ = au;
    query_and_log_device_latency();

    if (AudioOutputUnitStart(au) != noErr) {
        // 到这里 audio_unit_ 已经赋值，走正常的 stop_and_dispose_unit()
        // 收尾（AudioOutputUnitStop 对一个没成功 Start 的 unit 调用是
        // 安全的 no-op）。
        stop_and_dispose_unit();
        failed_ = true;
        return SYP_ERR_IO;
    }

    opened_ = true;

#if !TARGET_OS_OSX
    // 路由变化（拔耳机、切蓝牙等）会改变 AVAudioSession 的
    // outputLatency/IOBufferDuration——注册观察者，变化时重新查询并原子
    // 写回 device_latency_us（LatencyState 里的那个原子量，不是本类的
    // 成员）。stop_and_dispose_unit() 已经在本函数开头注销过上一轮的
    // 观察者（若有），这里注册的必然是唯一一个。queue:nil 意味着在发
    // 通知的线程上同步调用——那条线程是 AVAudioSession 内部的投递线程，
    // 不是这个调用方线程，也不保证跟析构/下一次 open() 有任何时序关系。
    //
    // 这个 block **只捕获
    // `state`**（LatencyState 的 shared_ptr 拷贝），不捕获 `this`、不
    // 捕获 `audio_unit_`。这是唯一让并发安全立得住的做法——
    // `removeObserver:` 不等待正在执行的 block，所以任何时候都可能有一
    // 个 block 调用还在飞：
    //   - 如果它捕获 `this`：析构之后 `this` 已经是悬空指针，block 却
    //     还可能在跑，是 UAF；
    //   - 如果它捕获 `this` 再回读 `audio_unit_`：重新 open() 会先把
    //     旧的 audio_unit_ Dispose 掉、`this` 上的字段也会被新一轮 open()
    //     改写，旧 block 会读到"半新半旧"的状态，甚至可能在新 open() 已
    //     经把新读数存好之后，才姗姗来迟地把新读数覆盖回旧读数（"stale
    //     overwrite"）。
    // 只捕获 `state`（shared_ptr 拷贝）从根上避免这两个问题：block 手里
    // 的这份引用计数独立保活它指向的 LatencyState，跟 `this`/
    // `audio_unit_`/`state_`（本类成员，随时可能指向一个新对象）完全
    // 脱钩。block 里也不再调用 query_and_log_device_latency()（那个函数
    // 要碰 `audio_unit_`/`sample_rate_`），而是调静态的
    // recompose_session_latency(*state)——"只重算 session 两个分量、配合
    // state 里缓存的另外三个分量重新 compose"这一小段逻辑——AudioUnit 自身延迟和 macOS HAL 分量本就不会因路由变化而变，
    // 不需要重新读硬件。
    std::shared_ptr<LatencyState> state = state_;
    id token = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioSessionRouteChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                    AudioUnitSink::recompose_session_latency(*state);
                }];
    route_observer_ = (void*)CFBridgingRetain(token);
    // 注册之后再按当前 session 读数重算一次、写回 state。
    // query_and_log_device_latency() 的初始读数发生在注册之前——两者之间若恰好发生
    // 路由变化，那次通知没有观察者接收，延迟会停在旧值直到下一次路由变化。注册后
    // 补读一次关掉这个窗口；与在飞 block 并发写同一个原子量无害（两者读的都是
    // "此刻"的 session 值，谁后写都是新读数）。
    recompose_session_latency(*state);
#endif

    return SYP_OK;
}

#if !TARGET_OS_OSX
// 路由变化 block 与 open() 注册后补读共用：只重读 session 两个分量，配合 state 里
// 缓存的另外三个分量重新 compose 并写回。只碰 state，不碰 this/audio_unit_（理由见
// open() 注册处注释）。
void AudioUnitSink::recompose_session_latency(LatencyState& state) noexcept {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    const int64_t session_output_us =
        seconds_to_us_clamped(static_cast<double>(session.outputLatency));
    const int64_t session_io_buffer_us =
        seconds_to_us_clamped(static_cast<double>(session.IOBufferDuration));
    const int64_t total = compose_device_latency_us(state.au_latency_us, state.safety_us,
                                                    state.buffer_us, session_output_us,
                                                    session_io_buffer_us);
    state.device_latency_us.store(total, std::memory_order_relaxed);
}
#endif

// ---------------------------------------------------------------------
// write()
// ---------------------------------------------------------------------

void AudioUnitSink::ensure_swr_for(const syp::media::Frame& f) {
    const int32_t in_rate = f.sample_rate();
    const int32_t in_ch   = f.channels();
    const int32_t in_fmt  = f.sample_fmt();

    if (swr_ != nullptr && in_rate == swr_in_rate_ && in_ch == swr_in_channels_ &&
        in_fmt == swr_in_sample_fmt_ && speed_ == swr_speed_) {
        return;   // 已经是当前配置（含 speed_），不用重建
    }
    if (in_rate <= 0 || in_ch <= 0 || in_fmt < 0) { failed_ = true; return; }

    if (swr_ != nullptr) { swr_free(&swr_); swr_ = nullptr; }

    AVChannelLayout in_layout{};
    AVChannelLayout out_layout{};
    av_channel_layout_default(&in_layout, in_ch);
    av_channel_layout_default(&out_layout, channels_);

    // 倍速：输出采样率 = 输入采样率 / speed——把"多久播完"的调节压在
    // 重采样比例上，设备端的 asbd.mSampleRate（sample_rate_）本身不变。
    // 四舍五入而不是截断，避免长时间播放下的比例误差系统性偏向一边。
    const int64_t out_rate_64 =
        static_cast<int64_t>(static_cast<double>(in_rate) / speed_ + 0.5);
    const int out_rate = static_cast<int>(out_rate_64);

    SwrContext* ctx = nullptr;
    const int rc = swr_alloc_set_opts2(&ctx, &out_layout, kDeviceSampleFmt, out_rate, &in_layout,
                                        static_cast<AVSampleFormat>(in_fmt), in_rate, 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    // swr_alloc_set_opts2 出错时自己把 *ctx 置回 NULL（libswresample 文档
    // 明写），不需要这里再 swr_free 一次。
    if (rc < 0 || ctx == nullptr || swr_init(ctx) < 0) {
        if (ctx != nullptr) swr_free(&ctx);
        failed_ = true;
        return;
    }

    swr_               = ctx;
    swr_in_rate_        = in_rate;
    swr_in_channels_    = in_ch;
    swr_in_sample_fmt_  = in_fmt;
    swr_speed_          = speed_;
}

bool AudioUnitSink::write(const syp::media::Frame& f) {
    if (!opened_ || failed_) return false;
    if (!f.valid() || f.nb_samples() <= 0) return true;   // 没有样本可写：视为"已处理、无需重试"

    ensure_swr_for(f);
    if (failed_) return false;

    const int in_samples = f.nb_samples();

    // 先问 swr 这次转换"最多"会产出多少样本，据此预留环空间——不能先
    // swr_convert() 再看写不写得进去：swr_convert 一旦执行就已经真正
    // 消费了输入样本（可能进了它内部的重采样 FIFO），若这时才发现环装
    // 不下而回退，源 Frame 会被 TrackPlayer 重新递给下一次 write()，
    // 但 swr 内部状态已经前进过一次——等价于把这段样本喂了两遍。所以
    // 背压检查必须在任何 swr_convert 调用之前完成。
    const int max_out_samples_i = swr_get_out_samples(swr_, in_samples);
    if (max_out_samples_i < 0) { failed_ = true; return false; }

    const int32_t max_out_samples = static_cast<int32_t>(max_out_samples_i);
    const int32_t max_out_bytes   = max_out_samples * bytes_per_frame_;
    // max_out_samples 可能是 0（极端比例/极小输入下
    // swr 判定这次不会有输出）——这种情况下没有字节要写进环，背压检查
    // 天然通过（0 <= writable() 恒成立），但**不能因此跳过下面的
    // swr_convert() 调用**：跳过意味着这些输入样本从来没有进入 swr 的
    // 内部状态就被 write() 声称"已消费"（返回 true），是一次静默丢帧。
    // 正确做法是仍然调 swr_convert()（out_count 就是 0，libswresample
    // 文档："samples may get buffered in swr if you provide insufficient
    // output space"——0 是"不够"的极端情形，行为一致：喂给它、不强求
    // 立刻有输出），产出 0 字节的结果由下面 `converted == 0` 分支处理。
    if (max_out_bytes > 0 && ring_->writable() < max_out_bytes) {
        return false;   // 背压：帧未被消费，调用方保留所有权
    }

    if (static_cast<int32_t>(scratch_.size()) < max_out_bytes) {
        scratch_.resize(static_cast<size_t>(std::max(max_out_bytes, 1)));
    }
    uint8_t* out_planes[1] = {scratch_.data()};

    // 输入声道数据：packed（非 planar）格式只需要 plane(0)；planar 格式
    // 每个声道各一个 plane。Frame::plane(i) 直接映射 AVFrame::data[i]，
    // 上限 AV_NUM_DATA_POINTERS（8）——超过 8 声道的 planar 音频（现实
    // 里几乎不存在）会丢后面的声道，接受这个简化，不为它专门接
    // extended_data。
    const bool in_planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(f.sample_fmt()));
    const int32_t n_in_planes = in_planar ? std::min<int32_t>(f.channels(), 8) : 1;
    const uint8_t* in_planes[8] = {};
    for (int32_t i = 0; i < n_in_planes; ++i) in_planes[i] = f.plane(i);

    const int converted = swr_convert(swr_, out_planes, max_out_samples, in_planes, in_samples);
    if (converted < 0) { failed_ = true; return false; }
    if (converted == 0) return true;   // 合法：这一帧被 swr 吸收进内部延迟缓冲，暂时没有输出（含 max_out_samples==0 这一种情形）

    const int32_t out_bytes = converted * bytes_per_frame_;
    const bool ok = commit_frame(*ring_, scratch_.data(), out_bytes);
    if (ok) armed_.store(true, std::memory_order_relaxed);   // 环里有过数据，此后拿不满才算欠载
    return ok;
}

// ---------------------------------------------------------------------
// played_us() / pause() / resume() / flush() / set_speed() / failed()
// ---------------------------------------------------------------------

int64_t AudioUnitSink::played_us() const noexcept {
    // 契约（见 src/media/audio_sink.h）：只在
    // failed() 时返回 AV_NOPTS_VALUE；"尚未 open"返回上一次 flush() 的
    // 基准 base_us_（从未 flush 则为 0），不是 AV_NOPTS_VALUE。
    //
    // 更正：这不是"当前 track_player.cpp 里存在一个 clock_kind_
    // 已经定为 Audio、但 sink 还没真正 open 成功的窗口"——它不存在，
    // track_player.cpp::create() 的判据就是 `audio_ready = (oerr ==
    // SYP_OK)`，`AudioClock` 只会建在已经 open 成功的 sink 上。这里选
    // 返回 base_us_ 而不是 AV_NOPTS_VALUE，是不依赖这条实现细节的防御性
    // 设计：即便以后调用方的判定方式变了（比如改成先定 Audio、再惰性
    // open），错误地在 sink 未 open 时把它当 Audio 主时钟读，也不会撞上
    // `pts - clock_->now_us()` 对 INT64_MIN 做减法的有符号整数溢出 UB。
    if (failed_) return AV_NOPTS_VALUE;
    if (!opened_ || ring_ == nullptr || state_ == nullptr) return base_us_;
    // state_（LatencyState 的 shared_ptr）本身只在 open()/析构这些调用方
    // 线程上的位置被赋值，跟这里的读取属于同一条调用方线程、天然不并发
    // ——需要原子性的只是它指向的对象里那个 device_latency_us，路由变化
    // block 可能在另一条线程上写它，这里 relaxed load 无锁读取。
    //
    // 目标延迟（路由变化 block 可能刚改过）→ 按单调时钟推进已生效延迟。暂停中不推进
    // （设备不消费，推进只会让读数回退）；首次读数直接取目标，不从 0 爬坡。
    const int64_t target = state_->device_latency_us.load(std::memory_order_relaxed);
    const auto    now    = std::chrono::steady_clock::now();
    if (!latency_applied_valid_) {
        applied_latency_us_    = target;
        latency_applied_valid_ = true;
        latency_updated_at_    = now;
    } else if (!paused_) {
        // 暂停期间本函数可能根本不被调用（TrackPlayer
        // 暂停分支不读时钟）——不能靠"每次调用都刷新 latency_updated_at_"来把
        // 暂停时长排除在 elapsed 之外，那个假设不成立。改为暂停分支完全不碰
        // latency_updated_at_，真正负责在恢复时把它对齐到"现在"的是 resume()。
        const int64_t elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(now - latency_updated_at_).count();
        const int64_t before = applied_latency_us_;
        applied_latency_us_ = slew_latency_us(before, target, elapsed);
        if (applied_latency_us_ == target) {
            // 已追上目标：不留下"未消费的富余时间"——下次调用 slew_latency_us
            // 会在 applied_us == target_us 那条提前返回分支直接短路，elapsed
            // 是否精确不再重要，直接对齐到此刻最简单。
            latency_updated_at_ = now;
        } else {
            // 只把这次真正被斜坡吃掉的那一截 elapsed 计入
            // "已消费"，未被吃掉的余量留到下次跟新的间隔一起累积。
            // kLatencySlewRate=0.1 ⇒ 每移动 1us 对应吃掉 1/kLatencySlewRate=10us
            // elapsed；否则高频轮询下单次 elapsed 不够 10us、max_step 截断成 0，
            // 而这里如果无条件把 latency_updated_at_ 推到 now，之后每次 elapsed
            // 又被清零重算，永远凑不够 10us，斜坡彻底停滞。
            const int64_t moved = applied_latency_us_ > before ? applied_latency_us_ - before
                                                                : before - applied_latency_us_;
            latency_updated_at_ += std::chrono::microseconds(
                static_cast<int64_t>(static_cast<double>(moved) / kLatencySlewRate));
        }
    }
    int64_t v = compute_played_us(ring_->consumed_bytes(), base_consumed_bytes_, applied_latency_us_,
                                  sample_rate_, bytes_per_frame_, speed_, base_us_);
    if (v < last_played_us_) v = last_played_us_;   // 单调护栏：环空转时斜坡推进不回退读数
    last_played_us_ = v;
    return v;
}

void AudioUnitSink::pause() {
    paused_ = true;
    if (audio_unit_ != nullptr) AudioOutputUnitStop(as_audio_unit(audio_unit_));
}

void AudioUnitSink::resume() {
    paused_ = false;
    // 暂停期间 played_us() 的暂停分支完全不碰
    // latency_updated_at_（见该函数注释），它可能已经停留在"暂停之前最后一次
    // 调用"的那一刻，一停就是整个暂停时长。这里在调用方线程（跟 played_us()
    // 同一线程）把它对齐到"现在"，让恢复后第一次 played_us() 算出的 elapsed
    // 是"恢复以来经过的时间"，不是"整个暂停时长"——否则会在恢复瞬间把斜坡
    // 一次性追出一大截（向前跳变）甚至因为超过 target 差值而直接钳死在单调
    // 护栏上。
    latency_updated_at_ = std::chrono::steady_clock::now();
    if (audio_unit_ != nullptr && opened_ && !failed_) {
        AudioOutputUnitStart(as_audio_unit(audio_unit_));
    }
}

void AudioUnitSink::flush(int64_t base_us) {
    // AudioRing::reset() 的前置条件是"消费者已停止"（audio_ring.h）——
    // flush() 在正常播放中途被调用（seek/变速），这时硬件回调可能正在
    // 跑，所以必须先真正 Stop，reset 完再按原先的暂停状态决定要不要
    // Start 回去。AudioOutputUnitStop 是同步调用，返回时保证 render_cb
    // 不会再被并发调用，这之后碰 ring_ 才安全——跟析构同一条纪律。
    if (audio_unit_ != nullptr) AudioOutputUnitStop(as_audio_unit(audio_unit_));

    if (ring_ != nullptr) {
        ring_->reset();
        // 快照：AudioRing::consumed_bytes() 单调递增、reset() 不清它，
        // played_us() 必须用"当前值 − 这个快照"才能算出"自本次 flush
        // 以来"的净消费量。
        base_consumed_bytes_ = ring_->consumed_bytes();
    }
    underrun_.store(false, std::memory_order_relaxed);
    armed_.store(false, std::memory_order_relaxed);   // 此刻 unit 已停，渲染线程不在跑
    // render_ctx_.in_underrun（渲染线程私有的"当前是否
    // 处于一段欠载中"标志）本该在这里一起清零——不清的后果：AudioOutputUnitStart
    // 是异步的，seek 之后新一段真实的欠载如果恰好发生在"渲染线程还没重新跑起来"
    // 的窗口，in_underrun 会因为残留 true 而把这一段误判成"同一段的延续"，
    // 漏计一次。但 in_underrun 本身绝不能被这里（调用方线程）直接写——即便
    // AudioOutputUnitStop() 已经同步返回，那只保证"以后不会再有新的回调"，
    // CoreAudio 内部这条停止时序不提供 TSan 能识别的 happens-before 边，直接
    // 跨线程写会被判成数据竞争（已用 TSan 实测复现）。改为只 store 一个原子
    // "请求位"，真正的清零仍由渲染线程自己在 render_cb 开头完成，见
    // RenderCtx::underrun_edge_reset 与 render_cb 顶部注释。
    underrun_edge_reset_.store(true, std::memory_order_relaxed);
    base_us_ = base_us;
    last_played_us_ = base_us;

    if (audio_unit_ != nullptr && opened_ && !failed_ && !paused_) {
        AudioOutputUnitStart(as_audio_unit(audio_unit_));
    }
}

void AudioUnitSink::set_speed(double speed) {
    // 只记录：真正的重采样器重建推迟到下一次 write()（ensure_swr_for()
    // 比对 swr_speed_ 跟当前 speed_ 是否一致）。IAudioSink::set_speed()
    // 的接口注释里那条"两处 × speed 不是重复计入"的约定在这里同样成立：
    // 这里改的是喂给设备多少样本对应多少媒体时长（重采样比例），
    // played_us() 里另外乘的那个 × speed 是把"设备已经消费了多少真实
    // 时间"换算回"对应多少媒体时间"。
    //
    // 为什么不在这里同步重建：set_speed() 可能在第一帧 Frame 到达之前
    // 就被调用（此时 swr_ 还不知道真实输入格式，没法重建）；统一走
    // ensure_swr_for() 的"跟缓存参数比对、任一项不同就重建"逻辑，既覆盖
    // "已经在播、speed 变了"，也覆盖"speed 先于第一帧设置"这个边界，
    // 不需要在这里和 write() 里各写一遍重建逻辑。
    speed_ = speed;
}

void AudioUnitSink::set_gain(double gain) noexcept {
    // 【不是暂停】见 audio_sink.h IAudioSink::set_gain() 的接口注释——这里
    // 只更新斜坡的目标值，时钟（played_us()/consumed_bytes）完全不受影响，
    // render_cb 该读多少字节还读多少字节，只是读出来的样本被乘一个 <1 的数。
    //
    // 非有限值的口径是 NaN → 0、+inf → 1、-inf → 0
    // （与 SYPlayer.volume 同源但各写一份、不互相
    // 引用）——不是"非有限值一律夹到 0"。只需要显式处理 NaN：
    // std::isnan(g) 参与的任何比较恒为 false，不特判的话 NaN 会绕过下面
    // 两条钳位比较、直接把 NaN 存进 gain_target_。±inf 不需要单独分支：
    // IEEE 754 下 -inf < 0.0 与 +inf > 1.0 都成立，会被下面两条既有的
    // 钳位语句自然夹到 0.0/1.0，跟">1.0 的普通有限值钳到 1.0"是完全
    // 同一条代码路径，不是巧合。
    double g = gain;
    if (std::isnan(g)) g = 0.0;
    if (g < 0.0) g = 0.0;
    if (g > 1.0) g = 1.0;
    gain_target_.store(static_cast<float>(g), std::memory_order_relaxed);
}

bool AudioUnitSink::failed() const noexcept { return failed_; }

// IAudioSink::output_drained() 契约：未 open / 已 failed → true；否则环里
// 已无可读字节。
//
// 线程安全：本函数在生产者（TrackPlayer 泵线程）侧调用，与 render_cb（消费者）
// 并发。AudioRing::readable() 对两个原子量各 load 一次（acquire）：write_pos_
// 只有生产者自己写，此刻不会变；read_pos_ 只会被消费者单调推进。于是读到的
// 值只可能"偏大"（消费者在两次 load 之间又读走了一些），不会偏小——
// 判定为 drained 时一定真的排空了，最坏是晚一次调用才看见 0。无锁、无 UB。
//
// 不计 swr_ 内部滞留的滤波延迟样本（几个毫秒量级，write() 从不 flush 它们
// 进环）：那部分样本本就永远不会被播出，不能让它们挡住"已播完"。
bool AudioUnitSink::output_drained() const noexcept {
    if (failed_ || !opened_ || ring_ == nullptr) return true;
    return ring_->readable() == 0;
}

bool AudioUnitSink::consume_underrun() noexcept {
    // test-and-clear：exchange(false) 原子地读出当前值并清零，不用
    // "load 再 store" 两步（避免跟 render_cb 的 store(true) 交错导致丢
    // 一次真实的欠载通知）。underrun_ 原来
    // 只有写者、没有读者，一次真实欠载对系统完全不可见。
    return underrun_.exchange(false, std::memory_order_relaxed);
}

int64_t AudioUnitSink::underrun_count() const noexcept {
    return underrun_count_.load(std::memory_order_relaxed);
}

}  // namespace syp::platform
