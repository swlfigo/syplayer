// syp_dl_purity_check.cpp — 只链 syp_dl 归档，不链任何 framework。
// 编过、链过 = dl 层此刻没有平台符号。默认随 ALL 构建，不进 ctest。
#include <dl/hole_set.h>

#include <syplayer/syp_http.h>
#include <syplayer/syp_types.h>

int main() {
    syp::dl::HoleSet hs;
    hs.add({0, 8});
    if (hs.total_bytes() != 8) return 1;
    if (!hs.contains({0, 8})) return 2;
    // 碰到公开 ABI 类型即可：实现尚未提供的 C 入口不要引用，否则缺符号。
    syp_http_backend table{};
    (void)table.create;
    syp_status st = SYP_OK;
    (void)st;
    return 0;
}
