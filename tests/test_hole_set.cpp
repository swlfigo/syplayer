// test_hole_set.cpp — HoleSet 的确定性边界用例 + 固定种子随机对照
#include "tiny_test.h"

#include <dl/hole_set.h>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <random>
#include <vector>

using syp::dl::HoleSet;
using syp::dl::Range;

namespace syp::dl {

// 仅本测试 TU：靠 ADL 让 tiny_test::stringify 打出 Range / vector<Range>。
// 库代码不要再定义同名重载，否则会和这里（或其它测试 TU）撞车。
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

int64_t sum_sizes(const std::vector<Range>& v) {
    int64_t s = 0;
    for (const Range& r : v) s += r.size();
    return s;
}

void check_invariants(const HoleSet& hs) {
    const auto rs = hs.ranges();
    CHECK_EQ(hs.count(), rs.size());
    CHECK_EQ(hs.empty(), rs.empty());

    int64_t sum = 0;
    for (size_t i = 0; i < rs.size(); ++i) {
        REQUIRE(rs[i].start >= 0);
        REQUIRE(rs[i].start < rs[i].end);
        if (i > 0) {
            REQUIRE(rs[i - 1].start < rs[i].start);
            REQUIRE(rs[i - 1].end < rs[i].start);  // 不重叠且不相邻
        }
        sum += rs[i].size();
    }
    CHECK_EQ(hs.total_bytes(), sum);
}

bool disjoint(const std::vector<Range>& a, const std::vector<Range>& b) {
    for (const Range& x : a) {
        for (const Range& y : b) {
            const int64_t s = x.start > y.start ? x.start : y.start;
            const int64_t e = x.end   < y.end   ? x.end   : y.end;
            if (s < e) return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE(empty_set) {
    HoleSet hs;
    const Range q{0, 100};
    CHECK(hs.empty());
    CHECK_EQ(hs.count(), static_cast<size_t>(0));
    CHECK_EQ(hs.total_bytes(), int64_t{0});
    CHECK_EQ(hs.holes_in(q), std::vector<Range>{q});
    CHECK(!hs.contains(q));
    CHECK(hs.contains(Range{5, 5}));  // 空查询视为已覆盖
    CHECK_EQ(hs.contiguous_from(0), int64_t{0});
    CHECK_EQ(hs.contiguous_from(50), int64_t{0});
    CHECK(hs.intersect(q).empty());
    CHECK(hs.holes_in(Range{10, 10}).empty());
    check_invariants(hs);
}

TEST_CASE(add_variants) {
    {
        HoleSet hs;
        hs.add({5, 10});
        CHECK_EQ(collected(hs), std::vector<Range>{{5, 10}});
        CHECK_EQ(hs.total_bytes(), int64_t{5});
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 5});
        hs.add({20, 25});
        hs.add({10, 15});
        CHECK_EQ(collected(hs), (std::vector<Range>{{0, 5}, {10, 15}, {20, 25}}));
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({5, 10});
        hs.add({10, 15});  // 相邻必须合并
        CHECK_EQ(collected(hs), std::vector<Range>{{5, 15}});
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({5, 10});
        hs.add({8, 15});
        CHECK_EQ(collected(hs), std::vector<Range>{{5, 15}});
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({5, 20});
        hs.add({8, 12});  // 被包含，无变化
        CHECK_EQ(collected(hs), std::vector<Range>{{5, 20}});
        CHECK_EQ(hs.count(), static_cast<size_t>(1));
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 5});
        hs.add({10, 15});
        hs.add({20, 25});
        hs.add({0, 25});  // 一段覆盖已有多段，塌缩
        CHECK_EQ(collected(hs), std::vector<Range>{{0, 25}});
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({40, 50});
        hs.add({0, 10});
        hs.add({20, 30});
        CHECK_EQ(collected(hs), (std::vector<Range>{{0, 10}, {20, 30}, {40, 50}}));
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({5, 5});
        hs.add({10, 3});
        CHECK(hs.empty());
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 10});
        hs.add({10, 20});
        hs.add({20, 30});
        CHECK_EQ(collected(hs), std::vector<Range>{{0, 30}});
        check_invariants(hs);
    }
}

TEST_CASE(add_idempotent) {
    HoleSet hs;
    hs.add({5, 15});
    const auto once = collected(hs);
    hs.add({5, 15});
    CHECK_EQ(collected(hs), once);
    hs.add({5, 15});
    CHECK_EQ(collected(hs), once);
    CHECK_EQ(hs.total_bytes(), int64_t{10});
    check_invariants(hs);
}

TEST_CASE(remove_variants) {
    {
        HoleSet hs;
        hs.add({0, 20});
        hs.remove({0, 20});  // 整段命中
        CHECK(hs.empty());
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 20});
        hs.remove({0, 5});  // 截头
        CHECK_EQ(collected(hs), std::vector<Range>{{5, 20}});
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 20});
        hs.remove({15, 20});  // 截尾
        CHECK_EQ(collected(hs), std::vector<Range>{{0, 15}});
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 20});
        hs.remove({5, 10});  // 中间挖洞，一分为二
        CHECK_EQ(collected(hs), (std::vector<Range>{{0, 5}, {10, 20}}));
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 10});
        hs.add({20, 30});
        hs.add({40, 50});
        hs.remove({5, 45});  // 跨多段
        CHECK_EQ(collected(hs), (std::vector<Range>{{0, 5}, {45, 50}}));
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({10, 20});
        hs.remove({0, 5});   // 完全不命中（左侧）
        hs.remove({30, 40}); // 完全不命中（右侧）
        hs.remove({20, 25}); // 相邻但不重叠（半开）
        CHECK_EQ(collected(hs), std::vector<Range>{{10, 20}});
        check_invariants(hs);
    }
    {
        // 挖掉的是已缓存部分，加回同一段应复原；不能把原本的空洞一并填上。
        HoleSet hs;
        hs.add({0, 30});
        const auto before = collected(hs);
        hs.remove({5, 25});
        CHECK_EQ(collected(hs), (std::vector<Range>{{0, 5}, {25, 30}}));
        hs.add({5, 25});
        CHECK_EQ(collected(hs), before);
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 10});
        hs.add({20, 30});
        const auto before = collected(hs);
        hs.remove({0, 10});
        hs.add({0, 10});
        CHECK_EQ(collected(hs), before);
        check_invariants(hs);
    }
    {
        HoleSet hs;
        hs.add({0, 10});
        hs.remove({3, 3});
        hs.remove({12, 8});
        CHECK_EQ(collected(hs), std::vector<Range>{{0, 10}});
        check_invariants(hs);
    }
}

TEST_CASE(holes_in_variants) {
    HoleSet hs;
    hs.add({10, 20});
    hs.add({30, 40});

    CHECK_EQ(hs.holes_in({12, 18}), std::vector<Range>{});           // 完全被覆盖
    CHECK_EQ(hs.holes_in({0, 5}), std::vector<Range>{{0, 5}});       // 落在洞中间（左侧空洞）
    CHECK_EQ(hs.holes_in({22, 28}), std::vector<Range>{{22, 28}});   // 两段之间的洞
    CHECK_EQ(hs.holes_in({5, 45}),                                  // 跨越多个洞
             (std::vector<Range>{{5, 10}, {20, 30}, {40, 45}}));
    CHECK_EQ(hs.holes_in({40, 60}), std::vector<Range>{{40, 60}});   // 超出已有区间右侧
    CHECK_EQ(hs.holes_in({0, 10}), std::vector<Range>{{0, 10}});     // 与第一段左侧相接
    CHECK_EQ(hs.holes_in({20, 30}), std::vector<Range>{{20, 30}});
    CHECK_EQ(hs.holes_in({15, 35}), std::vector<Range>{{20, 30}});
    CHECK(hs.holes_in({10, 10}).empty());
    check_invariants(hs);
}

TEST_CASE(intersect_complements_holes) {
    HoleSet hs;
    hs.add({10, 20});
    hs.add({30, 40});
    hs.add({50, 80});

    const Range queries[] = {
        {0, 100}, {12, 18}, {0, 5}, {15, 35}, {40, 50},
        {5, 10}, {80, 90}, {25, 55}, {10, 80}, {0, 0}, {100, 50},
    };
    for (Range q : queries) {
        const auto inter = hs.intersect(q);
        const auto holes = hs.holes_in(q);
        if (q.empty()) {
            CHECK(inter.empty());
            CHECK(holes.empty());
        } else {
            CHECK_EQ(sum_sizes(inter) + sum_sizes(holes), q.size());
            CHECK(disjoint(inter, holes));
        }
        CHECK_EQ(hs.contains(q), holes.empty());
    }
    check_invariants(hs);
}

TEST_CASE(contiguous_from_positions) {
    HoleSet hs;
    hs.add({10, 20});
    hs.add({30, 40});

    CHECK_EQ(hs.contiguous_from(10), int64_t{10});  // 段内起点
    CHECK_EQ(hs.contiguous_from(15), int64_t{5});   // 段中间
    CHECK_EQ(hs.contiguous_from(19), int64_t{1});   // 最后一字节
    CHECK_EQ(hs.contiguous_from(20), int64_t{0});   // 段末尾（半开，未缓存）
    CHECK_EQ(hs.contiguous_from(25), int64_t{0});   // 洞里
    CHECK_EQ(hs.contiguous_from(40), int64_t{0});   // 超出末尾
    CHECK_EQ(hs.contiguous_from(100), int64_t{0});
    CHECK_EQ(hs.contiguous_from(30), int64_t{10});
    CHECK_EQ(hs.contiguous_from(0), int64_t{0});
    check_invariants(hs);
}

TEST_CASE(first_hole_from_limits) {
    HoleSet hs;
    hs.add({10, 20});
    hs.add({30, 40});

    CHECK(!hs.first_hole_from(10, 10).has_value());  // limit == pos
    CHECK(!hs.first_hole_from(10, 5).has_value());   // limit < pos

    {
        auto h = hs.first_hole_from(0, 100);
        REQUIRE(h.has_value());
        CHECK_EQ(*h, Range{0, 10});
    }
    {
        auto h = hs.first_hole_from(10, 100);  // 已缓存，跳到段末
        REQUIRE(h.has_value());
        CHECK_EQ(*h, Range{20, 30});
    }
    {
        auto h = hs.first_hole_from(15, 18);  // limit 落在同一段内，无洞
        CHECK(!h.has_value());
    }
    {
        auto h = hs.first_hole_from(15, 25);  // limit 切过洞
        REQUIRE(h.has_value());
        CHECK_EQ(*h, Range{20, 25});
    }
    {
        auto h = hs.first_hole_from(20, 30);
        REQUIRE(h.has_value());
        CHECK_EQ(*h, Range{20, 30});
    }
    {
        auto h = hs.first_hole_from(35, 100);
        REQUIRE(h.has_value());
        CHECK_EQ(*h, Range{40, 100});
    }
    {
        auto h = hs.first_hole_from(40, 50);
        REQUIRE(h.has_value());
        CHECK_EQ(*h, Range{40, 50});
    }
    {
        HoleSet empty;
        auto h = empty.first_hole_from(3, 9);
        REQUIRE(h.has_value());
        CHECK_EQ(*h, Range{3, 9});
    }
    check_invariants(hs);
}

TEST_CASE(large_offsets_and_zero_start) {
    const int64_t B = INT64_MAX / 2;
    HoleSet hs;
    hs.add({0, 10});
    CHECK_EQ(hs.contiguous_from(0), int64_t{10});
    CHECK_EQ(collected(hs), std::vector<Range>{{0, 10}});

    hs.add({B, B + 100});
    hs.add({B + 100, B + 200});  // 相邻大数合并，禁止 start+size 那种可能溢出的写法
    CHECK_EQ(collected(hs), (std::vector<Range>{{0, 10}, {B, B + 200}}));

    hs.add({B - 50, B});  // 左侧相邻
    CHECK_EQ(collected(hs), (std::vector<Range>{{0, 10}, {B - 50, B + 200}}));

    hs.remove({B, B + 10});
    CHECK_EQ(collected(hs),
             (std::vector<Range>{{0, 10}, {B - 50, B}, {B + 10, B + 200}}));

    CHECK_EQ(hs.contiguous_from(B - 50), int64_t{50});
    CHECK_EQ(hs.contiguous_from(B), int64_t{0});
    CHECK_EQ(hs.contiguous_from(B + 10), int64_t{190});

    auto hole = hs.first_hole_from(B - 1, B + 20);
    REQUIRE(hole.has_value());
    CHECK_EQ(*hole, Range{B, B + 10});

    CHECK_EQ(hs.total_bytes(), int64_t{10 + 50 + 190});
    check_invariants(hs);
}

#if defined(NDEBUG)
TEST_CASE(negative_start_noop_in_release) {
    HoleSet hs;
    hs.add({-1, 10});
    hs.remove({-5, 5});
    CHECK(hs.empty());
    CHECK_EQ(hs.contiguous_from(-3), int64_t{0});
    CHECK(!hs.first_hole_from(-1, 10).has_value());
    check_invariants(hs);
}
#endif

TEST_CASE(random_vs_bitmap) {
    constexpr int kN     = 4096;
    constexpr int kIters = 2500;
    std::mt19937 rng{20260906};  // 固定种子，保证可复现
    std::uniform_int_distribution<int> coord{0, kN};
    // pos/limit 取到 kN 之外：bitmap 在 kN 之后概念上恒为未缓存，越界查询
    // 走这条自然路径，参考模型不需要特判。
    std::uniform_int_distribution<int> beyond{0, kN + 64};
    std::uniform_int_distribution<int> coin{0, 9};

    HoleSet hs;
    std::vector<uint8_t> bm(static_cast<size_t>(kN), 0);

    auto paint = [&](int lo, int hi, uint8_t v) {
        if (lo < 0) lo = 0;
        if (hi > kN) hi = kN;
        for (int i = lo; i < hi; ++i) {
            bm[static_cast<size_t>(i)] = v;
        }
    };

    // kN 之后恒为未缓存：HoleSet 没有"文件大小"，最后一段之后全是洞。
    auto covered = [&](int i) {
        return i >= 0 && i < kN && bm[static_cast<size_t>(i)] != 0;
    };

    auto from_bitmap = [&]() {
        std::vector<Range> out;
        int i = 0;
        while (i < kN) {
            if (!covered(i)) {
                ++i;
                continue;
            }
            int j = i + 1;
            while (covered(j)) ++j;
            out.push_back(Range{i, j});
            i = j;
        }
        return out;
    };

    auto bitmap_holes = [&](int lo, int hi) {
        std::vector<Range> out;
        int i = lo;
        while (i < hi) {
            if (covered(i)) {
                ++i;
                continue;
            }
            int j = i + 1;
            while (j < hi && !covered(j)) ++j;
            out.push_back(Range{i, j});
            i = j;
        }
        return out;
    };

    auto bitmap_intersect = [&](int lo, int hi) {
        std::vector<Range> out;
        int i = lo;
        while (i < hi) {
            if (!covered(i)) {
                ++i;
                continue;
            }
            int j = i + 1;
            while (j < hi && covered(j)) ++j;
            out.push_back(Range{i, j});
            i = j;
        }
        return out;
    };

    auto bitmap_contiguous = [&](int pos) -> int64_t {
        if (!covered(pos)) return 0;
        int j = pos;
        while (covered(j)) ++j;
        return static_cast<int64_t>(j - pos);
    };

    for (int iter = 0; iter < kIters; ++iter) {
        int a = coord(rng);
        int b = coord(rng);
        if (a > b) std::swap(a, b);
        const Range r{a, b};
        if (coin(rng) < 6) {
            hs.add(r);
            paint(a, b, 1);
        } else {
            hs.remove(r);
            paint(a, b, 0);
        }

        // 文件尾部还没下下来：让"洞延伸到已缓存区间之外"成为常态。
        if (iter % 8 == 0) {
            hs.remove({kN - 512, kN});
            paint(kN - 512, kN, 0);
        }

        const auto exp = from_bitmap();
        const auto got = collected(hs);
        if (got != exp) {
            CHECK_EQ(got, exp);
            REQUIRE(got == exp);
        }

        const int qa = coord(rng);
        const int qb = coord(rng);
        const int lo = qa < qb ? qa : qb;
        const int hi = qa < qb ? qb : qa;
        const Range q{lo, hi};

        const auto inter = hs.intersect(q);
        const auto holes = hs.holes_in(q);
        CHECK_EQ(inter, bitmap_intersect(lo, hi));
        CHECK_EQ(holes, bitmap_holes(lo, hi));
        if (!q.empty()) {
            CHECK_EQ(sum_sizes(inter) + sum_sizes(holes), q.size());
            CHECK(disjoint(inter, holes));
        }
        CHECK_EQ(hs.contains(q), holes.empty());
        CHECK_EQ(hs.contiguous_from(lo), bitmap_contiguous(lo));

        // first_hole_from 与 holes_in 是两条独立实现路径，第一段必须一致。
        {
            const auto fh = hs.first_hole_from(lo, hi);
            if (holes.empty()) {
                CHECK(!fh.has_value());
            } else {
                REQUIRE(fh.has_value());
                CHECK_EQ(*fh, holes.front());
            }
        }

        if (coin(rng) < 3) {
            const int pos   = beyond(rng);
            const int limit = beyond(rng);
            auto hole = hs.first_hole_from(pos, limit);
            std::optional<Range> exp_hole;
            if (limit > pos) {
                int i = pos;
                while (covered(i)) ++i;  // 跳过 pos 处连续已缓存；kN 之后 covered 恒 false
                if (i < limit) {
                    int j = i;
                    while (j < limit && !covered(j)) ++j;
                    exp_hole = Range{i, j};
                }
            }
            CHECK(hole == exp_hole);

            const auto holes_pl = hs.holes_in(Range{pos, limit});
            CHECK_EQ(holes_pl, bitmap_holes(pos, limit));
            if (holes_pl.empty()) {
                CHECK(!hole.has_value());
            } else {
                REQUIRE(hole.has_value());
                CHECK_EQ(*hole, holes_pl.front());
            }
        }

        if (iter % 50 == 0) check_invariants(hs);
    }
    check_invariants(hs);
}

int main() { return tiny_test_main(); }
