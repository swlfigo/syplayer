// fake_ffmpeg_internal.h — 假"接入方 FFmpeg"各成员之间共用的记账接口。
//
// 记账（计数、查询、清零）单独放在 fake_ffmpeg_counts.cpp 一个归档成员里，
// 各个假 FFmpeg 函数分散在各自的成员里、只向记账成员单向引用。这样接入方
// 代码引用 fake_ffmpeg_call_count 之类只会拉进记账成员，**不会**顺带把假的
// avformat_version 等定义拉进链接——与真实 libav* 静态库一样，某个 FFmpeg
// 函数的定义进不进链接，只取决于有没有未定义引用去找它。否则那个成员被
// 记账接口强制加载后，它里面的假定义会无视链接顺序胜过动态库，测试就测
// 不出"SYFFmpeg 退化成导出原名"了。
#pragma once

namespace fake_ffmpeg {

enum Fn : int {
    kAvformatVersion,
    kAvcodecVersion,
    kAvMalloc,
    kAvFree,
    kAvLogSetLevel,
    kAvLogGetLevel,
    kAvformatOpenInput,
    kAvformatFindStreamInfo,
    kAvformatCloseInput,
    kAvReadFrame,
    kAvcodecFindDecoder,
    kFnCount,
};

}  // namespace fake_ffmpeg

extern "C" void fake_ffmpeg_hit(int fn);
