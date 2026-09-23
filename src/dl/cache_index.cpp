// cache_index.cpp — 自定义小端二进制索引：magic+version、长度前缀字符串、CRC32、原子 save
#include "cache_index.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <limits>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace syp::dl {
namespace {

// 磁盘格式 v1，小端，不可信输入。按字节：
//   [0, 4)   magic      u32  'SYPI'（字节 S Y P I）
//   [4, 8)   version    u32  目前 = 1
//                            magic 对且 CRC 对但版本不认识 → SYP_ERR_EOF
//                            （降级读到新版本，当未命中，不是损坏）
//   [8, ...) url        u32 len + 原始字节（无 NUL）
//            etag       u32 len + 原始字节
//            last_mod   u32 len + 原始字节
//            total_len  i64  未知 = -1
//            created_ms i64
//            updated_ms i64
//            n_ranges   u32
//            ranges     n_ranges × (i64 start + i64 end)，半开区间
//            crc32      u32  IEEE，覆盖此前全部字节
// 任何长度字段使用前都对照剩余字节数卡上界；文件本身也有硬上限。
constexpr uint32_t kMagic          = 0x49505953u;  // 'SYPI' LE
constexpr uint32_t kVersion        = 1;
constexpr size_t   kMinIndexBytes  = 52;  // 三串皆空、0 段、含 CRC
constexpr size_t   kMaxIndexBytes  = 16u * 1024u * 1024u;

syp_status map_read_errno(int e) noexcept {
    if (e == ENOSPC) return SYP_ERR_NO_SPACE;
    if (e == ENOENT) return SYP_ERR_EOF;
    return SYP_ERR_IO;
}

syp_status map_write_errno(int e) noexcept {
    if (e == ENOSPC) return SYP_ERR_NO_SPACE;
    return SYP_ERR_IO;  // 写语境下 ENOENT 是目录没了之类，不是"没有缓存"
}

std::unexpected<syp_status> fail_read_errno() {
    return std::unexpected<syp_status>(map_read_errno(errno));
}

std::unexpected<syp_status> fail_write_errno() {
    return std::unexpected<syp_status>(map_write_errno(errno));
}

std::unexpected<syp_status> fail(syp_status s) {
    return std::unexpected<syp_status>(s);
}

syp_status status_from_write_ec(const std::error_code& ec) noexcept {
    if (!ec) return SYP_OK;
    if (ec == std::errc::no_space_on_device) return SYP_ERR_NO_SPACE;
    return SYP_ERR_IO;
}

int64_t now_ms() {
    using clock = std::chrono::system_clock;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch());
    return static_cast<int64_t>(ms.count());
}

// IEEE CRC-32（poly 0xEDB88320），位运算，无表。
uint32_t crc32_ieee(std::span<const uint8_t> data) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc ^= static_cast<uint32_t>(b);
        for (int i = 0; i < 8; ++i) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void append_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 24));
}

void append_u64(std::vector<uint8_t>& o, uint64_t v) {
    append_u32(o, static_cast<uint32_t>(v));
    append_u32(o, static_cast<uint32_t>(v >> 32));
}

void append_i64(std::vector<uint8_t>& o, int64_t v) {
    append_u64(o, static_cast<uint64_t>(v));
}

std::expected<void, syp_status> append_str(std::vector<uint8_t>& o, std::string_view s) {
    if (s.size() > std::numeric_limits<uint32_t>::max()) {
        return fail(SYP_ERR_INVALID_ARG);
    }
    append_u32(o, static_cast<uint32_t>(s.size()));
    for (char ch : s) {
        o.push_back(static_cast<uint8_t>(static_cast<unsigned char>(ch)));
    }
    return {};
}

struct Cursor {
    std::span<const uint8_t> buf;
    size_t pos = 0;

    size_t remain() const noexcept { return buf.size() - pos; }

    std::expected<uint32_t, syp_status> u32() {
        if (remain() < 4) return fail(SYP_ERR_CACHE_CORRUPT);
        const uint32_t v = static_cast<uint32_t>(buf[pos])
                         | (static_cast<uint32_t>(buf[pos + 1]) << 8)
                         | (static_cast<uint32_t>(buf[pos + 2]) << 16)
                         | (static_cast<uint32_t>(buf[pos + 3]) << 24);
        pos += 4;
        return v;
    }

    std::expected<int64_t, syp_status> i64() {
        if (remain() < 8) return fail(SYP_ERR_CACHE_CORRUPT);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(buf[pos + static_cast<size_t>(i)])
                 << (8 * i);
        }
        pos += 8;
        return static_cast<int64_t>(v);
    }

    std::expected<std::string, syp_status> str() {
        auto len = u32();
        if (!len) return fail(len.error());
        if (*len > remain()) return fail(SYP_ERR_CACHE_CORRUPT);
        std::string s(reinterpret_cast<const char*>(buf.data() + pos), *len);
        pos += *len;
        return s;
    }
};

struct Parsed {
    CacheMeta meta;
    std::vector<Range> ranges;
};

bool ranges_ok(const std::vector<Range>& rs, int64_t total_length) noexcept {
    for (size_t i = 0; i < rs.size(); ++i) {
        const Range r = rs[i];
        if (r.start < 0) return false;
        if (r.end <= r.start) return false;
        if (total_length >= 0 && r.end > total_length) return false;
        if (i > 0) {
            // 升序、不重叠、不相邻：prev.end < next.start。
            // prev.end > prev.start（上面对每一段都查过），所以 prev.start >= r.start
            // 已被 prev.end >= r.start 覆盖，不必再比 start。
            if (rs[i - 1].end >= r.start) return false;
        }
    }
    return true;
}

std::expected<Parsed, syp_status> parse_bytes(std::span<const uint8_t> all) {
    if (all.size() < kMinIndexBytes) return fail(SYP_ERR_CACHE_CORRUPT);
    if (all.size() > kMaxIndexBytes) return fail(SYP_ERR_CACHE_CORRUPT);

    const size_t body_n = all.size() - 4;
    Cursor crc_cur{all.subspan(body_n), 0};
    auto stored = crc_cur.u32();
    if (!stored) return fail(stored.error());
    const uint32_t calc = crc32_ieee(all.first(body_n));
    if (*stored != calc) return fail(SYP_ERR_CACHE_CORRUPT);

    Cursor c{all.first(body_n), 0};
    auto magic = c.u32();
    if (!magic) return fail(magic.error());
    if (*magic != kMagic) return fail(SYP_ERR_CACHE_CORRUPT);

    auto ver = c.u32();
    if (!ver) return fail(ver.error());
    // CRC 已过、magic 已对：版本不认识是降级读到了新版本写的索引，不是写坏。
    if (*ver != kVersion) return fail(SYP_ERR_EOF);

    Parsed out;
    auto url = c.str();
    if (!url) return fail(url.error());
    auto etag = c.str();
    if (!etag) return fail(etag.error());
    auto lm = c.str();
    if (!lm) return fail(lm.error());
    auto total = c.i64();
    if (!total) return fail(total.error());
    auto created = c.i64();
    if (!created) return fail(created.error());
    auto updated = c.i64();
    if (!updated) return fail(updated.error());
    auto n = c.u32();
    if (!n) return fail(n.error());

    // 先用剩余字节卡上界，禁止按 n 做超大分配。
    if (*n > c.remain() / 16u) return fail(SYP_ERR_CACHE_CORRUPT);
    if (c.remain() != static_cast<size_t>(*n) * 16u) return fail(SYP_ERR_CACHE_CORRUPT);

    out.ranges.reserve(*n);
    for (uint32_t i = 0; i < *n; ++i) {
        auto s = c.i64();
        if (!s) return fail(s.error());
        auto e = c.i64();
        if (!e) return fail(e.error());
        out.ranges.push_back(Range{*s, *e});
    }
    if (c.remain() != 0) return fail(SYP_ERR_CACHE_CORRUPT);

    if (!ranges_ok(out.ranges, *total)) return fail(SYP_ERR_CACHE_CORRUPT);

    out.meta.url            = std::move(*url);
    out.meta.etag           = std::move(*etag);
    out.meta.last_modified  = std::move(*lm);
    out.meta.total_length   = *total;
    out.meta.created_at_ms  = *created;
    out.meta.updated_at_ms  = *updated;
    return out;
}

std::expected<std::vector<uint8_t>, syp_status> encode(const CacheMeta& meta,
                                                       const HoleSet& ranges) {
    std::vector<uint8_t> o;
    o.reserve(kMinIndexBytes + meta.url.size() + meta.etag.size()
              + meta.last_modified.size() + ranges.count() * 16u);

    append_u32(o, kMagic);
    append_u32(o, kVersion);
    if (auto r = append_str(o, meta.url); !r) return fail(r.error());
    if (auto r = append_str(o, meta.etag); !r) return fail(r.error());
    if (auto r = append_str(o, meta.last_modified); !r) return fail(r.error());
    append_i64(o, meta.total_length);
    append_i64(o, meta.created_at_ms);
    append_i64(o, meta.updated_at_ms);

    const auto span = ranges.ranges();
    if (span.size() > std::numeric_limits<uint32_t>::max()) {
        return fail(SYP_ERR_INVALID_ARG);
    }
    append_u32(o, static_cast<uint32_t>(span.size()));
    for (Range r : span) {
        append_i64(o, r.start);
        append_i64(o, r.end);
    }

    const uint32_t crc = crc32_ieee(o);
    append_u32(o, crc);
    if (o.size() > kMaxIndexBytes) return fail(SYP_ERR_IO);
    return o;
}

std::expected<std::vector<uint8_t>, syp_status> read_file(const std::filesystem::path& p) {
    int fd = -1;
    do {
        fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return fail_read_errno();

    struct stat st {};
    while (::fstat(fd, &st) != 0) {
        if (errno == EINTR) continue;
        const int e = errno;
        ::close(fd);
        return std::unexpected<syp_status>(map_read_errno(e));
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > kMaxIndexBytes) {
        ::close(fd);
        return fail(SYP_ERR_CACHE_CORRUPT);
    }
    const size_t n = static_cast<size_t>(st.st_size);
    if (n < kMinIndexBytes) {
        ::close(fd);
        return fail(SYP_ERR_CACHE_CORRUPT);
    }

    std::vector<uint8_t> buf(n);
    size_t done = 0;
    while (done < n) {
        const ssize_t r = ::read(fd, buf.data() + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            return std::unexpected<syp_status>(map_read_errno(e));
        }
        if (r == 0) break;
        done += static_cast<size_t>(r);
    }
    ::close(fd);
    if (done != n) return fail(SYP_ERR_CACHE_CORRUPT);
    return buf;
}

std::expected<void, syp_status> write_tmp_atomic(const std::filesystem::path& tmp,
                                                 const std::filesystem::path& dst,
                                                 std::span<const uint8_t> bytes) {
    int fd = -1;
    do {
        fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return fail_write_errno();

    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            return std::unexpected<syp_status>(map_write_errno(e));
        }
        if (n == 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return fail(SYP_ERR_IO);
        }
        done += static_cast<size_t>(n);
    }

    while (::fsync(fd) != 0) {
        if (errno == EINTR) continue;
        const int e = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        return std::unexpected<syp_status>(map_write_errno(e));
    }
    ::close(fd);

    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        const int e = errno;
        ::unlink(tmp.c_str());
        return std::unexpected<syp_status>(map_write_errno(e));
    }
    return {};
}

std::expected<void, syp_status> remove_one(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    if (!ec) return {};
    if (ec == std::errc::no_such_file_or_directory) return {};
    return fail(status_from_write_ec(ec));
}

}  // namespace

std::string CacheIndex::key_for_url(std::string_view url) {
    // FNV-1a 64，输出 16 位小写 hex（APFS 默认大小写不敏感，不能用大写）。
    constexpr uint64_t kOffset = 14695981039346656037ULL;
    constexpr uint64_t kPrime  = 1099511628211ULL;
    uint64_t h = kOffset;
    for (char ch : url) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(ch));
        h *= kPrime;
    }
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<size_t>(i)] = "0123456789abcdef"[h & 0xFull];
        h >>= 4;
    }
    return s;
}

CacheIndex::CacheIndex(std::filesystem::path cache_dir, std::string key,
                       CacheMeta meta, HoleSet ranges)
    : cache_dir_(std::move(cache_dir))
    , key_(std::move(key))
    , meta_(std::move(meta))
    , ranges_(std::move(ranges)) {}

CacheIndex CacheIndex::create(const std::filesystem::path& cache_dir,
                              std::string_view url) {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);  // 失败留给 save 再报

    CacheMeta m;
    m.url           = std::string(url);
    m.created_at_ms = now_ms();
    m.updated_at_ms = m.created_at_ms;
    return CacheIndex(cache_dir, key_for_url(url), std::move(m), HoleSet{});
}

std::expected<CacheIndex, syp_status> CacheIndex::open(
    const std::filesystem::path& cache_dir, std::string_view url) {
    const std::string key = key_for_url(url);
    const auto path = cache_dir / (key + ".idx");

    auto bytes = read_file(path);
    if (!bytes) return fail(bytes.error());

    auto parsed = parse_bytes(*bytes);
    if (!parsed) return fail(parsed.error());

    // 哈希撞车：文件名对上了但里面是别人的 URL。按未命中处理，别把缓存借走。
    if (parsed->meta.url != url) return fail(SYP_ERR_EOF);

    HoleSet hs;
    for (Range r : parsed->ranges) hs.add(r);
    if (hs.count() != parsed->ranges.size()) return fail(SYP_ERR_CACHE_CORRUPT);

    // 索引声称有数据时，必须对照 .dat 实际大小。stat 一次 syscall。
    if (!hs.empty()) {
        const auto dat = cache_dir / (key + ".dat");
        struct stat st {};
        int rc = 0;
        do {
            rc = ::stat(dat.c_str(), &st);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            if (errno == ENOENT) return fail(SYP_ERR_EOF);
            return std::unexpected<syp_status>(map_read_errno(errno));
        }
        if (st.st_size <= 0) {
            hs.clear();
        } else {
            const int64_t dat_size = static_cast<int64_t>(st.st_size);
            // 整段超出 → 丢弃；跨越末尾 → 裁到 dat_size。remove 维持 HoleSet 不变式。
            hs.remove(Range{dat_size, std::numeric_limits<int64_t>::max()});
        }
    }

    return CacheIndex(cache_dir, key, std::move(parsed->meta), std::move(hs));
}

std::expected<void, syp_status> CacheIndex::save() {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    if (ec) return fail(status_from_write_ec(ec));

    const int64_t prev = meta_.updated_at_ms;
    meta_.updated_at_ms = now_ms();

    auto bytes = encode(meta_, ranges_);
    if (!bytes) {
        meta_.updated_at_ms = prev;
        return fail(bytes.error());
    }

    auto w = write_tmp_atomic(tmp_path(), index_path(), *bytes);
    if (!w) {
        meta_.updated_at_ms = prev;
        return fail(w.error());
    }
    return {};
}

const CacheMeta& CacheIndex::meta() const noexcept { return meta_; }
const HoleSet&   CacheIndex::ranges() const noexcept { return ranges_; }

void CacheIndex::add_range(Range r) {
    ranges_.add(r);
}

// validate_and_update 判定（确认已变才报 CONTENT_CHANGED；无法判定不误报）：
//
//   etag:
//     两侧都非空且不同                         → 变了
//     新响应 etag 为空（即便旧索引有 etag）     → 不能靠 etag 判定变化
//     旧为空、新非空                            → 首次填入，接受
//   total_length:
//     两侧都 >= 0 且不同                        → 变了
//     旧 < 0、新 >= 0                           → 首次填入，接受
//     新 < 0                                    → 不能靠长度判定，也不用未知覆盖已知
//   last_modified（仅当 etag 没法两侧比对时才看）：
//     两侧都非空且不同                          → 变了
//     旧为空、新非空                            → 首次填入，接受
//
// 报 CONTENT_CHANGED 时不改 meta，调用方还能看到旧值并决定是否丢掉缓存。
std::expected<void, syp_status> CacheIndex::validate_and_update(
    std::string_view etag, std::string_view last_modified, int64_t total_length) {
    const bool both_etags = !etag.empty() && !meta_.etag.empty();
    if (both_etags && etag != meta_.etag) {
        return fail(SYP_ERR_CONTENT_CHANGED);
    }
    if (total_length >= 0 && meta_.total_length >= 0
        && total_length != meta_.total_length) {
        return fail(SYP_ERR_CONTENT_CHANGED);
    }
    if (!both_etags && !last_modified.empty() && !meta_.last_modified.empty()
        && last_modified != meta_.last_modified) {
        return fail(SYP_ERR_CONTENT_CHANGED);
    }

    if (meta_.etag.empty() && !etag.empty()) {
        meta_.etag = std::string(etag);
    }
    if (meta_.last_modified.empty() && !last_modified.empty()) {
        meta_.last_modified = std::string(last_modified);
    }
    if (meta_.total_length < 0 && total_length >= 0) {
        meta_.total_length = total_length;
    }
    return {};
}

bool CacheIndex::is_complete() const noexcept {
    if (meta_.total_length < 0) return false;
    return ranges_.contains(Range{0, meta_.total_length});
}

std::filesystem::path CacheIndex::index_path() const {
    return cache_dir_ / (key_ + ".idx");
}

std::filesystem::path CacheIndex::data_path() const {
    return cache_dir_ / (key_ + ".dat");
}

std::filesystem::path CacheIndex::tmp_path() const {
    return cache_dir_ / (key_ + ".idx.tmp");
}

std::expected<void, syp_status> CacheIndex::remove_files() {
    // 先删 .idx 再删 .dat：先让索引失效。宁可留孤儿 .dat，也不能留一个
    // 声称有数据的索引（跟 open() 对照 .dat 大小是同一个道理）。
    // 三个文件都尝试删一遍，记住第一个错误最后返回，避免短路留下 .idx.tmp。
    syp_status first = SYP_OK;
    const auto note = [&](std::expected<void, syp_status> r) {
        if (!r && first == SYP_OK) first = r.error();
    };
    note(remove_one(index_path()));
    note(remove_one(data_path()));
    note(remove_one(tmp_path()));
    if (first != SYP_OK) return fail(first);
    return {};
}

}  // namespace syp::dl
