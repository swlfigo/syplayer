// cache_file.cpp — POSIX pread/pwrite + RAII fd；移动后原对象不再 close
#include "cache_file.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>
#include <utility>

namespace syp::dl {
namespace {

syp_status map_read_errno(int e) noexcept {
    if (e == ENOSPC) return SYP_ERR_NO_SPACE;
    if (e == ENOENT) return SYP_ERR_EOF;
    return SYP_ERR_IO;
}

syp_status map_write_errno(int e) noexcept {
    if (e == ENOSPC) return SYP_ERR_NO_SPACE;
    return SYP_ERR_IO;  // 写语境下 ENOENT 是路径没了，不是"没有缓存"
}

std::unexpected<syp_status> fail_read_errno() {
    return std::unexpected<syp_status>(map_read_errno(errno));
}

std::unexpected<syp_status> fail_write_errno() {
    return std::unexpected<syp_status>(map_write_errno(errno));
}

bool offset_ok(int64_t offset) noexcept {
    if (offset < 0) return false;
    if (offset > std::numeric_limits<off_t>::max()) return false;
    return true;
}

}  // namespace

CacheFile::CacheFile(int fd) noexcept : fd_(fd) {}

CacheFile::~CacheFile() {
    close_fd();
}

CacheFile::CacheFile(CacheFile&& o) noexcept : fd_(o.fd_) {
    o.fd_ = -1;
}

CacheFile& CacheFile::operator=(CacheFile&& o) noexcept {
    if (this != &o) {
        close_fd();
        fd_   = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

void CacheFile::close_fd() noexcept {
    if (fd_ < 0) return;
    // close 失败不重试：EINTR 后再 close 可能关掉别人刚拿到的 fd。
    ::close(fd_);
    fd_ = -1;
}

std::expected<CacheFile, syp_status> CacheFile::open(const std::filesystem::path& p,
                                                     bool create) {
    if (create) {
        const auto parent = p.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                if (ec == std::errc::no_space_on_device) {
                    return std::unexpected<syp_status>(SYP_ERR_NO_SPACE);
                }
                return std::unexpected<syp_status>(SYP_ERR_IO);
            }
        }
    }

    int flags = O_RDWR | O_CLOEXEC;
    if (create) flags |= O_CREAT;

    int fd = -1;
    do {
        fd = ::open(p.c_str(), flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        // create=false 是读/open 语境（没有这份内容 → EOF）；create=true 是写语境。
        return std::unexpected<syp_status>(
            create ? map_write_errno(errno) : map_read_errno(errno));
    }
    return CacheFile{fd};
}

std::expected<int64_t, syp_status> CacheFile::read_at(int64_t offset,
                                                      std::span<uint8_t> buf) {
    if (fd_ < 0) return std::unexpected<syp_status>(SYP_ERR_IO);
    if (buf.empty()) return int64_t{0};
    if (!offset_ok(offset)) return std::unexpected<syp_status>(SYP_ERR_INVALID_ARG);

    size_t done = 0;
    while (done < buf.size()) {
        const int64_t cur = offset + static_cast<int64_t>(done);
        if (!offset_ok(cur)) return std::unexpected<syp_status>(SYP_ERR_INVALID_ARG);
        const size_t want = buf.size() - done;
        const ssize_t n = ::pread(fd_, buf.data() + done, want, static_cast<off_t>(cur));
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail_read_errno();
        }
        if (n == 0) break;  // EOF
        done += static_cast<size_t>(n);
    }
    return static_cast<int64_t>(done);
}

std::expected<void, syp_status> CacheFile::write_at(int64_t offset,
                                                    std::span<const uint8_t> data) {
    if (fd_ < 0) return std::unexpected<syp_status>(SYP_ERR_IO);
    if (data.empty()) return {};
    if (!offset_ok(offset)) return std::unexpected<syp_status>(SYP_ERR_INVALID_ARG);

    size_t done = 0;
    while (done < data.size()) {
        const int64_t cur = offset + static_cast<int64_t>(done);
        if (!offset_ok(cur)) return std::unexpected<syp_status>(SYP_ERR_INVALID_ARG);
        const size_t want = data.size() - done;
        const ssize_t n = ::pwrite(fd_, data.data() + done, want, static_cast<off_t>(cur));
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail_write_errno();
        }
        if (n == 0) {
            // 没写进去又没报错，继续会空转。
            return std::unexpected<syp_status>(SYP_ERR_IO);
        }
        done += static_cast<size_t>(n);
    }
    return {};
}

std::expected<void, syp_status> CacheFile::sync() {
    if (fd_ < 0) return std::unexpected<syp_status>(SYP_ERR_IO);
    while (::fsync(fd_) != 0) {
        if (errno == EINTR) continue;
        return fail_write_errno();
    }
    return {};
}

std::expected<int64_t, syp_status> CacheFile::size() const {
    if (fd_ < 0) return std::unexpected<syp_status>(SYP_ERR_IO);
    struct stat st {};
    while (::fstat(fd_, &st) != 0) {
        if (errno == EINTR) continue;
        // 已打开 fd 上的查询，不是读缓存内容；ENOENT 不能映射成 EOF。
        return fail_write_errno();
    }
    return static_cast<int64_t>(st.st_size);
}

}  // namespace syp::dl
