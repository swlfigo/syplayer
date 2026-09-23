// hole_set.h — 已缓存字节区间集合：合并、挖洞、O(log n) 连续可读查询
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace syp::dl {

// 半开区间 [start, end)。布局必须与公开 ABI 的 syp_range 一致（static_assert 在 .cpp）。
struct Range {
    int64_t start = 0;
    int64_t end   = 0;
    constexpr int64_t size() const noexcept { return end - start; }
    constexpr bool empty() const noexcept { return end <= start; }
    friend constexpr bool operator==(Range, Range) noexcept = default;
};

// 已缓存区间的有序集合。
//
// 不变式（任何公开方法返回后均成立）：
//   1. ranges_ 按 start 升序；
//   2. 两两不重叠且不相邻（prev.end < next.start，相邻必须已合并）；
//   3. 不存在空区间（start < end）；
//   4. start >= 0。
//
// 用 vector 而非 set/map：区间数通常几十，连续内存 + 二分比节点跳转更快，
// 且 ranges() 可直接批量拷回 C ABI。
//
// 复杂度（n = 段数，k = 输出段数；定位一律二分，禁止线性扫描定位）：
//   add / remove / clear          O(n)     二分定位 + 向量搬移
//   empty / count / ranges        O(1)
//   total_bytes                   O(n)     不另维护缓存，避免与 ranges_ 失同步
//   intersect / holes_in          O(log n + k)
//   contains / contiguous_from / first_hole_from  O(log n)
class HoleSet {
public:
    HoleSet() = default;

    // 加入一段已缓存区间；与已有区间重叠或相邻则合并。空/无效区间为 no-op。
    void add(Range r);
    // 移除一段区间；跨越多段、落在某段中间（一分为二）都要正确处理。
    void remove(Range r);
    void clear() noexcept;

    bool    empty() const noexcept;
    size_t  count() const noexcept;          // 区间段数
    int64_t total_bytes() const noexcept;    // 所有区间长度之和，O(n)
    std::span<const Range> ranges() const noexcept;  // 只读视图，按序

    // q 内已有的部分（升序、不重叠）。q 为空返回空。
    std::vector<Range> intersect(Range q) const;
    // q 内缺失的部分 —— 这就是"洞"。q 为空返回空。
    // q 完全落在空洞里时返回 {q} 本身。
    std::vector<Range> holes_in(Range q) const;
    // q 是否被完全覆盖（q 为空时返回 true）。
    bool contains(Range q) const noexcept;

    // 从 pos 起连续可用的字节数（pos 未缓存则返回 0）。read 路径最热，必须 O(log n)。
    int64_t contiguous_from(int64_t pos) const noexcept;
    // 从 pos 起的第一个洞，限制在 [pos, limit) 内；没有洞返回 std::nullopt。
    std::optional<Range> first_hole_from(int64_t pos, int64_t limit) const noexcept;

private:
    std::vector<Range> ranges_;
};

}  // namespace syp::dl
