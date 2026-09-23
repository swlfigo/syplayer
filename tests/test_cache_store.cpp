// test_cache_store.cpp — 同一 cache key 在进程内只有一份 CacheIndex/CacheFile。
//
// 这个套件盯的是这个 bug 的根：两个 syp_source 打开同一个 URL 时
// 各开一份 CacheIndex，save() 用 tmp+rename，**后写的把先写的区间表整份覆盖**。
// 注册表让两边指向同一个对象，覆盖就不存在了。
//
// 本文件只测注册表本身（不经 SourceBridge）：引用计数、同对象、并发 acquire、
// 释放后重新打开。SourceBridge 那一侧的回归用例在 test_source_bridge.cpp。
#include "tiny_test.h"

#include <dl/cache_index.h>
#include <dl/cache_store.h>
#include <dl/source_bridge.h>   // set_log_callback（日志回调重入用例）

#include <syplayer/syp_config.h>
#include <syplayer/syp_types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using syp::dl::CacheStore;
using syp::dl::Range;

namespace {

struct TempDir {
    std::filesystem::path p;
    TempDir() {
        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed);
        p = base / ("syp-store-" + std::to_string(::getpid()) + "-" + std::to_string(n));
        std::filesystem::create_directories(p, ec);
    }
    ~TempDir() {
        std::error_code ec;
        if (!p.empty()) std::filesystem::remove_all(p, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

syp_config cfg_for(const std::string& dir) {
    syp_config c{};
    syp_config_init(&c);
    c.cache_dir = dir.c_str();
    c.cache_ttl_ms = 0;          // 本套件不测 TTL
    c.max_cache_bytes = 0;       // 本套件不测容量
    c.min_free_space_bytes = 0;
    return c;
}

}  // namespace

TEST_CASE(same_key_returns_same_objects) {
    TempDir td;
    const std::string dir = td.p.string();
    const syp_config c = cfg_for(dir);

    syp_status e1 = SYP_OK;
    auto h1 = CacheStore::get().acquire(dir, "http://x/v.mp4", c, &e1);
    CHECK_EQ(e1, SYP_OK);
    REQUIRE(h1.valid());

    syp_status e2 = SYP_OK;
    auto h2 = CacheStore::get().acquire(dir, "http://x/v.mp4", c, &e2);
    CHECK_EQ(e2, SYP_OK);
    REQUIRE(h2.valid());

    CHECK(h1.index.get() == h2.index.get());
    CHECK(h1.file.get() == h2.file.get());
    CHECK(h1.mu.get() == h2.mu.get());
    CHECK_EQ(h1.key, h2.key);
    CHECK_EQ(CacheStore::get().open_count(h1.key), static_cast<int64_t>(2));

    // 一边写、另一边立刻看得见 —— 这就是当前 bug 的反面。
    {
        std::lock_guard<std::mutex> g(*h1.mu);
        h1.index->add_range(Range{0, 1024});
    }
    {
        std::lock_guard<std::mutex> g(*h2.mu);
        CHECK(h2.index->ranges().contains(Range{0, 1024}));
    }

    CacheStore::get().release(h1.key);
    CHECK_EQ(CacheStore::get().open_count(h1.key), static_cast<int64_t>(1));
    CacheStore::get().release(h2.key);
    CHECK_EQ(CacheStore::get().open_count(h1.key), static_cast<int64_t>(0));
}

TEST_CASE(different_urls_and_dirs_are_different_keys) {
    TempDir td1;
    TempDir td2;
    const std::string d1 = td1.p.string();
    const std::string d2 = td2.p.string();
    const syp_config c1 = cfg_for(d1);
    const syp_config c2 = cfg_for(d2);

    syp_status err = SYP_OK;
    auto a = CacheStore::get().acquire(d1, "http://x/a.mp4", c1, &err);
    auto b = CacheStore::get().acquire(d1, "http://x/b.mp4", c1, &err);
    auto c = CacheStore::get().acquire(d2, "http://x/a.mp4", c2, &err);
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(c.valid());
    CHECK(a.index.get() != b.index.get());
    // 同 URL、不同缓存目录：key 必须不同，否则换目录会读到别处的索引。
    CHECK(a.key != c.key);
    CHECK(a.index.get() != c.index.get());

    CacheStore::get().release(a.key);
    CacheStore::get().release(b.key);
    CacheStore::get().release(c.key);
}

// 【cache key 是逐字节的字符串相等，不是路径等价】make_key 不做任何规范化
// （cache_store.h:83-86 写明了，这是有意的：realpath 对不存在的目录会失败）。
// 后果必须被钉死而不是靠人眼记住：同一批文件的两种目录写法会算出两个 key，
// 于是同一个 URL 又变成两份 CacheIndex —— 那个整份覆盖 bug 原样复活。
//
// 【谁来保证"逐字相同"】
// 不再靠"调用方碰巧传的是同一份字符串"，而是靠 syp::dl::normalize_cache_dir()：
// SourceBridge 与 Preloader 的**构造函数**（也就是 cache_dir_ 诞生的那一行）
// 各调它一次，所以 syp_source_open / syp_preloader_create 这两个 C ABI 入口
// 之后，拼法差异就已经被折平了。在那之前 C ABI 是一条没有闸的路：直接用
// C ABI 的调用方混用 "…/d" 与 "…//d" 会静默分裂成两份缓存。
//
// **归一化刻意不放进 make_key 里**（cache_store.h 里 make_key 上方有实测：
// 放进去 enforce_capacity 手工拼的那个 key 会与它分叉，把还开着的条目删掉；
// 而且本用例与下一条会红 5 条断言）。所以下面这三条仍然是"坑"：谁哪天给
// make_key 加了规范化，这里会红，请连同本注释与 cache_store.h 那段一起重写。
TEST_CASE(make_key_requires_byte_identical_dir_string) {
    const std::string dir = "/tmp/syp-cache-keytest";
    const std::string url = "http://x/v.mp4";
    const std::string k = CacheStore::make_key(dir, url);

    // 同一串字节 → 同一个 key（哪怕是两个不同的 std::string 对象）。
    CHECK_EQ(k, CacheStore::make_key(std::string(dir), url));

    // 路径等价但写法不同 → **不同**的 key。这几条是"坑"，不是"特性"：
    // 谁哪天给 make_key 加了规范化，这里会红，请连同本注释一起重写。
    CHECK(CacheStore::make_key(dir + "/", url) != k);
    CHECK(CacheStore::make_key(dir + "/.", url) != k);
    CHECK(CacheStore::make_key(dir + "//", url) != k);

    // key 的形状：cache_dir 原样 + US(0x1f) + CacheIndex 的 16 位小写 hex。
    const std::string hex = syp::dl::CacheIndex::key_for_url(url);
    REQUIRE(k.size() == dir.size() + 1 + hex.size());
    CHECK(k.compare(0, dir.size(), dir) == 0);
    CHECK(k[dir.size()] == '\x1f');
    CHECK(k.compare(dir.size() + 1, hex.size(), hex) == 0);
}

// 把上一条的后果做实：带尾斜杠的目录拿到的是**另一份** CacheIndex 对象，
// 而它落盘的却是同一个 .idx 文件 —— 两份内存索引对着一个文件 tmp+rename，
// 正是那个覆盖 bug。注册表挡不住这种"两种写法"的调用方；
// 唯一的防线是调用方传同一份字符串（上一条注释里那几个来源）。
TEST_CASE(trailing_slash_dir_resurrects_the_split_index) {
    TempDir td;
    const std::string dir  = td.p.string();
    const std::string dir2 = dir + "/";
    const syp_config c1 = cfg_for(dir);
    const syp_config c2 = cfg_for(dir2);

    syp_status err = SYP_OK;
    auto a = CacheStore::get().acquire(dir,  "http://x/v.mp4", c1, &err);
    auto b = CacheStore::get().acquire(dir2, "http://x/v.mp4", c2, &err);
    REQUIRE(a.valid());
    REQUIRE(b.valid());

    CHECK(a.key != b.key);
    CHECK(a.index.get() != b.index.get());   // 两份内存索引……
    // ……却指着同一个磁盘文件。std::filesystem::path 的 '/' 拼接把
    // "d" / "k.idx" 与 "d/" / "k.idx" 归一成同一条路径。
    CHECK_EQ(a.index->index_path().string(), b.index->index_path().string());
    CHECK_EQ(a.index->data_path().string(),  b.index->data_path().string());

    CacheStore::get().release(a.key);
    CacheStore::get().release(b.key);
}

TEST_CASE(refcount_zero_then_reacquire_reads_from_disk) {
    TempDir td;
    const std::string dir = td.p.string();
    const syp_config c = cfg_for(dir);

    syp_status err = SYP_OK;
    {
        auto h = CacheStore::get().acquire(dir, "http://x/v.mp4", c, &err);
        REQUIRE(h.valid());
        // 必须真的把 512 字节写进 .dat 再记区间：CacheIndex::open() 会拿
        // .dat 的实际大小裁剪区间（cache_index.h:37-41），只 add_range 不落字节
        // 的话重开时 [0,512) 整段超出 dat_size=0，会被丢掉。写入顺序也正是
        // cache_index.h:30-35 规定的 write_at → sync → add_range → save。
        const std::vector<uint8_t> bytes(512, 0xAB);
        REQUIRE(h.file->write_at(0, std::span<const uint8_t>(bytes)).has_value());
        REQUIRE(h.file->sync().has_value());
        std::lock_guard<std::mutex> g(*h.mu);
        h.index->add_range(Range{0, 512});
        REQUIRE(h.index->save().has_value());
    }
    CacheStore::get().release(CacheStore::make_key(dir, "http://x/v.mp4"));
    CHECK_EQ(CacheStore::get().open_count(CacheStore::make_key(dir, "http://x/v.mp4")),
             static_cast<int64_t>(0));

    auto h2 = CacheStore::get().acquire(dir, "http://x/v.mp4", c, &err);
    REQUIRE(h2.valid());
    {
        std::lock_guard<std::mutex> g(*h2.mu);
        CHECK(h2.index->ranges().contains(Range{0, 512}));
    }
    CacheStore::get().release(h2.key);
}

TEST_CASE(concurrent_acquire_same_key_opens_once) {
    TempDir td;
    const std::string dir = td.p.string();
    const syp_config c = cfg_for(dir);

    constexpr int kThreads = 8;
    std::vector<CacheStore::Handle> hs(kThreads);
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            syp_status err = SYP_OK;
            hs[static_cast<size_t>(i)] =
                CacheStore::get().acquire(dir, "http://x/race.mp4", c, &err);
        });
    }
    for (auto& t : ts) t.join();

    REQUIRE(hs[0].valid());
    for (int i = 1; i < kThreads; ++i) {
        CHECK(hs[static_cast<size_t>(i)].valid());
        CHECK(hs[static_cast<size_t>(i)].index.get() == hs[0].index.get());
    }
    CHECK_EQ(CacheStore::get().open_count(hs[0].key), static_cast<int64_t>(kThreads));
    for (int i = 0; i < kThreads; ++i) CacheStore::get().release(hs[0].key);
    CHECK_EQ(CacheStore::get().open_count(hs[0].key), static_cast<int64_t>(0));
}

TEST_CASE(release_unknown_key_is_noop) {
    CacheStore::get().release("no-such-key");
    CHECK_EQ(CacheStore::get().open_count("no-such-key"), static_cast<int64_t>(0));
}

TEST_CASE(scan_cache_dir_groups_idx_and_dat) {
    TempDir td;
    const std::string dir = td.p.string();
    const syp_config c = cfg_for(dir);
    syp_status err = SYP_OK;
    auto h = CacheStore::get().acquire(dir, "http://x/v.mp4", c, &err);
    REQUIRE(h.valid());
    {
        std::lock_guard<std::mutex> g(*h.mu);
        h.index->add_range(Range{0, 8});
        REQUIRE(h.index->save().has_value());
    }
    CacheStore::get().release(h.key);

    syp_status serr = SYP_OK;
    const auto entries = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    REQUIRE(entries.size() == 1);
    CHECK_EQ(entries[0].key, syp::dl::CacheIndex::key_for_url("http://x/v.mp4"));
    CHECK(entries[0].bytes > 0);
}

namespace {

// 把一个条目的**三个文件**（.idx / .dat / .idx.tmp）的 mtime 一起设成过去
// 某一刻：TTL 与 LRU 用的都是文件 mtime，所以用例能完全确定地构造"多久没
// 动过"，不需要 sleep，也不依赖墙钟。
//
// 【为什么必须三个一起设，而不是只设 .idx】只设 .idx 的话，用例对"排序键
// 到底取哪个文件"就失去了鉴别力——这里真的错过一次：scan_cache_dir 的
// mtime 曾经是三个文件的 max，.dat 的时间盖过 back-date 过的 .idx，于是
// "把两条的时间对调"照样通过，两条 LRU 用例实际上什么都没验。三个一起设，
// 对调就必然红。
void set_entry_mtime_ago(const std::filesystem::path& idx, int64_t ms_ago) {
    const auto now  = std::filesystem::file_time_type::clock::now();
    const auto when = now - std::chrono::milliseconds(ms_ago);
    std::filesystem::path stem = idx;
    stem.replace_extension();                       // 去掉 ".idx"
    const std::filesystem::path all[] = {
        idx,
        std::filesystem::path(stem.string() + ".dat"),
        std::filesystem::path(idx.string() + ".tmp"),
    };
    int touched = 0;
    for (const auto& p : all) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec) || ec) continue;
        std::filesystem::last_write_time(p, when, ec);
        CHECK(!ec);
        if (!ec) ++touched;
    }
    CHECK(touched >= 2);    // .idx 与 .dat 至少都在（.idx.tmp 正常情况下没有）
}

// 只动一个文件的 mtime（用来构造"三个文件时间不一致"的判别用例）。
void set_file_mtime_ago(const std::filesystem::path& p, int64_t ms_ago) {
    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(p, now - std::chrono::milliseconds(ms_ago), ec);
    CHECK(!ec);
}

std::filesystem::path dat_of(const std::filesystem::path& idx) {
    std::filesystem::path d = idx;
    d.replace_extension(".dat");
    return d;
}

// 造一个占 bytes 字节的缓存条目并落盘；返回它的 .idx 路径。
//
// 【这里只能用 CHECK，不能用 REQUIRE】REQUIRE 展开成裸 `return;`
// （tiny_test.h:97-103），放在非 void 的辅助函数里会直接编译不过。
// 所以断言用 CHECK（记录失败但继续），再手动早退，避免解引用无效句柄。
std::filesystem::path make_entry(const std::string& dir, const std::string& url,
                                 int64_t bytes) {
    syp_config c = cfg_for(dir);
    syp_status err = SYP_OK;
    auto h = CacheStore::get().acquire(dir, url, c, &err);
    CHECK(h.valid());
    if (!h.valid()) return {};
    std::vector<uint8_t> blob(static_cast<size_t>(bytes), 0x5a);
    {
        std::lock_guard<std::mutex> g(*h.mu);
        CHECK(h.file->write_at(0, std::span<const uint8_t>(blob.data(), blob.size()))
                  .has_value());
        h.index->add_range(Range{0, bytes});
        CHECK(h.index->save().has_value());
    }
    const auto idx = h.index->index_path();
    CacheStore::get().release(h.key);
    return idx;
}

// 造一个**真的带空洞**的条目：头上落 1 个字节，然后把 .dat 撑到 logical
// 字节长。于是逻辑长度是 logical（scan_cache_dir 用 file_size 看到的就是它），
// 实际占用的块只有一个——正是"按 file_size 记账会把自己骗死"的那种文件。
//
// 【为什么不用"直接在高偏移处 pwrite"】在 APFS 上那样写会把中间整段**真的
// 分配出来**（实测：在 8MiB 处写 1 字节，占用也是 8MiB），造不出空洞。
// 先写头、再 resize_file 撑长度才留得下洞（实测占用 4096）。
std::filesystem::path make_sparse_entry(const std::string& dir, const std::string& url,
                                        int64_t logical) {
    syp_config c = cfg_for(dir);
    syp_status err = SYP_OK;
    auto h = CacheStore::get().acquire(dir, url, c, &err);
    CHECK(h.valid());
    if (!h.valid()) return {};
    const uint8_t one = 0x5a;
    {
        std::lock_guard<std::mutex> g(*h.mu);
        CHECK(h.file->write_at(0, std::span<const uint8_t>(&one, 1)).has_value());
        h.index->add_range(Range{0, 1});
        CHECK(h.index->save().has_value());
    }
    const auto idx = h.index->index_path();
    CacheStore::get().release(h.key);
    std::error_code ec;
    std::filesystem::resize_file(dat_of(idx), static_cast<uintmax_t>(logical), ec);
    CHECK(!ec);
    return idx;
}

// .dat 实际占用的块字节数（不是逻辑长度）。用来确认"稀疏"这个前提真的成立。
int64_t allocated_bytes(const std::filesystem::path& p) {
    struct ::stat st {};
    if (::stat(p.c_str(), &st) != 0) return -1;
    return static_cast<int64_t>(st.st_blocks) * 512;
}

}  // namespace

TEST_CASE(ttl_expired_entry_is_discarded_on_first_acquire) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto idx = make_entry(dir, "http://x/old.mp4", 4096);
    set_entry_mtime_ago(idx, 10000);

    syp_config c = cfg_for(dir);
    c.cache_ttl_ms = 5000;          // 10s 前动过 > 5s TTL → 过期
    syp_status err = SYP_OK;
    auto h = CacheStore::get().acquire(dir, "http://x/old.mp4", c, &err);
    CHECK_EQ(err, SYP_OK);
    REQUIRE(h.valid());
    {
        std::lock_guard<std::mutex> g(*h.mu);
        CHECK(h.index->ranges().empty());     // 当成全新资源
    }
    CacheStore::get().release(h.key);
}

TEST_CASE(ttl_fresh_entry_survives) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto idx = make_entry(dir, "http://x/fresh.mp4", 4096);
    set_entry_mtime_ago(idx, 1000);

    syp_config c = cfg_for(dir);
    c.cache_ttl_ms = 5000;
    syp_status err = SYP_OK;
    auto h = CacheStore::get().acquire(dir, "http://x/fresh.mp4", c, &err);
    REQUIRE(h.valid());
    {
        std::lock_guard<std::mutex> g(*h.mu);
        CHECK(h.index->ranges().contains(Range{0, 4096}));
    }
    CacheStore::get().release(h.key);
}

TEST_CASE(enforce_capacity_evicts_lru_first) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto old_idx = make_entry(dir, "http://x/a.mp4", 64 * 1024);
    const auto new_idx = make_entry(dir, "http://x/b.mp4", 64 * 1024);
    set_entry_mtime_ago(old_idx, 60000);
    set_entry_mtime_ago(new_idx, 1000);

    syp_config c = cfg_for(dir);
    c.max_cache_bytes = 96 * 1024;   // 两条加起来 128KiB+，必须走掉一条
    // 【可用空间这一支开着但一开始就满足】此时它必须完全不参与判定：一轮只为
    // max_cache_bytes 干活、而且干成了，就得报 SYP_OK。若最终那次 statvfs 复核
    // 无条件跑，别的进程中途吃掉盘就会把这一轮判成 NO_SPACE——报了一个与本轮
    // 目标毫不相干的失败。（真正的竞态要另一个进程配合，做不成确定性用例；
    // 这条守的是"本来就满足时别去多此一举地量"。）
    c.min_free_space_bytes = 1024;
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_OK);

    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    REQUIRE(left.size() == 1);
    CHECK_EQ(left[0].key, syp::dl::CacheIndex::key_for_url("http://x/b.mp4"));
}

TEST_CASE(enforce_capacity_skips_referenced_keys) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto old_idx = make_entry(dir, "http://x/hot.mp4", 64 * 1024);
    const auto new_idx = make_entry(dir, "http://x/cold.mp4", 64 * 1024);
    set_entry_mtime_ago(old_idx, 60000);   // hot 是 LRU 里最该走的那个……
    set_entry_mtime_ago(new_idx, 1000);

    syp_config c = cfg_for(dir);
    syp_status err = SYP_OK;
    auto hot = CacheStore::get().acquire(dir, "http://x/hot.mp4", c, &err);
    REQUIRE(hot.valid());            // ……但它正被打开着

    c.max_cache_bytes = 96 * 1024;
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_OK);

    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    REQUIRE(left.size() == 1);
    // 被引用的 hot 必须还在，走的是没人用的 cold。
    CHECK_EQ(left[0].key, syp::dl::CacheIndex::key_for_url("http://x/hot.mp4"));
    // 被引用的条目只花一次 map::find，**不算一次删除尝试**——它一个系统调用
    // 都没发。看了 2 条（hot 跳过、cold 删掉），只尝试了 1 次。
    const auto st = CacheStore::get().last_round_stats_for_test();
    CHECK_EQ(st.examined, static_cast<int64_t>(2));
    CHECK_EQ(st.attempts, static_cast<int64_t>(1));
    CHECK_EQ(st.deleted,  static_cast<int64_t>(1));
    CacheStore::get().release(hot.key);
}

namespace {

// 两条路径是不是**同一个目录**（dev+ino）。这是下面那条用例每一类拼法的
// **前提检验**：拼法 B 必须真的指向拼法 A 那个目录，否则断言测的根本不是
// F1（大小写折叠、Unicode 归一化都取决于卷的属性，不能假定）。
bool same_dir(const std::string& a, const std::string& b) {
    struct ::stat sa {};
    struct ::stat sb {};
    if (::stat(a.c_str(), &sa) != 0) return false;
    if (::stat(b.c_str(), &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// 用拼法 A 打开一个条目并**保持 open**，用拼法 B 触发一轮淘汰，
// 断言这一组 .idx/.dat 一个字节都没被动过。
//
// 【为什么不能只断言"文件还在"】还要断言 attempts == 0：被引用的条目在
// 这一轮里连一次 unlink 都不该发出（头文件写的是"被引用的 key 只花一次
// map::find"）。只看文件在不在的话，"先 unlink 失败再报错"这种实现也能
// 蒙混过关。
void expect_live_entry_survives_eviction(const char* label,
                                         const std::string& dir_a,
                                         const std::string& dir_b) {
    std::printf("    [%s] A=%s B=%s\n", label, dir_a.c_str(), dir_b.c_str());
    const std::string url = "http://x/live.mp4";
    syp_config ca = cfg_for(dir_a);
    syp_status err = SYP_OK;
    auto h = CacheStore::get().acquire(dir_a, url, ca, &err);
    CHECK_EQ(err, SYP_OK);
    REQUIRE(h.valid());
    {
        std::lock_guard<std::mutex> g(*h.mu);
        std::vector<uint8_t> blob(200000, 0x5a);
        CHECK(h.file->write_at(0, std::span<const uint8_t>(blob.data(), blob.size()))
                  .has_value());
        h.index->add_range(Range{0, static_cast<int64_t>(blob.size())});
        CHECK(h.index->save().has_value());
    }
    const auto idx = h.index->index_path();
    const auto dat = h.index->data_path();
    CHECK_EQ(CacheStore::get().open_count(h.key), static_cast<int64_t>(1));

    syp_config cb = cfg_for(dir_b);
    cb.max_cache_bytes = 1000;       // 远低于 200KB：这一轮必须想删点什么
    const syp_status st = CacheStore::get().enforce_capacity(dir_b, cb);

    std::error_code ec;
    CHECK(std::filesystem::exists(idx, ec));
    CHECK(std::filesystem::exists(dat, ec));
    // 唯一的条目正被引用 ⇒ 名单走完了也腾不出来 ⇒ NO_SPACE。
    // 修复前这里是 SYP_OK（"删干净了"），而它删掉的正是那份活文件。
    CHECK_EQ(st, SYP_ERR_NO_SPACE);
    const auto rs = CacheStore::get().last_round_stats_for_test();
    CHECK_EQ(rs.examined, static_cast<int64_t>(1));
    CHECK_EQ(rs.attempts, static_cast<int64_t>(0));   // 一次 unlink 都不该发
    CHECK_EQ(rs.deleted,  static_cast<int64_t>(0));
    CacheStore::get().release(h.key);
}

}  // namespace

// 【enforce_capacity 删掉还开着的文件——本仓最严重的一类 bug】
//
// enforce_capacity 手上只有文件名里的哈希，原先它**手工拼**
// `cache_dir + US + hash` 去注册表里问"这个 key 还有没有人开着"。而
// normalize_cache_dir 只做词法归一，于是同一个目录的下面五类拼法归一化之后
// 仍然是两个 key，scan_cache_dir 拼出来的文件路径却**是同一组文件**
// ⇒ 查不到活条目 ⇒ 直接 unlink。
//
// 修复前本机实测（五类全中，open_count 仍为 1，返回值还是 SYP_OK）：
//     (a) abs-vs-rel        idx=GONE dat=GONE
//     (b) case              idx=GONE dat=GONE
//     (c) var-vs-privatevar idx=GONE dat=GONE
//     (d) symlink-dir       idx=GONE dat=GONE
//     (e) nfc-vs-nfd        idx=GONE dat=GONE
// 修复后五类全部 PRESENT、返回 SYP_ERR_NO_SPACE。
//
// (c) 这一类**不需要调用方犯任何错**：`/var → private/var` 是系统软链，
// NSTemporaryDirectory() 给 `/var/…` 而 resolvingSymlinksInPath()/realpath()
// 给 `/private/var/…`——这是 Foundation 自己对同一个目录给出的两个答案。
//
// 【(b)/(c)/(e) 为什么允许跳过】大小写折叠与 Unicode 归一化是**卷的属性**
// （APFS 默认不敏感，但可以格成敏感卷），`/var` 软链是 macOS 的事。所以每
// 一类先用 dev+ino 检验"这两种拼法真的指同一个目录"，不成立就跳过并打印。
// (a) 与 (d) 在任何 POSIX 文件系统上都成立，它们保证这条用例始终有鉴别力
// （把 live_here 那一道判定删掉 ⇒ 这条用例红在 5 类里的每一类）。
TEST_CASE(enforce_capacity_spares_a_live_entry_under_another_spelling) {
    // (a) 绝对 vs 相对 —— cache_store.h 自己就写着"C ABI 调用方完全可能写
    //     cfg.cache_dir = "cache""。lexically_normal 永远不会去拿 cwd 补前缀。
    {
        TempDir td;
        const std::string abs = (td.p / "ra").string();
        std::error_code ec;
        std::filesystem::create_directories(abs, ec);
        std::vector<char> cwd(4096, '\0');
        const bool got_cwd = ::getcwd(cwd.data(), cwd.size()) != nullptr;
        CHECK(got_cwd);
        if (got_cwd && ::chdir(td.p.c_str()) == 0) {
            expect_live_entry_survives_eviction("a/abs-vs-rel", abs, "ra");
            CHECK(::chdir(cwd.data()) == 0);   // 还回去：别的用例都用绝对路径，
                                               // 但 TempDir 的 remove_all 也是
        } else {
            std::printf("    [a/abs-vs-rel] SKIPPED (chdir failed)\n");
        }
    }
    // (b) APFS 默认大小写不敏感（保留大小写）。
    {
        TempDir td;
        const std::string lower = (td.p / "cachedir").string();
        const std::string upper = (td.p / "CACHEDIR").string();
        std::error_code ec;
        std::filesystem::create_directories(lower, ec);
        if (same_dir(lower, upper)) {
            expect_live_entry_survives_eviction("b/case", lower, upper);
        } else {
            std::printf("    [b/case] SKIPPED (case-sensitive volume)\n");
        }
    }
    // (c) /var vs /private/var。TempDir 落在 temp_directory_path() 下，
    //     macOS 上就是 /var/folders/…。
    {
        TempDir td;
        const std::string a = (td.p / "vv").string();
        std::error_code ec;
        std::filesystem::create_directories(a, ec);
        const std::string b = "/private" + a;
        if (a.starts_with("/var/") && same_dir(a, b)) {
            expect_live_entry_survives_eviction("c/var-vs-privatevar", a, b);
        } else {
            std::printf("    [c/var-vs-privatevar] SKIPPED (tmp not under /var)\n");
        }
    }
    // (d) 目录本身是符号链接。任何 POSIX 文件系统上都成立。
    {
        TempDir td;
        const std::string real = (td.p / "realdir").string();
        const std::string link = (td.p / "linkdir").string();
        std::error_code ec;
        std::filesystem::create_directories(real, ec);
        std::filesystem::create_directory_symlink(real, link, ec);
        REQUIRE(!ec);
        expect_live_entry_survives_eviction("d/symlink-dir", real, link);
    }
    // (e) Unicode NFC vs NFD。cache_store.h 的"不做 Unicode 归一化"那条说
    //     残留风险只在绕过 Swift 门面的路径上——那句话对**缓存分裂**是对的，
    //     对"删掉活文件"是错的，这里把它钉住。
    {
        TempDir td;
        const std::string nfc = (td.p / "caf\xc3\xa9").string();        // U+00E9
        const std::string nfd = (td.p / "cafe\xcc\x81").string();       // e + U+0301
        std::error_code ec;
        std::filesystem::create_directories(nfc, ec);
        if (same_dir(nfc, nfd)) {
            expect_live_entry_survives_eviction("e/nfc-vs-nfd", nfc, nfd);
        } else {
            std::printf("    [e/nfc-vs-nfd] SKIPPED (volume preserves both forms)\n");
        }
    }
}

TEST_CASE(enforce_capacity_is_noop_when_unlimited) {
    TempDir td;
    const std::string dir = td.p.string();
    (void)make_entry(dir, "http://x/a.mp4", 32 * 1024);
    (void)make_entry(dir, "http://x/b.mp4", 32 * 1024);

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = 0;      // 不限
    c.min_free_space_bytes = 0;      // 不看可用空间
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_OK);

    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(left.size(), static_cast<size_t>(2));
}

// 【min_free_space_bytes 这一支是可测的，而且必须测】把门槛设到真实可用
// 空间之上，这一支就必然触发，且无论删多少都达不到——正好把两件事同时钉住：
//   1. 达不到的时候也**绝不能**碰正被引用的 key。低磁盘设备上这是"正在播的
//      资源不会被自己的淘汰轮次抽掉"的唯一防线；
//   2. 腾不够时返回值要说实话（SYP_ERR_NO_SPACE），预加载据此减产。
TEST_CASE(enforce_capacity_low_volume_spares_referenced_key_and_reports) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto hot_idx   = make_entry(dir, "http://x/hot.mp4",   32 * 1024);
    const auto cold1_idx = make_entry(dir, "http://x/cold1.mp4", 32 * 1024);
    const auto cold2_idx = make_entry(dir, "http://x/cold2.mp4", 32 * 1024);
    // hot 是最旧的：LRU 第一个就该轮到它，被引用才是它活下来的唯一理由。
    set_entry_mtime_ago(hot_idx,   60000);
    set_entry_mtime_ago(cold1_idx, 30000);
    set_entry_mtime_ago(cold2_idx,  1000);

    syp_config c = cfg_for(dir);
    syp_status err = SYP_OK;
    auto hot = CacheStore::get().acquire(dir, "http://x/hot.mp4", c, &err);
    REQUIRE(hot.valid());

    c.max_cache_bytes      = 0;   // 只走"可用空间"这一支
    c.min_free_space_bytes = std::numeric_limits<int64_t>::max();
    // 没撞条目数上界（只有 3 条），但怎么删都够不着门槛 → NO_SPACE，不是 BUSY。
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);

    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    REQUIRE(left.size() == 1);
    CHECK_EQ(left[0].key, syp::dl::CacheIndex::key_for_url("http://x/hot.mp4"));
    CacheStore::get().release(hot.key);
}

// 【一轮是有界的】没有上界时，一轮要对目录里**每一个**条目做 3 次 unlink，
// 而这些 unlink 全在 CacheStore::mu_ 下——期间 syp_source_open 与别的源的
// close() 全卡住。上界 kMaxEvictPerRound = 32：40 条进去，一轮最多走 32 条，
// 至少剩 8 条，返回 BUSY（"还有得删，下一轮继续"，不是故障）。第二轮接着删，
// 证明封顶不会让淘汰停在半路。
TEST_CASE(enforce_capacity_round_is_capped_and_resumes_next_round) {
    TempDir td;
    const std::string dir = td.p.string();
    constexpr int kN = 40;
    for (int i = 0; i < kN; ++i) {
        const std::string url = "http://x/cap" + std::to_string(i) + ".mp4";
        const auto idx = make_entry(dir, url, 1024);
        // 年龄递增，顺序确定；谁先走无所谓，这里只数个数。
        set_entry_mtime_ago(idx, 1000 + i * 10);
    }
    syp_status serr = SYP_OK;
    REQUIRE(syp::dl::scan_cache_dir(td.p, &serr).size() == static_cast<size_t>(kN));

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = 0;
    c.min_free_space_bytes = std::numeric_limits<int64_t>::max();

    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_BUSY);
    {
        const auto st = CacheStore::get().last_round_stats_for_test();
        CHECK(st.attempts <= 32);       // 锁内 unlink 次数有上界
        CHECK_EQ(st.deleted, static_cast<int64_t>(32));
    }
    const auto after1 = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    // 一轮最多 32 条：剩下的必须 >= 8，而且必须真的少了（不是什么都没干）。
    CHECK(after1.size() >= static_cast<size_t>(kN - 32));
    CHECK(after1.size() < static_cast<size_t>(kN));

    // 第二轮：剩的 8 条一轮就删得完，于是不再是 BUSY。
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);
    const auto after2 = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    CHECK_EQ(after2.size(), static_cast<size_t>(0));
}

// 【LRU 的排序键必须是 .idx 的 mtime，不能是三个文件里最新的那个】
// set_entry_mtime_ago 把三个文件设成同一时刻，于是 max 与 .idx 相等——
// 那样的用例对"到底取哪个"没有鉴别力（把实现换回 max-of-three 照样全绿）。
// 这里故意让两者打架：a 的 .idx 最旧、.dat 却最新。
//   · 按 .idx（正确）：a 最旧 → 淘汰 a；
//   · 按 max-of-three：a 反而成了最新的 → 会去淘汰 b。
TEST_CASE(lru_orders_by_idx_mtime_not_by_the_newest_file) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto a_idx = make_entry(dir, "http://x/lru-a.mp4", 64 * 1024);
    const auto b_idx = make_entry(dir, "http://x/lru-b.mp4", 64 * 1024);

    set_file_mtime_ago(a_idx,         60000);   // a 的索引：很旧
    set_file_mtime_ago(dat_of(a_idx),  1000);   // a 的数据：很新
    set_entry_mtime_ago(b_idx,        30000);   // b：两个都 30s

    syp_config c = cfg_for(dir);
    c.max_cache_bytes = 96 * 1024;              // 必须走掉一条
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_OK);

    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    REQUIRE(left.size() == 1);
    // 活下来的必须是 b：a 的 .idx 更旧，哪怕它的 .dat 是全场最新的。
    CHECK_EQ(left[0].key, syp::dl::CacheIndex::key_for_url("http://x/lru-b.mp4"));
}

// 【缺口一补上就立刻收手，不许顺手把目录清空】这是"卷因为与我们无关的原因
// 吃紧"那条 Important 真正的防线：缺口只有 ~1MiB 时，删掉一个 4MiB 的条目就
// 够了，后面两条必须原封不动，而且返回 SYP_OK（本轮目标达成）——不是 BUSY、
// 更不是 NO_SPACE。
//
// 【为什么这里不会出现"预算用完但还不够"】释放量改成实测的 statvfs 差值之后，
// "预算用完"（freed >= want_free - avail0）与"卷已经够了"（avail >= want_free）
// 是同一个条件，那个中间状态在代数上就不存在了，所以也没有对应的 BUSY 分支。
// 收手与否只看实测的可用空间。
//
// 【这条用例与环境耦合，如实写在这里】它成立需要两个前提：
//   1. 本用例执行期间，卷的可用空间不会被别的进程大幅改动（缺口只有 1MiB，
//      别人吃掉 1MiB 以上就会让缺口变大、删得比预期多）；
//   2. 文件系统在删除之后**立刻**把空间还给 statvfs。若这台机器把释放的块
//      压在快照/延迟回收里（APFS 开了本地快照就会这样），删掉一条之后
//      avail 不动，这一轮会继续删下去，断言就会翻成 NO_SPACE 并变红。
// 两条都不是被测代码的问题，而是"用真实文件系统测可用空间策略"的固有代价。
// 真红的时候先排查环境，别急着改断言。
TEST_CASE(enforce_capacity_stops_as_soon_as_the_volume_recovers) {
    TempDir td;
    const std::string dir = td.p.string();
    constexpr int64_t kEntry  = 4 * 1024 * 1024;
    constexpr int64_t kMargin = 1024 * 1024;     // 缺口 ≈ 1MiB < 一个条目
    const auto a_idx = make_entry(dir, "http://x/bud-a.mp4", kEntry);
    const auto b_idx = make_entry(dir, "http://x/bud-b.mp4", kEntry);
    const auto c_idx = make_entry(dir, "http://x/bud-c.mp4", kEntry);
    set_entry_mtime_ago(a_idx, 90000);
    set_entry_mtime_ago(b_idx, 60000);
    set_entry_mtime_ago(c_idx, 30000);

    std::error_code ec;
    const auto sp = std::filesystem::space(td.p, ec);
    REQUIRE(!ec);
    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = 0;                  // 只走可用空间这一支
    c.min_free_space_bytes = static_cast<int64_t>(sp.available) + kMargin;

    const syp_status st = CacheStore::get().enforce_capacity(dir, c);
    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    // 前提：确实删了东西（缺口没被别的进程的抖动抹平）。
    CHECK(left.size() < static_cast<size_t>(3));
    // 要害：补上 1MiB 的缺口只需要走掉一条 4MiB 的，剩下两条必须还在——
    // 没有实测停止条件的话，这一轮会一路删到目录空掉。
    CHECK_EQ(left.size(), static_cast<size_t>(2));
    CHECK_EQ(st, SYP_OK);
    const auto rs = CacheStore::get().last_round_stats_for_test();
    CHECK_EQ(rs.deleted, static_cast<int64_t>(1));
    // 走掉的是最旧的那条。
    for (const auto& e : left) {
        CHECK(e.key != syp::dl::CacheIndex::key_for_url("http://x/bud-a.mp4"));
    }
}

// 【删了空间也不涨，就必须收手】.dat 是稀疏文件，逻辑长度可以远大于占用的
// 块数。按逻辑长度记账的话，删掉一个 8MiB 的稀疏条目就能把"缺口预算"用完，
// 而卷的可用空间一点没动；下一轮重新算出同样的缺口，于是缓存被一轮一轮
// 抽干——原来的"一次清空"只是变成了"每轮 32 个"。
// 实测 statvfs 差值 + "删了不涨就停"把这条路掐断：稀疏那条删完就收手，
// 后面两条普通条目必须还在，返回值是 NO_SPACE（再调也没用，别再删了）。
TEST_CASE(enforce_capacity_stops_when_deleting_does_not_recover_space) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto sparse_idx = make_sparse_entry(dir, "http://x/sparse.mp4",
                                              8 * 1024 * 1024);
    const auto n1_idx = make_entry(dir, "http://x/n1.mp4", 32 * 1024);
    const auto n2_idx = make_entry(dir, "http://x/n2.mp4", 32 * 1024);
    set_entry_mtime_ago(sparse_idx, 90000);   // 最旧：第一个轮到它
    set_entry_mtime_ago(n1_idx,     60000);
    set_entry_mtime_ago(n2_idx,     30000);

    // 【前提必须先立住】逻辑长度 >= 8MiB，而实际占用远小于它。文件系统若不
    // 支持空洞，这两条会红——那是"本用例不再验证它声称的东西"的正确信号，
    // 不该让它默默变绿。
    syp_status serr = SYP_OK;
    {
        const auto before = syp::dl::scan_cache_dir(td.p, &serr);
        REQUIRE(before.size() == 3);
        int64_t logical = 0;
        for (const auto& e : before) {
            if (e.key == syp::dl::CacheIndex::key_for_url("http://x/sparse.mp4")) {
                logical = e.bytes;
            }
        }
        CHECK(logical >= 8 * 1024 * 1024);
        const int64_t alloc = allocated_bytes(dat_of(sparse_idx));
        CHECK(alloc >= 0);
        CHECK(alloc * 8 < logical);      // 实际占用连逻辑长度的 1/8 都不到
    }

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = 0;               // 只走可用空间这一支
    c.min_free_space_bytes = std::numeric_limits<int64_t>::max();
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);

    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    // 删了稀疏那条、发现可用空间没动，就停了；两条普通条目必须还在。
    CHECK_EQ(left.size(), static_cast<size_t>(2));
}

// 【删除失败的路径也必须有上界】把目录 chmod 成不可写：每个条目的 3 次
// unlink 全部失败。如果上界只数"成功"，循环会把 N 个条目全试一遍、3N 次
// 失败的系统调用全压在 CacheStore::mu_ 下，而且按 1s 的节流每个源每秒一次
// ——正是上界要拦的事，只不过发生在失败路径上。
// 计尝试次数之后，一轮最多 32 次尝试，而不是把 40 条全试一遍。
TEST_CASE(enforce_capacity_gives_up_on_a_directory_it_cannot_delete_from) {
    TempDir td;
    const std::string dir = td.p.string();
    constexpr int kN = 40;
    for (int i = 0; i < kN; ++i) {
        const auto idx = make_entry(dir, "http://x/ro" + std::to_string(i) + ".mp4",
                                    1024);
        set_entry_mtime_ago(idx, 1000 + i * 10);
    }
    // 去掉写权限：unlink 需要对**目录**有写权限，对文件本身有没有无所谓。
    std::error_code ec;
    std::filesystem::permissions(td.p,
                                 std::filesystem::perms::owner_write
                                     | std::filesystem::perms::group_write
                                     | std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::remove, ec);
    REQUIRE(!ec);

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = 0;
    c.min_free_space_bytes = std::numeric_limits<int64_t>::max();
    // 删不动 → 再调也没用 → NO_SPACE（不是 BUSY：BUSY 会让调用方一直重试）。
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);

    // 【这条才是本用例的要害】返回值和磁盘状态区分不出"撞上界收手"与
    // "把 40 条全试一遍"——两者都是"什么也没删、NO_SPACE"。所以必须看锁内
    // 到底干了多少活：尝试次数封顶在 kMaxEvictPerRound(32)，不是 40。
    const auto st = CacheStore::get().last_round_stats_for_test();
    CHECK_EQ(st.deleted, static_cast<int64_t>(0));
    CHECK_EQ(st.attempts, static_cast<int64_t>(32));
    CHECK(st.attempts < static_cast<int64_t>(kN));

    // 把权限还回去，TempDir 才删得掉自己。
    std::filesystem::permissions(td.p, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, ec);
    CHECK(!ec);
    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(left.size(), static_cast<size_t>(kN));   // 一个都没删掉
}

// 【删不掉的条目不能把整轮堵死】LRU 的顺序是稳定的：如果撞上几条删不掉的
// 就 break，那么每一轮都会撞在同样那几条上、然后什么都不删，排在它们后面
// 删得动的条目**永远轮不到**——容量策略对这个目录终身失效。
// （这正是曾经的"连续失败 4 次就 break"造成的回归。）
//
// 构造"删不掉但仍然会被扫描到"的条目：把它的 .dat 换成一个**非空目录**。
//   · .idx 还是普通文件，所以 scan_cache_dir 照样认出这个 key；
//   · remove_cache_entry 删 .dat 时撞上"目录非空"，整条返回非 OK。
// 用目录而不是 chflags，是为了不依赖 BSD 专有的 immutable 标志。
TEST_CASE(enforce_capacity_walks_past_entries_it_cannot_delete) {
    TempDir td;
    const std::string dir = td.p.string();
    constexpr int kBad  = 4;
    constexpr int kGood = 8;
    constexpr int64_t kEach = 16 * 1024;

    // 前 4 条最旧、且删不掉；后 8 条较新、删得动。
    for (int i = 0; i < kBad; ++i) {
        const auto idx = make_entry(dir, "http://x/bad" + std::to_string(i) + ".mp4",
                                    kEach);
        set_entry_mtime_ago(idx, 90000 - i * 100);
        std::error_code ec;
        const auto d = dat_of(idx);
        std::filesystem::remove(d, ec);
        std::filesystem::create_directory(d, ec);       // .dat 变成目录……
        CHECK(!ec);
        std::ofstream(d / "occupied.txt") << "x";       // ……而且非空，删不掉
    }
    for (int i = 0; i < kGood; ++i) {
        const auto idx = make_entry(dir, "http://x/good" + std::to_string(i) + ".mp4",
                                    kEach);
        set_entry_mtime_ago(idx, 10000 - i * 100);
    }

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = kEach;   // 远低于合计：必须尽量删
    c.min_free_space_bytes = 0;

    (void)CacheStore::get().enforce_capacity(dir, c);
    const auto st = CacheStore::get().last_round_stats_for_test();
    // 要害：4 条删不掉的没有中断这一轮，后面 8 条删得动的必须真的被删掉。
    // 有 break 的版本在这里是 deleted == 0。
    CHECK_EQ(st.deleted, static_cast<int64_t>(kGood));
    CHECK(st.attempts >= static_cast<int64_t>(kBad + kGood));

    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    for (const auto& e : left) {
        // 活下来的只能是那 4 条删不掉的。
        bool is_bad = false;
        for (int i = 0; i < kBad; ++i) {
            if (e.key == syp::dl::CacheIndex::key_for_url(
                             "http://x/bad" + std::to_string(i) + ".mp4")) {
                is_bad = true;
            }
        }
        CHECK(is_bad);
    }
}

// 【上面那条用例取 kBad=4，刚好在闸下面，钉不住真正的边界】
//
// 删除失败**照样吃** attempts 额度（这条规矩本身是对的，它拦的是"3N 次失败
// 的 unlink 全压在 mu_ 下"），而被引用的条目在 ++attempts **之前** continue。
// 于是真正的死角是：只要最旧的 **kMaxEvictPerRound(32) 条**删不掉，每一轮
// 就把 32 次尝试原样花在同样那 32 条上、deleted 恒为 0，排在它们后面删得动
// 的条目永远轮不到。修复前本机实测（40 条 ×100KB、最旧 32 条 chflags uchg、
// max_cache_bytes=100000）：**10/10 轮 deleted=0，目录永远停在 4,099,350 B
// ——41 倍于上限**。修复（只在这个死角里生效的起步游标）之后：轮 1 deleted=0、
// **轮 2 deleted=8**，删得动的那 8 条真的被清掉了。
//
// 【为什么这里非用 chflags 不可】上面那条用例把 .dat 换成非空目录，代价是
// `remove_cache_entry` 仍然会把 **.idx 删掉**（它三个文件挨个删，不早退），
// 于是那个 key 下一轮就从 scan_cache_dir 里消失了——一轮之内够用，跨轮的
// 楔死形状造不出来。要造"跨轮一直可见且一直删不掉"，只能让 **.idx 本身**
// unlink 不掉，而 POSIX 里没有这个工具；BSD 的 uchg 有。本仓只在 Apple 上
// 构建，所以这是与平台一致，不是妥协；别的平台上整条用例跳过并打印。
TEST_CASE(enforce_capacity_reaches_deletable_entries_behind_a_full_round_of_locked_ones) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    constexpr int kLocked = 32;      // == kMaxEvictPerRound，边界本身
    constexpr int kGood   = 8;
    // 【条目故意做得很小】楔死的判据是**条数**（32 次尝试额度），不是字节数，
    // 所以 4KiB/条就够。曾经用过 100KB/条，那会让这条用例在 TempDir 析构时
    // 一次性释放 ~8MiB——而紧挨着它的 no_progress_backoff_survives_across_rounds
    // 正是靠"可用空间没回升"来判退避的，APFS 的删除又是异步的。
    // 实测：100KB/条时 `ctest -j4` 下那条邻居用例红在 `st.deleted 1 vs 0`
    // （这个用例负载敏感，这里只是别再往上加一份系统性扰动）。
    constexpr int64_t kEach = 4 * 1024;

    TempDir td;
    const std::string dir = td.p.string();
    std::vector<std::filesystem::path> locked;
    for (int i = 0; i < kLocked; ++i) {
        const auto idx = make_entry(dir, "http://x/lk" + std::to_string(i) + ".mp4", kEach);
        set_entry_mtime_ago(idx, 90000 - i * 100);      // 最旧的一批
        locked.push_back(idx);
    }
    for (int i = 0; i < kGood; ++i) {
        const auto idx = make_entry(dir, "http://x/gd" + std::to_string(i) + ".mp4", kEach);
        set_entry_mtime_ago(idx, 10000 - i * 100);      // 较新，删得动
    }
    // 只锁 .idx 就够：remove_cache_entry 先删它，EPERM 之后整条判失败，
    // 而 .idx 还在 ⇒ 这个 key 下一轮照样被 scan_cache_dir 看见。
    for (const auto& p : locked) CHECK(::chflags(p.c_str(), UF_IMMUTABLE) == 0);

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = kEach;    // 远低于 40×kEach：每一轮都必须想删点什么
    c.min_free_space_bytes = 0;

    // 轮 1：32 次尝试全打在锁死的条目上，一条都删不成。
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);
    {
        const auto st = CacheStore::get().last_round_stats_for_test();
        CHECK_EQ(st.deleted,  static_cast<int64_t>(0));
        CHECK_EQ(st.attempts, static_cast<int64_t>(32));   // 边界：吃满额度
    }
    // 轮 2：**这一条就是回归的要害**。修复前它与轮 1 逐字段相同
    // （deleted=0），删得动的 8 条永远轮不到。
    (void)CacheStore::get().enforce_capacity(dir, c);
    {
        const auto st = CacheStore::get().last_round_stats_for_test();
        CHECK_EQ(st.deleted, static_cast<int64_t>(kGood));
    }
    syp_status serr = SYP_OK;
    const auto left = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(serr, SYP_OK);
    CHECK_EQ(left.size(), static_cast<size_t>(kLocked));   // 只剩锁死的那些

    for (const auto& p : locked) CHECK(::chflags(p.c_str(), 0) == 0);  // TempDir 要删

    // 【边界的另一侧：31 条锁死就不会楔死】少一条，第 32 次尝试就落在删得动
    // 的条目上，轮 1 当场就有进展。这一条保证上面那个 32 不是随手写的数字。
    {
        TempDir td2;
        const std::string dir2 = td2.p.string();
        std::vector<std::filesystem::path> locked2;
        for (int i = 0; i < kLocked - 1; ++i) {
            const auto idx = make_entry(dir2, "http://x/l2" + std::to_string(i) + ".mp4",
                                        kEach);
            set_entry_mtime_ago(idx, 90000 - i * 100);
            locked2.push_back(idx);
        }
        for (int i = 0; i < kGood; ++i) {
            const auto idx = make_entry(dir2, "http://x/g2" + std::to_string(i) + ".mp4",
                                        kEach);
            set_entry_mtime_ago(idx, 10000 - i * 100);
        }
        for (const auto& p : locked2) CHECK(::chflags(p.c_str(), UF_IMMUTABLE) == 0);
        syp_config c2 = cfg_for(dir2);
        c2.max_cache_bytes      = kEach;
        c2.min_free_space_bytes = 0;
        (void)CacheStore::get().enforce_capacity(dir2, c2);
        const auto st2 = CacheStore::get().last_round_stats_for_test();
        CHECK_EQ(st2.deleted, static_cast<int64_t>(1));   // 31 锁 + 1 删成 = 32 次尝试
        for (const auto& p : locked2) CHECK(::chflags(p.c_str(), 0) == 0);
    }
#else
    std::printf("    SKIPPED (no chflags/UF_IMMUTABLE on this platform)\n");
#endif
}

// 【"删了也不涨"必须跨轮记住，否则只是把抽干放慢了一轮】
// 只在一轮之内收手的话：一轮删一条稀疏条目、发现可用空间没动、收手并报
// NO_SPACE；下一轮重新来过，又删一条……六轮之后目录照样空了，而且每一轮都在
// 宣称"再调也没用"。退避把结论按 cache_dir 记下来，在可用空间真正回升之前
// 后续各轮一条都不再删。
TEST_CASE(no_progress_backoff_survives_across_rounds) {
    TempDir td;
    const std::string dir = td.p.string();
    constexpr int kN = 6;
    for (int i = 0; i < kN; ++i) {
        const auto idx = make_sparse_entry(
            dir, "http://x/sp" + std::to_string(i) + ".mp4", 8 * 1024 * 1024);
        set_entry_mtime_ago(idx, 90000 - i * 1000);
    }
    syp_status serr = SYP_OK;
    REQUIRE(syp::dl::scan_cache_dir(td.p, &serr).size() == static_cast<size_t>(kN));

    // 【压舱文件必须在第一轮**之前**就占着盘】退避记下来的是"当时"的可用
    // 空间；要验证它能解除，就得让后来的可用空间真的比那一刻高。所以先占住
    // 48MiB，等退避记好之后再把它删掉，可用空间才会真的抬上去。
    // （名字不是 16 位 hex，scan_cache_dir 不会把它当成缓存条目。）
    const auto ballast = td.p / "ballast.bin";
    {
        std::ofstream f(ballast, std::ios::binary);
        const std::vector<char> chunk(1024 * 1024, 'b');
        for (int i = 0; i < 48; ++i) f.write(chunk.data(), 1024 * 1024);
    }
    {
        std::error_code ec;
        REQUIRE(std::filesystem::file_size(ballast, ec) == 48u * 1024 * 1024);
    }

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = 0;               // 只走可用空间这一支
    c.min_free_space_bytes = std::numeric_limits<int64_t>::max();

    // 第一轮：删掉最旧的那条，发现可用空间没动，收手并记下退避。
    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);
    const auto after1 = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK(after1.size() < static_cast<size_t>(kN));
    const size_t left1 = after1.size();

    // 后续各轮：**一条都不许再删**。没有跨轮退避的话，这里每轮会再少一条，
    // 六轮之后目录就空了——那正是这条用例要拦的事。
    for (int round = 0; round < 5; ++round) {
        CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);
        const auto st = CacheStore::get().last_round_stats_for_test();
        CHECK_EQ(st.deleted,  static_cast<int64_t>(0));
        CHECK_EQ(st.attempts, static_cast<int64_t>(0));
    }
    const auto after_n = syp::dl::scan_cache_dir(td.p, &serr);
    CHECK_EQ(after_n.size(), left1);

    // 【退避不是永久的】把压舱文件删掉，可用空间真的抬高 48MiB，退避就该
    // 解除、这一支重新开始干活。
    std::error_code rec;
    std::filesystem::remove(ballast, rec);
    CHECK(!rec);

    CHECK_EQ(CacheStore::get().enforce_capacity(dir, c), SYP_ERR_NO_SPACE);
    const auto st2 = CacheStore::get().last_round_stats_for_test();
    // 退避解除了：这一轮重新动手（又删了一条，然后重新记下退避）。
    CHECK(st2.deleted >= static_cast<int64_t>(1));
}

// 【退避中但仍在为 max_cache_bytes 删东西的那一轮，必须报 BUSY 而不是 NO_SPACE】
//
// 默认配置里两项限额**都是开着的**（max_cache_bytes 512MiB + min_free_space_bytes
// 256MiB），所以"可用空间那一支进了退避、字节上限那一支还在全速淘汰"是默认
// 形状下的常态，不是构造出来的角落。此时若先判 backed_off、后判 capped，返回
// 的是 NO_SPACE——按 cache_store.h 的契约那是"在外部条件变化之前再调也没用"。
// 而事实是这一轮刚删掉 32 条、下一轮还会再删 32 条：缓存正在被抽下去，唯一的
// 消费者（Preloader）却被告知停产干等。判据顺序反了，语义就整个反了。
//
// 构造：100 个 64MiB 的稀疏条目。删一条的逻辑量（64MiB）远超
// kProgressCheckBytes(1MiB)，而实测只回来一个块，于是"删了也不涨"正常在第一轮
// 第一条就成立。
//
// 【两处环境耦合，两处都要按住，否则这条用例会假红】
// 1. 判据 `space_freed < logical/8` 量的是**整卷**的可用空间：别的进程在这一瞬间
//    释放掉 8MiB，这一轮就不判退避、改为按上界删满 32 条收手。所以不写"第一轮
//    必须退避"，而是**有界地**逼出退避（最多 3 轮；条目留够 100 > 3×32，保证逼
//    的过程不会把名单吃空）。
// 2. **退避的解除没有滞回**：只要可用空间比记下
//    的那一刻高**一个字节**，下一轮就重新为可用空间删东西——于是要验的那一轮
//    根本不是"退避中的一轮"，实测在 TSan 构建下 20 次里中 4 次
//    （rs.deleted=1、返回 NO_SPACE）。这里用一个 64MiB 的**压舱文件**把可用空间
//    实打实地压到记下的水位之下，退避因此不会在这条用例里被解除。
//    （压舱文件名不是 16 位 hex，scan_cache_dir 不把它当缓存条目。）
TEST_CASE(backed_off_round_still_evicting_for_the_cap_reports_busy) {
    TempDir td;
    const std::string dir = td.p.string();
    constexpr int     kN  = 100;
    for (int i = 0; i < kN; ++i) {
        const auto idx = make_sparse_entry(
            dir, "http://x/bo" + std::to_string(i) + ".mp4", 64LL * 1024 * 1024);
        set_entry_mtime_ago(idx, 90000 - i * 100);
    }
    syp_status serr = SYP_OK;
    REQUIRE(syp::dl::scan_cache_dir(td.p, &serr).size() == static_cast<size_t>(kN));

    syp_config c = cfg_for(dir);
    c.max_cache_bytes      = 4096;                              // 远低于合计：必须一直删
    c.min_free_space_bytes = std::numeric_limits<int64_t>::max();  // 永远达不到

    // 逼出退避：NO_SPACE 就是"这一轮判了删也换不来空间，已记下退避"。
    // 记下这一刻的可用空间：退避里记的就是这个量级的数，压舱文件要压到它之下。
    int64_t avail_at_engage = 0;
    {
        std::error_code ec;
        const auto sp = std::filesystem::space(td.p, ec);
        CHECK(!ec);
        avail_at_engage = static_cast<int64_t>(sp.available);
    }
    bool engaged = false;
    for (int r = 0; r < 3 && !engaged; ++r) {
        engaged = CacheStore::get().enforce_capacity(dir, c) == SYP_ERR_NO_SPACE;
    }
    REQUIRE(engaged);
    // 下一轮要能撞上界，名单上必须还剩 > 32 条。
    REQUIRE(syp::dl::scan_cache_dir(td.p, &serr).size() > 32);

    // 【把退避按住】写 64MiB 真实字节，可用空间必然低于刚才记下的水位。
    const auto ballast = td.p / "ballast.bin";
    {
        std::ofstream f(ballast, std::ios::binary);
        const std::vector<char> chunk(1024 * 1024, 'b');
        for (int i = 0; i < 64; ++i) f.write(chunk.data(), 1024 * 1024);
    }
    {
        std::error_code ec;
        const auto sp = std::filesystem::space(td.p, ec);
        CHECK(!ec);
        // 前提自检：可用空间确实被压到了逼出退避那一刻之下。这条若红，说明
        // 环境在这几十毫秒里释放了 >64MiB，用例的前提不成立（而不是实现坏了）。
        CHECK(static_cast<int64_t>(sp.available) < avail_at_engage);
    }

    // 要验的那一轮：退避生效（可用空间那一支知情不做），而字节上限那一支
    // 照样删满 32 条撞上界——这一轮**确实**在推进，所以不能说"再调也没用"。
    const syp_status st2 = CacheStore::get().enforce_capacity(dir, c);
    const auto rs = CacheStore::get().last_round_stats_for_test();
    CHECK_EQ(rs.deleted, static_cast<int64_t>(32));
    CHECK_EQ(st2, SYP_ERR_BUSY);
}

// 【日志回调不得在 CacheStore::mu_ 下被调用】用户的 syp_log_fn 可以回头调
// 本层任何 API，其中好几条（acquire / release / open_count / enforce_capacity）
// 要取**同一把** CacheStore::mu_ —— 同线程重入非递归 mutex 就是挂死，不是
// 慢，而是永远不返回。TTL 过期那条日志正好落在 acquire 的锁内段里，所以这条
// 用例把它钉住：回调里调 open_count()，回归时这个套件会以超时（TIMEOUT 30）
// 告警，而不是无声退化。
namespace {
std::atomic<int> g_log_hits{0};

void reentrant_log_cb(void* ctx, syp_log_level lvl, const char* tag, const char* msg) {
    (void)lvl;
    (void)tag;
    (void)msg;
    const auto* key = static_cast<const std::string*>(ctx);
    // 取 CacheStore::mu_。若 log_msg 是在那把锁下被调的，这里直接死锁。
    (void)CacheStore::get().open_count(*key);
    g_log_hits.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

TEST_CASE(log_callback_may_reenter_the_store_from_the_ttl_path) {
    TempDir td;
    const std::string dir = td.p.string();
    const auto idx = make_entry(dir, "http://x/ttl-log.mp4", 4096);
    set_entry_mtime_ago(idx, 10000);

    const std::string key = CacheStore::make_key(dir, "http://x/ttl-log.mp4");
    g_log_hits.store(0, std::memory_order_relaxed);
    syp::dl::set_log_callback(&reentrant_log_cb, const_cast<std::string*>(&key),
                              SYP_LOG_DEBUG);

    syp_config c = cfg_for(dir);
    c.cache_ttl_ms = 5000;
    syp_status err = SYP_OK;
    auto h = CacheStore::get().acquire(dir, "http://x/ttl-log.mp4", c, &err);
    syp::dl::set_log_callback(nullptr, nullptr, SYP_LOG_INFO);
    CHECK_EQ(err, SYP_OK);
    REQUIRE(h.valid());
    // 过期日志确实打了（否则这条用例什么都没验到）。
    CHECK(g_log_hits.load(std::memory_order_relaxed) > 0);
    CacheStore::get().release(h.key);
}

int main() { return tiny_test_main(); }
