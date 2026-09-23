// cache_store.cpp — 注册表与目录扫描。所有公开方法在 mu_ 下完成（含文件 IO）。
#include "cache_store.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace syp::dl {
namespace {

constexpr int64_t kI64Max     = std::numeric_limits<int64_t>::max();
constexpr size_t  kKeyHexLen  = 16;
// key 的分隔符用 US（0x1f）：文件路径与 URL 里都不可能出现它，
// 不会出现 “dir=a, url=b” 与 “dir=a\x1fb, url=” 撞成同一个 key。
constexpr char    kKeySep     = '\x1f';

// 一轮最多**尝试**删多少个 key。计的是尝试次数（成功 + 失败），不是成功
// 次数——只数成功的话，一个删不动的目录（只读、EPERM、immutable）会让
// 循环把 N 个条目全试一遍、3N 次失败的 unlink 全在 mu_ 下，上界永远不触发，
// 而且按 1s 的节流每个源每秒来一次。正是这个上界要拦的事，只不过发生在
// 失败路径上。
constexpr size_t kMaxEvictPerRound  = 32;
// 【这里曾经有一个"连续失败 N 次就 break"，那是个 bug，别加回来】
// LRU 的顺序是稳定的：只要最旧的那几条删不掉（权限 / immutable / .dat 被
// 别的东西占着），每一轮都会撞在同样那几条上、然后 break，排在它们后面
// 删得动的条目**永远轮不到**——容量策略对这个目录终身失效。
// 删除失败照样消耗 attempts，所以锁内的系统调用次数已经被 kMaxEvictPerRound
// 兜住了，连续失败上界对"锁内有界"这件事不提供任何额外保证，只会制造死角。
// 正确的"真的删不动"判据放在循环之后：尝试用完了却一条都没删掉。
// 一轮最多**看**多少个条目。被引用的 key 只花一次 map::find（没有系统
// 调用），但"几乎全被引用"的目录仍然会让循环在 mu_ 下走满 N 次。给整个
// 循环一个硬上界，锁内工作量就与目录规模无关了。
constexpr size_t kMaxExaminePerRound = 512;
// "删了也不涨"的判定阈值。逻辑上删掉了这么多字节之后才开始判，避免把
// 文件系统的延迟回收（APFS 的删除是异步的）误判成没用。
constexpr int64_t kProgressCheckBytes = 1024 * 1024;
// 实测释放量不到逻辑删除量的 1/kProgressRatio，就认为"删了也不涨"。
constexpr int64_t kProgressRatio = 8;
// space_backoff_ 里最多留多少个目录。正常只有 1~2 个（整个进程共用一个
// 缓存目录），设上界只是防止调用方用一堆临时目录把它撑大。
constexpr size_t kMaxBackoffDirs = 32;

int64_t sat_add(int64_t a, int64_t b) noexcept {
    if (b > 0 && a > kI64Max - b) return kI64Max;
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
        return std::numeric_limits<int64_t>::min();
    }
    return a + b;
}

bool is_hex_key(std::string_view s) noexcept {
    if (s.size() != kKeyHexLen) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string key_from_filename(const std::filesystem::path& p) {
    const std::string name = p.filename().string();
    if (name.size() == kKeyHexLen + 4
        && (name.ends_with(".idx") || name.ends_with(".dat"))
        && is_hex_key(std::string_view(name.data(), kKeyHexLen))) {
        return name.substr(0, kKeyHexLen);
    }
    if (name.size() == kKeyHexLen + 8 && name.ends_with(".idx.tmp")
        && is_hex_key(std::string_view(name.data(), kKeyHexLen))) {
        return name.substr(0, kKeyHexLen);
    }
    return {};
}

int64_t file_size_or_0(const std::filesystem::path& p) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    if (ec) return 0;
    if (n > static_cast<uintmax_t>(kI64Max)) return kI64Max;
    return static_cast<int64_t>(n);
}

std::filesystem::file_time_type file_mtime_or_min(const std::filesystem::path& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return std::filesystem::file_time_type::min();
    return t;
}

syp_status status_from_ec(const std::error_code& ec) noexcept {
    if (!ec) return SYP_OK;
    if (ec == std::errc::no_space_on_device) return SYP_ERR_NO_SPACE;
    return SYP_ERR_IO;
}

syp_status remove_path(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    if (!ec || ec == std::errc::no_such_file_or_directory) return SYP_OK;
    return status_from_ec(ec);
}

// TTL / LRU 的时间基准都用**文件 mtime**，不用 CacheMeta::updated_at_ms。
// 两个理由：
//   1. 一致性——LRU（cache_dir_evict 与 enforce_capacity）本来就按 mtime 排，
//      TTL 再用另一个时间源，会出现“按 A 没过期、按 B 是最旧的”这种自相矛盾；
//   2. 时钟基准——Clock::now_ms（clock.h）走的是 **steady_clock**（开机起算），
//      而 CacheMeta::updated_at_ms 走的是 **system_clock**（epoch 起算），
//      两者相减没有意义。文件 mtime 与 filesystem clock 是同一个基准，
//      而且用例可以用 last_write_time() 直接构造，不需要 sleep。
bool entry_expired(const std::filesystem::path& idx, int64_t ttl_ms) {
    if (ttl_ms <= 0) return false;
    std::error_code ec;
    const auto mt = std::filesystem::last_write_time(idx, ec);
    if (ec) return false;                       // 读不到 mtime 就不判过期
    const auto now = std::filesystem::file_time_type::clock::now();
    if (mt > now) return false;                 // 未来时间：不判过期
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - mt);
    return age.count() > ttl_ms;
}

// 卷可用空间；查不到返回 -1（视为“不受可用空间约束”）。
int64_t free_space_bytes(const std::filesystem::path& dir) {
    std::error_code ec;
    const auto s = std::filesystem::space(dir, ec);
    if (ec) return -1;
    if (s.available > static_cast<uintmax_t>(kI64Max)) return kI64Max;
    return static_cast<int64_t>(s.available);
}

}  // namespace

std::vector<CacheDirEntry> scan_cache_dir(const std::filesystem::path& dir,
                                          syp_status* err) {
    std::vector<CacheDirEntry> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        if (err) *err = SYP_OK;  // 没有目录 = 空缓存
        return out;
    }
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        if (err) *err = status_from_ec(ec);
        return out;
    }
    const std::filesystem::directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        if (ec) {
            if (err) *err = status_from_ec(ec);
            return out;
        }
        const auto& de = *it;
        std::error_code rec;
        if (!de.is_regular_file(rec) || rec) continue;
        const std::string key = key_from_filename(de.path());
        if (key.empty()) continue;
        CacheDirEntry* slot = nullptr;
        for (auto& e : out) {
            if (e.key == key) {
                slot = &e;
                break;
            }
        }
        if (slot == nullptr) {
            out.push_back(CacheDirEntry{});
            slot = &out.back();
            slot->key = key;
            slot->idx = dir / (key + ".idx");
            slot->dat = dir / (key + ".dat");
            slot->tmp = dir / (key + ".idx.tmp");
        }
        slot->bytes = sat_add(slot->bytes, file_size_or_0(de.path()));
        // 【LRU 的排序键只取 .idx 的 mtime】不是三个文件的 max。TTL
        // （entry_expired）判的就是 .idx，两边必须同一个时间源，否则会出现
        // "按 TTL 没过期、按 LRU 却是最旧的"这种自相矛盾——Ruling P2 写下
        // "TTL 与 LRU 都用 .idx 的 mtime"正是为了避免它。
        // ".idx.tmp" 不会命中：ends_with(".idx") 对它是 false。
        if (de.path().filename().string().ends_with(".idx")) {
            slot->mtime = file_mtime_or_min(de.path());
        }
    }
    if (err) *err = SYP_OK;
    return out;
}

syp_status remove_cache_entry(const CacheDirEntry& e) {
    syp_status st = SYP_OK;
    const auto note = [&](syp_status s) {
        if (s != SYP_OK && st == SYP_OK) st = s;
    };
    note(remove_path(e.idx));
    note(remove_path(e.dat));
    note(remove_path(e.tmp));
    return st;
}

CacheStore& CacheStore::get() {
    // 函数内静态：线程安全初始化（C++11 起），且不参与静态初始化顺序问题。
    // 故意不提供析构清理——进程退出时残留的条目没有任何需要落盘的东西
    // （save() 在 SourceBridge::close() 里已经做过）。
    static CacheStore s;
    return s;
}

std::string normalize_cache_dir(std::string dir) {
    // 空串原样返回：见头文件（"目录没填"是调用方的错，不在这里编默认值）。
    // 不提前返回也只是多走一遍 fs::path("")，但显式写出来读者不用去推。
    if (dir.empty()) return dir;

    // 【顺序是 lexically_normal 在前、剥尾斜杠在后，反过来是错的】
    // lexically_normal 自己**会造出**尾斜杠：末段解析成 "." 或 ".." 时它补一个
    // 空的末元素。本机实测（libc++）：
    //     lexically_normal("a/./")   = "a/"     lexically_normal("/a/.")    = "/a/"
    //     lexically_normal("/a/b/..")= "/a/"    lexically_normal("/tmp/d/") = "/tmp/d/"
    // 所以"先剥后归一"这一版对 "a/./" 会留下 "a/"、对 "/a/." 会留下 "/a/"，
    // 而同一个目录的另一种写法 "a" / "/a" 归一化之后没有尾斜杠 ⇒ 仍然是两个 key。
    // 先归一化、再剥，两种写法才真的合流。
    std::string out = std::filesystem::path(std::move(dir)).lexically_normal().string();
    // 根目录 "/" 保留（剥空了就不是同一个目录了）。
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

// 目录的 dev+ino。用 POSIX ::stat 而不是 std::filesystem：标准库没有把 inode
// 暴露出来（file_status 只有类型与权限），而 std::filesystem::equivalent()
// 虽然内部就是比 dev+ino，却要求**两条路径都存在**并且每比一次两次 stat——
// 这里要的是"算一次、和 N 条活条目比"，所以直接取原始的那一对数。
// <sys/stat.h> 不破 dl 层的平台无关性：cache_file.cpp 早就在用它，purity 闸
// （CMakeLists.txt:59-80）只禁 .mm/.m 源与链接接口里的平台 framework。
CacheStore::DirId CacheStore::dir_identity(const std::filesystem::path& p) noexcept {
    DirId id;
    if (p.empty()) return id;
    struct ::stat sb{};
    if (::stat(p.c_str(), &sb) != 0) return id;   // 不存在 / 无权限：不参与判定
    if (!S_ISDIR(sb.st_mode)) return id;
    id.dev   = static_cast<uint64_t>(sb.st_dev);
    id.ino   = static_cast<uint64_t>(sb.st_ino);
    id.valid = true;
    return id;
}

std::string CacheStore::make_key(const std::string& cache_dir, const std::string& url) {
    std::string k;
    k.reserve(cache_dir.size() + 1 + kKeyHexLen);
    k.append(cache_dir);
    k.push_back(kKeySep);
    k.append(CacheIndex::key_for_url(url));
    return k;
}

CacheStore::Handle CacheStore::acquire(const std::string& cache_dir,
                                       const std::string& url,
                                       const syp_config& cfg,
                                       syp_status* err) {
    if (err != nullptr) *err = SYP_OK;
    Handle h;
    if (cache_dir.empty() || url.empty()) {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return h;
    }
    const std::string key = make_key(cache_dir, url);

    // 【日志必须在放开 mu_ 之后才打】log_msg() 会调用户装的 syp_log_fn；
    // 那个回调可以回头调本层的任何 API（syp_source_cached_ranges 之类），
    // 其中好几条路径要取**同一把** mu_——同线程重入非递归 mutex 直接挂死。
    // 这与 save_index_locked 里那段是同一形状的坑。
    // 声明在 lock_guard **之前**，所以析构在放锁**之后**：锁内只记结论，
    // 出了函数、放了锁才真的打。至多只会有一条待打的消息——过期删完之后
    // CacheIndex::open 只可能返回 EOF，不可能再报 CORRUPT。
    syp_log_level pending_lvl = SYP_LOG_INFO;
    const char*   pending_msg = nullptr;
    struct DeferredLog {
        const syp_log_level* lvl;
        const char* const*   msg;
        ~DeferredLog() {
            if (*msg != nullptr) log_msg(*lvl, "cache", *msg);
        }
    } deferred_log{&pending_lvl, &pending_msg};

    std::lock_guard<std::mutex> g(mu_);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        ++it->second.refs;
        h.mu    = it->second.mu;
        h.index = it->second.index;
        h.file  = it->second.file;
        h.key   = key;
        return h;
    }

    const std::filesystem::path dir(cache_dir);
    // TTL：只在本 key 当前没人打开时判（走到这里就意味着注册表里没有它）。
    // 过期即丢弃 .idx/.dat，当作全新资源重下。
    if (cfg.cache_ttl_ms > 0) {
        const std::string fkey = CacheIndex::key_for_url(url);
        CacheDirEntry de;
        de.key = fkey;
        de.idx = dir / (fkey + ".idx");
        de.dat = dir / (fkey + ".dat");
        de.tmp = dir / (fkey + ".idx.tmp");
        if (entry_expired(de.idx, cfg.cache_ttl_ms)) {
            pending_lvl = SYP_LOG_INFO;
            pending_msg = "cache entry expired, discarding";
            (void)remove_cache_entry(de);
        }
    }

    // 打开策略与原先 SourceBridge::open() 里那段逐字一致：
    //   命中 → 用；未命中（EOF）→ 新建；损坏 → 擦掉重建；其它错误 → 失败。
    std::shared_ptr<CacheIndex> index;
    auto opened = CacheIndex::open(dir, url);
    if (opened) {
        index = std::make_shared<CacheIndex>(std::move(*opened));
    } else if (opened.error() == SYP_ERR_EOF) {
        index = std::make_shared<CacheIndex>(CacheIndex::create(dir, url));
    } else if (opened.error() == SYP_ERR_CACHE_CORRUPT) {
        // 同上：锁内只记结论。这条原先是在 mu_ 下直接打的，既然 TTL
        // 已经把延迟日志的机制搭好了，一并搬过来。
        pending_lvl = SYP_LOG_WARN;
        pending_msg = "cache index corrupt, wiping";
        auto tmp = CacheIndex::create(dir, url);
        (void)tmp.remove_files();
        index = std::make_shared<CacheIndex>(CacheIndex::create(dir, url));
    } else {
        if (err != nullptr) *err = opened.error();
        return h;
    }

    auto cf = CacheFile::open(index->data_path(), true);
    if (!cf) {
        if (err != nullptr) *err = cf.error();
        return h;
    }

    Entry e;
    e.mu    = std::make_shared<std::mutex>();
    e.index = std::move(index);
    e.file  = std::make_shared<CacheFile>(std::move(*cf));
    e.refs  = 1;
    // 【在这里 stat，不在函数开头】走到这一行时目录**一定存在**：
    // CacheIndex::create 会建目录，CacheFile::open(create=true) 会建 .dat。
    // 在开头 stat 的话，第一次用这个缓存目录时它还不存在，id 恒为 invalid，
    // 于是整条跨拼法的保护在"全新缓存目录"这个最常见的场景上失效。
    e.dir   = dir_identity(dir);
    auto ins = entries_.emplace(key, std::move(e));
    h.mu    = ins.first->second.mu;
    h.index = ins.first->second.index;
    h.file  = ins.first->second.file;
    h.key   = key;
    return h;
}

void CacheStore::release(const std::string& key) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    if (--it->second.refs > 0) return;
    // 归零即摘除。对象本身活到最后一个 shared_ptr 消失——调用方手里
    // 可能还捏着一份（SourceBridge::close() 就是“先 release、再让局部
    // 句柄析构”），那份决定真正的销毁时刻。
    entries_.erase(it);
}

int64_t CacheStore::open_count(const std::string& key) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.refs;
}

syp_status CacheStore::enforce_capacity(const std::string& cache_dir,
                                        const syp_config& cfg) {
    if (cache_dir.empty()) return SYP_ERR_INVALID_ARG;
    const int64_t cap       = cfg.max_cache_bytes;
    const int64_t want_free = cfg.min_free_space_bytes;
    if (cap <= 0 && want_free <= 0) return SYP_OK;   // 两项都不限：什么也不做

    // 【扫描必须在 mu_ 之外】目录扫描是 O(条目数) 次 stat，而 acquire() 要
    // 取同一把 mu_：握着它扫目录，等于让后台预加载的淘汰轮次卡住播放线程的
    // acquire()。先无锁扫、排序，再取锁做“查引用 + 删文件”。
    const std::filesystem::path dir(cache_dir);
    syp_status serr = SYP_OK;
    auto entries = scan_cache_dir(dir, &serr);
    if (serr != SYP_OK) return serr;             // 扫不动就放弃这一轮
    // 本目录的文件系统身份，锁外算一次（一次 stat）。下面判"这一组文件还有
    // 没有人开着"要靠它跨拼法认人，见 cache_store.h 里 DirId 那段。
    const DirId self_id = dir_identity(dir);

    int64_t total = 0;
    for (const auto& e : entries) total = sat_add(total, e.bytes);

    const int64_t avail0 = want_free > 0 ? free_space_bytes(dir) : -1;
    int64_t       avail  = avail0;

    // 【"可用空间"这一支：删到卷真的恢复为止，而"恢复了多少"一律实测】
    //
    // 这里曾经有一个"本轮字节预算 = 开局缺口"，用 file_size 的累加去抵扣。
    // **那是不成立的**：.dat 天生带空洞（按区间随机落盘，或被 resize 撑长），
    // 逻辑长度可以远大于占用的块数，于是删掉一个 8MiB 的稀疏条目就能把预算
    // "用完"，而 avail 一点没动；下一轮重新算出同样的缺口，缓存被一轮一轮
    // 抽干，只是从"一次清空"变成"每轮 32 个"。
    //
    // 改成实测之后，"预算用完"与"卷已经够了"其实是同一个条件
    // （freed = avail - avail0 >= want_free - avail0  <=>  avail >= want_free），
    // 于是预算这个概念本身消失了：判据就是 avail < want_free，一分不多一分
    // 不少。真正给这一支兜底的是每轮的尝试/查看上界，以及"删了也不涨就退避"。
    bool space_engaged = want_free > 0 && avail0 >= 0 && avail0 < want_free;

    // 【跨轮退避：上一轮已经证明"删了也不涨"，就别再删了】
    // 只在**本轮之内**收手是不够的：一轮删一条、发现没用、收手，下一轮重新
    // 来过又删一条——六轮之后目录照样空了，而且每一轮都在返回"再调也没用"。
    // 所以把结论记下来，按 cache_dir 退避，直到卷的可用空间**真的**比当时高
    // 了才解除（别的进程放了空间、或者我们自己因为 max_cache_bytes 删出了
    // 空间）。不需要时钟——dl 层没有 Clock，可用空间本身就是解除条件。
    //
    // 这次取 mu_ 只做一次 map 查找，不碰文件系统，也不持有任何别的锁；
    // 与下面删除循环那次是两次独立的短持有，都没有跨目录扫描。
    bool backed_off = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        // 每一轮先清计数：下面有好几条早退路径，不清的话用例会读到上一轮的
        // 数字，把"这一轮什么也没干"误读成"干了活"。
        last_round_ = RoundStats{};
        if (space_engaged) {
            auto it = space_backoff_.find(cache_dir);
            if (it != space_backoff_.end()) {
                if (avail0 <= it->second) {
                    space_engaged = false;  // 还在退避里：这一轮不为空间删东西
                    backed_off    = true;
                } else {
                    space_backoff_.erase(it);   // 卷真的动了，重新试
                }
            }
        }
    }
    const auto space_freed = [&]() {
        return avail >= 0 && avail0 >= 0 ? avail - avail0 : 0;
    };

    const auto over_cap    = [&]() { return cap > 0 && total > cap; };
    const auto short_space = [&]() {
        return space_engaged && avail >= 0 && avail < want_free;
    };
    // 【退避中要如实报 NO_SPACE，不能报 OK】min_free_space_bytes 明明没达成，
    // 只是我们已经知道再删也没用而主动不动手——报 OK 等于谎称目标达成。
    if (!over_cap() && !short_space()) return backed_off ? SYP_ERR_NO_SPACE : SYP_OK;

    std::sort(entries.begin(), entries.end(),
              [](const CacheDirEntry& a, const CacheDirEntry& b) {
                  return a.mtime < b.mtime;
              });

    size_t  attempts      = 0;   // 删除尝试次数（成功 + 失败）= 锁内 unlink 的上界
    size_t  examined      = 0;   // 看过的条目数 = 整个循环的上界
    size_t  deleted       = 0;
    int64_t logical_freed = 0;   // 删掉的 file_size 之和，只用来判"删了涨不涨"
    bool    capped        = false;  // 撞上界：下一轮还有得做
    bool    no_progress   = false;  // 删了也不涨：记进 space_backoff_，下轮不再试
    size_t  start         = 0;   // 本轮从 LRU 名单的第几条起步（见下）
    size_t  stopped_at    = 0;   // 本轮停在第几条（= 第一条**没看过**的）
    // 【查引用与删文件必须在同一次持有 mu_ 里】中间放锁的话，别人可以在
    // “查到没人用”与“unlink”之间把这个 key acquire 走，于是活着的
    // CacheIndex 声称有数据、.dat 却已经没了（cache_index.h “残留风险”第 2 条）。
    // 这一段只做 find + unlink + statvfs，不扫目录，且三个上界都在，
    // 所以锁内开销与目录规模无关。
    {
        std::lock_guard<std::mutex> g(mu_);
        // 【"还有没有人开着"按**文件**判，不按 key 串判】
        // 先把"落在本目录（按 dev+ino）上的、还开着的那些哈希"收一遍。
        // 代价是一次 O(打开着的句柄数) 的遍历——不是目录规模，所以
        // "锁内工作量与目录规模无关"这条仍然成立（典型只有 1~4 条）。
        // key 串本身活在 entries_ 里、且整段都握着 mu_，string_view 安全。
        std::set<std::string_view> live_here;
        if (self_id.valid) {
            for (const auto& kv : entries_) {
                if (!(kv.second.dir == self_id)) continue;
                const std::string& k = kv.first;
                const size_t sep = k.rfind(kKeySep);
                if (sep == std::string::npos) continue;
                live_here.emplace(std::string_view(k).substr(sep + 1));
            }
        }
        // 【起步位置不总是 0：≥kMaxEvictPerRound 条删不掉的
        //   头部条目会把整个目录楔死】
        // "删除失败照样吃 attempts 额度"这条规矩本身是对的（它拦的是
        // "3N 次失败的 unlink 全压在 mu_ 下"），但它和"LRU 顺序稳定"合在一起
        // 会产生一个死角：只要最旧的 32 条删不掉，每一轮都把 32 次尝试额度
        // 原样花在同样那 32 条上、deleted 恒为 0，排在它们后面**删得动**的
        // 条目永远轮不到。本机实测（40 条 ×100KB、最旧 32 条
        // chflags uchg、max_cache_bytes=100000）：**10/10 轮 deleted=0，
        // 目录永远停在 4,099,350 B（41× 上限）**，后面 8 条一次都没轮到。
        //
        // 修法是一个**只在这个死角里才生效**的游标：一轮如果"撞上界且一条
        // 都没删成"，就把游标推到本轮停下的位置，下一轮从那里起步；任何
        // 别的结局（删成了 ≥1 条，或者根本没撞上界）都把游标清回 0。
        // 于是：
        //   · 正常情况下严格 LRU，一个字节的行为都没变（游标恒为 0）；
        //   · 楔死形状里每隔一轮就能推进一次（本机实测：轮 1 deleted=0、
        //     轮 2 deleted=8、目录回到上限之下），容量策略不再终身失效。
        // 代价说清楚：楔死那一轮的下一轮**不是严格 LRU**（它跳过了最旧的
        // 那几条），而那几条本来就删不掉；以及每轮仍然有 32 次注定失败的
        // unlink（退化率 97%），这条没修——修它要一份"删不掉"的黑名单，
        // 而黑名单什么时候失效本身是个需要时钟的新问题。
        {
            auto it = evict_cursor_.find(cache_dir);
            if (it != evict_cursor_.end()) start = it->second;
            if (start >= entries.size()) start = 0;
        }
        stopped_at = start;
        for (size_t i = start; i < entries.size(); ++i) {
            const CacheDirEntry& e = entries[i];
            stopped_at = i;
            // 两条判据都满足了就收工。【这里不需要再区分"预算用完"与"真的
            // 够了"】实测记账之下这两件事是同一个条件，见上面 space_freed
            // 那段：凡是走到这里，就是真的够了，报 SYP_OK 不会骗人。
            if (!over_cap() && !short_space()) break;
            if (++examined > kMaxExaminePerRound) { capped = true; break; }
            if (attempts >= kMaxEvictPerRound)    { capped = true; break; }
            stopped_at = i + 1;   // 这一条真的看过了

            // e.key 是文件名里的哈希，也就是 CacheIndex::key_for_url 的输出；
            // 注册表的 key 是 cache_dir + US + 它。**不能复用 make_key**——
            // make_key 的签名是 (cache_dir, url)，而这里手上没有 URL，只有哈希。
            //
            // 【两道判定，缺一不可】
            //  · 按 key 串：调用方与持有者用的是**同一种拼法**时，这一条就够，
            //    而且不依赖 stat 成不成功（DirId 无效时它是唯一的一道）；
            //  · 按 dev+ino（live_here）：两种拼法指向同一个目录时，上面那条
            //    查不到，而 scan_cache_dir 拼出来的却是**同一组文件**。这就是
            //    F1——绝对 vs 相对、APFS 大小写、`/var` vs `/private/var`、
            //    NFC vs NFD、符号链接目录，五类实测全中。
            // 两条都在 ++attempts **之前** continue：被引用的条目不吃尝试额度
            // （这条老规矩本身的代价见头文件"≥kMaxEvictPerRound 条删不掉的
            // 头部条目会楔死"那一段）。
            std::string full = cache_dir;
            full.push_back(kKeySep);
            full.append(e.key);
            if (entries_.find(full) != entries_.end()) continue;  // 被引用：不算尝试
            if (live_here.find(e.key) != live_here.end()) continue;  // 同上，跨拼法

            ++attempts;   // 【在任何 continue 之前计数】失败也是系统调用
            // 删不掉就跳过它继续往后走——**不能 break**。LRU 顺序是稳定的，
            // 撞上几条删不掉的就收手，等于让排在它们后面删得动的条目永远
            // 轮不到，容量策略对这个目录终身失效。attempts 已经计过了，
            // 锁内的系统调用次数照样有上界。
            if (remove_cache_entry(e) != SYP_OK) continue;
            ++deleted;
            total -= e.bytes;
            if (total < 0) total = 0;
            if (!space_engaged) continue;

            logical_freed = sat_add(logical_freed, e.bytes);
            avail = free_space_bytes(dir);
            // 【删了也不涨 → 收手】逻辑上已经删掉 ≥1MiB，实测却连 1/8 都没
            // 回来，说明删的是稀疏空洞（或者别人正以同样的速度吃盘）：继续
            // 删只是纯粹的破坏，换不来空间。这条是把"多轮抽干"真正掐断的地方。
            // 门槛设在 1MiB 而不是"第一次就判"，是给文件系统的延迟回收留余地
            // （APFS 的删除是异步的，刚删完 statvfs 可能还没反映出来）。
            if (logical_freed >= kProgressCheckBytes
                && space_freed() < logical_freed / kProgressRatio) {
                no_progress = true;
                break;
            }
        }
        if (no_progress) {
            // 记下"在这个可用空间水位上，删除是换不来空间的"。
            if (space_backoff_.size() >= kMaxBackoffDirs) space_backoff_.clear();
            space_backoff_[cache_dir] = avail;
        }
        // 游标：只有"撞上界 + 一条都没删成"这一个结局才推进（见上面那段）。
        // 推到 stopped_at（= 第一条本轮没看过的），越界就绕回 0。
        if (capped && deleted == 0 && !entries.empty()) {
            size_t next = stopped_at;
            if (next >= entries.size()) next = 0;
            if (evict_cursor_.size() >= kMaxBackoffDirs) evict_cursor_.clear();
            evict_cursor_[cache_dir] = next;
        } else {
            evict_cursor_.erase(cache_dir);
        }
        last_round_.examined = static_cast<int64_t>(examined);
        last_round_.attempts = static_cast<int64_t>(attempts);
        last_round_.deleted  = static_cast<int64_t>(deleted);
    }

    // 最终复核。【只有真的走了"可用空间"这一支才重新量】否则一轮只为
    // max_cache_bytes 干活、而且干成了，却因为别的进程中途吃了盘被判成
    // NO_SPACE——报了一个与本轮目标无关的失败。
    bool still_short = false;
    if (space_engaged) {
        avail = free_space_bytes(dir);
        still_short = avail >= 0 && avail < want_free;
    }
    const bool still_over = cap > 0 && total > cap;
    // 退避中：min_free 这一项是知情不做，无论 cap 那一项做得多好都不算达成，
    // 所以不能报 OK。但**也不能立刻报 NO_SPACE**——见下面 capped 那一条。
    if (!still_over && !still_short && !backed_off) return SYP_OK;
    // 删了也不涨：已经记了退避，在可用空间回升之前再调**确实**没用。
    if (no_progress) return SYP_ERR_NO_SPACE;
    // 试过了却一条都没删掉（权限 / 只读 / 全被引用）：再调也是这个结果。
    //
    // 【这句"再调也是这个结果"有一个例外，而且是刻意留着的 —— F12】
    // capped && deleted == 0 时游标已经推进，**下一轮看的是另一批条目**，
    // 所以严格说这里再调一次可能会有不同结果。仍然报 NO_SPACE 而不是 BUSY，
    // 是因为 BUSY 的语义是"还有得删、接着调"，而这一轮的事实是"手上这 32 条
    // 一条都删不动"——对一个整个目录都只读的调用方（
    // enforce_capacity_gives_up_on_a_directory_it_cannot_delete_from 那个形状）
    // 报 BUSY 会让它无限重试。代价是 NO_SPACE 在这个形状下**偏保守**：
    // 它说"没戏"，而下一轮其实可能删得动。保守方向是安全的那一侧
    // （唯一的消费者会减产，不会加产），而目录本身照样会被下一轮清理。
    if (attempts > 0 && deleted == 0) return SYP_ERR_NO_SPACE;
    // 撞上界：本轮主动收手，下一轮还有得做。**必须是 BUSY 不是 NO_SPACE**
    // ——NO_SPACE 的意思是"再调也没用"，按它减产的预加载器会停下来干等，
    // 而淘汰下一轮照样接着删，正好反了。
    //
    // 【这一条必须排在 backed_off 之前】默认配置两项限额都开着，于是"可用
    // 空间那一支进了退避、字节上限那一支还在全速淘汰"是常态形状。先判
    // backed_off 的话，这样一轮（刚删掉 32 条、下一轮还会再删 32 条）会被
    // 报成"再调也没用"，唯一的消费者 Preloader 就会在缓存正被抽下去的同时
    // 停产干等——正是 BUSY/NO_SPACE 这组语义要防的那件事，方向还反了。
    if (capped) return SYP_ERR_BUSY;
    // 退避中、这一轮又没别的进展可报：如实说"在可用空间回升之前再调没用"。
    if (backed_off) return SYP_ERR_NO_SPACE;
    // 名单走完了还不够：剩下的要么正被引用、要么就没有了。
    return SYP_ERR_NO_SPACE;
}

CacheStore::RoundStats CacheStore::last_round_stats_for_test() const {
    std::lock_guard<std::mutex> g(mu_);
    return last_round_;
}

void CacheStore::drop_unreferenced_for_test() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.refs <= 0) it = entries_.erase(it);
        else ++it;
    }
}

}  // namespace syp::dl
