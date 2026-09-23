// metal_renderer.mm — MetalRenderer 实现。
//
// 本文件里唯一真正碰 GPU 的部分是：构造时的设备/管线建立、present() 里
// 的纹理创建与上传、以及命令缓冲的提交与等待。三条硬要求各自对应的
// "可以脱离 GPU 单测的纯函数"（is_pix_fmt_supported / select_color_
// matrix / pack_plane_rows）全部是不碰任何 Apple 框架符号的普通 static
// 方法，声明在 metal_renderer.h、跟本文件其余部分之间没有隐藏的耦合——
// 这是刻意的组织方式，照抄 audio_unit_sink.{h,mm} 的结构（见 metal_
// renderer.h 顶部注释）。

#include "platform/apple/metal_renderer.h"
#include "platform/apple/inflight_gate.h"
#include "media/video_geometry.h"

#include "syp_video_shaders_metal_source.h"   // tools/gen-embedded-header.sh 生成，见该文件与本目录 video_shaders.metal 顶部注释

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

// dl 层的全局日志汇，转发到这里——跟 audio_unit_sink.mm 同一处理，只
// 前置声明签名，不 #include "dl/source_bridge.h"（那个头拖着一整套 dl
// 层内部类型，本文件只要这一个函数）。签名必须跟 source_bridge.h 里的
// 声明逐字匹配。
namespace syp::dl {
void log_msg(syp_log_level lvl, const char* tag, const char* msg);
}  // namespace syp::dl

// Metal 回调的线程交接对 TSan 不可见（commit → 驱动 → 回调派发
// 全在未插桩代码里），present() 线程在提交前写下的数据（回调块的捕获、令牌）会被
// 报成与回调线程的数据竞争。用 TSan 公开的同步注解把这次交接显式告诉它：提交前
// release(tag)，回调开头 acquire(tag)。非 TSan 构建里是空操作。
//
// 键是一个进程级静态标签（kTsanHandoffTag），不是 impl 或令牌：迟到的 presentedHandler
// 可能在渲染器析构之后才跑，拿堆地址当键会碰已释放内存；读块里的 ObjC 捕获本身也会先被报。
// 代价（精度下降）：所有渲染器、所有帧共用一个同步对象，任一回调的 acquire 都会合并
// 调用方线程最近一次 release 时的时钟——回调与"更早帧/其他渲染器"的调用方写之间的真实
// 竞争可能因此被掩盖。
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
extern "C" {
void __tsan_acquire(void* addr);
void __tsan_release(void* addr);
}
#    define SYP_TSAN_HANDOFF_RELEASE(p) __tsan_release(p)
#    define SYP_TSAN_HANDOFF_ACQUIRE(p) __tsan_acquire(p)
#  endif
#endif
#ifndef SYP_TSAN_HANDOFF_RELEASE
#  define SYP_TSAN_HANDOFF_RELEASE(p) ((void)(p))
#  define SYP_TSAN_HANDOFF_ACQUIRE(p) ((void)(p))
#endif
namespace {
char kTsanHandoffTag = 0;   // 只取地址，永不读写

// iOS 模拟器 SDK 的 MTLDrawable 协议没有声明 addPresentedHandler:（真机/macOS/Catalyst 有）。
// 模拟器上挂 layer 的帧退回"命令缓冲完成即归还名额"（同离屏），代价是 nextDrawable 可能
// 短暂阻塞——模拟器只用于开发调试，可接受。
#if TARGET_OS_SIMULATOR
constexpr bool kPresentedHandlerAvailable = false;
#else
constexpr bool kPresentedHandlerAvailable = true;
#endif
}  // namespace

// 一帧在途名额的 exactly-once 归还令牌。挂 layer 时 presentedHandler 与（执行失败时的）
// 完成回调都可能归还，claim 只有第一次返回 YES。令牌是每帧独立的堆对象：迟到的回调
// （例如失败兜底已归还、渲染器已析构之后才来的 presentedHandler）认领失败时只碰令牌，
// 不碰 Impl。
@interface SYPInflightToken : NSObject
- (BOOL)claim;
- (BOOL)isClaimed;
@end

@implementation SYPInflightToken {
    std::atomic<bool> _claimed;
}
- (BOOL)claim {
    return !_claimed.exchange(true) ? YES : NO;
}
- (BOOL)isClaimed {
    return _claimed.load() ? YES : NO;
}
@end

namespace syp::platform {

// ---------------------------------------------------------------------
// 可脱离 GPU 独立测试的纯函数（第 1/3 条硬要求 + 第 2 条硬要求的一半）
// ---------------------------------------------------------------------

bool MetalRenderer::is_pix_fmt_supported(int32_t pix_fmt) noexcept {
    // yuv420p（软解，CPU 上传）与 VIDEOTOOLBOX（硬解，CVPixelBuffer 零拷贝）。
    // 两种格式仍不值得一张表。VIDEOTOOLBOX 帧里 CVPixelBuffer 的具体格式
    // （只接受 NV12 420v/420f）在 encode_nv12() 里判。
    return pix_fmt == static_cast<int32_t>(AV_PIX_FMT_YUV420P) ||
           pix_fmt == static_cast<int32_t>(AV_PIX_FMT_VIDEOTOOLBOX);
}

MetalRenderer::ColorMatrixChoice MetalRenderer::select_color_matrix(int32_t colorspace,
                                                                     int32_t color_range,
                                                                     int32_t width,
                                                                     int32_t height) noexcept {
    ColorMatrixChoice choice;

    switch (colorspace) {
        case AVCOL_SPC_BT709:
            choice.matrix = ColorMatrixKind::BT709;
            break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            choice.matrix = ColorMatrixKind::BT601;
            break;
        default:
            // 取不到（AVCOL_SPC_UNSPECIFIED）或者是本渲染器没见过的值：
            // 按分辨率推定——≥720p 用 BT.709，否则 BT.601
            // （约定按高度判定，不看宽度：竖屏
            // 视频常见宽<高，仍然按"纵向像素数是否达到 720"这条更贴近
            // "720p/1080p" 命名习惯的规则判）。
            choice.matrix   = (height >= 720) ? ColorMatrixKind::BT709 : ColorMatrixKind::BT601;
            choice.presumed = true;
            break;
    }

    switch (color_range) {
        case AVCOL_RANGE_JPEG:
            choice.full_range = true;
            break;
        case AVCOL_RANGE_MPEG:
            choice.full_range = false;
            break;
        default:
            // 取不到（AVCOL_RANGE_UNSPECIFIED）：推定成 limited/tv
            // range——这是绝大多数视频内容的实际编码方式，也是比"猜 full
            // range"更安全的一侧：错误地把本来是 full range 的内容当
            // limited 处理，只会轻微压缩对比度；反过来把 limited 当 full
            // 处理会在两端明显削波，视觉上更容易被发现，但"更容易被发现
            // 的错误"不等于"该选的默认值"——这里选风险更低的一侧。
            choice.full_range = false;
            choice.presumed   = true;
            break;
    }

    (void)width;   // 目前的推定规则只看 height；width 保留在签名里是因为
                    // "分辨率"这个判据未来可能改成同时看两个维度（比如
                    // 竖屏 1080x1920 的场景），现在不用不代表接口不该留。
    return choice;
}

bool MetalRenderer::pack_plane_rows(uint8_t* dst, const uint8_t* src, int32_t src_stride,
                                     int32_t row_bytes, int32_t rows) noexcept {
    if (dst == nullptr || src == nullptr) return false;
    if (row_bytes <= 0 || rows <= 0) return false;
    if (src_stride < row_bytes) return false;   // stride 比一行有效数据还窄，调用方传错了

    for (int32_t r = 0; r < rows; ++r) {
        std::memcpy(dst + static_cast<size_t>(r) * static_cast<size_t>(row_bytes),
                    src + static_cast<size_t>(r) * static_cast<size_t>(src_stride),
                    static_cast<size_t>(row_bytes));
    }
    return true;
}

// ---------------------------------------------------------------------
// Impl：全部 Metal/Foundation 状态，只在本文件里可见
// ---------------------------------------------------------------------

struct MetalRenderer::Impl {
    id<MTLDevice>              device   = nil;
    id<MTLCommandQueue>        queue    = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLComputePipelineState> nv12_pipeline = nil;   // nv12_to_rgba

    // VideoToolbox CVPixelBuffer → MTLTexture 的零拷贝缓存（IOSurface 直接
    // 映射成纹理，不经 CPU）。CF 对象，ARC 不管，~Impl 手动释放。
    CVMetalTextureCacheRef tex_cache = nullptr;
    // yuv420p 路径 replaceRegion 上传的累计帧数（结构性验收 2：NV12 路径恒不增）。
    int64_t cpu_uploads = 0;

    // 生产上屏。layer 非 nil 时 present() 追加 blit pass 画到其 drawable。
    CAMetalLayer*               layer               = nil;
    id<MTLRenderPipelineState>  blit_pipeline       = nil;
    // 在命令缓冲完成回调（任意线程）里计数，所以是原子。
    std::atomic<int64_t>        drawables_presented{0};

    Impl() = default;
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (tex_cache != nullptr) CFRelease(tex_cache);
    }

    // 纹理槽。present() 不再等 GPU，同一时刻最多 kMaxInflightFrames 帧在途，
    // 每帧独占一个槽（cmd_pending 由 present() 置位、完成回调清零），CPU 的 replaceRegion
    // 永远不会改写 GPU 正在读写的纹理。slots 本身只在 present() 线程读写——完成回调
    // 不碰它，重建时在途帧的旧纹理由命令缓冲自身持有的引用保活。
    // out 按帧尺寸在重建时建；y/u/v 只有 yuv420p 路径用，首次用到时才建（NV12 硬解
    // 路径永远不分配，见 encode_yuv420p()）。
    //
    // 名额的归还时机按路径区分：离屏在命令缓冲完成时归还；挂 layer 时要等
    // drawable 的 presentedHandler——drawable 在那之后才回到 layer 的池子，提前归还会让
    // 下一次 nextDrawable 阻塞调用线程（#52 本身）；执行失败时完成回调兜底。
    //
    // cmd_pending[slot]：本槽命令缓冲未完成——present() 线程置 true，完成回调的最后一步
    // 清掉；为 false 槽才可复用（GPU 不再读写其 out）。名额另由每帧的 SYPInflightToken
    // 保证恰好归还一次。挂 layer 时名额可能早于或晚于槽归还，名额数 ≥ 忙槽数不再恒成立，
    // 取到名额却没有空闲槽时按 BUSY 处理（瞬态，防御）。
    //
    // keep：NV12 路径必须活到 GPU 完成的对象（CVMetalTextureRef、CVPixelBuffer）。存在槽
    // 里而不是交给回调块释放：只在 present() 线程读写——槽被复用时（此时上一帧必已完成）
    // 或 wait_until_idle() 等到空闲后清掉，保证全部早于 ~Impl 的 CFRelease(tex_cache)，
    // 也不在 Metal 线程上改 NSMutableArray。代价：每槽最多多留一帧的 CVPixelBuffer。
    //
    // token/cmd/reclaim_deadline/has_layer（只在 present() 线程读写）：本槽
    // 最近一帧的名额令牌、命令缓冲、兜底期限（提交时刻 + 期限，期限默认 kPermitReclaimAfterS，
    // 可由测试钩子缩短）与是否挂 layer。"挂 layer、命令缓冲已完成、令牌仍未认领、且已过
    // reclaim_deadline" 视为 presentedHandler 丢失，由 reclaim_stale_permits() 强制认领归还
    // （迟到的回调认领失败，不重复归还）。离屏帧的名额只在完成回调归还，不参与兜底。
    // 令牌未认领的槽不复用，避免丢掉它的记录。
    struct Slot {
        id<MTLTexture>       y = nil, u = nil, v = nil, out = nil;
        NSArray*             keep        = nil;
        SYPInflightToken*    token       = nil;
        id<MTLCommandBuffer> cmd         = nil;
        double               reclaim_deadline = 0;
        bool                 has_layer        = false;
    };
    std::array<Slot, syp::platform::kMaxInflightFrames> slots;
    int32_t next_slot = 0;
    int32_t last_slot = -1;                       // 最近一次提交所用槽（回读用）
    syp::platform::InflightGate gate{syp::platform::kMaxInflightFrames};
    std::array<std::atomic<bool>, syp::platform::kMaxInflightFrames> cmd_pending{};
    id<MTLCommandBuffer> last_cmd = nil;          // 最近一次提交的命令缓冲（回读前等它）
    std::atomic<double>  last_present_host_time{0};
    std::atomic<int64_t> gpu_errors{0};

    // 测试钩子 debug_hold_gate_releases()：hold 期间本该归还的名额只记账，放开时一次性归还。
    std::atomic<bool> hold_releases{false};
    std::mutex        hold_mu;
    int32_t           held_releases = 0;   // hold_mu 保护

    // 归还一个在途名额（任意线程）。调用方保证每个名额恰好调用一次。本函数自身的最后一步
    // 是 gate.release()；但各回调里最后一次碰 Impl 的语句因路径而异（离屏/失败是这里，挂
    // layer 正常完成是 finish_cmd），所以 wait_until_idle() 以"名额归零且槽全空"为准。
    void release_gate() noexcept {
        if (hold_releases.load()) {
            std::lock_guard<std::mutex> lk(hold_mu);
            if (hold_releases.load()) {
                ++held_releases;
                return;
            }
        }
        gate.release();
    }
    // 本槽命令缓冲已结束（完成回调，或未提交的提前返回）。
    void finish_cmd(int32_t slot) noexcept { cmd_pending[static_cast<size_t>(slot)].store(false); }
    // present() 线程：命令缓冲已完成且本槽令牌已认领（或没有令牌）才可复用。
    bool slot_free(int32_t slot) const noexcept {
        const auto& sl = slots[static_cast<size_t>(slot)];
        return !cmd_pending[static_cast<size_t>(slot)].load() &&
               (sl.token == nil || [sl.token isClaimed]);
    }

    // 测试钩子 debug_suppress_presented_release()：present() 时读入并按值捕获进回调块，
    // 回调里不再读 Impl（迟到回调不碰已析构的 Impl）。
    bool                 suppress_presented_release = false;
    // 测试钩子 debug_set_permit_reclaim_after_ms()：兜底期限（秒），present() 提交时读入槽。
    double               reclaim_after_s = kPermitReclaimAfterS;
    std::atomic<int64_t> reclaimed_permits{0};

    // presentedHandler 丢失的兜底（present() 线程）。只回收命令缓冲已完成、
    // 超过期限仍未认领的令牌；返回本次回收数。
    int32_t reclaim_stale_permits(double now) noexcept {
        int32_t n = 0;
        for (int32_t k = 0; k < syp::platform::kMaxInflightFrames; ++k) {
            auto& sl = slots[static_cast<size_t>(k)];
            if (sl.token == nil || !sl.has_layer || cmd_pending[static_cast<size_t>(k)].load()) continue;
            if (now < sl.reclaim_deadline) continue;
            if ([sl.token claim]) {
                release_gate();
                reclaimed_permits.fetch_add(1);
                ++n;
            }
            sl.token = nil;
            sl.cmd   = nil;
        }
        if (n > 0) {
            dl::log_msg(SYP_LOG_WARN, "metal_renderer",
                        "drawable 的 presentedHandler 超时未回调，已强制归还在途名额");
        }
        return n;
    }
    static constexpr double kPermitReclaimAfterS =
        static_cast<double>(syp::platform::kMaxPresentDueUs) / 1e6 + 0.2;

    int32_t tex_width  = 0;   // 当前各槽纹理对应的亮度尺寸；变了就整批重建
    int32_t tex_height = 0;

    std::vector<uint8_t> y_scratch;   // pack_plane_rows() 的输出暂存区，
    std::vector<uint8_t> u_scratch;   // 单线程调用方（present() 只在调用
    std::vector<uint8_t> v_scratch;   // 方线程跑）私有，不需要同步。

    bool init_failed      = false;   // 构造时设备/队列/管线任一步失败
    bool device_available = false;   // MTLCreateSystemDefaultDevice() 是否成功——
                                      // 独立于 init_failed，见 metal_renderer.h
                                      // device_available()/ready() 顶部注释
    bool has_output   = false;   // debug_copy_output_rgba() 用：是否已有
                                  // 一次成功 present() 提交过（写 slots[last_slot].out）

    // 色彩矩阵选择的缓存。缓存**只是为了少打日志**：30/60fps 下每帧一行
    // "这是推定的"日志本身就是噪音，第一次算出来、或者输入真的变了才值得
    // 再打一次。select_color_matrix() 本身是纯查表，重算一次的代价可以
    // 忽略。
    //
    // 【缓存键必须覆盖全部四个入参】缓存键必须是 select_color_matrix() 的
    // **全部四个入参**，不能只有 (width,height)。这里曾经写着「present()
    // 目前恒以 AVCOL_SPC_UNSPECIFIED/AVCOL_RANGE_UNSPECIFIED 调用
    // select_color_matrix()……结果因此只随分辨率变化」——那句话在接入
    // f.colorspace()/f.color_range() 之前是对的，接上之后当场失实，而
    // 缓存键没跟着改。后果是像素级可测的：同一个
    // renderer、同一个分辨率下，第 2 帧起无论 colorspace/color_range 怎么
    // 变都会沿用第 1 帧算出来的矩阵。实测（全部 640x480，同一个
    // renderer）：#1 cs=UNSPEC → rgba 213 19 203；#2 cs=BT709 →
    // 213 19 203（沿用）；#3 cs=BT709 range=JPEG → 213 19 203（同上）；
    // 对照组（每次新建 renderer）分别是 227 48 208 / 214 56 197。
    //
    // 现有 26 条用例每条都各建一个新 renderer，结构性地测不到这件事；
    // 已有用例只测了"尺寸变了要失效"，反方向零覆盖。回归用例见
    // test_metal_renderer.cpp::
    // object_level_same_size_different_colorspace_invalidates_matrix_cache。
    MetalRenderer::ColorMatrixChoice cached_choice{};
    bool    has_cached_choice = false;
    int32_t cached_colorspace  = 0;   // 只在 has_cached_choice 为真时有意义
    int32_t cached_color_range = 0;
};

namespace {

id<MTLTexture> make_texture(id<MTLDevice> device, MTLPixelFormat fmt, int32_t w, int32_t h,
                            bool writable) {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                            width:static_cast<NSUInteger>(w)
                                                           height:static_cast<NSUInteger>(h)
                                                        mipmapped:NO];
    // MTLStorageModeShared：统一内存下 CPU/GPU 共享同一份物理内存，
    // present() 的逐行上传（replaceRegion）与 debug_copy_output_
    // rgba() 的 CPU 侧回读（getBytes）都要求纹理可以被 CPU 直接摸到
    // ——Private 存储模式做不到这两件事中的任何一件。这台开发机是
    // Apple Silicon（统一内存架构），Shared 是最直接的选择；在有独立
    // 显存的 Intel Mac 上 Shared 仍然合法、只是可能不是性能最优的
    // 存储模式——目标是"颜色正确"，不是"上传延迟最优"。
    desc.storageMode = MTLStorageModeShared;
    desc.usage       = writable ? MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead
                                 : MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:desc];
}

}  // namespace

// 构造/尺寸变化时（重新）建每个槽的 out 纹理，并作废各槽的 y/u/v（yuv420p 路径下次
// 用到时按新尺寸懒建，见 encode_yuv420p()）。在途帧引用的旧纹理由其命令缓冲持有
// （编码时 setTexture 即被命令缓冲 retain），这里直接替换是安全的。
void MetalRenderer::rebuild_textures(MetalRenderer::Impl& impl, int32_t width, int32_t height) {
    for (auto& slot : impl.slots) {
        slot.y   = nil;
        slot.u   = nil;
        slot.v   = nil;
        slot.out = make_texture(impl.device, MTLPixelFormatRGBA8Unorm, width, height, /*writable=*/true);
    }

    impl.tex_width  = width;
    impl.tex_height = height;
    // 各槽 out 已换新：回读不能再读任何槽，直到下一次成功提交。
    impl.has_output = false;
    impl.last_slot  = -1;
}

namespace {

void log_unsupported_pix_fmt(int32_t pix_fmt) {
    const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(pix_fmt));
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "unsupported pix_fmt for MetalRenderer: %s (fmt=%d) — 只支持 yuv420p/videotoolbox, "
                  "如实报错而不是画出错误的颜色",
                  name != nullptr ? name : "?", pix_fmt);
    dl::log_msg(SYP_LOG_ERROR, "metal_renderer", buf);
}

void log_color_matrix_choice(const MetalRenderer::ColorMatrixChoice& c, int32_t width,
                              int32_t height) {
    const char* matrix_name = (c.matrix == MetalRenderer::ColorMatrixKind::BT709)
                                   ? "BT.709"
                                   : "BT.601";
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "color matrix: %s, range=%s%s (frame=%dx%d)", matrix_name,
                  c.full_range ? "full" : "limited",
                  c.presumed ? " [presumed: AVFrame 未提供有效 colorspace/color_range，按分辨率/"
                                "行业默认推定]"
                              : "",
                  width, height);
    dl::log_msg(c.presumed ? SYP_LOG_WARN : SYP_LOG_INFO, "metal_renderer", buf);
}

// 两条编码路径共用的 helper：只接显式参数、不碰 Impl（Impl 是 MetalRenderer 的
// 私有嵌套类型，类外自由函数无法命名它）。
//
// 布局必须与 video_shaders.metal 的 ColorParams 逐字段一致。
id<MTLBuffer> make_params_buffer(id<MTLDevice> device, MetalRenderer::ColorMatrixKind matrix,
                                 bool full_range) {
    struct ColorParamsGpu {
        uint32_t matrix;
        uint32_t full_range;
    };
    const ColorParamsGpu params{static_cast<uint32_t>(matrix), full_range ? 1u : 0u};
    return [device newBufferWithBytes:&params
                               length:sizeof(params)
                              options:MTLResourceStorageModeShared];
}

// blit 参数直接用 syp::media::BlitParams 按字节交给着色器，布局必须与
// video_shaders.metal 的 BlitParams（packed_float2, packed_float2, uint）逐字段一致。
static_assert(sizeof(syp::media::BlitParams) == 20, "BlitParams 布局与着色器不一致");
static_assert(offsetof(syp::media::BlitParams, pos_scale) == 0, "pos_scale 偏移不一致");
static_assert(offsetof(syp::media::BlitParams, uv_scale) == 8, "uv_scale 偏移不一致");
static_assert(offsetof(syp::media::BlitParams, rot_quadrant) == 16, "rot_quadrant 偏移不一致");

// blit pass 的**唯一**编码函数——生产 present()（dst = drawable 的纹理）与测试缝
// debug_blit_to_bgra()（dst = 离屏 BGRA8 纹理）都调它。测试缝不另写一份 render pass：
// 那样测的是副本，生产路径错了测试照绿。先清成不透明黑（letterbox 的黑边），再按 bp
// 画一个四顶点三角带。只接显式参数、不碰 Impl（同 make_params_buffer）。
void encode_blit(id<MTLCommandBuffer> cmd, id<MTLRenderPipelineState> pipeline,
                 id<MTLTexture> src, id<MTLTexture> dst, const syp::media::BlitParams& bp) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture     = dst;
    pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> renc = [cmd renderCommandEncoderWithDescriptor:pass];
    [renc setRenderPipelineState:pipeline];
    [renc setVertexBytes:&bp length:sizeof(bp) atIndex:0];
    [renc setFragmentTexture:src atIndex:0];
    [renc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [renc endEncoding];
}

// 16×16 线程组铺满 width×height；越界线程由核内的边界判断丢弃。
void dispatch_full_frame(id<MTLComputeCommandEncoder> enc, int32_t width, int32_t height) {
    const MTLSize per_group = MTLSizeMake(16, 16, 1);
    const MTLSize groups    = MTLSizeMake((static_cast<NSUInteger>(width) + 15) / 16,
                                          (static_cast<NSUInteger>(height) + 15) / 16, 1);
    [enc dispatchThreadgroups:groups threadsPerThreadgroup:per_group];
}

}  // namespace

// ---------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------

MetalRenderer::MetalRenderer() : impl_(std::make_unique<Impl>()) {
    impl_->device = MTLCreateSystemDefaultDevice();
    if (impl_->device == nil) {
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", "MTLCreateSystemDefaultDevice() 返回 nil，"
                                                       "没有可用的 Metal 设备");
        impl_->init_failed = true;
        return;
    }
    // 设备本身找到了——后面任何一步（命令队列/着色器编译/管线创建）
    // 再失败，都是"有 GPU 但我们的代码有问题"，跟这里的"环境里压根没
    // 有 GPU"是两类完全不同的失败，不能共用同一个布尔。
    impl_->device_available = true;

    impl_->queue = [impl_->device newCommandQueue];
    if (impl_->queue == nil) {
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", "newCommandQueue 失败");
        impl_->init_failed = true;
        return;
    }

    // 运行期编译着色器源码——见 metal_renderer.h / video_shaders.metal 顶部
    // 注释：这台机器没装独立的 Metal Toolchain 组件，走不了
    // `xcrun metal`/`metallib` 的离线编译流水线。
    NSString* src = [NSString stringWithUTF8String:kVideoShadersMetalSource];
    NSError*  err = nil;
    id<MTLLibrary> lib = [impl_->device newLibraryWithSource:src options:nil error:&err];
    if (lib == nil) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "着色器编译失败: %s",
                      err != nil ? [[err localizedDescription] UTF8String] : "?");
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", buf);
        impl_->init_failed = true;
        return;
    }

    id<MTLFunction> fn = [lib newFunctionWithName:@"yuv420p_to_rgba"];
    if (fn == nil) {
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", "着色器里找不到函数 yuv420p_to_rgba");
        impl_->init_failed = true;
        return;
    }

    NSError* perr = nil;
    impl_->pipeline = [impl_->device newComputePipelineStateWithFunction:fn error:&perr];
    if (impl_->pipeline == nil) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "创建计算管线失败: %s",
                      perr != nil ? [[perr localizedDescription] UTF8String] : "?");
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", buf);
        impl_->init_failed = true;
        return;
    }

    // NV12 零拷贝路径的管线 + CVMetalTextureCache。任一建不起来同样算
    // "有 GPU 但我们的代码有问题"（ready()==false，测试 FAIL 而非 SKIP）。
    id<MTLFunction> nv12_fn = [lib newFunctionWithName:@"nv12_to_rgba"];
    NSError* nerr = nil;
    impl_->nv12_pipeline = nv12_fn != nil
        ? [impl_->device newComputePipelineStateWithFunction:nv12_fn error:&nerr]
        : nil;
    if (impl_->nv12_pipeline == nil ||
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, impl_->device, nullptr,
                                  &impl_->tex_cache) != kCVReturnSuccess) {
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", "NV12 管线或 CVMetalTextureCache 创建失败");
        impl_->init_failed = true;
        return;
    }

    // blit 渲染管线（out_tex → CAMetalLayer drawable，BGRA8Unorm）。
    MTLRenderPipelineDescriptor* rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction   = [lib newFunctionWithName:@"blit_vertex"];
    rp.fragmentFunction = [lib newFunctionWithName:@"blit_fragment"];
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError* berr = nil;
    impl_->blit_pipeline = (rp.vertexFunction != nil && rp.fragmentFunction != nil)
        ? [impl_->device newRenderPipelineStateWithDescriptor:rp error:&berr]
        : nil;
    if (impl_->blit_pipeline == nil) {
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", "blit 渲染管线创建失败");
        impl_->init_failed = true;
        return;
    }
}

MetalRenderer::~MetalRenderer() {
    // 命令缓冲完成回调与 presentedHandler 捕获的是裸 Impl*，必须先等所有在途
    // 帧完成（回调跑完、名额全部归还）再销毁 Impl，否则回调会写已释放的内存。
    // Impl 里其余是 ARC 对象 + std::vector，唯一的 CF 对象 tex_cache 由 ~Impl 释放。
    wait_until_idle();
}

bool MetalRenderer::ready() const noexcept { return impl_ != nullptr && !impl_->init_failed; }

bool MetalRenderer::device_available() const noexcept {
    return impl_ != nullptr && impl_->device_available;
}

// ---------------------------------------------------------------------
// present()
// ---------------------------------------------------------------------

syp_status MetalRenderer::present(const syp::media::Frame& f, int64_t due_in_us) {
    if (!f.valid() || !f.is_video()) return SYP_ERR_INVALID_ARG;

    // 第 1 条硬要求：格式判定必须先于任何 GPU 操作——哪怕设备本身没建
    // 起来，不支持的格式也要如实报 SYP_ERR_NOT_IMPLEMENTED，不能被设备
    // 不可用这条分支"抢答"成一个语义不同的错误码。
    const int32_t pix_fmt = f.pix_fmt();
    if (!is_pix_fmt_supported(pix_fmt)) {
        log_unsupported_pix_fmt(pix_fmt);
        return SYP_ERR_NOT_IMPLEMENTED;
    }

    if (!ready()) return SYP_ERR_IO;

    const int32_t width  = f.width();
    const int32_t height = f.height();
    if (width <= 0 || height <= 0) return SYP_ERR_INVALID_ARG;

    // 让 CVMetalTextureCache 回收不再被引用的旧映射（已完成帧的 CVMetalTexture 随槽复用
    // 释放，见 Impl::Slot::keep），否则 VT 的 buffer pool 轮换时缓存会攒着已过期的
    // IOSurface 映射。放在调用方线程而不是完成回调里：建纹理与 flush 同在一个线程。
    if (impl_->tex_cache != nullptr) CVMetalTextureCacheFlush(impl_->tex_cache, 0);

    // 取在途名额——满了立刻 BUSY，不等 GPU、不调 nextDrawable。之后每条提前
    // 返回都必须归还。挂 layer 时上限受 drawable 池约束：maximumDrawableCount（2 或 3）
    // 里要留一个给屏幕上的前缓冲，否则第 maximumDrawableCount 次 nextDrawable 会阻塞到
    // 前缓冲被替换。maximumDrawableCount 是 layer 属性的只读现读，同 drawableSize。
    const bool has_layer = impl_->layer != nil;
    int32_t    limit     = kMaxInflightFrames;
    if (has_layer) {
        const auto pool = static_cast<int32_t>(impl_->layer.maximumDrawableCount);
        limit           = std::clamp(pool - 1, int32_t{1}, kMaxInflightFrames);
    }
    impl_->reclaim_stale_permits(CACurrentMediaTime());   // 先回收丢了 presentedHandler 的名额
    if (!impl_->gate.try_acquire(limit)) return SYP_ERR_BUSY;
    // 选一个空闲槽。离屏时名额数 ≥ 忙槽数（完成回调先清槽再归还名额），拿到名额必有空闲
    // 槽；挂 layer 时名额可能在 presentedHandler 里先于命令缓冲完成归还，瞬间可能没有
    // 空闲槽——按 BUSY 处理。
    int32_t slot = -1;
    for (int32_t k = 0; k < kMaxInflightFrames; ++k) {
        const int32_t s = (impl_->next_slot + k) % kMaxInflightFrames;
        // 只有本线程会把标志置 true，回调只会清 false，所以"读到 false 再置 true"无竞争。
        if (impl_->slot_free(s)) {
            impl_->cmd_pending[static_cast<size_t>(s)].store(true);
            slot = s;
            break;
        }
    }
    if (slot < 0) { impl_->release_gate(); return SYP_ERR_BUSY; }
    impl_->next_slot = (slot + 1) % kMaxInflightFrames;

    // 槽空闲 ⇒ 上一个用它的帧已完成：上一帧留下的 keep 这里（present() 线程）释放。
    impl_->slots[static_cast<size_t>(slot)].keep = nil;

    // 未提交的提前返回：同回调里的收尾（结束命令 + 归还名额），复用同一组成员函数。
    Impl*             impl  = impl_.get();
    SYPInflightToken* token = [SYPInflightToken new];
    const bool suppress_presented = impl_->suppress_presented_release;
    auto release_slot = [impl, token, slot] {
        impl->finish_cmd(slot);
        if ([token claim]) impl->release_gate();
    };

    if (width != impl_->tex_width || height != impl_->tex_height) {
        rebuild_textures(*impl_, width, height);
        impl_->has_cached_choice = false;   // 分辨率变了，缓存的矩阵选择连带失效
    }

    // 第 3 条硬要求：色彩矩阵从 AVFrame 取，取不到时按分辨率推定并记日志。
    // 接线：Frame::colorspace()/
    // color_range() 直接映射 AVFrame 对应字段，取不到时分别返回
    // AVCOL_SPC_UNSPECIFIED/AVCOL_RANGE_UNSPECIFIED——select_color_
    // matrix() 本身不用改一个字，只是现在真的喂到了真值。
    //
    // 【缓存键覆盖全部入参】缓存键 = select_color_matrix() 的全部四个入参。宽高的那
    // 一半由上面 rebuild_textures() 那个分支里的 has_cached_choice=false
    // 负责失效，colorspace/color_range 这一半在这里判——两半合起来才是
    // 完整的键，见 Impl::cached_choice 那段注释。
    const int32_t colorspace  = f.colorspace();
    const int32_t color_range = f.color_range();
    if (!impl_->has_cached_choice || colorspace != impl_->cached_colorspace ||
        color_range != impl_->cached_color_range) {
        impl_->cached_choice = select_color_matrix(colorspace, color_range, width, height);
        impl_->has_cached_choice  = true;
        impl_->cached_colorspace  = colorspace;
        impl_->cached_color_range = color_range;
        log_color_matrix_choice(impl_->cached_choice, width, height);
    }

    const bool is_hw = (pix_fmt == static_cast<int32_t>(AV_PIX_FMT_VIDEOTOOLBOX));

    // 第 2 条硬要求（仅 yuv420p）：按 stride 逐行拷，不能整块拷。放在建命令缓冲
    // 之前——这是 yuv420p 路径唯一可能失败的一步。
    if (!is_hw) {
        const int32_t ch_w = (width + 1) / 2;
        const int32_t ch_h = (height + 1) / 2;
        // 暂存区只在 yuv420p 路径用到时按需调整（尺寸不变时 resize 不做事）。
        impl_->y_scratch.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        impl_->u_scratch.resize(static_cast<size_t>(ch_w) * static_cast<size_t>(ch_h));
        impl_->v_scratch.resize(static_cast<size_t>(ch_w) * static_cast<size_t>(ch_h));
        if (!pack_plane_rows(impl_->y_scratch.data(), f.plane(0), f.stride(0), width, height) ||
            !pack_plane_rows(impl_->u_scratch.data(), f.plane(1), f.stride(1), ch_w, ch_h) ||
            !pack_plane_rows(impl_->v_scratch.data(), f.plane(2), f.stride(2), ch_w, ch_h)) {
            release_slot();
            return SYP_ERR_INVALID_ARG;
        }
    }

    // keep 收集必须活到 GPU 完成的对象（NV12 路径的 CVMetalTextureRef 与
    // CVPixelBuffer）；提交前挂到本槽，槽复用或空闲后才释放（见 Impl::Slot）。
    NSMutableArray*      keep = [NSMutableArray array];
    id<MTLCommandBuffer> cmd  = [impl_->queue commandBuffer];
    if (is_hw) {
        const syp_status st = encode_nv12(f, (__bridge void*)cmd, (__bridge void*)keep, slot);
        if (st != SYP_OK) {   // 命令缓冲不提交
            release_slot();
            return st;
        }
    } else {
        encode_yuv420p(f, (__bridge void*)cmd, slot);
    }

    // 挂了 layer 就在同一命令缓冲里、compute pass 之后追加 blit pass。
    if (has_layer) {
        id<CAMetalDrawable> drawable = [impl_->layer nextDrawable];
        if (drawable == nil) {   // 命令缓冲未提交，直接丢弃；对调用方就是"忙，稍后重试这帧"
            release_slot();
            return SYP_ERR_BUSY;
        }
        // 按显示几何（SAR / 旋转 / gravity）算 blit 参数；drawableSize 的非同步读见
        // set_output_layer() 注释。
        const CGSize ds = impl_->layer.drawableSize;
        const syp::media::BlitParams bp = current_blit_params(width, height, ds.width, ds.height);
        encode_blit(cmd, impl_->blit_pipeline, impl_->slots[static_cast<size_t>(slot)].out,
                    drawable.texture, bp);
        // 名额在 drawable 真正离开"待上屏"时归还（presentedHandler）。实测：
        // 无窗口 layer 上每个已提交的 drawable 都会回调（presentedTime==0）；上屏 layer
        // 上被后一帧顶掉而没显示的 drawable 同样回调。命令缓冲执行失败时完成回调兜底。
#if !TARGET_OS_SIMULATOR
        [drawable addPresentedHandler:^(id<MTLDrawable>) {
            SYP_TSAN_HANDOFF_ACQUIRE(&kTsanHandoffTag);
            if (suppress_presented) return;   // 测试钩子：模拟回调丢失
            // 先认领再碰 impl：认领失败（兜底已归还，渲染器可能已析构）时不解引用 impl。
            if ([token claim]) impl->release_gate();
        }];
#else
        (void)suppress_presented;   // 模拟器无 presentedHandler，名额由完成回调归还（见 kPresentedHandlerAvailable）
#endif
        // 按 due 用 presentDrawable:atTime: 上屏（CACurrentMediaTime 时基）；
        // due <= 0（已到期/立即）直接 presentDrawable。due 先夹到 [0, kMaxPresentDueUs]。
        const int64_t due = std::clamp(due_in_us, int64_t{0}, kMaxPresentDueUs);
        if (due > 0) {
            const CFTimeInterval at = CACurrentMediaTime() + static_cast<double>(due) / 1e6;
            impl_->last_present_host_time.store(at);
            [cmd presentDrawable:drawable atTime:at];
        } else {
            impl_->last_present_host_time.store(CACurrentMediaTime());
            [cmd presentDrawable:drawable];
        }
    }

    // 不再 waitUntilCompleted。完成回调（Metal 内部线程）里做原先提交后的
    // 同步收尾：报错计数、drawable 计数、归还槽（离屏/失败时连同名额）。
    // 回调捕获裸 Impl*——~MetalRenderer() 先等所有在途帧完成才销毁 Impl。
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
        SYP_TSAN_HANDOFF_ACQUIRE(&kTsanHandoffTag);
        const bool failed = done.status == MTLCommandBufferStatusError;
        if (failed) {
            impl->gpu_errors.fetch_add(1);
            NSError* cmd_err = done.error;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "命令缓冲执行失败: %s",
                          cmd_err != nil ? [[cmd_err localizedDescription] UTF8String] : "?");
            dl::log_msg(SYP_LOG_ERROR, "metal_renderer", buf);
        } else if (has_layer) {
            impl->drawables_presented.fetch_add(1);
        }
        // 先结束命令再归还名额：离屏时名额回来之前槽已空闲，不会出现
        // "有名额无槽"的假 BUSY。离屏/失败时 gate.release() 是最后一次碰 Impl；挂 layer
        // 正常完成时 finish_cmd 是最后一次，名额留给 presentedHandler（或丢失兜底）。
        // wait_until_idle 以"名额归零且槽全空"为准，两种顺序都覆盖。
        impl->finish_cmd(slot);
        if ((!has_layer || failed || !kPresentedHandlerAvailable) && [token claim]) impl->release_gate();
    }];
    {
        auto& sl       = impl_->slots[static_cast<size_t>(slot)];
        sl.keep        = keep;
        sl.token       = token;
        sl.cmd         = cmd;
        // 兜底回收只针对"等 presentedHandler"的帧；模拟器上名额走完成回调，不参与回收。
        sl.has_layer        = has_layer && kPresentedHandlerAvailable;
        sl.reclaim_deadline = CACurrentMediaTime() + impl_->reclaim_after_s;
    }
    impl_->last_cmd  = cmd;
    impl_->last_slot = slot;
    SYP_TSAN_HANDOFF_RELEASE(&kTsanHandoffTag);   // 回调块（含令牌）此后才可能被回调线程读
    [cmd commit];
    impl_->has_output = true;
    return SYP_OK;
}

// ---------------------------------------------------------------------
// present() 的两条编码路径
// ---------------------------------------------------------------------

void MetalRenderer::encode_yuv420p(const syp::media::Frame& f, void* cmd_ptr, int32_t slot) {
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)cmd_ptr;
    Impl::Slot&          tex = impl_->slots[static_cast<size_t>(slot)];
    ++impl_->cpu_uploads;

    const int32_t width  = f.width();
    const int32_t height = f.height();
    const int32_t ch_w   = (width + 1) / 2;
    const int32_t ch_h   = (height + 1) / 2;

    // 上传纹理懒建：本槽第一次走 yuv420p 或重建之后（重建把 y/u/v 置 nil）。
    if (tex.y == nil) {
        tex.y = make_texture(impl_->device, MTLPixelFormatR8Unorm, width, height, /*writable=*/false);
        tex.u = make_texture(impl_->device, MTLPixelFormatR8Unorm, ch_w, ch_h, /*writable=*/false);
        tex.v = make_texture(impl_->device, MTLPixelFormatR8Unorm, ch_w, ch_h, /*writable=*/false);
    }

    [tex.y replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width),
                                                 static_cast<NSUInteger>(height))
                     mipmapLevel:0
                       withBytes:impl_->y_scratch.data()
                     bytesPerRow:static_cast<NSUInteger>(width)];
    [tex.u replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(ch_w),
                                                 static_cast<NSUInteger>(ch_h))
                     mipmapLevel:0
                       withBytes:impl_->u_scratch.data()
                     bytesPerRow:static_cast<NSUInteger>(ch_w)];
    [tex.v replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(ch_w),
                                                 static_cast<NSUInteger>(ch_h))
                     mipmapLevel:0
                       withBytes:impl_->v_scratch.data()
                     bytesPerRow:static_cast<NSUInteger>(ch_w)];

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:impl_->pipeline];
    [enc setTexture:tex.y atIndex:0];
    [enc setTexture:tex.u atIndex:1];
    [enc setTexture:tex.v atIndex:2];
    [enc setTexture:tex.out atIndex:3];
    [enc setBuffer:make_params_buffer(impl_->device, impl_->cached_choice.matrix,
                                      impl_->cached_choice.full_range)
            offset:0
           atIndex:0];
    dispatch_full_frame(enc, width, height);
    [enc endEncoding];
}

// 返回 SYP_OK / SYP_ERR_INVALID_ARG / SYP_ERR_NOT_IMPLEMENTED / SYP_ERR_IO。
// keep_alive 收集要活到 GPU 完成的对象。
syp_status MetalRenderer::encode_nv12(const syp::media::Frame& f, void* cmd_ptr,
                                      void* keep_ptr, int32_t slot) {
    id<MTLCommandBuffer> cmd        = (__bridge id<MTLCommandBuffer>)cmd_ptr;
    NSMutableArray*      keep_alive = (__bridge NSMutableArray*)keep_ptr;

    auto* pb = static_cast<CVPixelBufferRef>(f.hw_handle());
    if (pb == nullptr) return SYP_ERR_INVALID_ARG;
    const OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    if (fmt != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        fmt != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        // 跟 log_unsupported_pix_fmt 同一纪律：如实报错并打出格式，不静默画错。
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "VIDEOTOOLBOX 帧的 CVPixelBuffer 格式不是 NV12 420v/420f（OSType=0x%08x），"
                      "不支持", static_cast<unsigned>(fmt));
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", buf);
        return SYP_ERR_NOT_IMPLEMENTED;
    }
    // out_tex 与线程网格按 Frame 声明的宽高建，nv12_to_rgba 按 gid 直接
    // read 亮度/色度纹理——buffer 比声明小就是 GPU 越界读，必须在建纹理之前拒绝。
    if (CVPixelBufferGetWidthOfPlane(pb, 0) < static_cast<size_t>(f.width()) ||
        CVPixelBufferGetHeightOfPlane(pb, 0) < static_cast<size_t>(f.height())) {
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer",
                    "VIDEOTOOLBOX 帧的 CVPixelBuffer 比 Frame 声明的宽高小，拒绝（防 GPU 越界读）");
        return SYP_ERR_INVALID_ARG;
    }

    CVMetalTextureRef y_ref = nullptr;
    CVMetalTextureRef c_ref = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, impl_->tex_cache, pb, nullptr, MTLPixelFormatR8Unorm,
            CVPixelBufferGetWidthOfPlane(pb, 0), CVPixelBufferGetHeightOfPlane(pb, 0), 0,
            &y_ref) != kCVReturnSuccess ||
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, impl_->tex_cache, pb, nullptr, MTLPixelFormatRG8Unorm,
            CVPixelBufferGetWidthOfPlane(pb, 1), CVPixelBufferGetHeightOfPlane(pb, 1), 1,
            &c_ref) != kCVReturnSuccess) {
        if (y_ref != nullptr) CFRelease(y_ref);
        if (c_ref != nullptr) CFRelease(c_ref);
        dl::log_msg(SYP_LOG_ERROR, "metal_renderer", "CVMetalTextureCacheCreateTextureFromImage 失败");
        return SYP_ERR_IO;
    }
    // CVMetalTextureRef 与 CVPixelBuffer 必须活到命令缓冲完成：
    // 纹理底下的 IOSurface 映射归 CVMetalTextureRef 所有，提前释放 GPU 读到的就是悬空内存。
    [keep_alive addObject:(__bridge_transfer id)y_ref];
    [keep_alive addObject:(__bridge_transfer id)c_ref];
    [keep_alive addObject:(__bridge id)pb];

    id<MTLTexture> y_tex = CVMetalTextureGetTexture(y_ref);
    id<MTLTexture> c_tex = CVMetalTextureGetTexture(c_ref);
    if (y_tex == nil || c_tex == nil) return SYP_ERR_IO;

    const bool full = (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:impl_->nv12_pipeline];
    [enc setTexture:y_tex atIndex:0];
    [enc setTexture:c_tex atIndex:1];
    [enc setTexture:impl_->slots[static_cast<size_t>(slot)].out atIndex:3];
    [enc setBuffer:make_params_buffer(impl_->device, impl_->cached_choice.matrix, full)
            offset:0
           atIndex:0];
    dispatch_full_frame(enc, f.width(), f.height());
    [enc endEncoding];
    return SYP_OK;
}

int64_t MetalRenderer::debug_cpu_upload_count() const noexcept {
    return impl_ != nullptr ? impl_->cpu_uploads : 0;
}

// ---------------------------------------------------------------------
// 显示几何
// ---------------------------------------------------------------------

void MetalRenderer::set_source_geometry(int32_t sar_num, int32_t sar_den,
                                        int32_t rotation_deg) noexcept {
    sar_num_.store(sar_num);
    sar_den_.store(sar_den);
    rotation_.store(rotation_deg);
}

void MetalRenderer::set_gravity(syp::media::Gravity g) noexcept { gravity_.store(g); }

syp::media::BlitParams MetalRenderer::current_blit_params(int32_t src_w, int32_t src_h,
                                                          double dst_w,
                                                          double dst_h) const noexcept {
    return syp::media::blit_transform(src_w, src_h, sar_num_.load(), sar_den_.load(),
                                      rotation_.load(), gravity_.load(), dst_w, dst_h);
}

// ---------------------------------------------------------------------
// set_output_layer()
// ---------------------------------------------------------------------

void MetalRenderer::set_output_layer(void* ca_metal_layer) noexcept {
    if (impl_ == nullptr) return;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)ca_metal_layer;
    if (layer == nil) {
        impl_->layer = nil;   // 回到仅离屏
        return;
    }
    // CALayer 属性只该在主线程改。调用方（demo 的 open 队列）可能在
    // 后台线程调本方法，所以这里只校验、不改——layer 应由持有它的视图在主线程预先
    // 配好 device / BGRA8Unorm / framebufferOnly。没配好：在主线程就地补上（兼容），
    // 不在主线程则拒绝挂载（打日志、保持仅离屏），绝不跨线程改 CALayer。
    // device 比对用 registryID：MTLCreateSystemDefaultDevice() 多次调用不保证是同一
    // 个 ObjC 实例，但指向同一块 GPU。
    const bool device_ok = impl_->device != nil && layer.device != nil &&
                           (layer.device == impl_->device ||
                            layer.device.registryID == impl_->device.registryID);
    const bool format_ok = layer.pixelFormat == MTLPixelFormatBGRA8Unorm;
    if (!device_ok || !format_ok) {
        if (![NSThread isMainThread]) {
            dl::log_msg(SYP_LOG_ERROR, "metal_renderer",
                        "set_output_layer: CAMetalLayer 的 device/pixelFormat 未预先配置，且当前不在主线程，"
                        "拒绝挂载（仅离屏）——请在主线程设 device=系统默认设备、pixelFormat=BGRA8Unorm");
            impl_->layer = nil;
            return;
        }
        dl::log_msg(SYP_LOG_WARN, "metal_renderer",
                    "set_output_layer: CAMetalLayer 未预先配置，主线程上就地设置 device/BGRA8Unorm/framebufferOnly");
        layer.device          = impl_->device;
        layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
    }
    impl_->layer = layer;   // ARC 强引用
}

int64_t MetalRenderer::debug_presented_drawable_count() const noexcept {
    return impl_ != nullptr ? impl_->drawables_presented.load() : 0;
}

double MetalRenderer::debug_last_present_host_time() const noexcept {
    return impl_ != nullptr ? impl_->last_present_host_time.load() : 0.0;
}

int64_t MetalRenderer::debug_gpu_error_count() const noexcept {
    return impl_ != nullptr ? impl_->gpu_errors.load() : 0;
}

void MetalRenderer::wait_until_idle() noexcept {
    if (impl_ == nullptr) return;
    debug_hold_gate_releases(false);   // 扣着的名额不放，下面会永远等不到零
    // 两个条件：槽全空（所有完成回调跑完——回调捕获裸 Impl*，这一条必须等到）与名额归零
    // （所有 presentedHandler 或兜底归还跑完）。
    //
    // 有界性：
    //   - 命令缓冲：先对每槽最近提交的命令缓冲 waitUntilCompleted。Metal 保证已提交的命令
    //     缓冲一定会完成（执行出错/GPU 超时也算完成并回调），之后完成回调随即跑完；
    //   - 名额：presentedHandler 丢失时，reclaim_stale_permits() 在各槽 reclaim_deadline
    //     后强制认领归还，所以最迟等到"最晚的兜底期限"。到期仍不归零
    //     （不应发生）打 ERROR 后返回，不挂死；迟到的回调认领失败、不碰 Impl。
    double latest_deadline = 0;
    for (auto& sl : impl_->slots) {
        if (sl.cmd != nil) [sl.cmd waitUntilCompleted];
        latest_deadline = std::max(latest_deadline, sl.reclaim_deadline);
    }
    const double deadline = latest_deadline + 0.1;
    using namespace std::chrono_literals;
    auto any_cmd_pending = [this] {
        for (const auto& p : impl_->cmd_pending)
            if (p.load()) return true;
        return false;
    };
    for (;;) {
        const double now = CACurrentMediaTime();
        impl_->reclaim_stale_permits(now);
        const bool cmd_busy = any_cmd_pending();
        if (!cmd_busy && impl_->gate.in_flight() == 0) break;
        if (!cmd_busy && now > deadline) {
            dl::log_msg(SYP_LOG_ERROR, "metal_renderer",
                        "wait_until_idle: 兜底期限已过在途名额仍未归零，放弃等待");
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    // 全部完成：释放各槽留着的对象（CVMetalTextureRef 不得活过 tex_cache）。
    for (auto& sl : impl_->slots) {
        sl.keep = nil;
        sl.cmd  = nil;
        if (sl.token != nil && [sl.token isClaimed]) sl.token = nil;
    }
}

void MetalRenderer::debug_set_permit_reclaim_after_ms(int32_t ms) noexcept {
    if (impl_ == nullptr) return;
    impl_->reclaim_after_s = ms > 0 ? static_cast<double>(ms) / 1e3 : Impl::kPermitReclaimAfterS;
}

void MetalRenderer::debug_suppress_presented_release(bool on) noexcept {
    if (impl_ != nullptr) impl_->suppress_presented_release = on;
}

int64_t MetalRenderer::debug_reclaimed_permit_count() const noexcept {
    return impl_ != nullptr ? impl_->reclaimed_permits.load() : 0;
}

void MetalRenderer::debug_hold_gate_releases(bool hold) noexcept {
    if (impl_ == nullptr) return;
    int32_t to_release = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->hold_mu);
        impl_->hold_releases.store(hold);
        if (!hold) {
            to_release           = impl_->held_releases;
            impl_->held_releases = 0;
        }
    }
    for (int32_t i = 0; i < to_release; ++i) impl_->gate.release();
}

int32_t MetalRenderer::debug_inflight_count() const noexcept {
    return impl_ != nullptr ? impl_->gate.in_flight() : 0;
}

int32_t MetalRenderer::debug_last_slot() const noexcept {
    return impl_ != nullptr ? impl_->last_slot : -1;
}

// ---------------------------------------------------------------------
// debug_copy_output_rgba()
// ---------------------------------------------------------------------

bool MetalRenderer::debug_copy_output_rgba(std::vector<uint8_t>& out, int32_t& width,
                                            int32_t& height) const {
    if (!ready() || !impl_->has_output || impl_->last_slot < 0) return false;
    // present() 不再等 GPU——回读前等最近一次提交完成，读它所用的槽。
    if (impl_->last_cmd != nil) [impl_->last_cmd waitUntilCompleted];
    id<MTLTexture> out_tex = impl_->slots[static_cast<size_t>(impl_->last_slot)].out;
    if (out_tex == nil) return false;

    width  = impl_->tex_width;
    height = impl_->tex_height;
    out.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    [out_tex getBytes:out.data()
                  bytesPerRow:static_cast<NSUInteger>(width) * 4
                   fromRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width),
                                               static_cast<NSUInteger>(height))
                  mipmapLevel:0];
    return true;
}

// ---------------------------------------------------------------------
// debug_blit_to_bgra()（测试缝）
// ---------------------------------------------------------------------

bool MetalRenderer::debug_blit_to_bgra(int32_t dst_w, int32_t dst_h,
                                       std::vector<uint8_t>& out) const {
    if (dst_w <= 0 || dst_h <= 0) return false;
    if (!ready() || !impl_->has_output || impl_->last_slot < 0) return false;
    // 同 debug_copy_output_rgba：present() 不等 GPU，先等最近一次提交完成再读它的槽。
    if (impl_->last_cmd != nil) [impl_->last_cmd waitUntilCompleted];
    id<MTLTexture> src = impl_->slots[static_cast<size_t>(impl_->last_slot)].out;
    if (src == nil) return false;

    // 与 drawable 同格式（blit 管线的颜色附件是 BGRA8Unorm）；Shared 以便 getBytes 回读，
    // 理由同 make_texture()。
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:static_cast<NSUInteger>(dst_w)
                                                           height:static_cast<NSUInteger>(dst_h)
                                                        mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> dst = [impl_->device newTextureWithDescriptor:desc];
    if (dst == nil) return false;

    const syp::media::BlitParams bp = current_blit_params(
        impl_->tex_width, impl_->tex_height, static_cast<double>(dst_w), static_cast<double>(dst_h));
    id<MTLCommandBuffer> cmd = [impl_->queue commandBuffer];
    encode_blit(cmd, impl_->blit_pipeline, src, dst, bp);   // 与生产 present() 同一个函数
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.status != MTLCommandBufferStatusCompleted) return false;

    out.assign(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 4, 0);
    [dst getBytes:out.data()
      bytesPerRow:static_cast<NSUInteger>(dst_w) * 4
       fromRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(dst_w),
                                   static_cast<NSUInteger>(dst_h))
      mipmapLevel:0];
    return true;
}

}  // namespace syp::platform
