// cache_store.h — 进程内缓存注册表：同一 cache key 只存在一份 CacheIndex/CacheFile
//
// 【为什么需要它】没有它的时候，同一个 URL 的两个 syp_source 各开一份
// CacheIndex：A 下了 [0,1M)、B 下了 [2M,3M)，两边 save() 都是
// tmp + rename 整份替换，**后写的把先写的区间表整份覆盖**，磁盘上的字节还在，
// 索引却只认得其中一半——下次打开就把另一半当洞重下（cache_index.h:54-55 的
// “同一实例的并发访问需要外部加锁”说的是实例内，这里是两个实例之间）。
// 预加载与播放共用缓存目录，这一条不解决就等于边下边丢。
//
// 【为什么是进程内注册表而不是文件锁】跨进程共享是独立课题（要处理锁的
// 崩溃遗留、跨进程可见性），而本项目的预加载与播放天然同进程。注册表几十行
// 就够，文件锁不够用还更难验证。跨进程共享明确是非目标。
//
// 【锁的纪律，与 source_bridge.h 的类注释互为引用】
//   · CacheStore::mu_ 保护注册表本身，**在它下面会做文件 IO**（首次 open）。
//     它与 Handle::mu / SourceBridge::mu_ **从不同时持有**，所以不可能成环。
//     调用 acquire/release/enforce_capacity 时不得持有任何其它锁。
//   · Handle::mu 是叶子锁，保护 *index 的全部访问，以及 sync+save 这一对的
//     原子性。持有它时不得取任何其它锁、不得调用户回调。
//   · *file 的 read_at/write_at **不需要** Handle::mu（cache_file.h 明写
//     pread/pwrite 并发安全），只需要一份 shared_ptr 保证对象存活——调用方
//     应在自己的锁下拷一份 shared_ptr 出来，在锁外用那份局部拷贝。
#pragma once

#include "cache_file.h"
#include "cache_index.h"

#include <syplayer/syp_config.h>
#include <syplayer/syp_types.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace syp::dl {

// log_msg 的定义在 source_bridge.cpp（同一个静态库、同一个命名空间）。
// 这里只前置声明，避免 cache_store.h ↔ source_bridge.h 互相 include。
void log_msg(syp_log_level lvl, const char* tag, const char* msg);

// 缓存目录里属于同一个 key 的一组文件（.idx / .dat / .idx.tmp）。
// 原先是 source_bridge.cpp 匿名命名空间里的 CacheEntry，现在提到这里：
// cache_dir_evict() 与 CacheStore::enforce_capacity() 用的是同一套扫描与
// LRU 判据，两处各写一份必然漂移。
struct CacheDirEntry {
    std::string                     key;
    std::filesystem::path           idx;
    std::filesystem::path           dat;
    std::filesystem::path           tmp;
    int64_t                         bytes = 0;
    // **`.idx` 的 mtime**，不是三个文件的 max。TTL（entry_expired）判的也是
    // `.idx`，两边必须是同一个时间源——否则会出现"按 TTL 没过期、按 LRU 是
    // 最旧的"这种自相矛盾，"TTL 与 LRU 都用 .idx 的 mtime"这条约定
    // 正是为了避免它。取 max 曾经让用例失去鉴别力：`.dat` 的 mtime 盖过
    // `.idx`，于是"把两条的 .idx 时间对调"照样通过。
    // `.idx` 不存在（只剩孤儿 `.dat`）时是 file_time_type::min()，排序上最旧、
    // 最先被淘汰——孤儿本来就读不出来，先走正合适。
    std::filesystem::file_time_type mtime{};
};

// 目录不存在 = 空缓存（*err 为 SYP_OK）。目录可读但中途出错时 *err 非 OK，
// 返回已扫到的部分。
std::vector<CacheDirEntry> scan_cache_dir(const std::filesystem::path& dir,
                                          syp_status* err);

// 删掉一个 key 的三个文件（幂等）。返回第一个非 OK 的错误。
syp_status remove_cache_entry(const CacheDirEntry& e);

// cache_dir 的**词法**归一化：lexically_normal()（折叠 "//"、消掉 "."、按字面
// 弹掉 ".."）再剥掉尾部多余的 '/'（根目录 "/" 保留）。**全仓唯一的一份实现**。
//
// 【为什么住在这里】它原先在
// src/media/media_info_provider.h，于是 src/dl 够不到：syp_source_open /
// syp_preloader_create 收到的 cache_dir 只能原样用（src/dl 不许依赖 src/media），
// 直接用 C ABI 的调用方混用两种拼法照样静默分裂成两份缓存。
// 当时的注释把修法写成了**假二选一**（"要么破 purity 闸、要么抄第二份"）——
// 那是错的，下沉到 src/dl 是第三扇门，而且是最便宜的一扇：purity 闸
// （CMakeLists.txt:59-80）只禁 `.mm`/`.m` 源与链接接口里的平台 framework，
// 对纯 C++ stdlib 没有意见，而本文件**早就** include 了 `<filesystem>`。
// 搬下来之后 src/media 调的是同一个函数，两层共一份实现、不可能漂移。
//
// 【为什么必须只有一份】CacheStore::make_key **不**归一化目录，
// 所以同一个目录的两种拼法在注册表里是**两个
// key**：同一组 .idx/.dat 会被两个 CacheIndex 同时打开、互相盖写，而 Preloader
// 靠 open_count(key) 判断"这个 key 上还有没有播放源开着"的让路逻辑也直接失效
// ——两边都不会报错。
//
// 【在哪里调 —— 收口点只有三个，都在"cache_dir 变成本对象自己那份 std::string"
// 的那一行】
//   · SourceBridge 的构造函数（source_bridge.cpp）—— 覆盖 syp_source_open 与
//     所有内部开源的路径；
//   · Preloader 的构造函数（preloader.cpp）—— 覆盖 syp_preloader_create；
//   · PreloadStack::create（src/media）与接入层 SYPBridge.mm 的
//     prepare_cache_dir —— 它们要**提前**拿到归一化后的串（一个用来建目录、
//     一个用来判空并让 provider 与 preloader 共享同一份），所以自己先调一次。
// 前两个是 C ABI 真正的闸；后两个靠幂等与前两个叠加无害（见下）。
// 选构造函数而不是 syp_source_open / syp_preloader_create 的函数体，是因为
// 那两处的 `copy_config` 只 memcpy，`cache_dir` 仍指向调用方的缓冲区——在那里
// 归一化要另起一个必须活过整次调用的局部 std::string；而构造函数里
// `cache_dir_` 这个成员**本来就是**那份拷贝，而且它同时是 make_key 的入参和
// 落盘路径的来源，一处改两用都对。
//
// 【守的到底是哪几种拼法，别按直觉记】
// Foundation 侧（probe_fnd.swift，本机实测）：
//     URL(fileURLWithPath:"/tmp/d/").path    == "/tmp/d"      ← 尾斜杠**产不出**
//     URL(fileURLWithPath:"/tmp//d").path    == "/tmp//d"     ← 原样保留
//     URL(fileURLWithPath:"/tmp/./d").path   == "/tmp/./d"    ← 原样保留
//     URL(fileURLWithPath:"/tmp/a/../b").path== "/tmp/a/../b" ← 原样保留
// 也就是说**公开 API（SYPlayerCacheConfiguration(directory:)）能产出的恰恰是
// "//"、"."、".." 这三类，唯独产不出尾斜杠**。而最初的实现只剥尾斜杠，方向
// 正好守反了。触发它的不是畸形输入：NSTemporaryDirectory() 自身以 '/' 结尾，
// 于是最自然的 NSTemporaryDirectory() + "/" + name 就产出 "…/T//name"，实测
// 后果是播放侧的 open_count 读到 0 —— 让路逻辑静默失效。
// 尾斜杠那一路仍要守：桥那层的 SypCacheSettings.directory 是裸 NSString，
// 绕过 URL 就能带尾斜杠进来。
//
// 【为什么不能只用 lexically_normal】它自己会造出尾斜杠：末段解析成 "." 或
// ".." 时补一个空末元素。实测 lexically_normal("a/./")=="a/"、
// ("/a/.")=="/a/"、("/a/b/..")=="/a/"、("/tmp/d/")=="/tmp/d/"。所以顺序必须是
// **先 lexically_normal、后剥尾斜杠**；反过来写 "a/./" 会停在 "a/"，与 "a"
// 仍然是两个 key。见 .cpp 里的注释与 tests/test_media_info_provider.cpp 的表。
//
// 【"//" 开头：POSIX 说实现定义，我们按 "/" 处理】POSIX 允许恰好两个前导
// '/' 表示一个独立的命名空间。Darwin **没有**这种语义（前导 "//" 与 "/" 等价），
// libc++ 的 lexically_normal 也把 "//a/b" 折成 "/a/b"，本函数直接沿用。本仓只
// 在 Apple 平台上构建，所以这不是妥协，是与平台一致；移植到有 "//" 语义的系统
// 时这里要重新判断。
//
// 【四条刻意不做的事 —— 是已知局限，不是缺陷】
//  0. **不把相对路径变成绝对路径**，所以 "cache" 与 "/abs/path/cache" 即便指向
//     同一个目录，也**仍然是两个 key**（实测：`"/…/n5"` vs `"n5"`
//     → SPLIT）。lexically_normal 是纯词法的，永远不会去拿 cwd 来补前缀。
//     Swift 门面产不出相对路径（URL.path 恒为绝对），但 **C ABI 调用方完全
//     可能写 `cfg.cache_dir = "cache"`**，那条路上这个分裂是真的。要合流得用
//     fs::absolute()，而它依赖进程的当前工作目录——一个会被任何人随时改掉的
//     全局状态，拿它当 key 的一部分比分裂更糟。
//  3. **不做大小写折叠**。APFS 默认是大小写不敏感（但保留大小写）的卷，
//     `Sub` 与 `sub` 是同一个目录却是两个 key（实测 SPLIT）。不折叠的理由与
//     符号链接同源：卷是否大小写敏感是**文件系统真值**，要查 statfs 才知道，
//     而这个函数刻意只做词法。后果是一份缓存被分成两半，不是错播。
//
// 上面 0/3 与下面 1/2 的共同点：全部是"同一个目录的两种拼法"。
//
// 【这里原先写着"后果一律是缓存分裂……不会串到别人的数据上"——那句话是错的】
// 分裂确实是**读写**那一侧的后果（key 里带着 URL 的
// 哈希，两个 key 指的仍是各自正确的内容），但 `enforce_capacity` 那一侧不是：
// 它手上只有哈希，原先拿调用方传进来的那个 cache_dir 串拼 key 去问"还有没有
// 人开着"，于是**两种拼法查不到对方的活条目，而扫描拼出来的却是同一组文件**
// ⇒ 把还开着的 .idx/.dat 直接 unlink 掉。实测五类拼法
// （绝对 vs 相对、APFS 大小写、`/var` vs `/private/var`、目录符号链接、
// NFC vs NFD）**全部 .idx/.dat GONE 而 open_count 仍为 1、返回值还是 SYP_OK**，
// 而活着的 CacheIndex 仍然声称有 [0,600000)、盘上一个字节都没有。
//
// 现在这个洞由 `Entry::dir`（目录的 dev+ino）关上了，见下面 DirId 那段：
// 判"这组文件还有没有人开着"按**文件系统身份**，不按 key 串。所以本清单
// 描述的仍然只是**缓存分裂**，但那是修完之后才成立的事实，不是当初以为的
// "词法归一化天然就够"。
// 还有一处同源的残留：`space_backoff_` 仍然按 cache_dir **串**记账，两种
// 拼法各记一份。后果只是退避不共享（另一种拼法照样会去删），不会删到活文件
// ——那一层由 dev+ino 挡着。登记在这里，不修：退避是性能策略不是正确性。
//
//  1. **不做 Unicode 归一化**。之所以不需要：Swift 门面两侧都经
//     SYPlayerCacheConfiguration.bridged() → URL.path，而 Foundation 的
//     URL.path **恒输出 NFD**（实测：NFC 的 "café" 进去，出来是 e + U+0301；
//     NFD 进去出来仍是 NFD），逐字节相同，带重音的目录名**不会**分裂成两个 key。
//     残留风险只在"绕过 Swift 门面、直接给桥的 SypCacheSettings.directory
//     塞一个 NFC 的 NSString"或"直接用 C ABI"这两条路上。
//  2. **不解析符号链接**，也就是 ".." 是**按字面**弹栈的。"/a/link/.." 在
//     link 是符号链接时词法结果 "/a" 与真实目录不同。对 cache_dir 这是可接受的：
//     这个函数要的是"同一个目录只有一种拼法"这个 key 的唯一性，不是文件系统
//     真值；realpath 会去碰盘、对尚不存在的目录还会失败（缓存目录第一次用时
//     正是不存在的）。要跨符号链接也合流，得由调用方自己先 resolve。
//     顺带一提，Foundation 自己的 standardizedFileURL 与 resolvingSymlinksInPath
//     在 "$T/x/link/../c" 上给的**也都是**词法答案，我们与平台的约定一致。
//
// 空串原样返回空串——"目录没填"是调用方要当错误处理的事，不是这个函数
// 该替它编一个默认值（SourceBridge::open 与 PreloadStack::create 各自判空并
// 报 SYP_ERR_INVALID_ARG）。
//
// 【幂等，而且必须幂等】预加载那条链上它被调用**三次**（接入层的
// prepare_cache_dir 一次、PreloadStack::create 一次、Preloader 构造函数再一次），
// 播放那条链两次。几条链要落在同一个串上，等价于
// normalize(normalize(x)) == normalize(x)。
// tests/test_media_info_provider.cpp 里整张表都顺带跑了第二遍来钉这一条。
std::string normalize_cache_dir(std::string dir);

class CacheStore {
public:
    // 引用计数句柄。持有者负责在不再需要时调一次 release(key)。
    // 【不是 RAII】故意的：SourceBridge::close() 必须在一个非常具体的时刻
    // （~Scheduler 返回之后、放开 mu_ 之后）才释放，RAII 的析构点由作用域
    // 决定，正好是这里不能接受的那种“由编译器挑时机”。
    struct Handle {
        std::shared_ptr<std::mutex> mu;
        std::shared_ptr<CacheIndex> index;
        std::shared_ptr<CacheFile>  file;
        std::string                 key;
        bool valid() const noexcept { return index != nullptr && file != nullptr; }
    };

    static CacheStore& get();

    // key = cache_dir + '\x1f' + CacheIndex::key_for_url(url)。
    // **本函数自己不做任何规范化**：传什么拼法就是什么 key。归一化由上面的
    // normalize_cache_dir() 负责，收口在 SourceBridge / Preloader 的构造函数里
    // （也就是 cache_dir_ 诞生的那一行），所以本函数的调用方拿到的串已经归一化过。
    //
    // 【为什么不把归一化塞进本函数里，两条理由】
    //  1. **它会把注册表的 key 与落盘路径拆成两个来源。** acquire() 同时用
    //     cache_dir 算 key **和** 造 std::filesystem::path(cache_dir)；
    //     更要命的是 enforce_capacity() 手上只有哈希、没有 URL，所以它是
    //     **手工拼** `cache_dir + US + hash` 去 entries_ 里查"这个 key 还有没有
    //     人开着"（见 .cpp 里那段注释）。只在 make_key 里归一化，这一处就查不到
    //     那条活着的条目，于是**把还开着的 .idx/.dat 删掉**——正是 CacheStore
    //     存在的理由那个 bug。要补就得在那里再写一次归一化，而这个里程碑已经被
    //     "同一道闸的第二份转写悄悄漂移"咬过两次。
    //  2. **它改的是一条被用例明确钉住的契约。** 本机实测：在 make_key 里加
    //     normalize_cache_dir 之后 `ctest` 红 1 个二进制、2 条用例、5 条断言——
    //     test_cache_store.cpp 的 make_key_requires_byte_identical_dir_string
    //     （:151/:152/:153）与 trailing_slash_dir_resurrects_the_split_index
    //     （:180/:181）。后一条正是"两种拼法 ⇒ 两份索引 ⇒ 整份覆盖"这个
    //     要修的 bug 的回归用例，它的鉴别力来自 make_key **不**归一化。
    static std::string make_key(const std::string& cache_dir, const std::string& url);

    // 取（必要时打开）一份共享的索引与内容文件。引用计数 +1。
    // 失败时返回 valid() == false 的句柄并写 *err，**不会**留下引用计数。
    //
    // 首次打开时按 cfg.cache_ttl_ms 判过期：过期就丢弃该 key 的
    // .idx/.dat 当作全新资源。已经被别人打开着的 key 不判 TTL——它正在被用。
    Handle acquire(const std::string& cache_dir, const std::string& url,
                   const syp_config& cfg, syp_status* err);

    // 引用计数 -1；归零时从注册表摘掉（对象本身活到最后一个 shared_ptr 消失）。
    // key 不存在时是 no-op。
    void release(const std::string& key);

    // 当前有几个持有者。0 表示没人打开（可被淘汰）。
    int64_t open_count(const std::string& key) const;

    // 按 cfg 的三项容量配置淘汰这个目录里的缓存。跑**一轮**，有界。
    //
    // 判据（任一成立即淘汰）：
    //   · max_cache_bytes > 0 且目录合计字节数 > 它；
    //   · min_free_space_bytes > 0 且卷的可用空间 < 它。
    // 顺序：.idx 的 mtime 升序（最久没动的先走）。
    // **open_count(key) > 0 的 key 一律跳过**——正在被读写的资源被删掉
    // 会让活着的 CacheIndex 声称有数据、文件里却是洞（cache_index.h 顶部
    // “残留风险”第 2 条描述的正是这个 TOCTOU）。
    //
    // 【一轮是有界的，这是刻意的设计决定，不是实现细节】两个上界：
    //   1. **删除尝试次数**（kMaxEvictPerRound）。计的是**尝试**，不是成功：
    //      只数成功的话，一个删不动的目录（只读 / EPERM / immutable）会让
    //      循环把 N 个条目全试一遍、3N 次失败的 unlink 全压在 mu_ 下，上界
    //      永远不触发——正是这个上界要拦的事，只不过在失败路径上。
    //   2. **看过的条目数**（kMaxExaminePerRound）。被引用的 key 只花一次
    //      map::find，没有系统调用，但"几乎全被引用"的目录仍然会让循环在
    //      mu_ 下走满 N 次。有了它，锁内工作量与目录规模无关。
    // 删除失败**不中断**这一轮：LRU 顺序稳定，撞上几条删不掉的就 break，
    // 等于让排在后面删得动的条目永远轮不到，容量策略对这个目录终身失效。
    // 失败照样消耗尝试次数，所以锁内的系统调用仍然有上界。
    // 撞上界不是错误，下一轮接着删（调用方本来就在反复调）。
    //
    // 【删掉 break 只是把楔死的阈值抬到了 32，没有消掉它】
    // 这段注释原先把"≥N 条删不掉的头部条目会楔死"写成一个**已经消掉的
    // bug**。那是错的：删除失败照样吃 attempts 额度，而被引用的条目在
    // ++attempts **之前** continue，所以只有"删不掉"这一类消耗尝试额度。
    // 于是阈值只是从 N 变成了 kMaxEvictPerRound = 32。实测
    // （40 条 ×100KB、最旧 32 条 chflags uchg、max_cache_bytes = 100000）：
    // **10/10 轮 deleted=0，目录永远停在 4,099,350 B——41 倍于上限**，
    // 后面 8 条删得动的一次都没轮到。
    //
    // 现在由一个**只在这个死角里才生效的起步游标**兜住（evict_cursor_）：
    // 一轮"撞上界且一条都没删成"时把游标推到本轮停下的位置，下一轮从那里
    // 起步；任何别的结局都把游标清回 0，所以正常路径严格 LRU、行为一个字节
    // 都没变。同一形状修复后实测：轮 1 deleted=0、**轮 2 deleted=8**，
    // 目录回到上限之下。
    //
    // 【没修的那一半，如实记着】每轮仍然有最多 32 次注定失败的 unlink
    // （LOCKED=31 时实测每轮 32 次尝试换 1 次成功，退化率 97%，96 次失败的
    // unlink 全在 mu_ 下）。修它要一份"删不掉"的黑名单，而"黑名单什么时候
    // 失效"本身是个需要时钟的新问题——dl 层没有 Clock。代价有上界（锁内
    // 系统调用次数照旧被 kMaxEvictPerRound 钉住），所以留着。
    //
    // 【释放量一律实测；"删了不涨"会**跨轮**退避】"释放了多少"用 statvfs 的
    // 差值算，不用 file_size 累加：.dat 天生稀疏（按区间随机落盘），逻辑长度
    // 可以远大于占用的块数，拿它记账的话删掉一个 8MiB 的稀疏条目就能把"缺口"
    // 抵完而 avail 一点没动。逻辑上删掉 ≥kProgressCheckBytes 而实测回来的不到
    // 1/kProgressRatio，就判定"删也换不来空间"，收手，并把当时的可用空间记进
    // space_backoff_：在可用空间回升到那个水位之上以前，**后续各轮都不再为
    // min_free_space_bytes 删任何东西**。只在一轮之内收手是不够的——一轮删一条、
    // 发现没用、收手，下一轮重新来过又删一条，若干轮之后目录照样空了。
    //
    // 【这条判据的已知盲区】它要求"32 次尝试之内累计删掉 ≥1MiB"才可能触发。
    // 平均条目小于约 32KiB 的目录永远凑不够这个量，于是退避不会生效——而那
    // 恰好是每轮丢条目最多的形状。代价有上界（每轮最多 32 条、且总量 <1MiB），
    // 但这个盲区是真实存在的，不是被覆盖掉的。
    //
    // 【目录扫描在 mu_ 之外做】扫描是 O(目录条目数) 次 stat，而 acquire()
    // 也要取同一把 mu_：在扫描期间握着它，等于让后台预加载的淘汰轮次
    // 阻塞播放线程的 acquire()。所以先无锁扫描、排序，再取 mu_ 做
    // “查引用计数 + 删文件”这一段——查与删必须在同一次持有里，否则
    // 中间会被别人 acquire 走（TOCTOU）。
    //
    // 本方法自己不节流：调用方负责（SourceBridge 在 save_index 成功之后
    // 按“距上次 ≥ 1s 或写入量 ≥ 8MiB”调）。
    //
    // 返回值。**注意：它至今没有任何消费者，而且本来应该声明成 `void`**。
    //
    // 事实（逐个查过，不是推断）：全仓唯一调用点是 source_bridge.cpp 的
    // `(void)enforce_capacity(...)`；`SYP_ERR_NO_SPACE` / `SYP_ERR_BUSY`
    // 在 preloader / API 层 / src/media / swift 里**一次都没有为它出现过**。
    // 也就是说这个值在外部完全不可观测，除了本类的测试缝
    // last_round_stats_for_test()（而那个缝自己还会说谎）。
    //
    // 真正该接它的是"缓存满了要不要停预加载"这条决策，那是将来的事。
    // 在接上之前，不必再为这个无人读的值反复修改。
    //
    // 语义（接的时候照这个理解）：
    //   · SYP_OK             —— 本轮结束时两条判据都满足（含一开始就满足）；
    //   · SYP_ERR_BUSY       —— 本轮撞上界主动收手，下一轮还有得做
    //                           （“本次未执行完，不是故障”，见 syp_types.h）。
    //                           【这里原先写的是"而且**确实删掉了东西**"——
    //                           那句话与代码矛盾】：撞的是
    //                           `examined > kMaxExaminePerRound` 那条上界时，
    //                           这一轮完全可以一条都没删成（512 条全被引用），
    //                           照样走到 BUSY。真正的不变式只有"主动收手，
    //                           名单没走完"。反过来"attempts 用完且 deleted==0"
    //                           在返回值上排在 BUSY **之前**，报的是 NO_SPACE，
    //                           所以 BUSY 里 deleted==0 的唯一来源就是
    //                           examined 那条上界。】删得动却没删完时必须报这个、
    //                           不能报 NO_SPACE：否则预加载器会在淘汰仍在
    //                           推进的同时停产干等，正好反了。**这一条压过
    //                           "可用空间那一支在退避中"**：默认配置两项限额
    //                           都开着，一轮完全可以一边为 min_free 退避、
    //                           一边为 max_cache_bytes 删满 32 条；那样的一轮
    //                           是 BUSY，不是 NO_SPACE；
    //   · SYP_ERR_NO_SPACE   —— 本轮没能达成，而且**在外部条件变化之前**再调
    //                           也没用：名单走完了还不够（剩下的正被引用）、
    //                           尝试过但一条都删不掉、或者"删了也不涨"已经
    //                           进入退避。注意"外部条件变化"这个限定不是
    //                           修辞：退避会在卷的可用空间回升时自动解除，
    //                           被引用的 key 会在别人 release 之后变得可删。
    //                           它保证的是"**立刻**再调一次不会有不同结果"，
    //                           不是"这个目录从此无药可救"；
    //                           【这条限定有两个例外，都要如实记着】
    //                           (1) "attempts 用完且 deleted==0"这一轮会推进
    //                               起步游标，所以**下一轮看的其实是另一批
    //                               条目**，可能删得动。这里仍然报 NO_SPACE
    //                               而不是 BUSY，是刻意偏保守：BUSY 的语义是
    //                               "还有得删、接着调"，对一个整个目录都只读
    //                               的调用方报 BUSY 会让它无限重试。保守的
    //                               方向是安全的那一侧（唯一的消费者会减产，
    //                               不会加产）。
    //                           (2) 在**游标还没修好之前**，"最旧的 ≥32 条
    //                               删不掉"这个形状下 NO_SPACE 是**永久**的
    //                               （实测 10/10 轮 deleted=0），也就是说那时
    //                               "不是无药可救"这句话在这个形状里是假的。
    //                               现在不是了，但这段历史值得留着：这条契约
    //                               的强度取决于循环有没有前进的办法；
    //   · SYP_ERR_INVALID_ARG —— cache_dir 为空；
    //   · 扫描目录的 IO 错误原样返回。
    syp_status enforce_capacity(const std::string& cache_dir, const syp_config& cfg);

    // 上一轮 enforce_capacity 在 mu_ 下干了多少活。
    //
    // 【为什么要把它暴露出来】"一轮是有界的"这条不变式在外部**完全不可观测**：
    // 一个把 N 个条目全试一遍的实现，和一个撞上界就收手的实现，返回值和磁盘
    // 上的结果可以一模一样（删不动的目录就是这样——两者都是"什么也没删、
    // 返回 NO_SPACE"）。没有这个计数器，上界只能靠人眼审代码，回归时不会红。
    // 生产代码不读它。
    struct RoundStats {
        int64_t examined = 0;   // 看过的条目数（含被引用而跳过的）
        int64_t attempts = 0;   // 尝试删除的次数（成功 + 失败）= unlink 次数 / 3
        int64_t deleted  = 0;   // 真的删掉的条目数
    };
    RoundStats last_round_stats_for_test() const;

    // 测试用：把引用计数为 0 的条目全部摘掉。生产代码里没有这个需求
    // （release 归零时就摘了），只是给用例一个“回到干净状态”的入口。
    void drop_unreferenced_for_test();

private:
    CacheStore() = default;

    // 一个目录的**文件系统身份**（dev + ino）。
    //
    // 【它存在的唯一理由】
    // enforce_capacity() 手上只有文件名里的哈希，没有 URL，所以它原先是
    // **手工拼** `cache_dir + US + hash` 去 entries_ 里问"这个 key 还有没有
    // 人开着"。而 normalize_cache_dir() 只做**词法**归一：同一个目录的几类
    // 拼法归一化之后仍然是两个 key，可 scan_cache_dir() 拼出来的**文件路径
    // 是同一组文件** ⇒ 查不到那条活着的条目 ⇒ 直接 unlink，把还开着的
    // .idx/.dat 删掉（实测三类拼法全中，open_count 仍为 1、返回值还是
    // SYP_OK，而活着的 CacheIndex 仍然声称有 [0,600000)、盘上一个字节都没有
    // ——正是 cache_index.h「残留风险」第 2 条那个状态）。
    //
    // 【为什么是 dev+ino，不是"再写一份更强的归一化"】词法归一化永远补不上
    // 这个洞：相对 vs 绝对要 cwd、大小写折叠要 statfs、`/var` vs
    // `/private/var` 要解符号链接、NFC vs NFD 要知道卷的归一化策略——四件事
    // 全是**文件系统真值**，而 stat 一次就能一次性全给。更要紧的是，这个
    // 里程碑已经被"同一道闸的第二份转写悄悄漂移"咬过两次（R5/M13），再加
    // 一份转写是被证伪过的形状。
    //
    // 【为什么不改 make_key 去用它】key 必须是可比较、可哈希的串，而且
    // "make_key 不归一化"是被用例明确钉住的契约（见 make_key 的注释）。
    // dev+ino 只在 enforce_capacity 判"活没活"这**一处**用，不进 key。
    //
    // valid == false（stat 失败、目录还不存在）时一律**不参与**判定，
    // 退回原先的 key 串比对——那条路只会少判活、不会多判活，所以加上
    // dev+ino 这一层严格是"活着的集合只增不减"，不可能让本来删得掉的
    // 条目变得删不掉之外的任何后果。
    struct DirId {
        uint64_t dev   = 0;
        uint64_t ino   = 0;
        bool     valid = false;
        bool operator==(const DirId& o) const noexcept {
            return valid && o.valid && dev == o.dev && ino == o.ino;
        }
    };
    // stat 一次。失败（含目录不存在）返回 valid=false。
    static DirId dir_identity(const std::filesystem::path& p) noexcept;

    struct Entry {
        std::shared_ptr<std::mutex> mu;
        std::shared_ptr<CacheIndex> index;
        std::shared_ptr<CacheFile>  file;
        int64_t                     refs = 0;
        // 本条目的 .idx/.dat **真正落在**哪个目录（acquire() 里 stat 一次）。
        // enforce_capacity 靠它跨拼法认出"这组文件还开着"。
        DirId                       dir{};
    };

    mutable std::mutex           mu_;
    std::map<std::string, Entry> entries_;
    RoundStats                   last_round_{};   // mu_ 保护
    // cache_dir → 上一次判定"删了也不涨"时测到的可用空间。只要可用空间没有
    // 回升到这个水位之上，就不再为 min_free_space_bytes 删任何东西。
    // 【为什么必须跨轮记住】只在一轮之内收手挡不住抽干：一轮删一条、发现
    // 没用、收手，下一轮重新来过又删一条——若干轮之后目录照样空了。mu_ 保护。
    std::map<std::string, int64_t> space_backoff_;
    // cache_dir → 下一轮从 LRU 名单的第几条起步。**只在"撞上界且一条都没
    // 删成"这一个死角里非 0**，其余一切情形都被清回 0，
    // 所以正常路径严格 LRU、一个字节的行为都没变。理由与实测见 .cpp 里
    // 那段注释。mu_ 保护。
    std::map<std::string, size_t>  evict_cursor_;
};

}  // namespace syp::dl
