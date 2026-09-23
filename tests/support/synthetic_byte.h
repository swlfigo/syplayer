// synthetic_byte.h — 测试用合成字节公式的唯一定义。
//
// loopback_server.h（服务端按这个公式合成
// 响应体）与 stub_backend.h（测试桩按这个公式算期望值）曾经各自内嵌一份
// 逐字节相同的定义，靠人工保持一致——这正是本仓库一贯在防的那类陷阱：
// 看起来一致、其实只是碰巧。两边都改成 #include 这个头，不要再复制一份
// 定义到别处；改这个公式会同时改变服务端合成的字节与桩期望的字节。
#pragma once

#include <cstdint>

namespace syp::dl::test {

// 合成资源：byte(i) = (i * 31 + 7) & 0xFF，任意区间都能独立算出期望字节。
inline uint8_t synthetic_byte(int64_t offset) noexcept {
    const uint64_t u = static_cast<uint64_t>(offset);
    return static_cast<uint8_t>((u * 31u + 7u) & 0xFFu);
}

}  // namespace syp::dl::test
