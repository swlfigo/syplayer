// source_bridge.h — 把 Scheduler / CacheIndex / CacheFile 串成 syp_source
#pragma once

#include "cache_store.h"
#include "clock.h"
#include "scheduler.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>
#include <syplayer/syp_source.h>
#include <syplayer/syp_types.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace syp::dl {

// 对外 C ABI 的 C++ 实现。read 阻塞在 cv_ 上，等 on_data 落盘后再读 CacheFile。
//
// 线程：
//   read / seek 通常来自解复用线程；interrupt / close 可来自任意线程。
//   Scheduler 回调（on_data 等）来自 HTTP 后端线程，调用用户 cb 时不持 mu_。
//   析构 / close 会 interrupt 阻塞中的 read，再 stop 调度器，等内部回调退出。
//   缓存索引/内容文件由 CacheStore 共享：锁序 mu_ → *handle_.mu，
//   且 CacheStore::acquire/release 必须在**不持有 mu_** 时调用
//   （它们自己会取 CacheStore::mu_，那是另一棵树的叶子）。
class SourceBridge {
public:
    static std::expected<std::unique_ptr<SourceBridge>, syp_status> open(
        std::string url,
        const syp_headers* headers,
        const syp_config& cfg,
        const syp_source_callbacks* cb,
        const syp_http_backend* backend,
        Clock clock,
        // 限速类别：播放源一律 Playing；预加载条目由
        // Preloader 按优先级映射后传入。既有调用点不传即为 Playing。
        RateClass cls = RateClass::Playing);

    ~SourceBridge();

    SourceBridge(const SourceBridge&)            = delete;
    SourceBridge& operator=(const SourceBridge&) = delete;
    SourceBridge(SourceBridge&&)                 = delete;
    SourceBridge& operator=(SourceBridge&&)      = delete;

    int32_t read(uint8_t* buf, int32_t size);
    int64_t seek(int64_t offset, int32_t whence);
    int64_t length() const;
    void    interrupt();
    void    resume();
    void    update_playback(const syp_playback_state* st);
    int32_t cached_ranges(syp_range* out, int32_t max) const;
    void    get_stats(syp_source_stats* out) const;

    void close();  // 可重复调用；析构会走这里

    // 改限速准入类别（Preload ↔ Playing），转发给 Scheduler。
    //
    // 【锁外调】Scheduler::set_rate_class() 放锁后会在**调用线程上**同步跑
    // 一轮 schedule()，那一轮可能经 on_idle / on_error 回到本类、去拿 mu_。
    // 所以这里照 apply_window() 的体例：mu_ 下记下类别、拷出 Scheduler* 并
    // 登记 in_public_，放锁之后再转发（close() 会 drain in_public_ 再拆
    // Scheduler，不会 UAF）。sched_ 为空（已 close）时只记下来。
    //
    // 两条线程并发调用时，Scheduler 最终持有的类别取决于两次锁外转发的
    // 先后，不保证等于 rate_class_ 里最后写的那个——唯一的生产调用方
    // Preloader 只从它自己的驱动线程调，没有并发。
    void      set_rate_class(RateClass c);
    // 测试缝：读的是 Scheduler::rate_class()（schedule() 准入时真正用的那个
    // 成员），不是本类的 rate_class_ 记录。sched_ 为空时退回记录值。
    RateClass rate_class_for_test() const;
    // 测试缝：Scheduler **构造时**收到的类别（Scheduler::initial_rate_class_
    // for_test），不受事后 set_rate_class 影响。sched_ 为空时退回记录值。
    RateClass open_rate_class_for_test() const;

    // 测试用：调度器内存里 notify_persisted 之后的已缓存区间。
    HoleSet scheduler_cached_for_test() const;

private:
    struct HeaderCopy {
        std::vector<std::string> names;
        std::vector<std::string> values;
        std::vector<const char*> name_c;
        std::vector<const char*> value_c;

        void rebuild_ptrs();
        syp_headers view() const noexcept;
        void assign(const syp_headers* h);
    };

    struct SpeedSample {
        int64_t t_ms  = 0;
        int32_t bytes = 0;
    };

    SourceBridge(const syp_http_backend* backend, Clock clock,
                 syp_config cfg, syp_source_callbacks cb,
                 std::string url, HeaderCopy headers, RateClass cls);

    static void cb_on_data(void* ctx, int64_t offset, const uint8_t* data, int32_t len);
    static void cb_on_total(void* ctx, int64_t total);
    static void cb_on_validators(void* ctx, const char* etag, const char* last_modified);
    static void cb_on_error(void* ctx, syp_status st, int32_t http_status);
    static void cb_on_idle(void* ctx);

    void on_data(int64_t offset, const uint8_t* data, int32_t len);
    void on_total(int64_t total);
    void on_validators(const char* etag, const char* last_modified);
    void on_error(syp_status st, int32_t http_status);
    void on_idle();

    struct PendingChunk {
        int64_t offset = 0;
        std::vector<uint8_t> bytes;
    };

    // 公开方法在途：apply_window / get_stats 放锁后仍握着 Scheduler*。
    // close 在 sched_.reset() 之前 drain，避免 UAF。和 Scheduler 的
    // SlotCbGuard 同一模式（计数 + 析构路径 wait）。
    struct PublicOpGuard {
        const SourceBridge* self;
        explicit PublicOpGuard(const SourceBridge* s) : self(s) {}
        ~PublicOpGuard() {
            std::lock_guard<std::mutex> g(self->mu_);
            --self->in_public_;
            self->cv_.notify_all();
        }
        PublicOpGuard(const PublicOpGuard&)            = delete;
        PublicOpGuard& operator=(const PublicOpGuard&) = delete;
    };

    void        set_fatal_locked(syp_status st, int32_t http);
    int64_t     lookahead_bytes_locked() const noexcept;
    int64_t     now_ms() const noexcept;
    int64_t     speed_bps_locked() const noexcept;
    void        record_speed_locked(int32_t n) noexcept;
    void        apply_window();          // 不持 mu_：调 Scheduler
    void        save_index_locked();
    // 不持 mu_、更不持句柄锁：它要取 CacheStore::mu_，那是另一棵树的叶子锁。
    // 消费 save_index_locked 置的 want_enforce_，真正去扫目录做淘汰。
    void        maybe_enforce_capacity();
    // 在 mu_ 下调用：拷一份 CacheFile 的 shared_ptr 出来，供锁外的
    // read_at/write_at 使用。返回空表示已经 close 过。
    std::shared_ptr<CacheFile> file_ref_locked() const;
    void        fire_buffering(bool on);
    void        fire_cached_ranges();
    syp_status  start_scheduler(int64_t total, const HoleSet& already);
    bool        identity_ready_locked() const noexcept;
    void        persist_chunk(int64_t offset, const uint8_t* data, int32_t len);
    void        flush_pending(std::vector<PendingChunk> chunks);

    const syp_http_backend* backend_;
    Clock                   clock_;
    syp_config              cfg_{};
    syp_source_callbacks    cb_{};
    std::string             url_;
    std::string             cache_dir_;
    HeaderCopy              extra_headers_;

    // 索引与内容文件改为向 CacheStore 取一份共享的。
    // 同一个 cache key 在进程内只有一份，于是预加载与播放互相看得见对方
    // 下好的区间，save() 也不再互相整份覆盖（cache_store.h 顶部有完整背景）。
    //
    // 【锁序】mu_ → *handle_.mu。handle_.mu 是叶子锁，只保护 *handle_.index
    // 的全部访问与 sync+save 这一对的原子性；*handle_.file 的 read_at/
    // write_at 不需要它（cache_file.h 明写并发安全），只需要在 mu_ 下拷一份
    // shared_ptr 出来保证对象存活。
    CacheStore::Handle         handle_;
    std::unique_ptr<Scheduler> sched_;

    mutable std::mutex              mu_;
    mutable std::condition_variable cv_;

    int64_t pos_           = 0;
    int64_t total_length_  = -1;
    bool    interrupted_   = false;
    bool    closing_       = false;
    bool    closed_        = false;
    bool    buffering_     = false;
    bool    index_dirty_   = false;
    bool    seen_playback_ = false;
    bool    total_notified_ = false;
    // 索引里已有 etag/lm 时，等本会话走过一次 validate_and_update
    // （on_validators）再落盘。只提供与校验回调的顺序，不额外验身份。
    bool    need_validators_ = false;
    bool    seen_validators_ = false;
    bool    identity_ok_     = true;
    // 本源的限速类别。mu_ 保护；start_scheduler 用它填 SchedulerConfig，
    // set_rate_class 改它（sched_ 已建好时另行锁外转发）。
    RateClass  rate_class_ = RateClass::Playing;
    syp_status fatal_      = SYP_OK;
    int32_t    fatal_http_ = 0;
    mutable int in_public_ = 0;
    std::vector<PendingChunk> pending_;
    int64_t pending_bytes_ = 0;

    syp_playback_state play_{};

    int64_t downloaded_bytes_ = 0;
    int64_t cache_hit_bytes_  = 0;
    int32_t completed_tasks_  = 0;
    int32_t failed_tasks_     = 0;
    int64_t unsaved_bytes_    = 0;
    int64_t current_speed_bps_ = 0;

    // enforce_capacity 的节流：距上次 ≥ 1s 或自上次以来写入
    // ≥ 8MiB 才真的扫一遍目录。扫描是 O(目录条目数) 的 stat，放在每次
    // save_index 之后会在长时间下载里累出可观的开销。
    int64_t last_enforce_ms_     = 0;
    int64_t bytes_since_enforce_ = 0;
    bool    want_enforce_        = false;   // save_index_locked 置位，锁外消费

    std::deque<SpeedSample> speed_samples_;
};

int64_t    cache_dir_size(const char* cache_dir);
syp_status cache_dir_evict(const char* cache_dir, int64_t target_bytes);
syp_status cache_dir_clear(const char* cache_dir);
syp_status cache_dir_remove(const char* cache_dir, const char* url);

void       set_log_callback(syp_log_fn fn, void* ctx, syp_log_level max_level);
syp_status set_http_backend(const syp_http_backend* backend);
// 【返回的是内部副本的地址，不是调用方当初传进来的那个对象】本层只保存
// 一块 syp_http_backend 存储（source_bridge.cpp 的 g_backend），
// set_http_backend() 做的是 `g_backend = *backend`——**就地覆盖它**。
// 所以：要保存"旧后端"以便事后还原，必须**按值拷贝**
// （`syp_http_backend prev = *current_http_backend();`）；存下返回的指针
// 再 set_http_backend(prev) 是一次自赋值，什么也不会还原，而且此后全局
// 后端会一直指着调用方那个可能早已析构的对象。RAII 守卫的正确写法见
// tests/test_hls_e2e.cpp 的 HttpBackendGuard。
const syp_http_backend* current_http_backend();
void       log_msg(syp_log_level lvl, const char* tag, const char* msg);

syp_config default_config();
syp_config copy_config(const syp_config* cfg, syp_status* err);

}  // namespace syp::dl
