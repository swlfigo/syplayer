#include "scenarios.h"

#include <unistd.h>       // ::getpid()

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <system_error>   // std::error_code
#include <thread>

#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>     // syp_set_http_backend 就在这里（syp_http.h:86）
#include <syplayer/syp_source.h>

#include "media/avio_bridge.h"
#include "platform/apple/apple_http_backend.h"

namespace syp::probe {

bool ensure_apple_backend() {
    static const bool ok = [] {
        return syp_set_http_backend(syp_apple_http_backend()) == SYP_OK;
    }();
    return ok;
}

std::string make_temp_cache_dir(const std::string& tag) {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() /
                      ("syp_probe_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base.string();
}

void remove_dir_recursive(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

namespace {

// 看门狗守卫（RAII）：到点调用 on_abort，析构无条件 join。
//
// 本项目没开 -fno-exceptions。旧写法是裸 std::thread + 手动
// done/join，如果 demux_avio 内部（std::string/std::vector 操作）抛出
// 异常，异常会在 watchdog 仍 joinable 时穿过这段作用域，std::thread
// 的析构器会调 std::terminate() —— 这跟"挂死必须变红、不能变成 CI 崩溃"
// 这条纪律直接相悖。RAII 把 join 挪进析构，任何路径（正常返回、
// 提前 return、异常展开）都保证先 join 再往下走。
class WatchdogGuard {
public:
    WatchdogGuard(int watchdog_ms, std::function<void()> on_abort)
        : thread_([this, watchdog_ms, cb = std::move(on_abort)] {
              const auto deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(watchdog_ms);
              while (!done_.load(std::memory_order_acquire)) {
                  if (std::chrono::steady_clock::now() >= deadline) {
                      fired_.store(true, std::memory_order_release);
                      cb();
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(20));
              }
          }) {}

    ~WatchdogGuard() { stop_and_join(); }

    WatchdogGuard(const WatchdogGuard&) = delete;
    WatchdogGuard& operator=(const WatchdogGuard&) = delete;

    // 可以提前显式调用（比如需要在还持有其它资源时就拿到 fired() 的结果）；
    // 析构里再调一次是安全的空操作（join 只能成功一次，用 joinable() 挡重复）。
    void stop_and_join() noexcept {
        done_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    bool fired() const noexcept { return fired_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> done_{false};
    std::atomic<bool> fired_{false};
    std::thread       thread_;
};

// 有界等待：把可能永久阻塞的调用包一层超时。
//
// `syp_source_close()` 的公开头写明"会阻塞等待内部线程退出"，
// 而这正是我们撞见过的那类 cancel/destroy 互等的调用形状（`ed1da0e`
// 修的是那一次具体成因，不代表这类调用形状以后不会再出问题）。
// `open`/`create` 是纯本地/异步操作，风险低，
// 犯不着为了它们把看门狗往前挪、平添 bridge 生命周期的复杂度；
// 真正值得单独兜底的是 close() 这一步。
//
// 若 fn 在 timeout_ms 内没返回：调用线程立刻放弃等待并返回 false，
// 探测线程原样留在后台跑（永久卡住也总比拖死整个测试进程强——
// 这条本任务明确记录为"不清理"的取舍之一）。
bool bounded_call(int timeout_ms, std::function<void()> fn) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([fn = std::move(fn), done] {
        fn();
        done->store(true, std::memory_order_release);
    });
    t.detach();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (!done->load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

// H3：on_error 回调可能在任意内部线程被调用（syp_source.h 的既有契约），
// 用一对原子字段记第一次收到的状态码——只取第一次，因为我们只关心
// "识别到了没有"，不关心后续是不是还有别的错误跟着。ctx 指向的对象
// 须存活到 syp_source_close() 返回，run_through_source() 里它是栈上
// 局部变量、生命周期跨过整个 open/read/close，满足这条约束。
struct ErrorCapture {
    std::atomic<bool>    got{false};
    std::atomic<int32_t> status{SYP_OK};
    std::atomic<int32_t> http_status{0};
};

void on_source_error(void* ctx, syp_status status, int32_t http_status) {
    auto* ec = static_cast<ErrorCapture*>(ctx);
    bool expected = false;
    if (ec->got.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ec->status.store(status, std::memory_order_relaxed);
        ec->http_status.store(http_status, std::memory_order_relaxed);
    }
}

}  // namespace

RunResult run_through_source(const RunSpec& spec) {
    RunResult out;

    if (!ensure_apple_backend()) {
        out.failure = "注册 Apple HTTP 后端失败";
        return out;
    }

    syp_config cfg;
    syp_config_init(&cfg);
    // 调用方要的调整（如钉死 min_segment_size/max_retries 做确定性构造）
    // 在默认值填好之后、struct_size/cache_dir 强制覆盖之前生效——后两个
    // 字段永远由 run_through_source 自己钉死，tweak_config 改了也不算数。
    if (spec.tweak_config) spec.tweak_config(cfg);
    cfg.struct_size = sizeof(syp_config);
    cfg.cache_dir = spec.cache_dir.c_str();

    // 堆分配而非栈上局部变量：下面 close() 那步若超时会把探测线程留在
    // 后台跑（既有取舍，见下方注释），此时 src 本身也被有意"泄漏"而非
    // 释放——回调仍可能在那之后继续触发。err_capture 若是栈变量，
    // run_through_source() 一旦返回它就失效，后台线程写进去就是 UAF。
    // 堆分配 + 只在确认 close() 真正返回后才 delete，让它和 src 的
    // 生命周期取舍保持一致（该泄漏就泄漏，不该用后就地释放）。
    auto* err_capture = new ErrorCapture();
    syp_source_callbacks cb{};
    cb.ctx = err_capture;
    cb.on_error = &on_source_error;

    syp_source* src = nullptr;
    const syp_status st = syp_source_open(&src, spec.url.c_str(), nullptr, &cfg, &cb);
    if (st != SYP_OK || src == nullptr) {
        out.failure = "syp_source_open 失败: " + std::string(syp_status_str(st));
        delete err_capture;  // open 失败：没有内部线程持有过 ctx，可以直接释放。
        return out;
    }

    auto bridge = syp::media::AvioBridge::create(src, 64 * 1024);
    if (!bridge) {
        syp_source_close(src);
        out.failure = "AvioBridge::create 失败";
        delete err_capture;  // 上面 syp_source_close 已同步返回，回调不会再触发。
        return out;
    }

    // 看门狗：到点就 request_abort()，把阻塞中的 read 打断，
    // 让 demux 带着 AVERROR_EXIT 返回，而不是永久挂住。
    //
    // 覆盖范围的真实边界（且被一次真实死锁验证过）：
    // 这个看门狗只能打断"FFmpeg 会轮询 interrupt_callback 的阻塞点"。
    // 我们撞见过的那次死锁恰恰不在这个范围内——主线程卡在
    // SourceBridge::read → apply_window → Scheduler::set_read_position
    // → reap_except → ~DLTask 这条链的析构等待里，FFmpeg 自己的
    // avio read 从未返回过、也就从来没有机会再去问一次 interrupt_callback。
    // 也就是说 request_abort() 在那种情况下发出去了，却永远等不到下一个
    // 检查点来读它。这不是这里能修的（dl 层内部同步阻塞点），只能确保
    // "万一又撞上"时至少不会把整个测试进程拖死——所以下面 close() 那步
    // 单独兜了一层有界等待，双保险而不是互相替代。
    WatchdogGuard watchdog(spec.watchdog_ms,
                          [b = bridge.get()] { b->request_abort(); });

    out.demux = demux_avio(bridge->ctx(), spec.opt, spec.seeks, bridge->interrupt_cb());

    watchdog.stop_and_join();
    if (watchdog.fired()) {
        out.failure = "看门狗触发：" + std::to_string(spec.watchdog_ms) + "ms 内没跑完（疑似挂死）";
    }

    syp_source_stats stats{};
    syp_source_get_stats(src, &stats);
    out.metrics.downloaded_bytes = stats.downloaded_bytes;
    out.metrics.cache_hit_bytes  = stats.cache_hit_bytes;
    out.metrics.cached_bytes     = stats.cached_bytes;
    out.metrics.failed_tasks     = stats.failed_tasks;
    out.metrics.redirect_count   = stats.redirect_count;

    // 先放 bridge 再关 source：bridge 的析构不碰 source，但顺序写死更省心。
    bridge.reset();

    // close() 单独兜一层有界等待：公开头写明它会阻塞等内部
    // 线程退出，这正是我们撞见过的那类互等挂死的调用形状。超时就不再等，
    // 把探测线程留在后台跑，函数本身必须能返回——不能因为 close() 卡死
    // 就把整条 ctest 拖到 TIMEOUT。
    const bool closed = bounded_call(spec.watchdog_ms, [src] { syp_source_close(src); });
    if (!closed) {
        const std::string msg = "syp_source_close 在 " + std::to_string(spec.watchdog_ms) +
                                "ms 内没返回（疑似挂死，探测线程已留在后台）";
        out.failure = out.failure.empty() ? msg : (out.failure + "；" + msg);
    }

    // 下面两条 load() 不管 closed 是 true 还是 false 都会执行——注释精度：
    // "close() 之后不会再有并发写入" 这句话只在 closed == true 时成立
    // （内部线程都已退出，此时读没有并发写在跑，delete 也才安全）；
    // closed == false（close() 超时）的路径上，后台线程可能仍在跑，
    // 这两个 load() 理论上可以跟 on_source_error() 里的 store() 并发。
    // 没有安全隐患：status/http_status 都是 std::atomic，load()/store()
    // 本身不会数据竞争，并发只影响读到的是新值还是旧值，不影响正确性。
    // err_capture 是否释放才是真正需要 closed==true 这个前提的地方——
    // 见下面 if (closed) delete，超时路径上它跟 src 一样被有意泄漏。
    out.error_status      = err_capture->status.load(std::memory_order_acquire);
    out.error_http_status = err_capture->http_status.load(std::memory_order_acquire);
    if (closed) delete err_capture;

    return out;
}

}  // namespace syp::probe
