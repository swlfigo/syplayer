// hls_options.h —— HLS 会话的调用方选项。
//
// 单独一个头，只为一件事：`pipeline.h` 与 `hls_session.h` 都要看到这个
// 结构体，但 `pipeline.h` **不能** include `hls_session.h`——后者拖着
// FFmpeg（libavformat）与 dl 层（syp_source/syp_config）的一整串头，
// 而 `pipeline.h` 被解码路径上几乎每个 TU include。把 POD 单独拆出来，
// 两边各 include 这一个零依赖的小头，`pipeline.h` 只前置声明
// `namespace hls { class HlsSession; }`。
//
// 这里只准放 POD 与 <cstdint>。任何"顺手加一个 std::string 字段"都会把
// <string> 拖进来，再往后就是慢慢长回一个大头。
#pragma once

#include <cstdint>

namespace syp::media {

struct HlsOptions {
    // 0 = 不限，选 BANDWIDTH 最高的那档。
    // 选定规则（HlsSession::select_variant）：
    //   1. 候选先限定在**含至少一条视频流的 program**里——ffmpeg 的 hls
    //      muxer 会给带 agroup 的音频轨额外写一条自引用的
    //      EXT-X-STREAM-INF，它是个纯音频"档"，选中它就是"有声音没画面"；
    //      只有在一个含视频的 program 都没有时（纯音频 HLS，如播客）才
    //      回退到全部 program 参选；
    //   2. 在候选里选满足 <= max_bandwidth_bps 的 BANDWIDTH 最大的那档；
    //   3. 一档都不满足时选候选里 BANDWIDTH 最小的那档——"网太慢"不该
    //      等于"播不了"。
    int64_t max_bandwidth_bps = 0;
};

}  // namespace syp::media
