// test_cache_index.cpp — CacheIndex 往返/损坏输入/etag 判定 + CacheFile 稀疏读写
#include "tiny_test.h"

#include <dl/cache_file.h>
#include <dl/cache_index.h>
#include <dl/hole_set.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using syp::dl::CacheFile;
using syp::dl::CacheIndex;
using syp::dl::CacheMeta;
using syp::dl::HoleSet;
using syp::dl::Range;

namespace syp::dl {

std::ostream& operator<<(std::ostream& os, const Range& r) {
    return os << "[" << r.start << ", " << r.end << ")";
}

std::ostream& operator<<(std::ostream& os, const std::vector<Range>& v) {
    os << "{";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) os << ", ";
        os << v[i];
    }
    return os << "}";
}

}  // namespace syp::dl

namespace {

std::vector<Range> collected(const HoleSet& hs) {
    auto s = hs.ranges();
    return {s.begin(), s.end()};
}

struct TempDir {
    std::filesystem::path p;

    TempDir() {
        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed);
        p = base / ("syp-cidx-" + std::to_string(::getpid()) + "-"
                    + std::to_string(n));
        std::filesystem::create_directories(p, ec);
    }

    ~TempDir() {
        std::error_code ec;
        if (!p.empty()) std::filesystem::remove_all(p, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

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

void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off]     = static_cast<uint8_t>(v);
    b[off + 1] = static_cast<uint8_t>(v >> 8);
    b[off + 2] = static_cast<uint8_t>(v >> 16);
    b[off + 3] = static_cast<uint8_t>(v >> 24);
}

void put_i64(std::vector<uint8_t>& b, size_t off, int64_t v) {
    auto u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        b[off + static_cast<size_t>(i)] = static_cast<uint8_t>(u >> (8 * i));
    }
}

void fix_crc(std::vector<uint8_t>& b) {
    const uint32_t c = crc32_ieee(std::span<const uint8_t>(b.data(), b.size() - 4));
    put_u32(b, b.size() - 4, c);
}

std::vector<uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> b(n < 0 ? 0 : static_cast<size_t>(n));
    if (!b.empty()) {
        in.read(reinterpret_cast<char*>(b.data()),
                static_cast<std::streamsize>(b.size()));
    }
    return b;
}

void write_all(const std::filesystem::path& path, const std::vector<uint8_t>& b) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!b.empty()) {
        out.write(reinterpret_cast<const char*>(b.data()),
                  static_cast<std::streamsize>(b.size()));
    }
}

void set_file_size(const std::filesystem::path& path, std::uintmax_t n) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        CHECK(out.good());
    }
    std::filesystem::resize_file(path, n, ec);
    CHECK(!ec);
}

// 与 cache_index.cpp 布局一致：range_count 在三串和三个 i64 之后。
size_t range_count_off(std::string_view url, std::string_view etag,
                       std::string_view lm) {
    return 8u + 4u + url.size() + 4u + etag.size() + 4u + lm.size()
           + 8u + 8u + 8u;
}

size_t ranges_off(std::string_view url, std::string_view etag,
                  std::string_view lm) {
    return range_count_off(url, etag, lm) + 4u;
}

constexpr const char* kUrl  = "https://cdn.example/v.mp4";
constexpr const char* kEtag = "\"abc123\"";
constexpr const char* kLm   = "Wed, 01 Jan 2020 00:00:00 GMT";

CacheIndex save_sample(const std::filesystem::path& dir) {
    auto idx = CacheIndex::create(dir, kUrl);
    idx.add_range({0, 10});
    idx.add_range({20, 40});
    (void)idx.validate_and_update(kEtag, kLm, 100);
    (void)idx.save();
    set_file_size(idx.data_path(), 100);  // 与区间上界对齐，open() 才不会当未命中
    return idx;
}

void expect_corrupt(const std::filesystem::path& dir, std::string_view url) {
    auto r = CacheIndex::open(dir, url);
    CHECK(!r.has_value());
    if (!r.has_value()) {
        CHECK_EQ(r.error(), SYP_ERR_CACHE_CORRUPT);
    }
}

// 在 path 上放一条悬空符号链接，目标的父目录不存在。
// open(path, O_CREAT) 跟随链接 → ENOENT（不是 ENOTDIR）。
// 不能把链接当缓存目录本身：create_directories 会先因 EEXIST 失败，
// 根本走不到 open，锁不住 map_write_errno 对写语境 ENOENT 的分类。
bool plant_dangling_creat_target(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_symlink("missing-parent/ghost", path, ec);
    return !ec;
}

}  // namespace

TEST_CASE(key_is_lowercase_hex) {
    const auto k = CacheIndex::key_for_url(kUrl);
    CHECK_EQ(k.size(), static_cast<size_t>(16));
    for (char c : k) {
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    CHECK(CacheIndex::key_for_url("a") != CacheIndex::key_for_url("b"));
}

TEST_CASE(roundtrip) {
    TempDir td;
    const auto dir = td.p / "nested";  // create() 要能自己建目录
    auto idx = CacheIndex::create(dir, kUrl);
    idx.add_range({100, 200});
    idx.add_range({0, 50});
    idx.add_range({50, 80});  // 与前一段相邻，应合并
    auto v = idx.validate_and_update(kEtag, kLm, 1000);
    REQUIRE(v.has_value());
    REQUIRE(idx.save().has_value());
    set_file_size(idx.data_path(), 1000);

    auto opened = CacheIndex::open(dir, kUrl);
    REQUIRE(opened.has_value());
    CHECK_EQ(opened->meta().url, std::string(kUrl));
    CHECK_EQ(opened->meta().etag, std::string(kEtag));
    CHECK_EQ(opened->meta().last_modified, std::string(kLm));
    CHECK_EQ(opened->meta().total_length, int64_t{1000});
    CHECK_EQ(opened->meta().created_at_ms, idx.meta().created_at_ms);
    CHECK_EQ(opened->meta().updated_at_ms, idx.meta().updated_at_ms);
    CHECK_EQ(collected(opened->ranges()), (std::vector<Range>{{0, 80}, {100, 200}}));
    CHECK(!opened->is_complete());
}

TEST_CASE(restart_restore_bytes) {
    TempDir td;
    const auto dir = td.p;
    const std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

    {
        auto idx = CacheIndex::create(dir, kUrl);
        auto cf  = CacheFile::open(idx.data_path(), true);
        REQUIRE(cf.has_value());
        REQUIRE(cf->write_at(100, std::span<const uint8_t>(payload)).has_value());
        REQUIRE(cf->sync().has_value());
        idx.add_range({100, 100 + static_cast<int64_t>(payload.size())});
        REQUIRE(idx.validate_and_update(kEtag, kLm, 1000).has_value());
        REQUIRE(idx.save().has_value());
    }

    auto opened = CacheIndex::open(dir, kUrl);
    REQUIRE(opened.has_value());
    CHECK_EQ(collected(opened->ranges()),
             std::vector<Range>{{100, 100 + static_cast<int64_t>(payload.size())}});
    auto cf = CacheFile::open(opened->data_path(), false);
    REQUIRE(cf.has_value());
    std::vector<uint8_t> got(payload.size());
    auto n = cf->read_at(100, std::span<uint8_t>(got));
    REQUIRE(n.has_value());
    CHECK_EQ(*n, static_cast<int64_t>(payload.size()));
    CHECK(got == payload);
}

TEST_CASE(validate_etag_changed) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    REQUIRE(idx.validate_and_update("etag-a", kLm, 100).has_value());
    CHECK_EQ(idx.meta().etag, std::string("etag-a"));

    auto r = idx.validate_and_update("etag-b", kLm, 100);
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_CONTENT_CHANGED);
    CHECK_EQ(idx.meta().etag, std::string("etag-a"));  // 失败不改
}

TEST_CASE(validate_total_length_changed) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    REQUIRE(idx.validate_and_update(kEtag, kLm, 100).has_value());
    auto r = idx.validate_and_update(kEtag, kLm, 200);
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_CONTENT_CHANGED);
    CHECK_EQ(idx.meta().total_length, int64_t{100});
}

TEST_CASE(validate_last_modified_changed) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    REQUIRE(idx.validate_and_update("", "Mon, 01 Jan 2020", -1).has_value());
    auto r = idx.validate_and_update("", "Tue, 02 Jan 2020", -1);
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_CONTENT_CHANGED);
    CHECK_EQ(idx.meta().last_modified, std::string("Mon, 01 Jan 2020"));
}

TEST_CASE(validate_empty_new_etag_not_a_change) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    REQUIRE(idx.validate_and_update(kEtag, kLm, 100).has_value());
    auto r = idx.validate_and_update("", kLm, 100);
    REQUIRE(r.has_value());
    CHECK_EQ(idx.meta().etag, std::string(kEtag));  // 空 etag 不覆盖
}

TEST_CASE(validate_first_fill_not_a_change) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    CHECK(idx.meta().etag.empty());
    CHECK_EQ(idx.meta().total_length, int64_t{-1});
    auto r = idx.validate_and_update(kEtag, kLm, 4096);
    REQUIRE(r.has_value());
    CHECK_EQ(idx.meta().etag, std::string(kEtag));
    CHECK_EQ(idx.meta().last_modified, std::string(kLm));
    CHECK_EQ(idx.meta().total_length, int64_t{4096});
}

TEST_CASE(open_missing_is_eof) {
    TempDir td;
    auto r = CacheIndex::open(td.p, "https://never-written.example/x");
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_EOF);
}

TEST_CASE(remove_files_idempotent) {
    TempDir td;
    auto idx = save_sample(td.p);
    {
        auto cf = CacheFile::open(idx.data_path(), true);
        REQUIRE(cf.has_value());
        const std::vector<uint8_t> one{0x11};
        REQUIRE(cf->write_at(0, std::span<const uint8_t>(one)).has_value());
    }

    REQUIRE(idx.remove_files().has_value());
    std::error_code ec;
    CHECK(!std::filesystem::exists(idx.index_path(), ec));
    CHECK(!std::filesystem::exists(idx.data_path(), ec));
    REQUIRE(idx.remove_files().has_value());
    REQUIRE(idx.remove_files().has_value());
}

TEST_CASE(hash_collision_is_miss) {
    TempDir td;
    const char* url_a = "https://a.example/v.mp4";
    const char* url_b = "https://b.example/v.mp4";
    auto ia = CacheIndex::create(td.p, url_a);
    auto ib = CacheIndex::create(td.p, url_b);
    REQUIRE(ia.index_path() != ib.index_path());
    REQUIRE(ib.validate_and_update("etag-b", "", 50).has_value());
    ib.add_range({0, 10});
    REQUIRE(ib.save().has_value());

    std::error_code ec;
    std::filesystem::copy_file(ib.index_path(), ia.index_path(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);

    auto opened = CacheIndex::open(td.p, url_a);
    REQUIRE(!opened.has_value());
    CHECK_EQ(opened.error(), SYP_ERR_EOF);
}

TEST_CASE(leftover_tmp_ignored_and_save_clears_it) {
    TempDir td;
    auto idx = save_sample(td.p);
    const auto tmp = td.p / (CacheIndex::key_for_url(kUrl) + ".idx.tmp");
    write_all(tmp, {0xFF, 0x00, 0xAA, 0x55});
    CHECK(std::filesystem::exists(tmp));

    auto opened = CacheIndex::open(td.p, kUrl);
    REQUIRE(opened.has_value());
    CHECK_EQ(opened->meta().etag, std::string(kEtag));

    opened->add_range({80, 90});
    REQUIRE(opened->save().has_value());
    std::error_code ec;
    CHECK(!std::filesystem::exists(tmp, ec));

    auto again = CacheIndex::open(td.p, kUrl);
    REQUIRE(again.has_value());
    CHECK_EQ(collected(again->ranges()),
             (std::vector<Range>{{0, 10}, {20, 40}, {80, 90}}));
}

TEST_CASE(corrupt_empty_file) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    write_all(idx.index_path(), {});
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_few_bytes) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    write_all(idx.index_path(), {0x53, 0x59, 0x50, 0x49, 0x01});
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_bad_magic) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    REQUIRE(b.size() >= 8);
    put_u32(b, 0, 0xDEADBEEFu);
    fix_crc(b);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(unknown_version_with_valid_crc_is_miss) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    REQUIRE(b.size() >= 8);
    put_u32(b, 4, 99);
    fix_crc(b);
    write_all(idx.index_path(), b);
    auto r = CacheIndex::open(td.p, kUrl);
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_EOF);
}

TEST_CASE(future_version_without_crc_fix_is_corrupt) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    REQUIRE(b.size() >= 8);
    put_u32(b, 4, 99);  // 不修 CRC：CRC 先拦住，仍是损坏
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_bad_crc) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    REQUIRE(b.size() > 10);
    b[10] = static_cast<uint8_t>(b[10] ^ 0x01u);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_huge_range_count) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    const auto off = range_count_off(kUrl, kEtag, kLm);
    REQUIRE(b.size() > off + 4);
    put_u32(b, off, 0xFFFFFFFFu);
    fix_crc(b);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_unordered_ranges) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    const auto off = ranges_off(kUrl, kEtag, kLm);
    REQUIRE(b.size() >= off + 32 + 4);
    // 两段各 16 字节，对调后乱序
    for (int i = 0; i < 16; ++i) {
        std::swap(b[off + static_cast<size_t>(i)],
                  b[off + 16 + static_cast<size_t>(i)]);
    }
    fix_crc(b);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_overlapping_ranges) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    const auto off = ranges_off(kUrl, kEtag, kLm);
    // 第二段原 [20, 40)，改成 [5, 40) 与 [0, 10) 重叠
    put_i64(b, off + 16, 5);
    fix_crc(b);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_negative_range) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    const auto off = ranges_off(kUrl, kEtag, kLm);
    put_i64(b, off, int64_t{-1});
    fix_crc(b);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_range_past_total) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    const auto off = ranges_off(kUrl, kEtag, kLm);
    // 第一段 [0, 10) 改成 [0, 150)，total_length 仍是 100
    put_i64(b, off + 8, int64_t{150});
    fix_crc(b);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(corrupt_truncated) {
    TempDir td;
    auto idx = save_sample(td.p);
    auto b   = read_all(idx.index_path());
    REQUIRE(b.size() > 20);
    b.resize(b.size() / 2);
    write_all(idx.index_path(), b);
    expect_corrupt(td.p, kUrl);
}

TEST_CASE(cache_file_sparse_short_read_move) {
    TempDir td;
    const auto p = td.p / "blob.dat";

    {
        auto o = CacheFile::open(p, true);
        REQUIRE(o.has_value());
        const std::vector<uint8_t> a{1, 2, 3, 4};
        const std::vector<uint8_t> b{9, 8, 7, 6};
        REQUIRE(o->write_at(0, std::span<const uint8_t>(a)).has_value());
        const int64_t gig = int64_t{1} << 30;
        REQUIRE(o->write_at(gig, std::span<const uint8_t>(b)).has_value());
        REQUIRE(o->sync().has_value());

        auto sz = o->size();
        REQUIRE(sz.has_value());
        CHECK_EQ(*sz, gig + 4);

        std::vector<uint8_t> got(4);
        auto n0 = o->read_at(0, std::span<uint8_t>(got));
        REQUIRE(n0.has_value());
        CHECK_EQ(*n0, int64_t{4});
        CHECK(got == a);

        auto n1 = o->read_at(gig, std::span<uint8_t>(got));
        REQUIRE(n1.has_value());
        CHECK_EQ(*n1, int64_t{4});
        CHECK(got == b);

        // 稀疏空洞：逻辑大小之内未写过的区域读到 0
        std::vector<uint8_t> hole(8, 0xFF);
        auto nh = o->read_at(4096, std::span<uint8_t>(hole));
        REQUIRE(nh.has_value());
        CHECK_EQ(*nh, int64_t{8});
        CHECK(hole == std::vector<uint8_t>(8, 0));

        // 读到文件尾：短读
        std::vector<uint8_t> tail(16, 0xAA);
        auto nt = o->read_at(gig, std::span<uint8_t>(tail));
        REQUIRE(nt.has_value());
        CHECK_EQ(*nt, int64_t{4});
        CHECK_EQ(tail[0], static_cast<uint8_t>(9));

        // 完全未写过且在 EOF 之后：读 0 字节
        std::vector<uint8_t> past(4, 0xAA);
        auto np = o->read_at(gig + 4, std::span<uint8_t>(past));
        REQUIRE(np.has_value());
        CHECK_EQ(*np, int64_t{0});
    }

    std::optional<CacheFile> live;
    {
        auto o = CacheFile::open(p, false);
        REQUIRE(o.has_value());
        live = std::move(*o);
    }  // 移出后的原对象在这里析构，不能关掉 live 的 fd
    REQUIRE(live.has_value());
    std::vector<uint8_t> got(4);
    auto n = live->read_at(0, std::span<uint8_t>(got));
    REQUIRE(n.has_value());
    CHECK_EQ(*n, int64_t{4});
    CHECK_EQ(got[0], static_cast<uint8_t>(1));
}

TEST_CASE(is_complete_needs_known_length) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    idx.add_range({0, 100});
    CHECK(!idx.is_complete());
    REQUIRE(idx.validate_and_update("", "", 100).has_value());
    CHECK(idx.is_complete());
    idx.add_range({100, 120});  // 超出 total 的额外区间不由 is_complete 拒绝
    CHECK(idx.is_complete());   // [0, 100) 仍被覆盖
}

TEST_CASE(open_empty_index_without_dat_ok) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    REQUIRE(idx.save().has_value());
    std::error_code ec;
    CHECK(!std::filesystem::exists(idx.data_path(), ec));

    auto opened = CacheIndex::open(td.p, kUrl);
    REQUIRE(opened.has_value());
    CHECK(opened->ranges().empty());
    CHECK_EQ(opened->ranges().total_bytes(), int64_t{0});
}

TEST_CASE(open_dat_truncated_to_zero) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    const std::vector<uint8_t> payload(4096, 0xAB);
    {
        auto cf = CacheFile::open(idx.data_path(), true);
        REQUIRE(cf.has_value());
        REQUIRE(cf->write_at(0, std::span<const uint8_t>(payload)).has_value());
        REQUIRE(cf->sync().has_value());
    }
    idx.add_range({0, 4096});
    REQUIRE(idx.validate_and_update(kEtag, kLm, 4096).has_value());
    REQUIRE(idx.save().has_value());

    std::error_code ec;
    std::filesystem::resize_file(idx.data_path(), 0, ec);
    REQUIRE(!ec);

    auto opened = CacheIndex::open(td.p, kUrl);
    REQUIRE(opened.has_value());
    CHECK(!opened->is_complete());
    CHECK(opened->ranges().empty());
    CHECK_EQ(opened->ranges().total_bytes(), int64_t{0});
    CHECK_EQ(opened->meta().total_length, int64_t{4096});
}

TEST_CASE(open_dat_truncated_to_half_clips_range) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    const std::vector<uint8_t> payload(4096, 0xCD);
    {
        auto cf = CacheFile::open(idx.data_path(), true);
        REQUIRE(cf.has_value());
        REQUIRE(cf->write_at(0, std::span<const uint8_t>(payload)).has_value());
        REQUIRE(cf->sync().has_value());
    }
    idx.add_range({0, 4096});
    REQUIRE(idx.validate_and_update(kEtag, kLm, 4096).has_value());
    REQUIRE(idx.save().has_value());

    std::error_code ec;
    std::filesystem::resize_file(idx.data_path(), 2048, ec);
    REQUIRE(!ec);

    auto opened = CacheIndex::open(td.p, kUrl);
    REQUIRE(opened.has_value());
    CHECK_EQ(collected(opened->ranges()), std::vector<Range>{{0, 2048}});
    CHECK_EQ(opened->ranges().total_bytes(), int64_t{2048});
    CHECK(!opened->is_complete());
}

TEST_CASE(open_dat_deleted_is_miss) {
    TempDir td;
    auto idx = save_sample(td.p);
    std::error_code ec;
    REQUIRE(std::filesystem::remove(idx.data_path(), ec));
    REQUIRE(!ec);

    auto opened = CacheIndex::open(td.p, kUrl);
    REQUIRE(!opened.has_value());
    CHECK_EQ(opened.error(), SYP_ERR_EOF);
}

TEST_CASE(open_dat_larger_than_index_unaffected) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    const std::vector<uint8_t> payload(4096, 0xEF);
    {
        auto cf = CacheFile::open(idx.data_path(), true);
        REQUIRE(cf.has_value());
        REQUIRE(cf->write_at(0, std::span<const uint8_t>(payload)).has_value());
        REQUIRE(cf->sync().has_value());
    }
    idx.add_range({0, 4096});
    REQUIRE(idx.validate_and_update(kEtag, kLm, 4096).has_value());
    REQUIRE(idx.save().has_value());

    std::error_code ec;
    std::filesystem::resize_file(idx.data_path(), 8192, ec);
    REQUIRE(!ec);

    auto opened = CacheIndex::open(td.p, kUrl);
    REQUIRE(opened.has_value());
    CHECK_EQ(collected(opened->ranges()), std::vector<Range>{{0, 4096}});
    CHECK_EQ(opened->ranges().total_bytes(), int64_t{4096});
    CHECK(opened->is_complete());
}

TEST_CASE(open_dat_clips_and_drops_mixed_ranges) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    idx.add_range({0, 100});
    idx.add_range({200, 400});
    idx.add_range({500, 600});
    REQUIRE(idx.validate_and_update(kEtag, kLm, 1000).has_value());
    REQUIRE(idx.save().has_value());
    set_file_size(idx.data_path(), 250);

    auto opened = CacheIndex::open(td.p, kUrl);
    REQUIRE(opened.has_value());
    // [0,100) 保留；[200,400) 裁到 [200,250)；[500,600) 整段超出丢掉
    CHECK_EQ(collected(opened->ranges()), (std::vector<Range>{{0, 100}, {200, 250}}));
    CHECK_EQ(opened->ranges().total_bytes(), int64_t{150});
}

TEST_CASE(remove_files_attempts_all_even_on_error) {
    TempDir td;
    auto idx = save_sample(td.p);
    const auto dat = idx.data_path();
    const auto tmp = td.p / (CacheIndex::key_for_url(kUrl) + ".idx.tmp");
    write_all(tmp, {0x01});

    std::error_code ec;
    std::filesystem::remove(dat, ec);
    REQUIRE(!ec);
    std::filesystem::create_directory(dat, ec);
    REQUIRE(!ec);
    write_all(dat / "stub", {0x02});  // 非空目录，remove 会失败

    auto r = idx.remove_files();
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_IO);

    CHECK(!std::filesystem::exists(idx.index_path(), ec));
    CHECK(!std::filesystem::exists(tmp, ec));
    CHECK(std::filesystem::is_directory(dat, ec));
}

TEST_CASE(cache_file_open_missing_is_eof) {
    // 读语境：文件不存在的 ENOENT 仍是 EOF。写语境映射改了也不能把这条改坏。
    TempDir td;
    auto r = CacheFile::open(td.p / "nope.dat", false);
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_EOF);
}

TEST_CASE(save_blocked_path_is_io_not_eof) {
    // ENOTDIR：把文件当目录。另一条失败路径，锁不住写语境 ENOENT。
    TempDir td;
    const auto blocker = td.p / "blocker";
    write_all(blocker, {0x01});
    auto idx = CacheIndex::create(blocker, kUrl);
    auto r = idx.save();
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_IO);
}

TEST_CASE(cache_file_create_under_file_is_io) {
    // ENOTDIR：把文件当目录。另一条失败路径，锁不住写语境 ENOENT。
    TempDir td;
    const auto blocker = td.p / "blocker";
    write_all(blocker, {0x01});
    auto r = CacheFile::open(blocker / "x.dat", true);
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_IO);
}

TEST_CASE(save_dangling_symlink_is_io_not_eof) {
    TempDir td;
    auto idx = CacheIndex::create(td.p, kUrl);
    auto tmp = idx.index_path();
    tmp += ".tmp";  // <key>.idx.tmp，save() 里 O_CREAT 打开的就是它
    REQUIRE(plant_dangling_creat_target(tmp));
    auto r = idx.save();
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_IO);
}

TEST_CASE(cache_file_create_dangling_symlink_is_io) {
    TempDir td;
    const auto p = td.p / "x.dat";
    REQUIRE(plant_dangling_creat_target(p));
    auto r = CacheFile::open(p, true);
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error(), SYP_ERR_IO);
}

int main() { return tiny_test_main(); }
