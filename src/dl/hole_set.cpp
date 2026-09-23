// hole_set.cpp — HoleSet 实现；与 syp_range 的布局钉死在这里，避免内部头依赖 C ABI
#include "hole_set.h"

#include <syplayer/syp_types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <ranges>

namespace syp::dl {
namespace {

// 调用方错误：负偏移。Debug 用 assert 拦住，Release 走 no-op，避免 UB。
bool reject_negative(int64_t start) noexcept {
    if (start >= 0) return false;
    assert(start >= 0 && "HoleSet: start/pos must be >= 0");
    return true;
}

// end 随不变式严格递增（prev.end < next.start < next.end），故可对 end 二分。
// first_ge_end：第一个 end >= pos（add 合并用，相邻也要并进来）。
// first_gt_end：第一个 end >  pos（与 [pos, ...) 相交 / 覆盖判定）。
template<typename C>
auto first_ge_end(C& c, int64_t pos) {
    return std::ranges::lower_bound(c, pos, {}, &Range::end);
}

template<typename C>
auto first_gt_end(C& c, int64_t pos) {
    return std::ranges::upper_bound(c, pos, {}, &Range::end);
}

}  // namespace

static_assert(sizeof(Range) == sizeof(syp_range));
static_assert(alignof(Range) == alignof(syp_range));
static_assert(offsetof(Range, start) == offsetof(syp_range, start));
static_assert(offsetof(Range, end) == offsetof(syp_range, end));

void HoleSet::add(Range r) {
    if (reject_negative(r.start)) return;
    if (r.end <= r.start) return;

    // 第一个可能与 r 相邻或重叠的段：end >= r.start（半开区间下 end==r.start 即相邻）。
    auto first = first_ge_end(ranges_, r.start);
    auto last  = first;
    // 循环条件读的是循环体可能改过的 r.end。这是安全的：不变式保证
    // prev.end < next.start，所以把 r.end 扩到 last->end 之后，下一段的
    // start 必然 > 扩张后的 r.end，while 会停，不会把不相邻的段吞进来。
    // 必须用扩张后的 r.end：合并中间段后，原本只与中间段相邻的右侧段
    // 会变成与新 r 相邻，需要继续并进来。
    while (last != ranges_.end() && last->start <= r.end) {
        if (last->start < r.start) r.start = last->start;
        if (last->end   > r.end)   r.end   = last->end;
        ++last;
    }

    if (first == last) {
        ranges_.insert(first, r);
        return;
    }
    *first = r;
    if (std::next(first) != last) {
        ranges_.erase(std::next(first), last);
    }
}

void HoleSet::remove(Range r) {
    if (reject_negative(r.start)) return;
    if (r.end <= r.start) return;

    // 第一个与 r 有交集的段：end > r.start（半开，end==r.start 不相交）。
    auto first = first_gt_end(ranges_, r.start);
    if (first == ranges_.end() || first->start >= r.end) return;

    auto last = first;
    Range left{};
    Range right{};
    bool has_left  = false;
    bool has_right = false;
    while (last != ranges_.end() && last->start < r.end) {
        if (last->start < r.start) {
            left     = Range{last->start, r.start};
            has_left = true;
        }
        if (last->end > r.end) {
            right     = Range{r.end, last->end};
            has_right = true;
        }
        ++last;
    }

    auto it = ranges_.erase(first, last);
    if (has_right) it = ranges_.insert(it, right);
    if (has_left)  ranges_.insert(it, left);
}

void HoleSet::clear() noexcept {
    ranges_.clear();
}

bool HoleSet::empty() const noexcept {
    return ranges_.empty();
}

size_t HoleSet::count() const noexcept {
    return ranges_.size();
}

int64_t HoleSet::total_bytes() const noexcept {
    int64_t sum = 0;
    for (const Range& r : ranges_) {
        sum += r.size();  // 不重叠且 start>=0，总和 ≤ 覆盖 [0, INT64_MAX) 的长度
    }
    return sum;
}

std::span<const Range> HoleSet::ranges() const noexcept {
    return std::span<const Range>(ranges_);
}

std::vector<Range> HoleSet::intersect(Range q) const {
    if (q.end <= q.start) return {};
    if (reject_negative(q.start)) return {};

    std::vector<Range> out;
    auto it = first_gt_end(ranges_, q.start);
    for (; it != ranges_.end() && it->start < q.end; ++it) {
        const int64_t s = it->start > q.start ? it->start : q.start;
        const int64_t e = it->end   < q.end   ? it->end   : q.end;
        if (s < e) out.push_back(Range{s, e});
    }
    return out;
}

std::vector<Range> HoleSet::holes_in(Range q) const {
    if (q.end <= q.start) return {};
    if (reject_negative(q.start)) return {};

    std::vector<Range> holes;
    int64_t cur = q.start;
    auto it = first_gt_end(ranges_, q.start);
    for (; it != ranges_.end() && it->start < q.end; ++it) {
        if (it->start > cur) {
            holes.push_back(Range{cur, it->start});
        }
        if (it->end > cur) cur = it->end;
        if (cur >= q.end) return holes;
    }
    if (cur < q.end) {
        holes.push_back(Range{cur, q.end});
    }
    return holes;
}

bool HoleSet::contains(Range q) const noexcept {
    if (q.end <= q.start) return true;
    if (reject_negative(q.start)) return false;

    // 相邻已合并，q 被完全覆盖 ⇔ 落在单一段内。
    auto it = first_gt_end(ranges_, q.start);
    return it != ranges_.end() && it->start <= q.start && it->end >= q.end;
}

int64_t HoleSet::contiguous_from(int64_t pos) const noexcept {
    if (reject_negative(pos)) return 0;

    auto it = first_gt_end(ranges_, pos);
    if (it == ranges_.end() || it->start > pos) return 0;
    return it->end - pos;  // it->end > pos 且两者非负，减法不溢出
}

std::optional<Range> HoleSet::first_hole_from(int64_t pos, int64_t limit) const noexcept {
    if (reject_negative(pos)) return std::nullopt;
    if (limit <= pos) return std::nullopt;

    auto it = first_gt_end(ranges_, pos);
    int64_t hole_start = pos;
    if (it != ranges_.end() && it->start <= pos) {
        hole_start = it->end;  // pos 已缓存，洞从这段结束处才开始
        ++it;
    }
    if (hole_start >= limit) return std::nullopt;

    const int64_t hole_end =
        (it != ranges_.end() && it->start < limit) ? it->start : limit;
    if (hole_start >= hole_end) return std::nullopt;
    return Range{hole_start, hole_end};
}

}  // namespace syp::dl
