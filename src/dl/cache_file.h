// cache_file.h — 内容文件按绝对偏移的 pread / pwrite，不理解区间语义
#pragma once

#include <syplayer/syp_types.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

namespace syp::dl {

// 稀疏内容文件。只保证：短读短写循环干净、EINTR 重试、fd 由 RAII 持有。
// 写入顺序由调用方保证（先 write_at + sync，再把区间记进 CacheIndex）。
//
// 线程安全：pread/pwrite 不使用、也不修改 fd 的文件偏移，所以同一已构造
// 实例上并发 read_at / write_at / sync / size 没有偏移/fd 竞态，是安全的。
// 依据：POSIX 规定 pread/pwrite 与当前文件偏移无关；本对象除 fd_ 外没有
// 可变共享状态。一次 read_at/write_at 可能拆成多次短 I/O，重叠区间的并发
// 读写不保证数据原子性（可能读到半新半旧）。open（工厂函数）、析构、
// 移动赋值会关/换 fd，不能与任何读写并发。
class CacheFile {
public:
    static std::expected<CacheFile, syp_status> open(const std::filesystem::path& p,
                                                     bool create);
    ~CacheFile();
    CacheFile(CacheFile&&) noexcept;
    CacheFile& operator=(CacheFile&&) noexcept;
    CacheFile(const CacheFile&)            = delete;
    CacheFile& operator=(const CacheFile&) = delete;

    // 全部按绝对文件偏移。读到 EOF 返回已读字节数（可短于 buf）；
    // 写必须写完，短写会继续循环，不能把短写当成功。
    std::expected<int64_t, syp_status> read_at(int64_t offset, std::span<uint8_t> buf);
    std::expected<void, syp_status>    write_at(int64_t offset, std::span<const uint8_t> data);
    std::expected<void, syp_status>    sync();  // fsync
    std::expected<int64_t, syp_status> size() const;

private:
    explicit CacheFile(int fd) noexcept;
    void close_fd() noexcept;

    int fd_ = -1;
};

}  // namespace syp::dl
