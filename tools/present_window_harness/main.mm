// present_window_harness — 真实窗口上屏压测（手动运行，不进 ctest）。
//
// 为什么需要它：ctest 里的 CAMetalLayer 不挂窗口，presentedHandler 在命令缓冲完成时
// 就回调，名额几乎立刻归还——"挂 layer 路径在途上限 2 + 名额在上屏后才归还 + 播放器
// 最多提前 40ms 提交"这个组合在真实窗口上会让 BUSY 成为常态，而 ctest 结构性地看不见。
// 本工具开一个 NSWindow + CAMetalLayer，按 60fps 合成帧、模拟 TrackPlayer 泵线程的
// 节奏（每 2ms 一轮，同 demo/shared/bridge.mm 的 usleep(2000)）驱动 MetalRenderer：
//   - 帧 i 的上屏时刻 = t0 + i/60 秒；
//   - diff = 上屏时刻 − 现在；diff > early_ms → 等；diff < −80ms → 迟到丢弃（lost）；
//     其余 present(f, max(0, diff))；
//   - retry 模式：BUSY → 帧保留、下一轮重试（TrackPlayer 现行为）；
//     discard 模式：BUSY → 帧丢弃（旧行为，作对照）。
// 输出每种组合的 presented / busy 返回次数 / lost。
//
// 构建：cmake --build build --target present_window_harness（EXCLUDE_FROM_ALL，默认不构建）
// 运行：./build/tools/present_window_harness [帧数，默认 600]
// 需要能开窗口的图形会话（锁屏/无显示器时 presentedHandler 不按显示节奏回调，数字无意义）。
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "platform/apple/metal_renderer.h"
#include "support/metal_layer_helper.h"

#include <syplayer/syp_types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

struct RunResult {
    int64_t presented = 0;
    int64_t busy      = 0;   // BUSY 返回次数（retry 模式下即重试次数）
    int64_t lost      = 0;   // 迟到丢弃 + discard 模式下的 BUSY 丢弃
    int64_t reclaimed = 0;   // 渲染器兜底回收的名额数（应为 0：presentedHandler 正常回调）
    int64_t max_call_us = 0; // 单次 present() 最长耗时（nextDrawable 阻塞会体现在这里）
};

int64_t now_us() { return static_cast<int64_t>(CACurrentMediaTime() * 1e6); }

RunResult run_once(void* layer, const std::vector<syp::media::Frame>& frames, int frame_count,
                   int64_t early_us, bool retry_on_busy) {
    syp::platform::MetalRenderer r;
    RunResult res;
    if (!r.ready()) {
        std::fprintf(stderr, "MetalRenderer 未就绪\n");
        return res;
    }
    r.set_output_layer(layer);   // layer 已在主线程预先配置好，任意线程可挂
    constexpr int64_t kFrameUs = 1'000'000 / 60;
    constexpr int64_t kDropUs  = 80'000;
    const int64_t t0 = now_us() + 100'000;
    int i = 0;
    while (i < frame_count) {
        const int64_t due  = t0 + static_cast<int64_t>(i) * kFrameUs + (i / 3);   // 60fps 精确到 µs
        const int64_t diff = due - now_us();
        if (diff > early_us) {
            std::this_thread::sleep_for(std::chrono::microseconds(2000));
            continue;
        }
        if (diff < -kDropUs) {
            ++res.lost;
            ++i;
            continue;
        }
        const int64_t    call0 = now_us();
        const syp_status st =
            r.present(frames[static_cast<size_t>(i) % frames.size()], std::max<int64_t>(0, diff));
        res.max_call_us = std::max(res.max_call_us, now_us() - call0);
        if (st == SYP_ERR_BUSY) {
            ++res.busy;
            if (retry_on_busy) {
                std::this_thread::sleep_for(std::chrono::microseconds(2000));
                continue;
            }
            ++res.lost;
            ++i;
            continue;
        }
        if (st != SYP_OK) std::fprintf(stderr, "present 失败 status=%d\n", static_cast<int>(st));
        ++res.presented;
        ++i;
    }
    r.wait_until_idle();
    res.reclaimed = r.debug_reclaimed_permit_count();
    r.set_output_layer(nullptr);
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        const int frame_count = argc > 1 ? std::max(1, std::atoi(argv[1])) : 600;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::fprintf(stderr, "无 Metal 设备\n");
            return 2;
        }
        NSRect   frame  = NSMakeRect(200, 200, 640, 360);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"present_window_harness";
        CAMetalLayer* layer    = [CAMetalLayer layer];
        layer.device           = device;
        layer.pixelFormat      = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly  = YES;
        layer.drawableSize     = CGSizeMake(640 * window.backingScaleFactor, 360 * window.backingScaleFactor);
        // 先设 layer 再 wantsLayer：layer-hosting view，CAMetalLayer 由我们自己持有与配置。
        window.contentView.layer      = layer;
        window.contentView.wantsLayer = YES;
        window.level = NSFloatingWindowLevel;   // 浮在其它窗口之上，尽量避免被遮挡
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        std::printf("window visible=%d screen=%s maximumDrawableCount=%lu frames=%d\n",
                    window.visible ? 1 : 0, window.screen != nil ? "yes" : "nil",
                    static_cast<unsigned long>(layer.maximumDrawableCount), frame_count);

        // 等窗口真正上屏再开始（被遮挡/不可见时合成器不刷新，presentedHandler 不按显示节奏回调）。
        for (int k = 0; k < 100; ++k) {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
        std::printf("occlusionVisible=%d isOnActiveSpace=%d\n",
                    (window.occlusionState & NSWindowOcclusionStateVisible) != 0 ? 1 : 0,
                    window.onActiveSpace ? 1 : 0);

        std::vector<syp::media::Frame> frames;
        frames.push_back(syp::test::make_nv12_frame(640, 360, 81, 90, 240, false, 0));
        frames.push_back(syp::test::make_nv12_frame(640, 360, 145, 54, 34, false, 0));
        frames.push_back(syp::test::make_nv12_frame(640, 360, 41, 240, 110, false, 0));

        void*             layer_ptr = (__bridge void*)layer;
        std::atomic<bool> done{false};
        std::thread pump([&] {
            struct Combo { int64_t early_us; bool retry; const char* name; };
            const Combo combos[] = {
                {40'000, false, "discard@40ms"}, {40'000, true, "retry@40ms"},
                {0, false, "discard@0ms"},       {0, true, "retry@0ms"},
            };
            for (const auto& c : combos) {
                const RunResult rr = run_once(layer_ptr, frames, frame_count, c.early_us, c.retry);
                std::printf("%-13s presented=%lld busy_returns=%lld lost=%lld reclaimed=%lld max_present_call=%.1fms\n",
                            c.name, static_cast<long long>(rr.presented), static_cast<long long>(rr.busy),
                            static_cast<long long>(rr.lost), static_cast<long long>(rr.reclaimed),
                            static_cast<double>(rr.max_call_us) / 1000.0);
                std::fflush(stdout);
            }
            done.store(true);
        });

        // 主线程跑 run loop：窗口合成与 presentedHandler 需要它转起来。
        while (!done.load()) {
            @autoreleasepool {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
        }
        pump.join();
        [window close];
    }
    return 0;
}
