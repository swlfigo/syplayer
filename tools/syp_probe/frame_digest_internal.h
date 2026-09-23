// frame_digest_internal.h — 帧摘要算法本身的内部实现，仅供单测直接验证。
//
// 稳定接口是 frame_digest.h（decode_reference / decode_pipeline /
// diff_frames）。这个头只暴露 digest_of_frame，好让测试能拿一个手工构造的
// syp::media::Frame（不跑完整解码）直接验证「摘要真的按 stride 剥了行」，
// 而不是只能通过跑一次完整解码再比对的间接方式——跟 packet_digest_internal.h
// 暴露 sha256_of 的理由同构（那边的注释：换成 memset(out,0,32) 也能骗过
// 只测端到端的用例，因为素材本身逐字节相同）。
#pragma once

#include <cstdint>
#include <string>

#include "frame_digest.h"
#include "media/frame.h"

namespace syp::probe::detail {

// 把一个已解码的 Frame 压成 FrameDigest。track_index 原样填进结果，不做
// 任何校验——校验（是否是被管理轨、是否越界）是调用方 decode_pipeline /
// decode_reference 的职责，这个函数只管「一帧 → 一条摘要」的纯映射。
//
// 返回 false 表示这一帧的像素/采样数据没法在当前策略下求出有意义的摘要
// （不支持的 pix_fmt、planar 声道数超过 AV_NUM_DATA_POINTERS 等）——*out
// 的 track_index/pts_us/duration_us/fmt/width/height/... 等元数据字段仍会
// 填好，但 data_sha256 未定义，调用方不应该使用它，必须把这次失败当成
// 「整体终止」上报（写 error_stage），不能悄悄退化成一个「摘要为空」的
// 假 FrameDigest——那样两条路径会在同一个不支持的格式上产出同一个假摘要，
// diff_frames 因此永远判「相等」，是本层要防的「该红时不红」。*error 非
// null 时写入失败原因，便于调用方拼 error_stage。
bool digest_of_frame(const syp::media::Frame& frame, int32_t track_index,
                     FrameDigest* out, std::string* error);

}  // namespace syp::probe::detail
