// syp_source_api.cpp — 公开 C ABI 薄壳：转发到 SourceBridge，不引入平台依赖
#include "source_bridge.h"

#include <new>
#include <string>

using syp::dl::SourceBridge;
using syp::dl::copy_config;
using syp::dl::current_http_backend;
using syp::dl::default_config;
using syp::dl::system_clock;

struct syp_source {
    std::unique_ptr<SourceBridge> impl;
};

extern "C" {

void syp_config_init(syp_config* out) {
    if (out == nullptr) return;
    *out = default_config();
}

const char* syp_status_str(syp_status s) {
    switch (s) {
    case SYP_OK:                     return "ok";
    case SYP_ERR_EOF:                return "eof";
    case SYP_ERR_INVALID_ARG:        return "invalid_arg";
    case SYP_ERR_CANCELED:           return "canceled";
    case SYP_ERR_TIMEOUT:            return "timeout";
    case SYP_ERR_OOM:                return "oom";
    case SYP_ERR_BUSY:               return "busy";
    case SYP_ERR_IO:                 return "io";
    case SYP_ERR_NO_SPACE:           return "no_space";
    case SYP_ERR_CACHE_CORRUPT:      return "cache_corrupt";
    case SYP_ERR_NETWORK:            return "network";
    case SYP_ERR_HTTP_STATUS:        return "http_status";
    case SYP_ERR_TOO_MANY_REDIRECTS: return "too_many_redirects";
    case SYP_ERR_RANGE_UNSUPPORTED:  return "range_unsupported";
    case SYP_ERR_CONTENT_CHANGED:    return "content_changed";
    case SYP_ERR_NOT_IMPLEMENTED:    return "not_implemented";
    default:                         return "unknown";
    }
}

const char* syp_version(void) { return "0.1.0"; }

void syp_set_log_callback(syp_log_fn fn, void* ctx, syp_log_level max_level) {
    syp::dl::set_log_callback(fn, ctx, max_level);
}

syp_status syp_set_http_backend(const syp_http_backend* backend) {
    return syp::dl::set_http_backend(backend);
}

syp_status syp_source_open(syp_source**               out,
                           const char*                url,
                           const syp_headers*         headers,
                           const syp_config*          cfg,
                           const syp_source_callbacks* cb) {
    if (out == nullptr || url == nullptr || url[0] == '\0' || cfg == nullptr) {
        return SYP_ERR_INVALID_ARG;
    }
    *out = nullptr;
    syp_status cerr = SYP_OK;
    syp_config copied = copy_config(cfg, &cerr);
    if (cerr != SYP_OK) return cerr;

    const syp_http_backend* be = current_http_backend();
    if (be == nullptr) return SYP_ERR_INVALID_ARG;

    auto impl = SourceBridge::open(std::string(url), headers, copied, cb, be,
                                   system_clock());
    if (!impl) return impl.error();

    auto* s = new (std::nothrow) syp_source();
    if (s == nullptr) return SYP_ERR_OOM;
    s->impl = std::move(*impl);
    *out = s;
    return SYP_OK;
}

void syp_source_close(syp_source* s) {
    if (s == nullptr) return;
    if (s->impl) s->impl->close();
    delete s;
}

int32_t syp_source_read(syp_source* s, uint8_t* buf, int32_t size) {
    if (s == nullptr || s->impl == nullptr) return SYP_ERR_INVALID_ARG;
    return s->impl->read(buf, size);
}

int64_t syp_source_seek(syp_source* s, int64_t offset, int32_t whence) {
    if (s == nullptr || s->impl == nullptr) return SYP_ERR_INVALID_ARG;
    return s->impl->seek(offset, whence);
}

int64_t syp_source_length(syp_source* s) {
    if (s == nullptr || s->impl == nullptr) return -1;
    return s->impl->length();
}

void syp_source_interrupt(syp_source* s) {
    if (s == nullptr || s->impl == nullptr) return;
    s->impl->interrupt();
}

void syp_source_resume(syp_source* s) {
    if (s == nullptr || s->impl == nullptr) return;
    s->impl->resume();
}

void syp_source_update_playback(syp_source* s, const syp_playback_state* st) {
    if (s == nullptr || s->impl == nullptr) return;
    s->impl->update_playback(st);
}

int32_t syp_source_cached_ranges(syp_source* s, syp_range* out, int32_t max) {
    if (s == nullptr || s->impl == nullptr) return 0;
    return s->impl->cached_ranges(out, max);
}

void syp_source_get_stats(syp_source* s, syp_source_stats* out) {
    if (s == nullptr || s->impl == nullptr || out == nullptr) return;
    s->impl->get_stats(out);
}

int64_t syp_cache_size(const char* cache_dir) {
    return syp::dl::cache_dir_size(cache_dir);
}

syp_status syp_cache_evict(const char* cache_dir, int64_t target_bytes) {
    return syp::dl::cache_dir_evict(cache_dir, target_bytes);
}

syp_status syp_cache_clear(const char* cache_dir) {
    return syp::dl::cache_dir_clear(cache_dir);
}

syp_status syp_cache_remove(const char* cache_dir, const char* url) {
    return syp::dl::cache_dir_remove(cache_dir, url);
}

}  // extern "C"
