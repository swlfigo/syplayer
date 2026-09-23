// scenarios.h — 跑一趟「经由 syp_source 的 demux」所需的全部脚手架。
//
// 看门狗不是可选项：仓库自己的教训（README 里那张复现分布表）说明
// dl 层的并发缺陷会表现为永久挂死。挂死必须变成红，不能变成 CI 卡死。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <syplayer/syp_config.h>

#include "packet_digest.h"

namespace syp::probe {

struct SourceMetrics {
    int64_t downloaded_bytes = 0;
    int64_t cache_hit_bytes  = 0;
    int64_t cached_bytes     = 0;
    int32_t failed_tasks     = 0;
    int32_t redirect_count   = 0;
};

struct RunSpec {
    std::string           url;
    std::string           cache_dir;
    DemuxOptions          opt;
    std::vector<SeekPlan> seeks;
    int                   watchdog_ms = 120000;
    // 可选：在 syp_config_init() 填好默认值之后、syp_source_open() 之前，
    // 对 cfg 做最后调整（如钉死 min_segment_size / max_retries 做确定性
    // 构造）。纯 probe 侧机制，不改 src/dl/ 里任何一个默认值——不设就是
    // 原来的行为，cfg.struct_size/cache_dir 由 run_through_source 兜底，
    // tweak_config 里改了也会被后面的赋值覆盖，只留调用方真正想调的字段。
    std::function<void(syp_config&)> tweak_config;
};

struct RunResult {
    DemuxResult   demux;
    SourceMetrics metrics;
    std::string   failure;   // 非空 = 这一趟没跑成（含看门狗触发）

    // H3：syp_source_open() 挂 on_error 回调，把状态码
    // 记下来。此前 probe 从头到尾没接这个回调——第 ⑤ 条验收标准
    // （etag/Content-Length 变化能识别）只能靠 test_source_bridge 的两条
    // 单测证明，端到端路径上 SYP_ERR_CONTENT_CHANGED 从未被直接观测过，
    // 场景 H 只能从 FFmpeg 的 AVERROR_INVALIDDATA 反推。接上之后 H 可以
    // 直接断言状态码本身。SYP_OK（=0）表示全程没有触发过 on_error。
    syp_status error_status      = SYP_OK;
    int32_t    error_http_status = 0;
};

// 幂等注册 Apple HTTP 后端。返回 false 表示注册失败。
bool ensure_apple_backend();

// 建一个空的临时缓存目录并返回路径。调用方负责删——除非用下面的
// TempCacheDir（推荐：手写 remove_dir_recursive() 只覆盖函数正常
// 走到结尾的那条路径，测试里散落的 REQUIRE 提前 return 会跳过它，
// 异常/CHECK 失败提前退出同理，都会在 fixture 目录旁边留下孤儿目录）。
std::string make_temp_cache_dir(const std::string& tag);
void        remove_dir_recursive(const std::string& path);

// RAII 版：析构时无条件清理，覆盖 REQUIRE 提前 return / CHECK 失败继续跑
// 完全程但中途异常等所有正常 C++ 栈展开路径。进程被信号杀死（比如
// ctest TIMEOUT 命中，SIGTERM/SIGKILL）时析构不会跑，这种情况不在覆盖
// 范围内——下一次同 pid 复用这个 tag 时 make_temp_cache_dir() 自己会先
// remove_all 兜底，代价只是"占用到下次同 pid 复用之前"，影响很小。
struct TempCacheDir {
    std::string path;
    explicit TempCacheDir(const std::string& tag) : path(make_temp_cache_dir(tag)) {}
    ~TempCacheDir() { if (!path.empty()) remove_dir_recursive(path); }
    TempCacheDir(const TempCacheDir&) = delete;
    TempCacheDir& operator=(const TempCacheDir&) = delete;
};

RunResult run_through_source(const RunSpec& spec);

}  // namespace syp::probe
