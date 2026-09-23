// packet_digest_internal.h — 摘要算法本身的内部实现，仅供单测直接验证。
//
// 稳定接口是 packet_digest.h（demux_file / demux_avio / diff_report）。这个头
// 只暴露 sha256_of / hex32，好让测试能在不跑完整 demux 的前提下，用已知向量
// 证明摘要确实是 SHA-256、确实读了传入的 payload——而不是譬如被误换成
// memset(out, 0, 32) 之后，四条端到端用例却因为素材本身逐字节相同而全绿。
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace syp::probe::detail {

void sha256_of(const uint8_t* data, int32_t len, uint8_t out[32]);
// 数组引用而不是退化指针：签名本身就把长度钉死成 32，调用方传短了/传成
// 裸指针都过不了编译，不用再靠调用约定人肉保证。
std::string hex32(const uint8_t (&v)[32]);

// diff_report（packet_digest.cpp）与 diff_frames（frame_digest.cpp）共用的
// 「谁先失败」前置检查：参照路径失败即判负、被测路径失败即判负、
// averror 不一致即判负（这条实际不可达，见下方长注释）。
//
// 只抽这一段、不抽「参照侧空集合」那部分：DemuxResult 的空集合语义分两层
// （streams 空、packets 空，各自要报不同的中文提示），DecodeResult 只有
// frames 一层，字段名也不同（streams/packets vs frames），硬套成同一个
// 模板参数反而会把调用点写得比直接手写这三四行还绕。这一段（error_stage
// ×2 + averror）两边字段名、类型、语义完全相同，才是真正值得抽的部分——
// 已经实测到「两份平行逻辑会漂移」这条警告应验了
// 一次（diff_report 里解释 averror 冗余检查的长注释，最初在 diff_frames
// 里就漏抄了），抽成共享函数之后这类漂移不再可能发生。
inline std::optional<std::string> diff_failure_hardlines(
        const std::string& e_error_stage, int e_averror,
        const std::string& a_error_stage, int a_averror) {
    // 参照路径失败：比对没有意义——不存在「两边一起挂了所以相等」这回事。
    // 不看 a 长什么样：哪怕 a 报的是逐字段相同的同一个失败，也依然非空，
    // 这正是早先在 packet 层踩过的坑（「两边都失败就判相等」）
    // 要堵的门。
    if (!e_error_stage.empty()) {
        return "参照路径失败，比对无意义: " + e_error_stage;
    }
    if (!a_error_stage.empty()) {
        return "被测路径失败: " + a_error_stage;
    }
    // 当前不可达：两条实现（run_demux() / run_decode()）里每一处写
    // averror 的路径都同时写了 error_stage（见上面两个 error_stage 判空
    // 分支），走到这里时两边 error_stage 必然都是空，也就意味着两边
    // averror 必然都还是默认值 0——这条分支永远不会被真正触发。有意保留
    // 而不删：它是防御性冗余，防的是以后有人加一条新的失败路径时只顾着
    // 写 averror 却忘了配 error_stage（两者本该总是成对出现），这条判断
    // 能在那种回归发生时兜住而不是悄悄放过；万一 error_stage 的判定逻辑
    // 本身以后被改动，这里也不会因为丢了这条冗余检查而多一个漏判窗口。
    if (e_averror != a_averror) {
        return "averror 不同: 参照 " + std::to_string(e_averror) +
               ", 实际 " + std::to_string(a_averror);
    }
    return std::nullopt;
}

}  // namespace syp::probe::detail
