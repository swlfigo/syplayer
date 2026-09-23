// syp_preload_api.cpp — 公开 C ABI 薄壳：转发到 Preloader，不引入平台依赖
#include "preloader.h"
#include "source_bridge.h"

#include <syplayer/syp_preload.h>

#include <cstring>
#include <new>
#include <string>

using syp::dl::MediaInfoProvider;
using syp::dl::PreloadConfig;
using syp::dl::PreloadPriority;
using syp::dl::PreloadStats;
using syp::dl::Preloader;
using syp::dl::copy_config;
using syp::dl::current_http_backend;
using syp::dl::system_clock;

struct syp_preloader {
    std::unique_ptr<Preloader> impl;
};

namespace {

PreloadConfig default_preload_config() {
    return PreloadConfig{};   // 默认值就在 preloader.h 的成员初值上，单一来源
}

// 与 copy_config 同一套 struct_size 兼容规则：调用方结构体小于我们的，
// 多出来的字段保持默认；大于我们的，多出来的部分忽略。
PreloadConfig copy_preload_config(const syp_preload_config* cfg, syp_status* err) {
    PreloadConfig out = default_preload_config();
    if (err != nullptr) *err = SYP_OK;
    if (cfg == nullptr) {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return out;
    }
    syp_preload_config tmp{};
    syp_preload_config_init(&tmp);
    uint32_t sz = cfg->struct_size;
    if (sz < static_cast<uint32_t>(sizeof(uint32_t))) {
        if (err != nullptr) *err = SYP_ERR_INVALID_ARG;
        return out;
    }
    const uint32_t cap = static_cast<uint32_t>(sizeof(syp_preload_config));
    if (sz > cap) sz = cap;
    std::memcpy(&tmp, cfg, sz);
    out.max_total_tasks       = tmp.max_total_tasks;
    out.reserved_for_playing  = tmp.reserved_for_playing;
    out.default_preload_bytes = tmp.default_preload_bytes;
    out.default_preload_ms    = tmp.default_preload_ms;
    return out;
}

PreloadPriority to_priority(syp_preload_priority p) {
    switch (p) {
    case SYP_PRELOAD_PRIORITY_PLAYING:    return PreloadPriority::Playing;
    case SYP_PRELOAD_PRIORITY_NEXT:       return PreloadPriority::Next;
    case SYP_PRELOAD_PRIORITY_BACKGROUND: return PreloadPriority::Background;
    }
    return PreloadPriority::Background;
}

}  // namespace

extern "C" {

void syp_preload_config_init(syp_preload_config* out) {
    if (out == nullptr) return;
    const PreloadConfig d = default_preload_config();
    out->struct_size           = static_cast<uint32_t>(sizeof(syp_preload_config));
    out->max_total_tasks       = d.max_total_tasks;
    out->reserved_for_playing  = d.reserved_for_playing;
    out->default_preload_bytes = d.default_preload_bytes;
    out->default_preload_ms    = d.default_preload_ms;
}

syp_preloader* syp_preloader_create(const syp_config* dl_cfg,
                                    const syp_preload_config* cfg,
                                    const syp_media_info_provider* provider) {
    if (dl_cfg == nullptr) return nullptr;
    syp_status cerr = SYP_OK;
    syp_config dl = copy_config(dl_cfg, &cerr);
    if (cerr != SYP_OK) return nullptr;
    // copy_config 只 memcpy，cache_dir 仍指向调用方的字符串；Preloader
    // 构造时会把它拷进自己的 std::string（preloader.cpp 的构造函数），
    // 所以这里不需要额外延长它的寿命。
    syp_status perr = SYP_OK;
    const PreloadConfig pc = copy_preload_config(cfg, &perr);
    if (perr != SYP_OK) return nullptr;

    MediaInfoProvider mp{};
    if (provider != nullptr) {
        mp.ctx = provider->ctx;
        mp.estimate_range_for_ms = provider->estimate_range_for_ms;
    }

    const syp_http_backend* be = current_http_backend();
    if (be == nullptr) return nullptr;

    syp_status err = SYP_OK;
    auto impl = Preloader::create(dl, pc,
                                  mp.estimate_range_for_ms != nullptr ? &mp : nullptr,
                                  be, system_clock(), &err);
    if (impl == nullptr) return nullptr;

    auto* p = new (std::nothrow) syp_preloader();
    if (p == nullptr) return nullptr;
    p->impl = std::move(impl);
    return p;
}

void syp_preloader_destroy(syp_preloader* p) {
    if (p == nullptr) return;
    p->impl.reset();   // 析构里 interrupt + join + close_all
    delete p;
}

// 【下面四个入口各自兜一次异常】
// `std::string(url)` 会分配，Preloader::add 里还有 make_unique<Entry> /
// by_url_.emplace / entries_.push_back——全都能抛 bad_alloc。而这里是
// `extern "C"`：异常穿出去是未定义行为，实践上就是 std::terminate。
// 接住之后按各自已有的失败形状返回（有返回值的报 SYP_ERR_OOM，void 的
// 静默放弃），调用方不需要多认一种错误。
syp_status syp_preloader_add(syp_preloader* p, const char* url,
                             syp_preload_priority prio, int64_t ms_or_zero) {
    if (p == nullptr || p->impl == nullptr || url == nullptr || url[0] == '\0') {
        return SYP_ERR_INVALID_ARG;
    }
    try {
        return p->impl->add(std::string(url), to_priority(prio), ms_or_zero);
    } catch (...) {
        return SYP_ERR_OOM;
    }
}

syp_status syp_preloader_set_priority(syp_preloader* p, const char* url,
                                      syp_preload_priority prio) {
    if (p == nullptr || p->impl == nullptr || url == nullptr || url[0] == '\0') {
        return SYP_ERR_INVALID_ARG;
    }
    try {
        return p->impl->set_priority(std::string(url), to_priority(prio));
    } catch (...) {
        return SYP_ERR_OOM;
    }
}

void syp_preloader_remove(syp_preloader* p, const char* url) {
    if (p == nullptr || p->impl == nullptr || url == nullptr) return;
    try {
        p->impl->remove(std::string(url));
    } catch (...) {
        // remove 没有返回值；抛出的只可能是那一次 std::string 分配，
        // 此时条目原样留着——和"没调过"是同一个状态，没有中间态。
    }
}

void syp_preloader_remove_all(syp_preloader* p) {
    if (p == nullptr || p->impl == nullptr) return;
    try {
        p->impl->remove_all();
    } catch (...) {
    }
}

void syp_preloader_get_stats(const syp_preloader* p, syp_preload_stats* out) {
    if (out == nullptr) return;
    std::memset(out, 0, sizeof(*out));
    if (p == nullptr || p->impl == nullptr) return;
    PreloadStats st{};
    p->impl->get_stats(&st);
    out->entries          = st.entries;
    out->active_tasks     = st.active_tasks;
    out->downloaded_bytes = st.downloaded_bytes;
    out->completed        = st.completed;
    out->failed           = st.failed;
    out->provider_miss    = st.provider_miss;
}

}  // extern "C"
