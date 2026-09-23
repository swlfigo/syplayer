// cache_index.h — URL 缓存的磁盘索引：已落盘区间、etag 校验、崩溃安全持久化
#pragma once

#include "hole_set.h"

#include <syplayer/syp_types.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace syp::dl {

struct CacheMeta {
    std::string url;
    std::string etag;           // 可空
    std::string last_modified;  // 可空
    int64_t total_length = -1;  // 未知为 -1
    int64_t created_at_ms = 0;
    int64_t updated_at_ms = 0;
};

// 磁盘布局（key = FNV-1a 64 的小写 hex，完整 URL 另存在索引里防撞车）：
//   <cache_dir>/<key>.idx      索引
//   <cache_dir>/<key>.dat      内容（稀疏文件，由 CacheFile 写）
//   <cache_dir>/<key>.idx.tmp  save() 的临时文件；load 忽略，下次 save 覆盖
//
// 写入顺序（正确性关键，不能反）：
//   1. CacheFile::write_at 把字节写进 .dat；
//   2. CacheFile::sync 把数据落稳；
//   3. add_range 把对应区间记进本对象；
//   4. save() 原子替换 .idx。
// 反过来会让索引声称有数据、文件里实际是洞：重启后读到垃圾还以为有效。
//
// 与 .dat 的一致性：open() 对 .dat 做一次 stat，拿到实际大小 dat_size，然后：
//   .dat 不存在而索引区间非空 → SYP_ERR_EOF（未命中，不要返回一份声称有数据的索引）；
//   区间整段超出 dat_size → 丢弃；
//   区间跨越 dat_size → 裁到 dat_size。
// 裁剪走 HoleSet::remove，所以返回后仍满足 HoleSet 不变式。
//
// 残留风险（stat 方案本质上挡不住）：
//   1. 中间被打洞：文件大小没变，洞里读出来是零，会被当成有效媒体数据
//      交给解复用器。
//   2. 校验只在 open() 那一瞬间做。会话进行中 .dat 被外部截断或删除时，
//      已经在内存里的 CacheIndex 不会再对照文件，仍按旧区间去读
//      （典型 TOCTOU）。
// 将来要彻底解决：每段存校验和，或用 SEEK_DATA / SEEK_HOLE 探测真实数据分布。
// 这一步只做 open() 时的 stat，不实现校验和 / SEEK_DATA，也不在读路径上重验。
//
// save()：写 .idx.tmp → fsync → rename 覆盖 .idx。崩溃后只能读到旧的或新的整份索引。
//
// 线程安全：CacheIndex 不是线程安全的。同一实例的并发访问（包括一边
// add_range/save、一边读 meta/ranges）需要外部加锁。
//
// 下游告警：total_length 未知（-1）时，索引里的区间上界只受 INT64 约束，
// 恶意/损坏的索引可以声称 [0, INT64_MAX)。下游做 pos + available 这类
// 偏移加法必须自己防溢出。
class CacheIndex {
public:
    static std::string key_for_url(std::string_view url);  // 小写 hex，文件名安全

    // 打开已有索引。
    //   文件不存在 / URL 与索引内不一致（哈希撞了）/ 版本不认识
    //   / .dat 不存在但索引区间非空 → SYP_ERR_EOF（未命中，可重下）。
    //   损坏（magic / CRC / 结构） → SYP_ERR_CACHE_CORRUPT。
    //   残留的 .idx.tmp 被忽略。
    //   .dat 比索引短：超出 dat_size 的区间丢弃，跨越的裁到 dat_size。
    static std::expected<CacheIndex, syp_status> open(
        const std::filesystem::path& cache_dir, std::string_view url);

    // 新建空索引（不落盘，直到 save）。目录不存在时会尝试创建。
    static CacheIndex create(const std::filesystem::path& cache_dir, std::string_view url);

    std::expected<void, syp_status> save();  // 原子替换；成功后更新 updated_at_ms

    const CacheMeta& meta() const noexcept;
    const HoleSet&   ranges() const noexcept;

    void add_range(Range r);

    // 用一次 HTTP 响应的校验信息与索引比对。
    //   一致 / 无法判定 → SYP_OK（无法判定时不误报，宁可漏报）
    //   确认已变        → SYP_ERR_CONTENT_CHANGED（此时不改 meta）
    // 判定规则见 cache_index.cpp 里 validate_and_update 的注释。
    std::expected<void, syp_status> validate_and_update(
        std::string_view etag, std::string_view last_modified, int64_t total_length);

    // 整个资源是否已完整缓存（total_length 未知时返回 false）
    bool is_complete() const noexcept;

    std::filesystem::path index_path() const;
    std::filesystem::path data_path() const;

    // 删除该条目的索引与内容文件（幂等：文件不存在不算失败）
    std::expected<void, syp_status> remove_files();

private:
    CacheIndex(std::filesystem::path cache_dir, std::string key,
               CacheMeta meta, HoleSet ranges);

    std::filesystem::path tmp_path() const;

    std::filesystem::path cache_dir_;
    std::string           key_;
    CacheMeta             meta_;
    HoleSet               ranges_;
};

}  // namespace syp::dl
