// SYPBridge.mm — SYPBridge.h 的实现。ObjC++，是本项目里唯一把 TrackPlayer /
// Pipeline / AudioUnitSink / MetalRenderer / syp_source 全部串在一起的
// 地方。**这是 SYPlayerKit 的框架内部实现，不是公开 API**——见 SYPBridge.h
// 顶部注释：公开 API 是 SYPlayerKit 的那层纯 Swift 接口，本文件与它的头文件
// 一起走 PrivateHeaders + module.private.modulemap，业务侧看不见。
//
// 泵线程在这一层：一条专用的串行
// dispatch_queue，循环调用 TrackPlayer::step()，按返回值决定是否让出
// CPU（Waiting/Blocked 时 usleep 一小段，其余立即继续下一轮）。
//
// 线程安全：TrackPlayer 是"内核零线程"的被动对象，没有
// 内部锁，契约是"调用方驱动、单线程语义"。这个壳有两类调用方——泵线程
// （反复调 step()）和 UI 线程（play/pause/setSpeed/seek/snapshot，用户
// 一点按钮就发生）——两者必须不能真的并发进入 TrackPlayer。选择用一把
// std::mutex 把所有触达 player_ 的路径都串起来，而不是把控制操作也塞进
// 同一条 dispatch_queue：TrackPlayer::step() 本身"无 sleep、无锁等待"
// （pipeline.h/track_player.h 反复强调的不变量），所以持锁时间恒短，
// mutex 不会造成可感知的 UI 卡顿；比起用 GCD 让控制操作在泵循环的自我
// 重新调度间隙里插队，mutex 的正确性更容易独立验证（不依赖"泵循环必须
// 每轮都把自己重新 dispatch_async 回队尾，不能写成真正的 while(true)"
// 这条容易被后人破坏的隐性约定）。
//
// MetalRenderer 构造时同步编译着色器（真机几十到几百毫秒，见
// metal_renderer.h 顶部注释）。选择：**放到 open 这
// 一步、在后台完成**，不在 App 启动时构造、也不在主线程构造——
// -openLocalFile:/-openURLString: 本来就要做 Pipeline::create_file()/
// create_avio()（探测流信息，文件 IO/网络 IO，本身就不是能在主线程做的
// 事），调用方（PlayerViewController）已经把这两个方法派到后台队列，
// MetalRenderer 的构造搭这班车，不需要额外派发，也不会让主线程感到任何
// 卡顿。选择的理由：比起"接受卡顿 + 加个 loading 提示"，这个壳本来就要
// 为文件/网络 IO 做后台派发，顺路把 GPU 初始化也挪过去成本几乎为零。
#import "SYPBridge.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>   // usleep

#include "dl/cache_store.h"   // normalize_cache_dir + open_count 检验缝的 make_key
#include "dl/preconnector.h"   // Preconnector::instance().inflight_for_test()（测试缝）
#include "dl/source_bridge.h"  // log_msg（prepare_cache_dir 的非致命诊断通道）
#include "media/avio_bridge.h"
#include "media/media_info_provider.h"
#include "media/hls/hls_options.h"
#include "media/hls/url_rewrite.h"
#include "media/pipeline.h"
#include "media/track_player.h"
#include "platform/apple/audio_unit_sink.h"
#include "platform/apple/metal_renderer.h"
#include "platform/apple/apple_http_backend.h"
#include "platform/apple/vt_decode_backend.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_net.h>
#include <syplayer/syp_preload.h>
#include <syplayer/syp_source.h>
#include <syplayer/syp_types.h>

extern "C" {
#include <libavutil/avutil.h>   // AV_NOPTS_VALUE
}

using syp::media::AvioBridge;
using syp::media::Frame;
using syp::media::IAudioSink;
using syp::media::IVideoRenderer;
using syp::media::Pipeline;
using syp::media::PipelineConfig;
using syp::media::TrackPlayer;
using syp::platform::AudioUnitSink;
using syp::platform::MetalRenderer;

namespace {

// SypClockSwitchReason 与 TrackPlayer::ClockSwitchReason 显式映射，不靠
// static_cast 隐式依赖枚举顺序；静态断言再钉一次数值一致（改动任一侧枚举会编译失败）。
static_assert(static_cast<NSInteger>(syp::media::ClockSwitchReason::None) == SypClockSwitchReasonNone);
static_assert(static_cast<NSInteger>(syp::media::ClockSwitchReason::AudioFailed) ==
              SypClockSwitchReasonAudioFailed);
static_assert(static_cast<NSInteger>(syp::media::ClockSwitchReason::AudioEnded) ==
              SypClockSwitchReasonAudioEnded);

SypClockSwitchReason to_objc_clock_switch_reason(syp::media::ClockSwitchReason r) noexcept {
    switch (r) {
        case syp::media::ClockSwitchReason::None:        return SypClockSwitchReasonNone;
        case syp::media::ClockSwitchReason::AudioFailed: return SypClockSwitchReasonAudioFailed;
        case syp::media::ClockSwitchReason::AudioEnded:  return SypClockSwitchReasonAudioEnded;
    }
    return SypClockSwitchReasonNone;
}

// SypBufferingReason 与 TrackPlayer::BufferingReason 显式映射，同上不靠
// static_cast 隐式依赖枚举顺序；静态断言再钉一次数值一致。
static_assert(static_cast<NSInteger>(syp::media::BufferingReason::None) == SypBufferingReasonNone);
static_assert(static_cast<NSInteger>(syp::media::BufferingReason::Startup) == SypBufferingReasonStartup);
static_assert(static_cast<NSInteger>(syp::media::BufferingReason::Seek) == SypBufferingReasonSeek);
static_assert(static_cast<NSInteger>(syp::media::BufferingReason::Stall) == SypBufferingReasonStall);

SypBufferingReason to_objc_buffering_reason(syp::media::BufferingReason r) noexcept {
    switch (r) {
        case syp::media::BufferingReason::None:    return SypBufferingReasonNone;
        case syp::media::BufferingReason::Startup: return SypBufferingReasonStartup;
        case syp::media::BufferingReason::Seek:    return SypBufferingReasonSeek;
        case syp::media::BufferingReason::Stall:   return SypBufferingReasonStall;
    }
    return SypBufferingReasonNone;
}

// SypVideoGravity 与 syp::media::Gravity 显式双向映射，同上不靠
// static_cast 隐式依赖枚举顺序；静态断言再钉一次数值一致。两个 switch 都**全枚举、
// 不写 default**：任一侧加 case，-Wswitch 在这里就报（demo 工程 -Wall 下是警告，
// 零新增告警是硬要求）。switch 之后的 return 只为让编译器相信函数一定有返回值
// ——越界整数塞进枚举这种 UB 输入落到默认值 AspectFit。
static_assert(static_cast<int32_t>(syp::media::Gravity::AspectFit) == SypVideoGravityAspectFit);
static_assert(static_cast<int32_t>(syp::media::Gravity::AspectFill) == SypVideoGravityAspectFill);
static_assert(static_cast<int32_t>(syp::media::Gravity::Resize) == SypVideoGravityResize);

syp::media::Gravity to_cpp_gravity(SypVideoGravity g) noexcept {
    switch (g) {
        case SypVideoGravityAspectFit:  return syp::media::Gravity::AspectFit;
        case SypVideoGravityAspectFill: return syp::media::Gravity::AspectFill;
        case SypVideoGravityResize:     return syp::media::Gravity::Resize;
    }
    return syp::media::Gravity::AspectFit;
}

SypVideoGravity to_objc_gravity(syp::media::Gravity g) noexcept {
    switch (g) {
        case syp::media::Gravity::AspectFit:  return SypVideoGravityAspectFit;
        case syp::media::Gravity::AspectFill: return SypVideoGravityAspectFill;
        case syp::media::Gravity::Resize:     return SypVideoGravityResize;
    }
    return SypVideoGravityAspectFit;
}

// SypStatusCode 与 include/syplayer/syp_types.h 的取值一一对齐。本文件
// 是全仓唯一同时看得见两侧的地方，断言放这里；任一侧改数值都会在这里编译失败。
// 两侧都显式 static_cast<NSInteger>：syp_status 是匿名 enum，直接跟 NS_ENUM 比
// 会触发 C++20 起弃用的"不同枚举类型比较"（-Wdeprecated-anon-enum-enum-conversion），
// 而这个仓库的 demo 工程开着 -Wall -Wextra、不允许新增警告。
static_assert(static_cast<NSInteger>(SypStatusCodeOK)             == static_cast<NSInteger>(SYP_OK));
static_assert(static_cast<NSInteger>(SypStatusCodeEof)              == static_cast<NSInteger>(SYP_ERR_EOF));
static_assert(static_cast<NSInteger>(SypStatusCodeInvalidArg)       == static_cast<NSInteger>(SYP_ERR_INVALID_ARG));
static_assert(static_cast<NSInteger>(SypStatusCodeCanceled)         == static_cast<NSInteger>(SYP_ERR_CANCELED));
static_assert(static_cast<NSInteger>(SypStatusCodeTimeout)          == static_cast<NSInteger>(SYP_ERR_TIMEOUT));
static_assert(static_cast<NSInteger>(SypStatusCodeOutOfMemory)      == static_cast<NSInteger>(SYP_ERR_OOM));
static_assert(static_cast<NSInteger>(SypStatusCodeBusy)             == static_cast<NSInteger>(SYP_ERR_BUSY));
static_assert(static_cast<NSInteger>(SypStatusCodeIO)               == static_cast<NSInteger>(SYP_ERR_IO));
static_assert(static_cast<NSInteger>(SypStatusCodeNoSpace)          == static_cast<NSInteger>(SYP_ERR_NO_SPACE));
static_assert(static_cast<NSInteger>(SypStatusCodeCacheCorrupt)     == static_cast<NSInteger>(SYP_ERR_CACHE_CORRUPT));
static_assert(static_cast<NSInteger>(SypStatusCodeNetwork)          == static_cast<NSInteger>(SYP_ERR_NETWORK));
static_assert(static_cast<NSInteger>(SypStatusCodeHttpStatus)       == static_cast<NSInteger>(SYP_ERR_HTTP_STATUS));
static_assert(static_cast<NSInteger>(SypStatusCodeTooManyRedirects) == static_cast<NSInteger>(SYP_ERR_TOO_MANY_REDIRECTS));
static_assert(static_cast<NSInteger>(SypStatusCodeRangeUnsupported) == static_cast<NSInteger>(SYP_ERR_RANGE_UNSUPPORTED));
static_assert(static_cast<NSInteger>(SypStatusCodeContentChanged)   == static_cast<NSInteger>(SYP_ERR_CONTENT_CHANGED));
static_assert(static_cast<NSInteger>(SypStatusCodeNotImplemented)   == static_cast<NSInteger>(SYP_ERR_NOT_IMPLEMENTED));

// PresentTrackingRenderer 装饰 MetalRenderer：present() 之外的一切原样转发；
// present() 成功时额外记下这一帧的 pts（漂移读数用）。
//
// 不做 GPU→CPU 回读：MetalRenderer 的生产上屏入口 set_output_layer()
// 把内部纹理直接经 blit pass 画到调用方给的 CAMetalLayer 的 drawable，
// GPU 到 GPU，零 CPU 拷贝，不需要 on_frame_ 回调、也不需要 std::vector
// 回读缓冲。
//
// 原名 `DemoVideoRenderer`，是这份代码还住在 `demo/shared/` 时留下的
// 名字；搬进 `SYPlayerKit` framework 之后，"Demo" 这个前缀就成了对读者
// 的误导（它是产品代码里唯一的视频渲染器装饰层，不是示例）。同一轮一起
// 改掉的还有泵队列标签与 HTTP 缓存目录名。
class PresentTrackingRenderer final : public IVideoRenderer {
public:
    PresentTrackingRenderer() { real_ = std::make_unique<MetalRenderer>(); }

    syp_status present(const Frame& f, int64_t due_in_us) override {
        const syp_status st = real_->present(f, due_in_us);
        if (st == SYP_OK) {
            last_presented_pts_us_.store(f.pts_us(), std::memory_order_relaxed);
            has_presented_.store(true, std::memory_order_relaxed);
        }
        return st;
    }

    int64_t last_presented_pts_us() const noexcept {
        return last_presented_pts_us_.load(std::memory_order_relaxed);
    }
    bool has_presented() const noexcept { return has_presented_.load(std::memory_order_relaxed); }
    bool device_available() const noexcept { return real_->device_available(); }

    // 转发给 MetalRenderer::set_output_layer()——见 -attachVideoLayer: 与
    // open_common()（PlayerCore::layer_ 非空时，建完 renderer 之后立即接上）。
    void set_output_layer(void* l) noexcept { real_->set_output_layer(l); }

    // 显示几何与填充方式**必须**转发。IVideoRenderer 给这两个方法
    // 的是默认空实现（不是纯虚，video_renderer.h），所以这个装饰层漏写 override
    // 不会有任何编译错误——TrackPlayer::create()/set_gravity() 下发的 SAR、旋转、
    // gravity 会被静默吞在这一层，MetalRenderer 永远按 1:1、不旋转、AspectFit 画。
    // 此前发生过这个状态（当时 TrackPlayer 还不下发，没人发现）。
    void set_source_geometry(int32_t sar_num, int32_t sar_den,
                             int32_t rotation_deg) noexcept override {
        real_->set_source_geometry(sar_num, sar_den, rotation_deg);
    }
    void set_gravity(syp::media::Gravity g) noexcept override {
        gravity_in_use_ = g;
        real_->set_gravity(g);
    }

    // 检验缝（-videoGravityInUseForTest 的数据源）：TrackPlayer 最近一次交给
    // 渲染器的 gravity。TrackPlayer 本身没有 gravity 的 getter（它的四组
    // 访问器里只有 set_gravity），而它是 player_ 往下唯一的 gravity 消费者——
    // 读这里比读 player_ 更深一层：证明的是"TrackPlayer 真的收到并转发了"。
    // 局限：这一行记在转发之前，删掉上面的 real_->set_gravity(g) 这条缝看不见
    // ——那一跳由下面的 debug_blit_to_bgra() 透传经像素回读守着。
    // 写（create()/TrackPlayer::set_gravity()）与读都在 PlayerCore::mu_ 下。
    syp::media::Gravity gravity_in_use() const noexcept { return gravity_in_use_; }

    // 测试专用：原样透传 MetalRenderer::debug_blit_to_bgra()
    // ——用 MetalRenderer **自己手里**的几何与 gravity、走与生产 present() 同一个
    // encode_blit() 把最近一帧画进离屏纹理并回读。上面两个 override 的"转发给 real_"
    // 只有从这里才看得见：gravity_in_use_ 记在转发之前，删掉 real_->set_gravity(g)
    // 或 real_->set_source_geometry(...) 它都照旧；像素不会。
    // 与 present() 必须串行（metal_renderer.h 的契约）：调用方持 PlayerCore::mu_，
    // 而泵线程的 step()（唯一会触发 present() 的路径）也在 mu_ 下，天然串行。
    bool debug_blit_to_bgra(int32_t dst_w, int32_t dst_h, std::vector<uint8_t>& out) const {
        return real_->debug_blit_to_bgra(dst_w, dst_h, out);
    }

private:
    std::unique_ptr<MetalRenderer> real_;
    syp::media::Gravity gravity_in_use_ = syp::media::Gravity::AspectFit;
    std::atomic<int64_t> last_presented_pts_us_{AV_NOPTS_VALUE};
    std::atomic<bool>    has_presented_{false};
};

// 惰性、进程内只做一次：把 NSURLSession 版 HTTP 后端接到 dl 层。
// syp_set_http_backend() 内部有没有做幂等去重不是本文件该假设的事，用
// std::once_flag 在调用方这一侧保证只调用一次。
void ensure_http_backend_registered() {
    static std::once_flag once;
    std::call_once(once, [] { syp_set_http_backend(syp_apple_http_backend()); });
}

// 目录准备 + syp_config 的容量字段填充，
// SypPlayerBridge 与 SypPreloaderBridge 共用这一组。
//
// 归一化走 syp::dl::normalize_cache_dir()——**全仓唯一一份**（它住在
// src/dl/cache_store.h，紧挨 CacheStore::make_key），PreloadStack::create 与
// SourceBridge / Preloader 的构造函数走的也是它。不要在这里抄第二份（见那个
// 函数上方的注释：目录一字之差就是两份缓存，且不报任何错）。
//
// 纯函数，**一个字节都不写盘**：本配置最终会交给 dl 层的那个目录（已归一化）。
// 拆出来是为了让 -cacheDirectoryInUse 这种 getter 真的是个 getter——
// 那个 getter 当时走的是 prepare_cache_dir，光读一下就在
// 开发机上建出了 ~/Library/Caches/syplayer-http-cache，而预加载侧的同名方法
// 无副作用，两者不对称。目录建在**配置那一刻**（-setCacheSettings:），
// 不建在读的时候。
std::string effective_cache_dir(SypCacheSettings* cache) {
    NSString* dir = cache.directory.length > 0 ? cache.directory
                                               : [SypCacheSettings defaultDirectory];
    return syp::dl::normalize_cache_dir(
        dir.UTF8String != nullptr ? std::string(dir.UTF8String) : std::string());
}

// 容量/TTL 三个字段（外加 syp_config_init + struct_size）。cache_dir 不碰。
void fill_cache_config(SypCacheSettings* cache, syp_config* cfg) {
    syp_config_init(cfg);
    cfg->struct_size          = sizeof(syp_config);
    cfg->max_cache_bytes      = cache.maxBytes;
    cfg->min_free_space_bytes = cache.minFreeSpaceBytes;
    cfg->cache_ttl_ms         = cache.ttlMs;
}

// 建目录 + 填容量 + 返回归一化后的目录。SypPlayerBridge 与 SypPreloaderBridge
// 共用这一份。
//
// **cache_dir 故意不在这里写进 *cfg**：那样就要让 cfg->cache_dir 指向本函数
// 的返回值，而返回值的生命周期归调用方——"靠 NRVO 让指针恰好仍然有效"是一条
// 不该写进产品代码的赌注（NRVO 不是强制的，而且一旦调用方把返回值挪进别的
// 变量，指针就悬了）。调用方拿到返回的 std::string、自己保证它活得够久之后
// 再写 cfg->cache_dir = s.c_str()，悬垂就变成了编译期看得见的局部问题。
//
// 【建的是归一化之后的路径，不是原串】R6 给归一化加了 lexically_normal 之后
// 这件事不再是形式问题：原串写 "/a/link/../c" 时，建 "/a/link/../c" 与建
// "/a/c" 在 link 是符号链接时是**两个目录**，而后面真正拿去打开索引文件的是
// 归一化后的那个。建谁用谁，不留这个缝。
// 【建目录这一步是"提前试一次"，失败**不会**被上报】
// **代码不会**在配置那一刻暴露"建不出来"这件事，而且做不到：error: 传的是
// nil，-setCacheSettings: 返回 void，整条链上没有任何一个出参能把它带回
// 调用方。实测：把词法父目录 chmod 0500 之后，目录建不出来，调用方拿到的
// 信号是**零**。
//
// 那为什么还要在这里建？两条真实收益，与"上报"无关：
//   1. 目录存在时后面的 CacheIndex::open / CacheFile::open 少一次失败往返；
//   2. 建目录这件事发生在**主线程的配置调用**上，而不是第一次 open 的 IO
//      路径上（open 排在 openQueue 上，那里建目录会把一次网络打开的延迟
//      与一次可能很慢的文件系统操作绑在一起）。
// 建不出来的真实后果：第一次 open 时 dl 层报 SYP_ERR_IO，走 on_error。
// 也就是说**它确实伪装成了一次"打开失败"**——这是已知代价，不是设计意图。
//
// 【失败该有一条可观测的记录，即使不改公开 API】
// error: 不能留成 nil：仓里早就有一条现成的非致命诊断通道：
// `syp::dl::log_msg`（source_bridge.h:214，cache_store.cpp 的 acquire()
// 自己就在用）。换成真的 NSError** + 失败时 log_msg：**零公开 API 变更、
// 零 source-breaking、不崩任何人**，把"零信号"变成"日志里有一条带 errno
// 的记录"。
//
// 它**不是**可编程检测（调用方仍然读不到一个错误码），那一半的局限
// 依然成立；这里只是不再把已经知道的事实丢掉。
std::string prepare_cache_dir(SypCacheSettings* cache, syp_config* cfg) {
    const std::string dir = effective_cache_dir(cache);
    NSError* err = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:@(dir.c_str())
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&err]) {
        // 目录已存在不算失败（withIntermediateDirectories:YES 本来就不报
        // EEXIST），所以走到这里的都是真问题：父目录只读、磁盘满、路径被
        // 一个同名普通文件占着。
        NSString* msg = [NSString stringWithFormat:@"缓存目录建不出来：%s —— %@（"
                                                   @"第一次打开会报 SYP_ERR_IO）",
                                                   dir.c_str(),
                                                   err.localizedDescription ?: @"未知原因"];
        syp::dl::log_msg(SYP_LOG_WARN, "cache",
                         msg.UTF8String != nullptr ? msg.UTF8String
                                                   : "缓存目录建不出来");
    }
    fill_cache_config(cache, cfg);
    return dir;
}

// syp_config 的缓存四元组 → 一个可以从 Swift 读的 SypCacheSettings 快照。
// 检验缝专用：断言的是**真的落进 syp_config 的那几个数**。
SypCacheSettings* cache_settings_from_config(const syp_config& cfg) {
    SypCacheSettings* s = [[SypCacheSettings alloc] init];
    s.directory         = cfg.cache_dir != nullptr ? @(cfg.cache_dir) : @"";
    s.maxBytes          = cfg.max_cache_bytes;
    s.minFreeSpaceBytes = cfg.min_free_space_bytes;
    s.ttlMs             = cfg.cache_ttl_ms;
    return s;
}

// -openURLString: **真的交给 dl 层**的那一份 syp_config。
//
// 【为什么提成一个函数 —— 缝之后还能再改一手配置】
// 就地拼 cfg 的写法里，拍完 _lastOpenCacheConfig 快照之后，cfg 还要再走
// 多行才到 Pipeline::create_hls / syp_source_open——中间那一段**对所有缝
// 都不可见**，插进去的改动不会被任何用例发现。SYPBridge.mm 不在 CMake
// 里，ctest 看不见它，xcodebuild 是唯一可能的闸门。后果比预加载侧那几处
// 都重：**播放**的 HTTP 缓存无上限、永不过期、不看可用空间，而这是每一次
// SYPlayer.open(.url(…)) 都要走的路。
//
// 修法与 MediaInfoProvider::probe_config() / HlsSession::dl_cfg_ 同形：
// **缝读的是交给消费者的那个东西**，不是上游拍的快照。塑造这份配置的每
// 一行都收进本函数，调用点拿到的是 `const syp_config`——想在缝之后再改
// 一手，得先写一个 const_cast，那不再是"手滑"级别的改动。
//
// cache_dir 是裸指针，指向 *dir_out；调用方必须让那个 string 活过所有
// 消费点（syp_source_open / create_hls 都在文档里写明"内容会被拷贝"，
// 所以活到调用返回就够）。
syp_config open_dl_config(SypCacheSettings* cache, std::string* dir_out) {
    syp_config c{};
    *dir_out    = prepare_cache_dir(cache, &c);
    c.cache_dir = dir_out->c_str();
    return c;
}

// hw=YES 时接上本平台的 VideoToolbox 硬解后端；hw=NO 时保持
// 默认（Software）。videotoolbox_decode_backend() 是进程内单例，
// hw_backend 非拥有——只要不选硬解就不碰它，Catalyst 上选了硬解会在
// Pipeline::create_*() 内部经 supports() 拒绝，返回 SYP_ERR_NOT_IMPLEMENTED
// （vt_decode_backend.h 顶部注释），不是这里判断的事。
PipelineConfig make_pipeline_config(BOOL hw) {
    PipelineConfig c{};
    // demux（含网络读）在 Pipeline 内部加载线程上做：泵线程持 mu_ 调 step() 不再
    // 被网络 IO 卡住，play/pause/seek/snapshot 不跟着卡。本地文件同样开启，统一路径。
    c.demux_thread = true;
    if (hw) {
        c.video_decode = syp::media::VideoDecodeMode::Hardware;
        c.hw_backend   = &syp::platform::videotoolbox_decode_backend();
    }
    return c;
}

// 【UAF 修复】PlayerCore::close_internal() 需要知道
// "当前是不是正跑在泵队列自己身上"，才能决定要不要 dispatch_sync 等它
// 退出——对自己所在的串行队列 dispatch_sync 会自死锁。用变量自己的地址
// 当 key，进程内天然唯一，不需要额外分配。见 close_internal() 的注释。
const void* const kPumpQueueSpecificKey = &kPumpQueueSpecificKey;

}  // namespace

// ---------------------------------------------------------------------
// PlayerCore：桥接层的 C++ 内核，SypPlayerBridge 只是它的一层 ObjC 皮。
// 拆开纯粹是为了让 std::mutex/std::unique_ptr 等 C++ 类型不用出现在
// SypPlayerBridge 的 ivar 声明里（尽管 .mm 文件本来就能混写，拆分只是
// 让读者更容易分清"状态与生命周期"和"ObjC 外观"两件事）。
struct PlayerCore {
    std::mutex mu_;   // 见文件顶部长注释：串起 player_ 的所有触达路径。

    // 【close 路径的中止口】同步模式（demux_thread=false）下泵线程
    // 可能正卡在持 mu_ 执行的那次 step() 里（av_read_frame 网络挂住），close 拿不到
    // mu_，所以打断它不能经 player_。demo 现在固定开线程模式（见
    // make_pipeline_config()）：网络读（含 HLS 的 io_open/分片读）挪到 Pipeline 内部的
    // 加载线程，step() 不再阻塞在 IO 上，持 mu_ 的时间恒短；但卡住的读仍然存在，只是
    // 换到了加载线程上——request_abort() 这时打断的是加载线程的读，让随后
    // ~Pipeline() join 加载线程不必等到网络层超时。中止口两种模式都需要，照旧保留。
    // abortable_ 是 player_ 的一份裸指针副本，由 abort_mu_ 单独保护：
    //   · 写（发布/撤销）与 player_ 的创建/销毁在同一临界区内完成，锁序
    //     固定 abort_mu_ → mu_；
    //   · 读（request_abort）只拿 abort_mu_——泵线程从不持 abort_mu_，所以
    //     close 在泵线程卡住时也拿得到。
    // TrackPlayer::request_abort() 明示可与 step() 并发调用。
    std::mutex   abort_mu_;
    TrackPlayer* abortable_ = nullptr;

    std::unique_ptr<TrackPlayer> player_;
    PresentTrackingRenderer*           renderer_ = nullptr;   // 非拥有；生命周期随 player_
    // 非拥有；生命周期随 player_（与 renderer_ 同形）。只给
    // -sinkGainAtOpenForTest 读"open 那一刻 sink 的增益起点"用，持 mu_ 读写。
    AudioUnitSink*                     sink_ = nullptr;
    int64_t                      duration_us_ = 0;

    // URL 播放专用：Pipeline::create_avio() 不接管 AVIOContext 的所有权
    // （pipeline.h 顶部注释），这两个必须活得比 player_（进而它内部的
    // Pipeline）更久，close 时也必须晚于 player_.reset() 才能释放。
    std::unique_ptr<AvioBridge> avio_;
    syp_source*                 src_ = nullptr;

    std::atomic<bool> running_{false};
    dispatch_queue_t  pump_queue_ = nullptr;

    // 泵线程侧的一次性通知，避免 Eof 每轮 step() 都回调一次。
    bool eof_notified_ = false;

    // -snapshot 钳位 positionUs 用的兜底源：最后一次读到的
    // **非哨兵**播放位置。跟 TrackPlayer::sanitize_position() 是同一个
    // 形状、同一个理由，只是守在桥这一层——SYPBridge.h 的契约说
    // positionUs 不是哨兵值，兑现它是这层自己的事，不能押在下游的内部
    // 不变量上。只在持 mu_ 的 -snapshot 里读写。
    int64_t last_good_position_us_ = 0;

    __weak SypPlayerBridge* owner_ = nil;   // 只用来转发 onEof/onError

    // 真上屏用的 CAMetalLayer，(__bridge) 指针。持有的强引用由
    // -attachVideoLayer: 的 CFBridgingRetain 负责；替换旧 layer 或本对象析构
    // 时 CFBridgingRelease 释放。open_common() 建完 renderer 之后，若已经
    // attach 过一个 layer，立即把它接到新 renderer 上——不需要调用方在每次
    // open 之后重新调一次 attachVideoLayer:。
    void* layer_ = nullptr;

    // 本次 open 是否在硬解模式下工作（Pipeline::video_hardware_decoding()，
    // 在 player_ 被 move 进 TrackPlayer 之前、pipeline 还是 unique_ptr<Pipeline>
    // 时读出——TrackPlayer 不转发这个查询）。只在持 mu_ 时读写。
    bool hardware_decoding_ = false;

    // 音量 / 静音 / 填充方式的**跨 open 持久值**。
    // TrackPlayer 只管"本实例内"，每次 open 都被新建（下面 open_common()），
    // 用户设过的值住在这里，每次 open 由 open_common() 作为 InitialSettings
    // 传进 TrackPlayer::create()（必须在 sink open 之前落下）。
    // 与其余字段一样只在 mu_ 下读写。volume_ 原样存调用方给的值（-setVolume:
    // 不夹取，理由见那里）；Swift 侧唯一的调用方已经夹过。
    double              volume_  = 1.0;
    bool                muted_   = false;
    syp::media::Gravity gravity_ = syp::media::Gravity::AspectFit;

    PlayerCore() {
        pump_queue_ = dispatch_queue_create("com.syplayer.kit.pump", DISPATCH_QUEUE_SERIAL);
        // 打一个可在运行期识别的标记，close_internal() 用它判断"现在是不
        // 是正跑在泵队列自己身上"。context 不需要析构（指向的是一个
        // 静态常量地址，不是堆分配），第 4 个参数传 nullptr。
        dispatch_queue_set_specific(pump_queue_, kPumpQueueSpecificKey,
                                     const_cast<void*>(kPumpQueueSpecificKey), nullptr);
    }

    ~PlayerCore() {
        close_internal();
        // 释放 -attachVideoLayer: 用 CFBridgingRetain 存下的那份
        // 强引用——layer_ 不随 close_internal() 一起清（它要跨 open/close 存
        // 活），只在本对象真正析构（等价于 SypPlayerBridge -dealloc）或
        // -attachVideoLayer: 换成另一个 layer 时释放，见该方法实现。
        if (layer_ != nullptr) {
            CFBridgingRelease(layer_);
            layer_ = nullptr;
        }
    }

    // 关掉泵线程、释放播放相关对象。可重入（open 前先调一次是安全的）。
    void close_internal() {
        running_.store(false, std::memory_order_relaxed);

        // 【用 ASan 复现的 use-after-free，修复】
        // 只置 running_=false 不够：泵线程完全可能正睡在 pump_loop() 的
        // usleep() 里（不持 mu_），这时如果调用方紧接着释放 PlayerCore
        // 本身（典型触发：SypPlayerBridge 最后一个强引用被释放，
        // .cxx_destruct 销毁 unique_ptr<PlayerCore> 这个 ivar），泵线程
        // 睡醒后会继续碰一块已经被 free 的内存——pump_loop() 里那个裸
        // 指针 `this` 指向的正是 PlayerCore 自己。旧版本这里只有一句
        // "不会在 PlayerCore 析构之后继续碰 this" 的注释，但没有代码兑现
        // 这句承诺，用一个"play() 后立即释放最后一个强引用、循环
        // 200 次"的驱动程序在 build-asan 下第一次迭代就复现了
        // heap-use-after-free（读地址落在 pump_loop() 的
        // `if (player_ == nullptr)`）。
        //
        // 修法：dispatch_sync 一个空 block 到 pump_queue_——pump_queue_
        // 是严格 FIFO 的串行队列，start_pump() 里那个跑 pump_loop() 的
        // block 是唯一先于这个空 block 入队的任务，空 block 执行到、
        // 返回，就意味着 pump_loop() 那次调用已经完整返回（要么它已经
        // 看到 running_==false 从循环退出，要么它压根还没被调度到、
        // GCD 保证按入队顺序执行，空 block 不会插到它前面）。这之后再
        // 往下继续析构才是安全的。
        //
        // 例外：如果 close_internal() 恰好是从泵线程自己身上被调用的
        // ——比如某个 onEof/onError 回调里把 bridge 最后一个强
        // 引用释放掉，触发的 -dealloc 就会嵌套在 pump_loop() 自己的调用
        // 栈里——对自己所在的串行队列 dispatch_sync 会自死锁，这里跳过。
        // 跳过是安全的：此时我们自己就是当前唯一的执行上下文，没有"另一
        // 个正在跑的 pump_loop() 调用"需要等。**但这不代表这种"回调内部
        // 把自己所在 bridge 的最后一个强引用释放掉"的写法是安全的**——
        // PlayerCore 对象在 close_internal() 返回之后、.cxx_destruct 里
        // 才真正被 delete，而那一步仍然嵌套在 pump_loop() 自己的调用栈
        // 里（step() → present() → 回调 → dealloc → close → 这里返回→
        // .cxx_destruct → ~PlayerCore() → delete → 栈继续回溯到
        // pump_loop() 的 while 循环，此时 `this` 已经是悬空指针）。这是
        // 一个结构性更深的问题（对象在自己的调用栈还活着的时候被释放），
        // 不是"等不等泵线程"这类队列同步问题能解的，也不是这里修的
        // 那个 bug——如实记在这里：调用方（两个壳目前都不会这么用）
        // 不能在 onEof/onError 回调里释放持有 SypPlayerBridge 的
        // 唯一一个强引用。
        //
        // 【dispatch_sync 的等待有上界了】这里等的是
        // pump_queue_ 上那次 pump_loop() 返回。同步模式下它可能正卡在 step() 内的
        // av_read_frame() 里（网络挂住）而无人能打断，主线程上的 close/
        // -dealloc 会冻结到网络层超时。现在先 request_abort()：打断阻塞 IO
        // （URL 播放经 create_avio 的 io_abort 钩子 → AvioBridge；HLS 经
        // HlsSession），被打断的 step() 报 Error(CANCELED)，pump_loop() 看到
        // running_==false 就退出、不把这次 CANCELED 当成播放错误上报。
        // demo 固定线程模式：阻塞读在加载线程上，step() 本身不卡，这里的等待
        // 通常只是一次 step() 或一次 usleep；request_abort() 打断加载线程的读，
        // 下面 player_.reset() → ~Pipeline() join 加载线程时就不必等网络超时。
        // running_ 必须先于 request_abort() 置 false（上面第一行）——反过来
        // 泵线程可能在两步之间把 CANCELED 当成真错误回调给 UI。
        {
            std::lock_guard<std::mutex> a(abort_mu_);
            if (abortable_ != nullptr) abortable_->request_abort();
        }
        if (dispatch_get_specific(kPumpQueueSpecificKey) == nullptr) {
            dispatch_sync(pump_queue_, ^{
              });
        }

        {
            std::lock_guard<std::mutex> a(abort_mu_);   // 锁序 abort_mu_ → mu_
            std::lock_guard<std::mutex> lk(mu_);
            abortable_ = nullptr;
            // player_ 析构顺序：pipeline_ → sink_ → renderer_ → clock_
            // （track_player.h 顶部长注释），renderer_ 这里持有的是裸指针，
            // 跟着 player_ 一起被销毁，这里只清引用。
            player_.reset();
            renderer_ = nullptr;
            sink_ = nullptr;
            eof_notified_ = false;
            last_good_position_us_ = 0;   // 换素材后不该沿用上一份的位置
            // 与 open_common 对称：duration_us_ 只在
            // mu_ 下写。原来它在锁外（下面 avio_ 收尾之后）清零，而 -snapshot
            // 在 mu_ 下读它——同一个数据竞争的另一半。清零放在这里也更早，
            // 正好落在 player_.reset() 这一批"媒体已经没了"的字段里。
            duration_us_ = 0;
            // 同理复位硬解标志：本头承诺"close 会把内部
            // 状态复位"，漏掉它就是一个 hasMedia == NO 却自称"正在硬解"的快照。
            // 今天无害只是因为上层用 hasMedia 挡着，但契约不该靠上层兜。
            hardware_decoding_ = false;
        }
        // Pipeline（在 player_ 内部）已经销毁，AVIOContext 不会再被访问，
        // 现在关闭 dl 层资源才安全——顺序反过来会是 UAF。
        avio_.reset();
        if (src_ != nullptr) {
            syp_source_close(src_);
            src_ = nullptr;
        }
    }

    // pipeline 的所有权在这里转移进 TrackPlayer；调用方（open 方法）在
    // 调用前已经先 close_internal() 过一轮，这里不用再处理"覆盖一个正在
    // 播放的实例"的情况。
    bool open_common(std::unique_ptr<Pipeline> pipeline, int32_t* out_err) {
        // 算进局部变量，**不在这里**直接写
        // duration_us_：本函数拿 mu_ 之前的这一段是没有锁保护的，而 -snapshot
        // 在 mu_ 下读 duration_us_，且完全可能与本次 open 并发——Swift 侧的
        // refresh() 会在 play/pause/seek/setRate 与泵回调里调 snapshot()，
        // 没有任何一条被"openQueue 上正在 open"挡住。直接写就是一个真实的
        // int64_t 数据竞争（TSan 可见），不是理论上的。
        int64_t duration_us = 0;
        for (const auto& t : pipeline->tracks()) {
            if (t.duration_us > duration_us) duration_us = t.duration_us;
        }
        // 必须在 pipeline 被 move 进 TrackPlayer::create() 之前读——
        // TrackPlayer 不转发这个查询，move 之后 pipeline 就是空壳了。
        const bool hw_decoding = pipeline->video_hardware_decoding();

        auto sink = std::make_unique<AudioUnitSink>();
        AudioUnitSink* sink_ptr = sink.get();
        auto renderer = std::make_unique<PresentTrackingRenderer>();
        PresentTrackingRenderer* renderer_ptr = renderer.get();

        syp_status err = SYP_OK;
        std::lock_guard<std::mutex> a(abort_mu_);   // 锁序 abort_mu_ → mu_
        std::lock_guard<std::mutex> lk(mu_);
        // 真上屏：renderer 建好、还没交给 TrackPlayer 之前先接上当前挂着的
        // layer（如果有）。这样即便 -attachVideoLayer: 早于本次 open 被调用
        // 过一次，新素材的第一帧也能直接画到同一个 layer 上，不需要调用方
        // 每次 open 之后重新调一次。
        if (layer_ != nullptr) renderer_ptr->set_output_layer(layer_);
        // 【订正时序】音量/静音/gravity 的
        // 持久值住在 PlayerCore 上（跨 open 存活，Ruling P2 的意图不变），
        // 但必须作为 InitialSettings **传进** create()，不能 create() 成功之后
        // 再 set_volume()/set_muted()：create() 在 sink->open() 之前落增益，
        // AudioUnitSink::open() 在那一刻快照 gain_current（首次
        // 打开不淡入）。之后才改目标的话，起点是 1.0，持久静音每次 open
        // 开头漏出约 15ms 近满音量。
        //
        // 原先 create() 之后那次"重新下发三者"**已删除**，不保留作冗余保险：
        // 留着它，InitialSettings 这一跳漏传/传错时 -volumeInUseForTest /
        // -mutedInUseForTest / -videoGravityInUseForTest 全被它盖成正确值，
        // 三条缝就守不住这一跳了；删掉之后三条缝 + -sinkGainAtOpenForTest
        // 都直接反映 create() 收到的是什么。volume_ 原样传（本层不夹取，
        // 见 -setVolume:），create() 按 set_volume 同一口径夹。
        syp::media::InitialSettings initial;
        initial.volume  = volume_;
        initial.muted   = muted_;
        initial.gravity = gravity_;
        player_ = TrackPlayer::create(std::move(pipeline), std::move(sink), std::move(renderer),
                                       /*clock_override=*/nullptr, &err,
                                       syp::media::BufferPolicy{}, initial);
        if (player_ == nullptr) {
            if (out_err != nullptr) *out_err = static_cast<int32_t>(err);
            return false;
        }
        abortable_ = player_.get();
        renderer_ = renderer_ptr;
        sink_ = sink_ptr;
        eof_notified_ = false;
        duration_us_ = duration_us;   // 与其余字段一样，只在 mu_ 下写
        hardware_decoding_ = hw_decoding;
        if (out_err != nullptr) *out_err = SYP_OK;
        return true;
    }

    void start_pump() {
        running_.store(true, std::memory_order_relaxed);
        // weak self 不适用（PlayerCore 不是 ObjC 对象），泵线程访问的是
        // 裸指针 this——这句话本身不构成生命周期保证，保证由
        // close_internal() 显式兑现（dispatch_sync 空 block 到
        // pump_queue_，等这里发起的 pump_loop() 调用确认返回后才让
        // PlayerCore 被析构，见 close_internal() 顶部长注释；旧版本这里
        // 曾经只写"不会在 PlayerCore 析构之后继续碰 this"却没有代码兑现，
        // 用 ASan 复现了对应的 use-after-free，那句话已经不成立，
        // 这是订正后的版本）。
        PlayerCore* self = this;
        dispatch_async(pump_queue_, ^{
            self->pump_loop();
        });
    }

    void pump_loop() {
        // 错误通知闩锁：同一段连续 Error 只回调一次，任何非 Error
        // 结果解除。泵线程独占，不需要锁。
        bool error_reported = false;
        while (running_.load(std::memory_order_relaxed)) {
            syp::media::PlayOutcome outcome;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (player_ == nullptr) break;
                outcome = player_->step();
            }
            using Kind = syp::media::PlayOutcome::Kind;
            if (outcome.kind != Kind::Error) error_reported = false;
            switch (outcome.kind) {
                case Kind::Waiting:
                case Kind::Blocked:
                    // 短暂让出 CPU——泵线程是唯一被允许在这忙等的地方，
                    // 2ms 是"不忙转、也不明显拖慢恢复呈现"的经验折中，
                    // 跟 AudioRing 200ms 默认环深比起来有充分余量。
                    usleep(2000);
                    break;
                case Kind::Eof:
                    notify_eof_once();
                    usleep(20000);   // 已经播完，没必要继续紧凑轮询
                    break;
                case Kind::Error:
                    // close_internal() 先置 running_=false 再 request_abort()：
                    // 这时的 CANCELED 是我们自己叫停的，不是播放错误。
                    if (!running_.load(std::memory_order_relaxed)) break;
                    // 报错后泵线程不再停下，改为慢速轮询：线程模式 seek
                    // 失败（或加载线程读错）后 Pipeline 每步报 Error 直到下一次 seek，
                    // 以前这里置 running_=false 退出循环，泵线程永久停止，用户再 seek
                    // 也恢复不了。为什么不在 -seekToUs: 里"泵停了就重新 start_pump()"：那要
                    // 在 mu_ 下读"泵是否因错误停下"、再改 running_，与 close_internal()
                    // 不持 mu_ 置 running_=false + request_abort() + dispatch_sync 等泵返回
                    // 的顺序交错时，seek 可能把 running_ 翻回 true、在 dispatch_sync 之后才
                    // 排进一次 pump_loop()，在 PlayerCore 析构后碰 this。保持泵线程活着就没有
                    // "重新武装"这一步：running_ 仍只由 start_pump()/close_internal() 写。
                    // 线程模式下错误是闩住的（不再发起 IO），20ms 一次 step() 只是读标记；
                    // seek 成功后下一步不再报 Error，闩锁解除、播放照常继续。
                    if (!error_reported) {
                        error_reported = true;
                        notify_error(outcome.status);
                    }
                    usleep(20000);
                    break;
                case Kind::Presented:
                case Kind::Dropped:
                case Kind::Queued:
                    break;   // 有活干，立即继续下一轮
            }
        }
    }

    void notify_eof_once() {
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!eof_notified_) {
                eof_notified_ = true;
                should_notify = true;
            }
        }
        if (!should_notify) return;
        SypPlayerBridge* strong = owner_;
        if (strong == nil) return;
        void (^cb)(void) = strong.onEof;
        if (cb != nil) cb();
    }

    void notify_error(syp_status status) {
        SypPlayerBridge* strong = owner_;
        if (strong == nil) return;
        void (^cb)(NSInteger) = strong.onError;
        if (cb != nil) cb(static_cast<NSInteger>(status));
    }
};

@interface SypPlayerSnapshot ()
@property (nonatomic, readwrite) BOOL hasMedia;
@property (nonatomic, readwrite) int64_t positionUs;
@property (nonatomic, readwrite) int64_t durationUs;
@property (nonatomic, readwrite) int64_t lastPresentedPtsUs;
@property (nonatomic, readwrite) BOOL hasPresentedFrame;
@property (nonatomic, readwrite) BOOL hasVideo;
@property (nonatomic, readwrite) SypClockKind clockKind;
@property (nonatomic, readwrite) int64_t droppedFrames;
@property (nonatomic, readwrite) int64_t presentFailures;
@property (nonatomic, readwrite) BOOL paused;
@property (nonatomic, readwrite) double speed;
@property (nonatomic, readwrite) BOOL eof;
@property (nonatomic, readwrite) BOOL hardwareDecoding;
@property (nonatomic, readwrite) int64_t renderBusyFrames;
@property (nonatomic, readwrite) BOOL catchingUp;
@property (nonatomic, readwrite) SypClockSwitchReason clockSwitchReason;
@property (nonatomic, readwrite) BOOL buffering;
@property (nonatomic, readwrite) SypBufferingReason bufferingReason;
@property (nonatomic, readwrite) int64_t bufferedMs;
@property (nonatomic, readwrite) int64_t rebufferCount;
@property (nonatomic, readwrite) int64_t startupUs;
@property (nonatomic, readwrite) int64_t audioUnderruns;
@property (nonatomic, readwrite) int32_t videoDisplayWidth;
@property (nonatomic, readwrite) int32_t videoDisplayHeight;
@end

@implementation SypPlayerSnapshot
@end

// ----------------------------------------------------------- 缓存配置与预加载

@implementation SypCacheSettings

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _directory         = [SypCacheSettings defaultDirectory];
        _maxBytes          = 512LL * 1024 * 1024;
        _minFreeSpaceBytes = 256LL * 1024 * 1024;
        _ttlMs             = 7LL * 24 * 60 * 60 * 1000;
    }
    return self;
}

// 默认目录从 NSTemporaryDirectory() 挪到 Caches。
// tmp 由系统在空间紧张时**无预警**回收，而预加载花了带宽暖出来的字节恰恰
// 最不该被这样回收；Caches 同样可被系统回收，但优先级低得多，且是 App 自己
// 的清理逻辑扫得到的位置。路径本身现在可配，这条默认值只是起点。
+ (NSString *)defaultDirectory {
    NSArray<NSString*>* dirs =
        NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString* base = dirs.firstObject ?: NSTemporaryDirectory();   // 取不到 Caches 时退回 tmp
    return [base stringByAppendingPathComponent:@"syplayer-http-cache"];
}

@end

@interface SypPreloadStatisticsSnapshot ()
@property (nonatomic, readwrite) int64_t entries;
@property (nonatomic, readwrite) int64_t activeTasks;
@property (nonatomic, readwrite) int64_t downloadedBytes;
@property (nonatomic, readwrite) int64_t completed;
@property (nonatomic, readwrite) int64_t failed;
@property (nonatomic, readwrite) int64_t providerMiss;
@end

@implementation SypPreloadStatisticsSnapshot
@end

namespace {

// SypPreloadPriority 与 syp::dl::PreloadPriority 显式
// 映射，不靠 static_cast 隐式依赖枚举顺序；静态断言再钉一次数值一致。
static_assert(static_cast<NSInteger>(syp::dl::PreloadPriority::Background) ==
              SypPreloadPriorityBackground);
static_assert(static_cast<NSInteger>(syp::dl::PreloadPriority::Next) == SypPreloadPriorityNext);
static_assert(static_cast<NSInteger>(syp::dl::PreloadPriority::Playing) ==
              SypPreloadPriorityPlaying);

syp::dl::PreloadPriority to_dl_priority(SypPreloadPriority p) noexcept {
    switch (p) {
        case SypPreloadPriorityBackground: return syp::dl::PreloadPriority::Background;
        case SypPreloadPriorityNext:       return syp::dl::PreloadPriority::Next;
        case SypPreloadPriorityPlaying:    return syp::dl::PreloadPriority::Playing;
    }
    return syp::dl::PreloadPriority::Next;
}

}  // namespace

@implementation SypPreloaderBridge {
    // **必须**是 PreloadStack，不是各建
    // 各的 provider + preloader：cache_dir 只在它那里归一化一次再分发给两边，
    // 而 CacheStore::make_key 不归一化目录——目录一字之差（多一个尾斜杠）就是
    // 两份缓存，探测和预加载暖的字节播放时命中不了，且**不报任何错**。
    // 它同时兜住了析构顺序（preloader 先死，provider 后死；syp_preload.h 明写
    // provider->ctx 必须活过 preloader，因为驱动线程可能正卡在探测里）。
    std::unique_ptr<syp::media::PreloadStack> _stack;
}

- (instancetype)initWithCache:(SypCacheSettings *)cache
                maxTotalTasks:(int32_t)maxTotalTasks
           reservedForPlaying:(int32_t)reservedForPlaying
          defaultPreloadBytes:(int64_t)defaultPreloadBytes
             defaultPreloadMs:(int64_t)defaultPreloadMs {
    self = [super init];
    if (self == nil) return nil;
    ensure_http_backend_registered();

    syp_config dl{};
    const std::string cache_dir_std = prepare_cache_dir(cache, &dl);
    dl.cache_dir = cache_dir_std.c_str();   // 只在本次 create 调用期间被读

    syp::dl::PreloadConfig pc{};
    pc.max_total_tasks       = maxTotalTasks;
    pc.reserved_for_playing  = reservedForPlaying;
    pc.default_preload_bytes = defaultPreloadBytes;
    pc.default_preload_ms    = defaultPreloadMs;

    // backend 传 nullptr = 用刚注册好的全局后端。失败（目录为空、后端没装上）
    // 时 _stack 为空，本类的每个方法都按"底层未就绪"处理，不崩。
    syp_status err = SYP_OK;
    _stack = syp::media::PreloadStack::create(dl, pc, /*backend=*/nullptr, &err);
    return self;
}

- (void)dealloc {
    // ~PreloadStack 会等驱动线程退出，而那条线程可能正卡在 provider 的探测里
    // （实测最坏：永不响应的服务端 6,015–6,021ms；1 KiB/s 慢速滴流的服务端
    // 由 kProbeWallClockMs 兜住，15,047–15,065ms —— 之前这一类
    // 是**无界**的，理论 ~34 分钟）。**所以不要在主线程释放本对象**——Swift 侧的
    // SYPlayerPreloader.deinit 把最后一份强引用交给后台队列，见 SYPBridge.h。
    _stack.reset();
}

- (BOOL)addURLString:(NSString *)urlString
            priority:(SypPreloadPriority)priority
        milliseconds:(int64_t)milliseconds {
    if (_stack == nullptr || urlString.length == 0) return NO;
    const char* u = urlString.UTF8String;
    if (u == nullptr) return NO;
    return _stack->preloader()->add(std::string(u), to_dl_priority(priority), milliseconds) ==
                   SYP_OK
               ? YES
               : NO;
}

- (void)setPriority:(SypPreloadPriority)priority forURLString:(NSString *)urlString {
    if (_stack == nullptr || urlString.UTF8String == nullptr) return;
    // 返回值是 SYP_ERR_INVALID_ARG（URL 不在表里）时**不上报**：调用方给一个
    // 没 add 过的 URL 调这个是无害的 no-op，不是错误。
    (void)_stack->preloader()->set_priority(std::string(urlString.UTF8String),
                                            to_dl_priority(priority));
}

- (void)removeURLString:(NSString *)urlString {
    if (_stack == nullptr || urlString.UTF8String == nullptr) return;
    _stack->preloader()->remove(std::string(urlString.UTF8String));
}

- (void)removeAll {
    if (_stack != nullptr) _stack->preloader()->remove_all();
}

- (SypPreloadStatisticsSnapshot *)statistics {
    syp::dl::PreloadStats s{};
    if (_stack != nullptr) _stack->preloader()->get_stats(&s);
    SypPreloadStatisticsSnapshot* snap = [[SypPreloadStatisticsSnapshot alloc] init];
    snap.entries         = s.entries;
    snap.activeTasks     = s.active_tasks;
    snap.downloadedBytes = s.downloaded_bytes;
    snap.completed       = s.completed;
    snap.failed          = s.failed;
    snap.providerMiss    = s.provider_miss;
    return snap;
}

- (NSString *)cacheDirectoryInUse {
    if (_stack == nullptr) return nil;
    // 取 preloader 自己那一份，不是 PreloadStack 存的那一份：要断言的是
    // "真正拿去 make_key 的字符串"，取中间变量就又回到了"我们传了同一个
    // 变量进去"这种不鉴别的断言。
    return @(_stack->preloader_cache_dir_for_test().c_str());
}

// 检验缝：容量/TTL 真的穿过 ObjC++→C 这一跳了没有。
// 读的是 **provider 自己手里那份 syp_config**（PreloadStack::create 交给它、
// 它在构造时整份拷下来的那一个，probe() 里 `syp_config c = cfg_` 用的就是它），
// 不是 initWithCache: 里那个局部 `dl`——读局部变量等于断言"我们传了同一个变量
// 进去"，一点鉴别力都没有。preloader 那一侧拿的是同一个 `c`（create() 里相邻
// 两句），而 syp::dl::Preloader 没有对应的缝、本轮不改 src/dl。
// _stack 为空（目录为空 / 后端没装上）时返回 nil。
- (SypCacheSettings *)cacheSettingsInUse {
    if (_stack == nullptr) return nil;
    return cache_settings_from_config(_stack->provider_config_for_test());
}

// 上面那条断在 provider（探测那一支）上，这一条断在
// **Preloader** 那一支上——承担全部预加载下载的是它。读的是
// syp::dl::Preloader::base_config()，也就是它派给每一条 SourceBridge 的那份
// 配置的起点，不是 PreloadStack::create 里的局部变量。
// 按值返回一份 syp_config：里面的 cache_dir 指向 Preloader 自己的 cache_dir_，
// 与 _stack 同寿，本整式内读它是安全的。
- (SypCacheSettings *)preloaderCacheSettingsInUse {
    if (_stack == nullptr) return nil;
    return cache_settings_from_config(_stack->preloader_config_for_test());
}

- (int64_t)cacheOpenCountForURLString:(NSString *)urlString {
    if (_stack == nullptr || urlString.UTF8String == nullptr) return -1;
    const std::string key =
        syp::dl::CacheStore::make_key(_stack->preloader_cache_dir_for_test(),
                                      std::string(urlString.UTF8String));
    return syp::dl::CacheStore::get().open_count(key);
}

@end

@implementation SypPlayerBridge {
    std::unique_ptr<PlayerCore> _core;
    // 本实例的缓存配置。nil = 用 SypCacheSettings 的默认。
    // 只在主线程/调用方线程读写，-openURLString: 一进来就取一次快照。
    SypCacheSettings* _cacheSettings;
    // 【用例从不驱动真正的 -openURLString:】
    // 最近一次 -openURLString: **真的构造出来交给 dl 层**的那份 syp_config 的
    // 缓存四元组快照。nil = 还没 open 过。写在任何网络 IO 之前，所以即使这次
    // open 最终失败（甚至卡住）它也已经落定。
    SypCacheSettings* _lastOpenCacheConfig;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _core = std::make_unique<PlayerCore>();
        _core->owner_ = self;
    }
    return self;
}

- (void)setCacheSettings:(SypCacheSettings *)cache {
    _cacheSettings = cache;
    // **在这里就把目录建出来**，而不是等到第一次 -openURLString:。
    // 【建不出来这件事不会被这里的调用方看到】本方法返回 void，
    // prepare_cache_dir 的 error: 传的是 nil，失败一路被丢光（实测父目录
    // chmod 0500 → 零信号）。真实理由只剩两条：少一次失败往返；以及把这次
    // 文件系统操作留在配置调用上，而不是压进 -openURLString: 的 IO 路径。
    // prepare_cache_dir 顺带做了这件事，这里只丢弃它的返回值。
    syp_config throwaway{};
    (void)prepare_cache_dir(cache, &throwaway);
}

// 纯 getter：走 effective_cache_dir 而不是 prepare_cache_dir，
// 读一下**不建目录**。建目录归 -setCacheSettings:（失败不上报，见
// prepare_cache_dir 上方）与 -openURLString:。
- (NSString *)cacheDirectoryInUse {
    SypCacheSettings* cache = _cacheSettings ?: [[SypCacheSettings alloc] init];
    return @(effective_cache_dir(cache).c_str());
}

// 检验缝：**下一次** -openURLString: 会交给 dl 层的那四个数
// （目录 + 容量/最小可用空间/TTL）。同样是纯 getter，不建目录。
//
// 它与下面的 -lastOpenCacheConfig 是两件事，都要：这一条读的是"配置算出来
// 是什么"，那一条读的是"真的 open 时落进 syp_config 的是什么"。只有后者能
// 发现"-openURLString: 自己不再走 prepare_cache_dir 了"。
- (SypCacheSettings *)cacheSettingsInUse {
    SypCacheSettings* cache = _cacheSettings ?: [[SypCacheSettings alloc] init];
    syp_config cfg{};
    fill_cache_config(cache, &cfg);
    const std::string dir = effective_cache_dir(cache);
    cfg.cache_dir = dir.c_str();
    return cache_settings_from_config(cfg);
}

- (SypCacheSettings *)lastOpenCacheConfig {
    return _lastOpenCacheConfig;
}

- (void)dealloc {
    [self close];
}

+ (BOOL)hardwareDecodeAvailable {
    return syp::platform::videotoolbox_available_for_h264() ? YES : NO;
}

- (void)attachVideoLayer:(CAMetalLayer* _Nullable)layer {
    // 只拿 mu_，不拿 abort_mu_——layer_/renderer_ 只在
    // mu_ 下读写，本方法不碰 abortable_，没有理由把 abort_mu_ 也牵进来。之前
    // 版本按"跟 open_common() 一致"套用了 abort_mu_ → mu_ 这个锁序，但那是
    // 死锁隐患：泵线程可能正卡在持 mu_ 执行的那次 step() 里（同步模式下网络挂住，
    // demo 固定线程模式之后 step() 不再做网络读，这条
    // 是对同步模式与未来改动的防御），这时主线程调 -attachVideoLayer: 会卡在等
    // mu_（同时已经先拿到了 abort_mu_）；而 close_internal() 打断这次卡住的
    // step() 靠的正是先拿 abort_mu_ 调 request_abort()（SYPBridge.mm 顶部长注释）
    // ——abort_mu_ 被 -attachVideoLayer: 占着，close_internal() 连
    // request_abort() 都调不了，这条中止口被这里重新堵死。SYPBridge.h 承诺
    // -attachVideoLayer: 可以在任意时刻调用，包括泵线程正卡住的时候，所以
    // 这里不能重蹈这个覆辙。
    // CALayer 属性只在主线程改。本方法由视图控制器在主线程调用——在这里
    // （拿 mu_ 之前，不让可能卡住的泵线程拖住主线程上的属性设置）把 layer 配成
    // MetalRenderer 要求的形状；open_common() 在后台队列上调 set_output_layer()
    // 时渲染器只校验、不改。PlayerView.MetalVideoView 构造时已经配过一次，这里对
    // 其它调用方兜底，重复设置同值无副作用。
    if (layer != nil && [NSThread isMainThread]) {
        if (layer.device == nil) layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
    }
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->layer_ != nullptr) {
        CFBridgingRelease(_core->layer_);
        _core->layer_ = nullptr;
    }
    if (layer != nil) {
        _core->layer_ = const_cast<void*>(CFBridgingRetain(layer));
    }
    if (_core->renderer_ != nullptr) {
        _core->renderer_->set_output_layer(_core->layer_);
    }
}

- (BOOL)openLocalFile:(NSString*)path hardwareDecode:(BOOL)hw errorOut:(int32_t*)sypStatusOut {
    _core->close_internal();

    std::string p = path.UTF8String != nullptr ? path.UTF8String : "";
    PipelineConfig cfg = make_pipeline_config(hw);
    syp_status err = SYP_OK;
    auto pipeline = Pipeline::create_file(p, cfg, &err);
    if (pipeline == nullptr) {
        if (sypStatusOut != nullptr) *sypStatusOut = static_cast<int32_t>(err);
        return NO;
    }

    int32_t oerr = SYP_OK;
    const bool ok = _core->open_common(std::move(pipeline), &oerr);
    if (sypStatusOut != nullptr) *sypStatusOut = oerr;
    if (ok) _core->start_pump();
    return ok ? YES : NO;
}

- (BOOL)openURLString:(NSString*)urlString hardwareDecode:(BOOL)hw errorOut:(int32_t*)sypStatusOut {
    _core->close_internal();
    ensure_http_backend_registered();

    // 目录与容量走 -setCacheSettings: 传进来的
    // 配置。未设置时用 SypCacheSettings 的默认值（Caches/syplayer-http-cache）。
    // 归一化与 SypPreloaderBridge 走同一个 prepare_cache_dir() → 同一个
    // syp::dl::normalize_cache_dir()，所以"两边写法不同的同一个目录"落在
    // 同一份缓存里是结构事实，不靠调用方自己对齐。
    SypCacheSettings* cache = _cacheSettings ?: [[SypCacheSettings alloc] init];
    // cfg 是 **const**，且塑造它的每一行都在 open_dl_config()
    // 里（见该函数上方长注释）。于是下面这条缝与两个消费点之间，再没有任何
    // 一行能改动这份配置——"缝之后还能再改一手"这个形状在本函数里被关死了。
    // cache_dir_std 必须活到最后一个消费点（cfg.cache_dir 指着它）。
    std::string cache_dir_std;
    const syp_config cfg = open_dl_config(cache, &cache_dir_std);
    // 检验缝：在任何网络 IO 之前把这份 cfg 的缓存四元组拍下来。
    // 断言落在**真的传给 syp_source_open / Pipeline::create_hls 的那份配置**
    // 上，而不是"我们又调了一次 prepare_cache_dir"。
    _lastOpenCacheConfig = cache_settings_from_config(cfg);

    std::string url_std = urlString.UTF8String != nullptr ? urlString.UTF8String : "";

    // HLS：按 URL 路径后缀判定（与 HlsSession 分流子资源用的是同一个
    // 启发式，局限见 url_rewrite.h channel_for 上方注释——不以 .m3u8 结尾
    // 的播放列表会被当成普通文件去 demux，打开失败而不是静默错播）。
    // 所有网络 IO 走 dl 层，没有 src_/avio_ 要托管。
    if (syp::media::hls::channel_for(url_std) == syp::media::hls::Channel::Playlist) {
        PipelineConfig pcfg = make_pipeline_config(hw);
        syp_status perr = SYP_OK;
        auto pipeline = Pipeline::create_hls(url_std, pcfg, cfg, syp::media::HlsOptions{}, &perr);
        if (pipeline == nullptr) {
            if (sypStatusOut != nullptr) *sypStatusOut = static_cast<int32_t>(perr);
            return NO;
        }
        int32_t oerr = SYP_OK;
        const bool ok = _core->open_common(std::move(pipeline), &oerr);
        if (sypStatusOut != nullptr) *sypStatusOut = oerr;
        if (ok) _core->start_pump();
        return ok ? YES : NO;
    }

    syp_source* src = nullptr;
    syp_status err = syp_source_open(&src, url_std.c_str(), /*headers=*/nullptr, &cfg, /*cb=*/nullptr);
    if (err != SYP_OK || src == nullptr) {
        if (sypStatusOut != nullptr) *sypStatusOut = static_cast<int32_t>(err);
        return NO;
    }

    auto avio = AvioBridge::create(src, /*buffer_size=*/0);
    if (avio == nullptr) {
        syp_source_close(src);
        if (sypStatusOut != nullptr) *sypStatusOut = static_cast<int32_t>(SYP_ERR_IO);
        return NO;
    }

    PipelineConfig pcfg = make_pipeline_config(hw);
    syp_status perr = SYP_OK;
    // io_abort：close 路径的 request_abort() 经它打断卡在 syp_source_read
    // 里的读。avio_ 晚于 player_ 销毁（close_internal），
    // 钩子里的裸指针在 Pipeline 存活期间始终有效。
    AvioBridge* avio_raw = avio.get();
    auto pipeline = Pipeline::create_avio(avio->ctx(), pcfg, &perr,
                                          [avio_raw] { avio_raw->request_abort(); });
    if (pipeline == nullptr) {
        avio.reset();
        syp_source_close(src);
        if (sypStatusOut != nullptr) *sypStatusOut = static_cast<int32_t>(perr);
        return NO;
    }

    _core->src_ = src;
    _core->avio_ = std::move(avio);

    int32_t oerr = SYP_OK;
    const bool ok = _core->open_common(std::move(pipeline), &oerr);
    if (sypStatusOut != nullptr) *sypStatusOut = oerr;
    if (ok) {
        _core->start_pump();
    } else {
        _core->avio_.reset();
        syp_source_close(_core->src_);
        _core->src_ = nullptr;
    }
    return ok ? YES : NO;
}

- (void)play {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ != nullptr) _core->player_->play();
}

- (void)pause {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ != nullptr) _core->player_->pause();
}

- (BOOL)setSpeed:(double)speed {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ == nullptr) return NO;
    return _core->player_->set_speed(speed) == SYP_OK ? YES : NO;
}

// 三个 setter 同一形状：持 mu_，先写 PlayerCore 上的
// 持久值（跨 open 保留），再转发给当前 TrackPlayer（如果有）。getter 读持久值。
- (void)setVolume:(double)volume {
    // **这一层不夹取**，原样存、原样转发。夹取归 Swift 侧 SYPlayer.volume（公开
    // 入口，唯一调用方）；TrackPlayer::set_volume 为它自己的消费者再夹一次。
    // 为什么不在这里再加一道：三层同口径的夹取会让 Swift 那一道的变异全部
    // 等价（NaN 穿下来被这里夹成 0，XCTest 永远看不见 Swift 层少了一个分支），
    // 于是"公开 API 的夹取"就成了没有用例守着的代码。见 SYPBridge.h 的属性注释。
    std::lock_guard<std::mutex> lk(_core->mu_);
    _core->volume_ = volume;
    if (_core->player_ != nullptr) _core->player_->set_volume(volume);
}

- (double)volume {
    std::lock_guard<std::mutex> lk(_core->mu_);
    return _core->volume_;
}

- (void)setMuted:(BOOL)muted {
    std::lock_guard<std::mutex> lk(_core->mu_);
    _core->muted_ = muted ? true : false;
    if (_core->player_ != nullptr) _core->player_->set_muted(_core->muted_);
}

- (BOOL)isMuted {
    std::lock_guard<std::mutex> lk(_core->mu_);
    return _core->muted_ ? YES : NO;
}

- (void)setVideoGravity:(SypVideoGravity)videoGravity {
    std::lock_guard<std::mutex> lk(_core->mu_);
    _core->gravity_ = to_cpp_gravity(videoGravity);
    if (_core->player_ != nullptr) _core->player_->set_gravity(_core->gravity_);
}

- (SypVideoGravity)videoGravity {
    std::lock_guard<std::mutex> lk(_core->mu_);
    return to_objc_gravity(_core->gravity_);
}

// 三条检验缝读的是消费者（player_ / 它的渲染器），不是上面的
// 持久副本——见 SYPBridge.h 的声明注释。
- (double)volumeInUseForTest {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ == nullptr) return -1.0;
    return _core->player_->volume();
}

- (int32_t)mutedInUseForTest {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ == nullptr) return -1;
    return _core->player_->muted() ? 1 : 0;
}

- (int32_t)videoGravityInUseForTest {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ == nullptr || _core->renderer_ == nullptr) return -1;
    return static_cast<int32_t>(to_objc_gravity(_core->renderer_->gravity_in_use()));
}

// 见 SYPBridge.h 的声明注释。
- (double)sinkGainAtOpenForTest {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ == nullptr || _core->sink_ == nullptr) return -1.0;
    return static_cast<double>(_core->sink_->gain_at_open_for_test());
}

// 两条缝都经 renderer_（PresentTrackingRenderer 装饰层）而不是
// 绕过它直接摸 MetalRenderer——被测的正是装饰层那一跳转发。持 mu_ 与泵线程串行。
- (BOOL)debugBlitPixelForTest:(int32_t)dstW
                       height:(int32_t)dstH
                          atX:(double)fx
                            y:(double)fy
                         bgra:(uint32_t*)out {
    if (out == nullptr || dstW <= 0 || dstH <= 0) return NO;
    std::vector<uint8_t> buf;
    {
        std::lock_guard<std::mutex> lk(_core->mu_);
        if (_core->player_ == nullptr || _core->renderer_ == nullptr) return NO;
        if (!_core->renderer_->debug_blit_to_bgra(dstW, dstH, buf)) return NO;
    }
    // 归一化坐标 → 像素（与 tests/test_metal_renderer.cpp 的 bgra_at 同一取法：
    // 截断后夹进 [0, n-1]）。
    int32_t x = static_cast<int32_t>(fx * static_cast<double>(dstW));
    int32_t y = static_cast<int32_t>(fy * static_cast<double>(dstH));
    x = std::min(std::max(x, int32_t{0}), dstW - 1);
    y = std::min(std::max(y, int32_t{0}), dstH - 1);
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x)) * 4;
    if (i + 3 >= buf.size()) return NO;
    *out = static_cast<uint32_t>(buf[i]) | (static_cast<uint32_t>(buf[i + 1]) << 8) |
           (static_cast<uint32_t>(buf[i + 2]) << 16) | (static_cast<uint32_t>(buf[i + 3]) << 24);
    return YES;
}

- (BOOL)debugSetSourceGeometryForTest:(int32_t)sarNum
                               sarDen:(int32_t)sarDen
                          rotationDeg:(int32_t)rotationDeg {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ == nullptr || _core->renderer_ == nullptr) return NO;
    // 经装饰层的虚函数 override 下去（renderer_ 的静态类型就是装饰层）。
    _core->renderer_->set_source_geometry(sarNum, sarDen, rotationDeg);
    return YES;
}

- (void)seekToUs:(int64_t)us {
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ != nullptr) {
        _core->player_->seek(us);
        // seek 成功后**必须**解除 EOF 闩。
        //
        // 漏了这一步的后果不是"少一次回调"，而是状态永久错：eof_notified_ 之前
        // 只在 close_internal()/open_common() 里清，而 -snapshot 原样上报它，
        // 于是播到结尾之后 seek 回去再播，快照里 eof 恒 YES——上层
        // （SYPlayerState.make）把它翻成 .ended，位置一边前进一边显示"已结束"，
        // 而且每 seek 一次就再触发一次 playerDidReachEnd。本头（SYPBridge.h 的
        // onEof 注释）承诺的是"直到下一次 open/seek"，这里才是兑现它的地方。
        //
        // 放在同一个 mu_ 临界区里：notify_eof_once() 也在 mu_ 下读改这个标志，
        // 分开两段会留出"seek 已生效、闩还在"的窗口，正是泵线程最可能采样的时刻。
        _core->eof_notified_ = false;
    }
}

- (SypPlayerSnapshot*)snapshot {
    SypPlayerSnapshot* snap = [SypPlayerSnapshot new];
    std::lock_guard<std::mutex> lk(_core->mu_);
    if (_core->player_ != nullptr) {
        snap.hasMedia = YES;
        // 钳位，兑现 SYPBridge.h 对 positionUs 的契约。
        // position_us() 直接转发 clock_->now_us()，AudioClock 又转发
        // played_us()——sink 已 failed() 时那是 AV_NOPTS_VALUE
        // （INT64_MIN）。调用方（PlayerViewController:221）拿它做
        // `lastPresentedPtsUs - positionUs`，Swift 的整数溢出是 trap，
        // 漏这一步 demo 会崩而不是显示一个离谱数字。
        const int64_t raw_pos = _core->player_->position_us();
        if (raw_pos != AV_NOPTS_VALUE) _core->last_good_position_us_ = raw_pos;
        snap.positionUs   = _core->last_good_position_us_;
        snap.durationUs   = _core->duration_us_;
        snap.clockKind    = _core->player_->clock_kind() == syp::media::ClockKind::Audio
                                 ? SypClockKindAudio
                                 : SypClockKindSystem;
        snap.droppedFrames    = _core->player_->dropped_frames();
        snap.presentFailures  = _core->player_->present_failures();
        snap.paused        = _core->player_->paused() ? YES : NO;
        snap.speed         = _core->player_->speed();
        // hasVideo 的契约是"本次 open 是否绑定了视频轨"（SYPBridge.h），判据
        // 必须来自 TrackPlayer 绑了哪条轨，不能用 renderer_ != nullptr 这个
        // 代理判据 —— open() 路径无条件构造 renderer，那个表达式恒为真，
        // 对"素材里到底有没有视频"零信息量。带封面图的音乐文件正是它答错
        // 的场景：素材里有一条 is_video 的封面图轨，但 TrackPlayer 按
        // AV_DISPOSITION_ATTACHED_PIC 判据不会绑定它，真实答案是"没有视频"，
        // 而代理判据会答 YES，让 PlayerViewController 永远停在"尚无已呈现
        // 帧"而不是"无视频轨"。
        snap.hasVideo      = _core->player_->video_track_index() >= 0 ? YES : NO;
        if (_core->renderer_ != nullptr && _core->renderer_->has_presented()) {
            snap.hasPresentedFrame  = YES;
            snap.lastPresentedPtsUs = _core->renderer_->last_presented_pts_us();
        } else {
            snap.hasPresentedFrame  = NO;
            snap.lastPresentedPtsUs = AV_NOPTS_VALUE;
        }
        snap.eof = _core->eof_notified_ ? YES : NO;
        snap.hardwareDecoding = _core->hardware_decoding_ ? YES : NO;
        snap.renderBusyFrames   = _core->player_->render_busy_frames();
        snap.catchingUp         = _core->player_->catching_up() ? YES : NO;
        snap.clockSwitchReason  = to_objc_clock_switch_reason(_core->player_->clock_switch_reason());
        snap.buffering       = _core->player_->buffering() ? YES : NO;
        snap.bufferingReason = to_objc_buffering_reason(_core->player_->buffering_reason());
        const int64_t buffered_us = _core->player_->buffered_us();
        snap.bufferedMs      = buffered_us == std::numeric_limits<int64_t>::max() ? -1 : buffered_us / 1000;
        snap.rebufferCount   = _core->player_->rebuffer_count();
        snap.startupUs      = _core->player_->startup_us();
        snap.audioUnderruns = _core->player_->audio_underruns();
        const syp::media::DisplaySize ds = _core->player_->video_display_size();
        snap.videoDisplayWidth  = ds.width;
        snap.videoDisplayHeight = ds.height;
    } else {
        // 没有播放器：除下面这一个字段外，其余一律保持 [SypPlayerSnapshot new]
        // 的零值，并由 hasMedia = NO 声明它们无意义（SYPBridge.h 顶部那条契约）。
        // **不要在这里填"合理默认值"**——零值在 speed/clockKind/paused 上
        // 恰好都是看起来合法的值，填了只会让调用方更难分辨。
        snap.hasMedia           = NO;
        snap.lastPresentedPtsUs = AV_NOPTS_VALUE;
        snap.startupUs          = -1;   // 0 会被读成"瞬间起播"，必须给未知值
    }
    return snap;
}

- (void)close {
    _core->close_internal();
}

@end

@implementation SypNetworkBridge

+ (void)setRateLimitBytesPerSecond:(int64_t)bytesPerSecond {
    syp_rate_limit_set(bytesPerSecond);
}

+ (int64_t)rateLimitBytesPerSecond {
    return syp_rate_limit_get();
}

// 转发到 syp_preconnect（syp_net.h）。存储（两个 vector<std::string>）与
// 指针数组（两个 vector<const char*>）活到 syp_preconnect() 返回为止即可。
+ (void)preconnectURL:(NSString *)url
           headerNames:(NSArray<NSString *> *)names
          headerValues:(NSArray<NSString *> *)values {
    // 与 SypPlayerBridge/SypPreloaderBridge 不同，预连接可能是
    // 进程里第一次触达 dl 层的调用（不经过任何 SYPlayer/SYPlayerPreloader
    // 实例）。不在这里补一次，SYPlayerNetwork.preconnect 在没有播放器/预加载
    // 器存在时就会静默什么都不做——不是"未注册后端"那条设计内的静默失败
    // （syp_net.h 已声明的行为），而是本可以避免的一处遗漏。
    ensure_http_backend_registered();
    if (url.length == 0) return;
    std::string url_std(url.UTF8String != nullptr ? url.UTF8String : "");
    if (url_std.empty()) return;

    const NSUInteger n = MIN(names.count, values.count);
    std::vector<std::string> name_storage;
    std::vector<std::string> value_storage;
    name_storage.reserve(n);
    value_storage.reserve(n);
    for (NSUInteger i = 0; i < n; ++i) {
        NSString *name  = names[i];
        NSString *value = values[i];
        name_storage.emplace_back(name.UTF8String != nullptr ? name.UTF8String : "");
        value_storage.emplace_back(value.UTF8String != nullptr ? value.UTF8String : "");
    }
    std::vector<const char*> name_ptrs;
    std::vector<const char*> value_ptrs;
    name_ptrs.reserve(name_storage.size());
    value_ptrs.reserve(value_storage.size());
    for (const auto& s : name_storage) name_ptrs.push_back(s.c_str());
    for (const auto& s : value_storage) value_ptrs.push_back(s.c_str());

    syp_headers h{};
    h.names  = name_ptrs.empty() ? nullptr : name_ptrs.data();
    h.values = value_ptrs.empty() ? nullptr : value_ptrs.data();
    h.count  = static_cast<int32_t>(name_ptrs.size());

    syp_preconnect(url_std.c_str(), h.count > 0 ? &h : nullptr);
}

+ (int32_t)preconnectInflightForTest {
    return syp::dl::Preconnector::instance().inflight_for_test();
}

// 测试缝：读 current_http_backend() != nullptr——预连接
// 前应已惰性注册好后端。**已知局限**：单元测试跑在同一进程里，若在本条用例
// 之前已有别的用例（SypPlayerBridge/SypPreloaderBridge）触发过
// ensure_http_backend_registered()，即使把 preconnectURL: 里那行新增调用
// 删掉，这条测试也可能仍然绿——不是本条用例能独立钉住的那类回归。
+ (BOOL)preconnectBackendRegisteredForTest {
    return syp::dl::current_http_backend() != nullptr;
}

@end
