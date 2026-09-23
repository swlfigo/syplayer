// audio_unit_sink.h — IAudioSink 的 AudioUnit 实现（macOS: HALOutput /
// iOS: RemoteIO）。第一个真正碰硬件的组件。
//
// 本头本身不 #include 任何 Apple 框架头（AudioToolbox/CoreAudio 都只在
// audio_unit_sink.mm 里出现）：AudioUnit（= AudioComponentInstance）以
// void* 存放，SwrContext 前置声明。这不是纪律洁癖——它让两段真正值得
// 独立单测的纯算术（commit_frame() / compute_played_us()，见下）可以被
// 一个普通 .cpp 测试文件直接 #include 本头调用，不需要该 TU 本身编成
// Objective-C++、也不需要链 AudioToolbox/CoreAudio。
//
// —— 结构约束（硬性）——
// render callback 的 inRefCon 只指向 RenderCtx：一个环 + 若干原子量指针 +
// 若干渲染线程私有字段（加欠载段计数/武装标志；加增益目标
// 原子量指针 + 渲染线程私有的当前增益/声道数/采样率，见 RenderCtx 定义）。
// 回调拿不到 AudioUnitSink 本身，更拿不到 TrackPlayer / Pipeline——这不是
// "别在回调里加东西"这种靠记性的提醒，是结构上直接做不到。回调实现在
// audio_unit_sink.mm，逐行理由见该文件。
//
// —— 两处架构落差，本类必须自己处理——
// 1. flush() 快照 AudioRing::consumed_bytes()，played_us() 用"当前值 −
//    快照"算净消费量——AudioRing::consumed_bytes() 单调递增、reset() 不
//    清它，不能直接拿当前值当"自上次 flush 以来"的消费量用。
// 2. write() 把 AudioRing::write()"按字节、允许部分写入"的语义折算成
//    IAudioSink::write() 要求的"整帧成败"语义：环里空间不够整帧就整帧
//    不写，不接受"写了一半"。
// 这两段折算都不碰 AudioComponentInstance，所以拆成 commit_frame() /
// compute_played_us() 两个 static 方法，可以在没有真实设备的环境下直接
// 单测（tests/test_audio_unit_sink.cpp）。真正碰硬件的路径（open() 里
// 的 AudioComponentInstanceNew/Initialize/Start，以及 render callback）
// 结构性地没有自动化覆盖。
#pragma once

#include "media/audio_ring.h"
#include "media/audio_sink.h"

#include <syplayer/syp_types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

struct SwrContext;   // libswresample；只有 .mm 真正用到，这里只需要指针本身

namespace syp::platform {

class AudioUnitSink final : public syp::media::IAudioSink {
public:
    // ring_ms：环形缓冲容量按时长配置，默认 200ms——要够大以吸收泵线程的
    // 调度抖动。测试/调用方需要更大缓冲吸收更大抖动时可以显式传更大的值。
    //
    // 【历史记录】首版实现把这个默认值硬编码成
    // "约 1 秒"，不是真机测出 200ms 不够撑住——是把一句粗略估计
    // "AudioUnitSink 典型环深约 1 秒"当成了实现参数直接抄了过来，写实现时
    // 没有回头核对正式数字，是失误，不是权衡后的工程判断。后来把默认值
    // 改回 200ms，如实记录这一条，不假装当初有个站得住的理由。
    explicit AudioUnitSink(int32_t ring_ms = 200);
    ~AudioUnitSink() override;

    AudioUnitSink(const AudioUnitSink&)            = delete;
    AudioUnitSink& operator=(const AudioUnitSink&) = delete;

    syp_status open(int32_t sample_rate, int32_t channels, int32_t sample_fmt) override;
    bool       write(const syp::media::Frame& f) override;
    int64_t    played_us() const noexcept override;
    void       pause() override;
    void       resume() override;
    void       flush(int64_t base_us) override;
    void       set_speed(double speed) override;
    // 把入参夹到 [0,1]（含 NaN → 0）后存进 gain_target_（原子、
    // relaxed）——render_cb 每次回调都读它，按 15ms 斜坡向它靠拢，不是瞬跳。
    void       set_gain(double gain) noexcept override;

    // 【测试专用】读回 set_gain() 夹取后的目标值。跟
    // consume_underrun()/underrun_count() 同一类"给调用方线程读内部状态"
    // 的只读接口：gain_target_ 本来就是"调用方线程写、渲染线程读"的原子
    // 量，这里只是多一个"调用方线程也能读"的口子，不引入新的并发面，
    // 也不碰渲染线程私有的 gain_current。存在的必要性是实测出来的，不是
    // 顺手加的：set_gain() 的夹取逻辑（> 1.0 钳到 1.0、负数/NaN 钳到 0）
    // 在真实设备上没有任何读回手段能观测输出样本，去掉这个口子的话，
    // "去掉 > 1.0 夹取"这条变异会无声存活——已用 mutation testing 实测
    // 确认过。
    float gain_target_for_test() const noexcept {
        return gain_target_.load(std::memory_order_relaxed);
    }
    // 【测试专用】最近一次 open() 里渲染线程的增益起点
    // （render_ctx_.gain_current 的快照值，"open() 时 gain_current =
    // gain_target，首次打开不淡入"）。open() 在构造 render_ctx_ 之后、
    // AudioOutputUnitStart() 之前把它记进 gain_at_open_（普通 float，调用方
    // 线程写、调用方线程读；桥经 -sinkGainAtOpenForTest 在 mu_ 下读）。
    // 最近一次 open() 没走到那一步（从未 open、或参数校验阶段就失败）返回 NaN。
    // 存在的必要性：桥在 create() 之后才下发持久静音，open()
    // 快照到 1.0，开头约 15ms 近满音量；gain_target_for_test() 只看得到最终
    // 目标 0，看不出起点是 1.0。
    float gain_at_open_for_test() const noexcept { return gain_at_open_; }
    bool       failed() const noexcept override;
    // ring_ 为空（未 open）或环里没有可读字节。线程安全性见 .mm 实现处注释。
    bool       output_drained() const noexcept override;

    // 实时线程唯一被允许发出的信号——test-and-clear：返回自上次调用以来
    // 是否发生过欠载，并把标志清零。underrun_ 原来只有写者（render_cb）、没有任何读者（flush()/open()
    // 只是清零，不算"消费"），一次真实的欠载（可听的爆音/断音）对系统
    // 完全不可见。这是唯一暴露它的口子——调用方（demo 壳的轮询/日志线程）
    // 决定多久 poll 一次，本类不替它做决定。
    bool consume_underrun() noexcept;

    // 自 open() 以来的欠载段数。一段 = 环里写入过样本（armed）之后，渲染回调从
    // "拿得满"变成"拿不满"；持续断粮不重复计。flush()/open() 解除武装（环刚被清空，
    // 接下来拿不满是预期的）。播放自然结束后环放空也算一段。consume_underrun() 保留。
    int64_t underrun_count() const noexcept override;

    // render_cb 的欠载段判定，纯函数。定义在头文件内联：实时线程上不引入函数调用。
    // starved：本次回调 got < want；armed：自上次 flush/open 以来是否写入过；
    // in_underrun：渲染线程私有的"当前是否处于一段欠载中"。返回 true 表示该计一次。
    static bool underrun_edge(bool starved, bool armed, bool* in_underrun) noexcept {
        const bool counted = starved && armed && !*in_underrun;
        *in_underrun       = starved && armed;
        return counted;
    }

    // 线性斜坡的一步：按实际输出的 frames 推进
    // current 向 target 逼近。frames <= 0 时返回 current（没有真实输出可供插值，
    // 0 帧就是"推进 0"——不能凭空跳到 target，那等价于完全欠载时斜坡自己走完，
    // 遵循"斜坡只按实际输出的帧数推进"这条规则）；sample_rate <= 0（但
    // frames > 0）时返回 target（真的有输出，但采样率非法算不出步长，没有
    // "半吊子过渡"可言）。每 1 帧最多移动 1/kGainRampMs 毫秒对应的步长，
    // 15ms @48kHz = 720 帧走完全程。
    static constexpr int32_t kGainRampMs = 15;
    static float advance_gain(float current, float target,
                              int32_t frames, int32_t sample_rate) noexcept;

    // 对一块已交织的 float32
    // 样本就地施加增益，并按本块帧数推进斜坡。返回本块结束时的增益（下一块的
    // 起点）。块内在 current → 返回值之间**按帧**线性插值（同一帧的所有声道乘
    // 同一个增益值），块间连续（本块最后一帧的增益恰好等于返回值）。
    // nfloats <= 0 或 channels <= 0 时不碰 samples，返回
    // advance_gain(current, target, 0, sample_rate)（即 current——frames <= 0
    // 不再跳到 target）。
    // nfloats 不是 channels 整数倍时，末尾凑不成一帧的零头样本按返回值 g1
    // 处理（不能维持原样，否则静音时这几个样本会以全音量漏出）。
    // render_cb 里 current == 1.0f && target == 1.0f 的零开销跳过留在调用方
    // （render_cb），不放在这里——见 audio_unit_sink.mm render_cb 里的调用点注释。
    static float apply_gain(float* samples, int32_t nfloats, int32_t channels,
                            float current, float target, int32_t sample_rate) noexcept;

    // 设备延迟变化（iOS/Catalyst 路由变化）按限速斜坡生效：已生效
    // 延迟每经过 1µs 最多向目标移动 kLatencySlewRate µs。斜率 < 1 ⇒ 设备按实时消费时
    // played_us() 不回跳不突跳；100ms/s 的变速人耳难察觉。只在读数侧计算。
    static constexpr double kLatencySlewRate = 0.1;
    static int64_t slew_latency_us(int64_t applied_us, int64_t target_us,
                                   int64_t elapsed_us) noexcept;

    // —— 可脱离硬件独立测试的两段算术——

    // write() 的"部分写入 → 整帧成败"折算：ring 里可写空间不够 bytes 就
    // 整帧不写（不接受部分写入），足够就整帧写入。
    //
    // 之所以敢在"检查 writable() 足够"和"真正 write()"之间不加锁：本类
    // 是唯一的生产者（TrackPlayer 单线程调用 write()），消费者（render
    // callback）只会让 writable() 变大、不会变小——检查完之后空间不会
    // 再变得不够。返回值仍然是 ring.write() 的真实结果而不是硬编码
    // true，万一这条假设被打破，这里如实报告失败而不是撒谎。
    static bool commit_frame(syp::media::AudioRing& ring, const uint8_t* data,
                              int32_t bytes) noexcept;

    // played_us() 的核心公式：快照相减 → 延迟扣除 →
    // 倍速换算。consumed_bytes / base_consumed_bytes 都是 AudioRing::
    // consumed_bytes() 的原始读数（单调递增、不因 flush 清零），"自上次
    // flush 以来的净消费量"由这个函数内部做减法算出，调用方不需要、也
    // 不应该自己先减一次。
    static int64_t compute_played_us(int64_t consumed_bytes, int64_t base_consumed_bytes,
                                      int64_t device_latency_us, int32_t sample_rate,
                                      int32_t bytes_per_frame, double speed,
                                      int64_t base_us) noexcept;

    // query_and_log_device_latency() 的合成公式：五个分量（AU 自身延迟、macOS HAL 的 safety offset / buffer
    // frame 时长、iOS/Catalyst 的 AVAudioSession.outputLatency /
    // .IOBufferDuration）里恒有两个是 0（macOS 分支不读 session 两个量，
    // iOS/Catalyst 分支不读 HAL 两个量），负值一律按 0 处理（防御性——
    // 这些量理论上不该是负的，但来源是系统 API 返回值，不假设它们守规矩），
    // 饱和相加避免中间和溢出，最终钳位到全局约束 [0, 500000]。
    static int64_t compose_device_latency_us(int64_t au_latency_us, int64_t safety_us,
                                              int64_t buffer_us, int64_t session_output_us,
                                              int64_t session_io_buffer_us) noexcept;

    // AVAudioSession.outputLatency/
    // .IOBufferDuration、AudioUnit 的 kAudioUnitProperty_Latency 都是秒的
    // double，来自系统 API 返回值——不假设它们守规矩。直接
    // static_cast<int64_t>(seconds * 1e6) 在 seconds 是 NaN/±inf/或换算后
    // 超出 int64_t 表示范围时是未定义行为（[conv.fpint] 对超范围/非有限值
    // 的浮点转整数不做定义）。这个函数把任意 double 安全钳到 [0, 500000]
    // us，不触发那条 UB：非正数（含 NaN——`!(s > 0)` 对 NaN 恒真，因为
    // NaN 参与的比较恒为 false）视为 0；>= 0.5s（含 +inf）钳到上限
    // 500000us；否则在已知安全范围内正常换算。
    static int64_t seconds_to_us_clamped(double seconds) noexcept;

    // 回调只碰这个：一个环 + 三个原子量指针 + 一个渲染线程私有标志。硬性
    // 要求，见文件头注释。特意声明成 public 嵌套类型（而不是塞进 private）：render
    // callback 是 audio_unit_sink.mm 里的一个自由函数（AURenderCallback
    // 是 C 函数指针类型，不能是成员函数），它需要能命名这个类型——但它
    // 依然拿不到 AudioUnitSink 本身，这条可见性放宽不改变"回调只碰环和
    // 原子量"这条结构事实，只是让类型名字对外可见。
    struct RenderCtx {
        syp::media::AudioRing* ring            = nullptr;
        std::atomic<bool>*     underrun        = nullptr;
        std::atomic<int64_t>*  underrun_count  = nullptr;   // 欠载段数
        std::atomic<bool>*     armed           = nullptr;   // 自上次 flush/open 以来写入过
        // flush() 需要在下一段播放前把 in_underrun 清零，
        // 但 in_underrun 是渲染线程私有的、绝不能被调用方线程直接写（跨线程无
        // 同步访问，TSan 会报数据竞争——AudioOutputUnitStop() 同步停止硬件回调
        // 这件事本身不提供 TSan 能识别的 happens-before 边）。flush() 只 store
        // 这个原子"请求位"，render_cb 开头 exchange 消费掉它、由渲染线程自己
        // 完成真正的清零，见 audio_unit_sink.mm render_cb 顶部注释。
        std::atomic<bool>*     underrun_edge_reset = nullptr;
        int32_t                bytes_per_frame = 0;
        bool                   in_underrun     = false;     // 渲染线程私有，见 underrun_edge()
        // gain_target：调用方线程（set_gain()）写、渲染线程读的原子量，
        // 跟 underrun/armed 等同一形状。gain_current 是渲染线程私有的斜坡起点，
        // 不需要原子——只有 render_cb 自己读写它，跟 in_underrun 同一理由。
        // channels/sample_rate 不能从 bytes_per_frame 反推：bytes_per_frame 把
        // "每样本字节数（恒为 float32=4）"和 channels 揉在一起，这里需要的是
        // channels 单独一个值（按帧插值）与 sample_rate（算斜坡步长），二者在
        // RenderCtx 里都还没有同义字段，不是重复。
        std::atomic<float>*    gain_target     = nullptr;
        float                  gain_current    = 1.0f;      // 渲染线程私有，见 advance_gain()/apply_gain()
        int32_t                channels        = 0;
        int32_t                sample_rate     = 0;
    };

private:
    // 停止真正的硬件回调、Uninitialize、Dispose——open() 里重复 open 时
    // 复用，析构里也复用。返回后 render_cb 保证不会再被调用，之后碰
    // ring_/swr_ 才安全（AudioRing::reset() 的前置条件，见 audio_ring.h）。
    void stop_and_dispose_unit() noexcept;

    // write() 内部：确认 swr_ 已经按 f 的输入格式 + 当前 speed_ 配置好，
    // 格式/速度任一项变化（含"第一次见到真实格式"）都会重建。sample_fmt
    // 的来源是 Frame，不是 open() 的参数——TrackPlayer::create() 那边
    // sample_fmt 恒为 AV_SAMPLE_FMT_NONE（见
    // track_player.cpp create() 注释），这是本类唯一能拿到真实格式的地方。
    // 失败置 failed_ = true。
    void ensure_swr_for(const syp::media::Frame& f);

    // 只在 open() 里调用一次（调用方线程，跟其它 open() 逻辑一样单线程、
    // 不并发）：读五个分量（AudioUnit 自身延迟；macOS 分支再读 HAL 的
    // safety offset + 缓冲帧数；iOS/Catalyst 分支再读 AVAudioSession 的
    // outputLatency + IOBufferDuration 的初始值），分别打日志——真机漂移
    // 读数不对时，第一件事就是看这几个数。
    // 建一个全新的 LatencyState（见下）并把这五个分量的快照写进去。
    //
    // 路由变化触发的后续重算**不**经过这个函数——它改为
    // 直接内联在 open() 里注册的观察者 block 中（只重算 session 两个
    // 分量 + 用 LatencyState 里缓存的另外三个分量重新 compose），原因见
    // open() 注册处的注释：block 不能碰 `this`/`audio_unit_`，而这个
    // 成员函数需要两者都碰。
    void query_and_log_device_latency();

    struct LatencyState;
    // iOS/Catalyst（定义在 .mm 的 `#if !TARGET_OS_OSX` 分支里；macOS 不定义也不调用）：
    // 重读 AVAudioSession.outputLatency/IOBufferDuration，配合 state 里缓存的另外三个
    // 分量重新 compose 并写回 state.device_latency_us。路由变化 block 与 open() 注册
    // 观察者之后的补读共用。静态、只碰 state，不碰 this。
    static void recompose_session_latency(LatencyState& state) noexcept;

    void*                                    audio_unit_ = nullptr;   // AudioComponentInstance
    // iOS/Catalyst：NSNotificationCenter 观察者 token（__bridge_retained）
    // ——AVAudioSessionRouteChangeNotification 的注册句柄，析构与
    // stop_and_dispose_unit()（重复 open() 时先收干净再重新注册，见
    // audio_unit_sink.mm open() 注释）里注销。
    // `removeObserver:` **不保证**等待一个已经在跑的 block 结束——注销
    // 只保证"以后不会再被调用"，不保证"这一刻没有正在执行的调用"。这个
    // 字段本身管的是"避免重复 open() 攒出第二个观察者"（否则以后每次
    // 路由变化会触发两次重算，纯浪费，不是正确性问题），不是并发安全的
    // 唯一防线——真正的并发安全靠 block 只捕获 LatencyState 的
    // shared_ptr、不碰 `this`/`audio_unit_`（见 open() 注册处注释与
    // LatencyState 定义）。macOS 编译目标上这个字段恒为 nullptr、且全部
    // 读写点都在 audio_unit_sink.mm 的 `#if !TARGET_OS_OSX` 分支里（macOS
    // 分支不注册观察者）——纯 macOS 构建下这个字段在整个 TU 里没有任何
    // 引用，会被 -Werror=unused-private-field 判为错误；[[maybe_unused]]
    // 只是如实告诉编译器"这是有意的、按平台选择性使用"，不改变字段本身
    // 的语义。
    [[maybe_unused]] void* route_observer_ = nullptr;
    std::unique_ptr<syp::media::AudioRing>   ring_;
    RenderCtx                                render_ctx_{};
    std::atomic<bool>                        underrun_{false};
    std::atomic<int64_t>                     underrun_count_{0};
    std::atomic<bool>                        armed_{false};
    // flush() 写、render_cb 读并 exchange 消费——见
    // RenderCtx::underrun_edge_reset 的注释。
    std::atomic<bool>                        underrun_edge_reset_{false};
    // set_gain() 写（调用方线程，relaxed）、render_cb 读（渲染
    // 线程，relaxed）。默认 1.0f：首次 open() 之前/未调用过 set_gain() 时
    // 不衰减——与 open() 构造 render_ctx_ 时"gain_current = gain_target_.
    // load()"（不淡入，见 open() 处注释）配合。
    std::atomic<float>                       gain_target_{1.0f};
    float                                    gain_at_open_ = std::numeric_limits<float>::quiet_NaN();   // 见 gain_at_open_for_test()

    SwrContext* swr_ = nullptr;
    std::vector<uint8_t> scratch_;   // write() 的重采样输出暂存区，生产者线程私有

    // swr_ 当前是按这组输入参数 + speed 建的；write() 里跟 Frame 的真实
    // 参数比对，任一项不同就重建（懒初始化 + 变速重建统一走这一条路径）。
    int32_t swr_in_rate_       = 0;
    int32_t swr_in_channels_   = 0;
    int32_t swr_in_sample_fmt_ = -1;
    double  swr_speed_         = 0.0;

    int32_t sample_rate_     = 0;   // 输出（设备）格式的采样率，open() 定死
    int32_t channels_        = 0;   // 输出（设备）格式的声道数，open() 定死
    int32_t bytes_per_frame_ = 0;   // 输出格式恒为交错 Float32：channels_ * 4

    // 装设备延迟的原子量原本
    // 直接是 AudioUnitSink 的成员，路由变化观察者 block 靠捕获 `this`
    // 去够到它——这不安全：block 在 AVAudioSession 的投递线程上
    // 跑，`removeObserver:` 不等待正在执行的 block，`this`/`audio_unit_`
    // 可能在 block 还在飞的时候被 stop_and_dispose_unit()/析构/重新
    // open() 动过，也可能在新 open() 已经写了新读数之后，旧 block 才
    // 姗姗来迟地拿旧读数把它覆盖回去。
    //
    // 现在的做法：真正跨线程共享的状态只剩这一个原子量，单独拎出来放进
    // 一个用 shared_ptr 持有的小对象——block 只捕获这个 shared_ptr（拷贝，
    // 不是 this），生命周期靠引用计数自己管，不依赖"谁先注销/谁先析构"
    // 这类时序假设：
    //   - 重新 open()：query_and_log_device_latency() 建一个全新的
    //     LatencyState 换掉 state_——旧 block（如果还在飞）手里攥着的是
    //     旧 shared_ptr 副本，只会写进那个旧对象，不会碰新对象，也不会
    //     碰已经不存在的 this。
    //   - 析构：析构函数不显式重置 state_，它作为成员随 AudioUnitSink 一起
    //     销毁（引用计数减一）；旧 block 的 shared_ptr 副本独立保活
    //     LatencyState，写完即弃，不是 UAF。
    //   - played_us() 读的是 state_（当前这一份）里的原子量，无锁。
    // state_ 这个指针本身（不是它指向的对象）只在 open()（query_and_log_
    // device_latency()）这个"调用方线程"上的位置被赋值，从不被 render_cb/
    // 通知线程碰——这跟
    // sample_rate_/channels_ 等其它"open() 定死、只读"字段是同一条既有
    // 纪律，所以 state_ 指针本身不需要是原子量（不是 std::atomic
    // <shared_ptr<...>>）：唯一真正需要原子性的是指针指向的对象内部那个
    // int64_t，用一个普通 std::atomic<int64_t> 就够、天然无锁，不需要
    // atomic<shared_ptr> 那种在部分实现下退化成内部加锁的重量级方案。
    struct LatencyState {
        // 三个"打开时定死、路由变化不会改变"的分量，构造时写一次、此后
        // 只读（AudioUnit 自身延迟；macOS 的 HAL safety offset / 缓冲
        // 帧数；iOS/Catalyst 上这两个恒为 0）。block 只读它们，不需要
        // 原子——它们在 block 有机会被调用之前（观察者注册之前）就已经
        // 写完，且此后再也不会被写。
        int64_t au_latency_us = 0;
        int64_t safety_us     = 0;
        int64_t buffer_us     = 0;
        // 真正跨线程共享、可能被路由变化 block 反复覆写的合成总和。
        // relaxed 就够：这里没有其它内存访问需要靠这次写入去
        // happens-before（跟原来 device_latency_us_ 的理由一样）。
        std::atomic<int64_t> device_latency_us{0};
    };
    std::shared_ptr<LatencyState> state_;

    int64_t base_consumed_bytes_ = 0;   // flush() 时快照的 AudioRing::consumed_bytes()
    int64_t base_us_             = 0;

    // 延迟斜坡与单调护栏。只在 played_us()（调用方线程：TrackPlayer 泵线程，demo
    // 的快照也在同一把 mu_ 下）读写，因此 mutable 且不需要原子；路由变化 block 只写
    // LatencyState::device_latency_us（目标值）。
    mutable int64_t                               applied_latency_us_    = 0;
    mutable bool                                  latency_applied_valid_ = false;
    mutable std::chrono::steady_clock::time_point latency_updated_at_{};
    // 自上次 flush/open 以来 played_us() 的最大读数：暂停中/环空时设备不按实时消费，斜坡
    // 继续推进会让读数回退，护栏把它钉住。
    mutable int64_t                               last_played_us_        = 0;

    double  speed_  = 1.0;

    int32_t ring_ms_ = 200;   // 构造时定死，open() 用它算环容量

    bool paused_  = false;
    bool opened_  = false;
    bool failed_  = false;
};

}  // namespace syp::platform
