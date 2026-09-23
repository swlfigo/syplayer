// watchdog.h — 用例自带的两级看门狗（软超时打诊断 / 硬超时 _exit）
//
// 挂死时给出明确的失败点与诊断，而不是把 ctest 拖到超时（超时的表现是日志里
// 连用例名都没有，因为 stdout 默认是块缓冲的；tiny_test.h 已经改成行缓冲，
// 但那只解决「看得见跑到哪条用例」，解决不了「进程要不要死、什么时候死」）。
//
// 软超时：打印诊断并把 fired() 置位，不做断言 —— 它是一个墙钟阈值，天生
// 非确定性，一次「正确但慢」的运行（40 路并行叠加回环 server 启动抖动）会把
// 它撞响。本项目最忌讳假红，用例的判据必须是确定性的 CHECK(...)，看门狗的
// 职责只是「挂死时让日志可读」，不是计时断言。
// 硬超时：连主线程都没能返回，直接 _exit 保证日志落地。
//
// 原本只长在 test_apple_http_backend.cpp 里（三条 destroy_in_on_complete_*
// 用例用它兜底）。test_scheduler.cpp 的 dl 层级重入回归用例
// （window_advance_reaps_task_reentrant_destroy_from_worker_thread）也需要
// 同一套安全网 —— 那条用例的 Scheduler/Harness/StubBackend 都是栈上局部量，
// 一旦真的挂死、用例又提前 return，还在摸这些对象的后台线程就会踩到析构后的
// 内存（真实 UAF）。见 block_until_hard_exit()。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace syp::test {

class Watchdog {
public:
    // soft_hint：软超时那条日志后面追加的用例专属提示（怎么读这次挂死），
    // 传 nullptr / "" 则不追加。
    Watchdog(const char* name, int soft_ms, int hard_ms, const char* soft_hint = nullptr)
        : name_(name), soft_ms_(soft_ms), hard_ms_(hard_ms),
          soft_hint_(soft_hint != nullptr ? soft_hint : ""),
          th_([this] { run(); }) {}

    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> g(mu_);
            done_ = true;
        }
        cv_.notify_all();
        th_.join();
    }

    bool fired() const { return fired_.load(std::memory_order_acquire); }

    // 挂死且「继续往下走会踩到 UAF」时用这条：不返回、不析构任何东西，
    // 把进程交给硬超时的 _exit()。调用方应当在此之前已经用
    // tiny_test::fail(...) 记下失败点（FAIL 行会先落地）。
    //
    // 为什么不是简单地 return：调用方栈上的 Scheduler/StubBackend 之类局部量
    // 一旦析构，还卡在它们内部的后台线程就会踩到已释放的内存。
    [[noreturn]] void block_until_hard_exit(const char* why) {
        std::fprintf(stdout,
                     "  WATCHDOG %s: %s —— 不再往下走（往下走会析构还被后台线程"
                     "持有的对象，那是真实 UAF），等硬超时 %d ms 强制退出。\n",
                     name_, why != nullptr ? why : "", hard_ms_);
        std::fflush(stdout);
        std::fflush(stderr);
        // 兜底：万一看门狗线程自己出了意外（比如被别处 join 掉），
        // 这里再给 hard_ms_ + 5s 的余量后自行退出，绝不无限期挂住 ctest。
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(hard_ms_ + 5000);
        std::unique_lock<std::mutex> lk(mu_);
        while (std::chrono::steady_clock::now() < deadline) {
            cv_.wait_for(lk, std::chrono::milliseconds(200));
        }
        std::fprintf(stdout,
                     "  WATCHDOG %s: 硬超时兜底路径触发，强制退出。\n", name_);
        std::fflush(stdout);
        _exit(70);
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(mu_);
        if (cv_.wait_for(lk, std::chrono::milliseconds(soft_ms_),
                         [this] { return done_; })) {
            return;
        }
        fired_.store(true, std::memory_order_release);
        std::fprintf(stdout, "  WATCHDOG %s: 卡住超过 %d ms。%s\n",
                     name_, soft_ms_, soft_hint_);
        std::fflush(stdout);
        std::fflush(stderr);
        if (cv_.wait_for(lk, std::chrono::milliseconds(hard_ms_ - soft_ms_),
                         [this] { return done_; })) {
            return;
        }
        std::fprintf(stdout,
                     "  WATCHDOG %s: 主线程也没能返回，硬超时 %d ms，强制退出。\n",
                     name_, hard_ms_);
        std::fflush(stdout);
        std::fflush(stderr);
        _exit(70);
    }

    const char*             name_;
    int                     soft_ms_;
    int                     hard_ms_;
    const char*             soft_hint_;
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    done_ = false;
    std::atomic<bool>       fired_{false};
    std::thread             th_;
};

}  // namespace syp::test
