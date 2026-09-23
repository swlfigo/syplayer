// loopback_server.cpp — POSIX 回环 HTTP/1.1：临时端口、并发连接、故障注入
#include "loopback_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace syp::dl::test {
namespace {

constexpr int kListenBacklog = 64;
constexpr size_t kMaxHeaderBytes = 64 * 1024;

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

int set_nosigpipe(int fd) {
    int yes = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
}

bool write_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::send(fd, p + off, n - off, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

bool write_str(int fd, const std::string& s) {
    return write_all(fd, s.data(), s.size());
}

// 路由未命中 → 404，无 body。只在 routes 非空这条路径上
// 用到，不影响既有的 error_body/status_code 那条响应路径。
bool write_simple_status(int fd, int32_t status);

std::string fmt_i64(int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%" PRId64, v);
    return std::string(buf);
}

const char* reason_phrase(int32_t status) {
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 302: return "Found";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "X";
    }
}

bool write_simple_status(int fd, int32_t status) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << reason_phrase(status) << "\r\n"
        << "Content-Length: 0\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    return write_str(fd, oss.str());
}

bool ascii_ieq_prefix(std::string_view line, std::string_view key) {
    if (line.size() < key.size()) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(line[i]);
        unsigned char b = static_cast<unsigned char>(key[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool parse_i64(std::string_view s, int64_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        const uint64_t d = static_cast<uint64_t>(ch - '0');
        if (v > (std::numeric_limits<uint64_t>::max() - d) / 10u) return false;
        v = v * 10u + d;
    }
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    out = static_cast<int64_t>(v);
    return true;
}

struct ParsedReq {
    std::string path;
    // 请求行里的路径，含 query（未做 '?' 截断）。路由匹配
    // 用这个字段——HLS 分片 URL 常带 query，断言方要求完全一样的字符串，
    // 模糊匹配会把「请求了 A」和「请求了 A?x=1」混成一件事。
    std::string raw_path;
    bool has_range = false;
    int64_t range_start = 0;
    int64_t range_end   = -1;  // inclusive; -1 = open
    bool accepts_gzip = false;  // Accept-Encoding 头里出现 gzip
};

// gzip 封装（RFC 1952）+ deflate stored 块（RFC 1951 §3.2.4）：不压缩，只为让测试
// 服务端能发出"合法 gzip、长度与原文不同"的响应体。
uint32_t crc32_of(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::vector<uint8_t> gzip_stored(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    size_t off = 0;
    do {
        const size_t n    = std::min<size_t>(data.size() - off, 65535);
        const bool   last = off + n == data.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<uint8_t>(n & 0xff));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(~n & 0xff));
        out.push_back(static_cast<uint8_t>((~n >> 8) & 0xff));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(off),
                   data.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
    } while (off < data.size());
    const uint32_t crc  = crc32_of(data);
    const auto     size = static_cast<uint32_t>(data.size());
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(crc >> s));
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(size >> s));
    return out;
}

bool parse_request(const std::string& raw, ParsedReq& out) {
    const size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;
    const std::string_view reqline(raw.data(), line_end);
    // GET /path HTTP/1.1
    const size_t sp1 = reqline.find(' ');
    if (sp1 == std::string_view::npos) return false;
    const size_t sp2 = reqline.find(' ', sp1 + 1);
    std::string_view full_path = (sp2 == std::string_view::npos)
                                ? reqline.substr(sp1 + 1)
                                : reqline.substr(sp1 + 1, sp2 - sp1 - 1);
    out.raw_path.assign(full_path.data(), full_path.size());
    std::string_view path = full_path;
    const size_t q = path.find('?');
    if (q != std::string_view::npos) path = path.substr(0, q);
    out.path.assign(path.data(), path.size());

    size_t pos = line_end + 2;
    while (pos + 1 < raw.size()) {
        const size_t nl = raw.find("\r\n", pos);
        if (nl == std::string::npos) break;
        if (nl == pos) break;  // empty line
        const std::string_view line(raw.data() + pos, nl - pos);
        pos = nl + 2;
        if (ascii_ieq_prefix(line, "accept-encoding:")) {
            out.accepts_gzip = line.find("gzip") != std::string_view::npos;
            continue;
        }
        if (!ascii_ieq_prefix(line, "range:")) continue;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view val = trim(line.substr(colon + 1));
        if (!ascii_ieq_prefix(val, "bytes=")) continue;
        val.remove_prefix(6);
        const size_t comma = val.find(',');
        if (comma != std::string_view::npos) val = val.substr(0, comma);
        val = trim(val);
        const size_t dash = val.find('-');
        if (dash == std::string_view::npos) continue;
        int64_t a = 0;
        if (!parse_i64(trim(val.substr(0, dash)), a)) continue;
        std::string_view rest = trim(val.substr(dash + 1));
        out.has_range = true;
        out.range_start = a;
        if (rest.empty()) {
            out.range_end = -1;
        } else {
            int64_t b = 0;
            if (!parse_i64(rest, b)) {
                out.has_range = false;
                continue;
            }
            out.range_end = b;
        }
    }
    return true;
}

// 读一个请求的头部（到空行为止）放进 out。carry 进来时是上次多读的字节、
// 出去时是这次空行之后多读的字节（keep-alive 下属于下一个请求；
// 非保活连接读完一个请求就关，carry 被丢弃，线上行为与旧实现相同）。
bool read_headers(int fd, std::string& carry, std::string& out) {
    out = std::move(carry);
    carry.clear();
    char buf[1024];
    for (;;) {
        const size_t eoh = out.find("\r\n\r\n");
        if (eoh != std::string::npos) {
            carry = out.substr(eoh + 4);
            out.resize(eoh + 4);
            return true;
        }
        if (out.size() >= kMaxHeaderBytes) return false;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        out.append(buf, static_cast<size_t>(n));
    }
}

}  // namespace

LoopbackServer::LoopbackServer(LoopbackConfig cfg) : cfg_(std::move(cfg)) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return;
    }
    wake_rd_ = fds[0];
    wake_wr_ = fds[1];
    fcntl(wake_rd_, F_SETFL, O_NONBLOCK);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        close_fd(wake_rd_);
        close_fd(wake_wr_);
        return;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_nosigpipe(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || ::listen(listen_fd_, kListenBacklog) != 0) {
        close_fd(listen_fd_);
        close_fd(wake_rd_);
        close_fd(wake_wr_);
        return;
    }
    sockaddr_in got{};
    socklen_t glen = sizeof(got);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&got), &glen) != 0) {
        close_fd(listen_fd_);
        close_fd(wake_rd_);
        close_fd(wake_wr_);
        return;
    }
    port_ = ntohs(got.sin_port);
    acceptor_ = std::thread([this] { accept_loop(); });
}

LoopbackServer::~LoopbackServer() {
    stop_.store(true, std::memory_order_release);
    if (wake_wr_ >= 0) {
        char x = 'x';
        ::write(wake_wr_, &x, 1);
    }
    if (acceptor_.joinable()) acceptor_.join();

    shutdown_all_clients();

    {
        std::lock_guard<std::mutex> g(threads_mu_);
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

    close_fd(listen_fd_);
    close_fd(wake_rd_);
    close_fd(wake_wr_);
}

uint16_t LoopbackServer::port() const noexcept { return port_; }

std::string LoopbackServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(static_cast<unsigned>(port_));
}

std::string LoopbackServer::url(std::string_view path) const {
    std::string u = base_url();
    if (path.empty() || path.front() != '/') u.push_back('/');
    u.append(path.data(), path.size());
    return u;
}

void LoopbackServer::set_config(LoopbackConfig cfg) {
    std::lock_guard<std::mutex> g(mu_);
    cfg_ = std::move(cfg);
}

LoopbackConfig LoopbackServer::config() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_;
}

int64_t LoopbackServer::total_requests() const noexcept {
    return total_requests_.load(std::memory_order_acquire);
}

int64_t LoopbackServer::total_bytes_sent() const noexcept {
    return total_bytes_sent_.load(std::memory_order_acquire);
}

int64_t LoopbackServer::server_capped_count() const noexcept {
    return server_capped_count_.load(std::memory_order_acquire);
}

int64_t LoopbackServer::early_close_count() const noexcept {
    return early_close_count_.load(std::memory_order_acquire);
}

int64_t LoopbackServer::accepted_connections() const noexcept {
    return accepted_connections_.load(std::memory_order_acquire);
}

int32_t LoopbackServer::peak_concurrent_requests() const noexcept {
    return peak_inflight_requests_.load(std::memory_order_acquire);
}

std::vector<LoopbackRequestRecord> LoopbackServer::requests_snapshot() const {
    std::lock_guard<std::mutex> g(requests_mu_);
    return requests_;
}

void LoopbackServer::record_request(int64_t start, int64_t end, int64_t bytes_sent,
                                    bool server_capped, bool early_close,
                                    int32_t concurrent_at_start, std::string path) {
    total_requests_.fetch_add(1, std::memory_order_acq_rel);
    total_bytes_sent_.fetch_add(bytes_sent, std::memory_order_acq_rel);
    if (server_capped) server_capped_count_.fetch_add(1, std::memory_order_acq_rel);
    if (early_close) early_close_count_.fetch_add(1, std::memory_order_acq_rel);
    // 每路径计数，mu_ 保护（与 set_route()/requests_for()
    // 同一把锁）。
    {
        std::lock_guard<std::mutex> g(mu_);
        per_path_counts_[path] += 1;
    }
    std::lock_guard<std::mutex> g(requests_mu_);
    LoopbackRequestRecord rec;
    rec.start = start;
    rec.end = end;
    rec.bytes_sent = bytes_sent;
    rec.server_capped = server_capped;
    rec.early_close = early_close;
    rec.concurrent_at_start = concurrent_at_start;
    rec.path = std::move(path);
    requests_.push_back(rec);
}

void LoopbackServer::set_route(std::string_view path, std::vector<uint8_t> body,
                               std::string_view content_type) {
    std::lock_guard<std::mutex> g(mu_);
    cfg_.routes[std::string(path)]              = std::move(body);
    cfg_.route_content_types[std::string(path)] = std::string(content_type);
}

void LoopbackServer::remove_route(std::string_view path) {
    std::lock_guard<std::mutex> g(mu_);
    cfg_.routes.erase(std::string(path));
    cfg_.route_content_types.erase(std::string(path));
}

void LoopbackServer::hold_route(std::string_view path) {
    std::lock_guard<std::mutex> g(mu_);
    held_routes_.insert(std::string(path));
}

void LoopbackServer::release_route(std::string_view path) {
    std::lock_guard<std::mutex> g(mu_);
    held_routes_.erase(std::string(path));
}

int64_t LoopbackServer::requests_for(std::string_view path) const {
    std::lock_guard<std::mutex> g(mu_);
    const auto it = per_path_counts_.find(std::string(path));
    return it == per_path_counts_.end() ? 0 : it->second;
}

int64_t LoopbackServer::requests_received_for(std::string_view path) const {
    std::lock_guard<std::mutex> g(mu_);
    const auto it = per_path_received_.find(std::string(path));
    return it == per_path_received_.end() ? 0 : it->second;
}

void LoopbackServer::sleep_interruptible(int32_t ms) {
    if (ms <= 0) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto slice = std::min(left, std::chrono::milliseconds(20));
        std::this_thread::sleep_for(slice);
    }
}

void LoopbackServer::add_client_fd(int fd) {
    std::lock_guard<std::mutex> g(clients_mu_);
    client_fds_.push_back(fd);
}

void LoopbackServer::remove_client_fd(int fd) {
    std::lock_guard<std::mutex> g(clients_mu_);
    client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), fd),
                      client_fds_.end());
}

void LoopbackServer::shutdown_all_clients() {
    std::lock_guard<std::mutex> g(clients_mu_);
    for (int fd : client_fds_) {
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    }
}

void LoopbackServer::accept_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        pollfd pfds[2]{};
        pfds[0].fd = listen_fd_;
        pfds[0].events = POLLIN;
        pfds[1].fd = wake_rd_;
        pfds[1].events = POLLIN;
        const int pr = ::poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (stop_.load(std::memory_order_acquire)) break;
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) break;
        if (!(pfds[0].revents & POLLIN)) continue;

        const int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (stop_.load(std::memory_order_acquire)) break;
            continue;
        }
        accepted_connections_.fetch_add(1, std::memory_order_acq_rel);
        set_nosigpipe(cfd);
        int yes = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        add_client_fd(cfd);

        std::thread th([this, cfd] {
            handle_conn(cfd);
            remove_client_fd(cfd);
            ::close(cfd);
        });
        {
            std::lock_guard<std::mutex> g(threads_mu_);
            if (stop_.load(std::memory_order_acquire)) {
                // 析构已经开始：自己 join，避免 dtor 漏掉这条刚建的线程。
                th.join();
            } else {
                conn_threads_.push_back(std::move(th));
            }
        }
    }
}

void LoopbackServer::handle_conn(int fd) {
    // 非 keep-alive：serve_one 恒返回 false，只服务一个请求（旧行为）。
    std::string carry;
    while (serve_one(fd, carry)) {
        if (stop_.load(std::memory_order_acquire)) return;
    }
}

bool LoopbackServer::serve_one(int fd, std::string& carry) {
    std::string raw;
    if (!read_headers(fd, carry, raw)) return false;
    if (stop_.load(std::memory_order_acquire)) return false;

    ParsedReq preq;
    if (!parse_request(raw, preq)) return false;

    // 收到即计数——必须在 delay_ms/hang/redirect/路由**之前**。
    // requests_for() 那个计数跑在供完体之后，对 hang 住的请求永远数不到；
    // 要断言"卡住之后对方确实又发过一次请求"只能靠这一个。见头文件里
    // requests_received_for() 上方的注释。
    {
        std::lock_guard<std::mutex> g(mu_);
        per_path_received_[preq.raw_path] += 1;
    }

    // hold_route：扣住直到放行或析构。
    for (;;) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (held_routes_.count(preq.raw_path) == 0) break;
        }
        if (stop_.load(std::memory_order_acquire)) return false;
        sleep_interruptible(10);
    }

    const LoopbackConfig cfg = config();
    if (cfg.delay_ms > 0) sleep_interruptible(cfg.delay_ms);
    if (stop_.load(std::memory_order_acquire)) return false;

    if (cfg.hang) {
        while (!stop_.load(std::memory_order_acquire)) {
            sleep_interruptible(50);
        }
        return false;
    }

    if (!cfg.redirect_to.empty()) {
        std::string loc = cfg.redirect_to;
        std::ostringstream oss;
        oss << "HTTP/1.1 302 Found\r\n"
            << "Location: " << loc << "\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n"
            << "\r\n";
        write_str(fd, oss.str());
        return false;
    }

    // routes 非空 = 路由模式，优先于 body_file/合成公式。
    // routes 为空时下面这段整体跳过，既有行为一个字节不变。
    // 注意：mu_ 只护住对 cfg_.routes 的读取——record_request() 自己也要拿
    // mu_（更新 per_path_counts_），且 write_simple_status() 是阻塞 IO，
    // 两者都不能在还持有 mu_ 的时候做，否则前者死锁、后者违反"持锁不做
    // 阻塞 IO"的纪律。所以先在小括号里拿完数据就放锁，未命中的 404 分支
    // 放到锁外面处理。
    bool                  routed = false;
    bool                  routed_miss = false;
    std::vector<uint8_t>  routed_body;
    std::string           routed_ctype;
    const std::string&    req_path = preq.raw_path;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!cfg_.routes.empty()) {
            routed = true;
            const auto it = cfg_.routes.find(req_path);
            if (it == cfg_.routes.end()) {
                routed_miss = true;
            } else {
                routed_body = it->second;
                const auto ct = cfg_.route_content_types.find(req_path);
                routed_ctype  = (ct == cfg_.route_content_types.end())
                                    ? std::string("application/octet-stream") : ct->second;
            }
        }
    }
    if (routed_miss) {
        // 未命中 → 404。HLS 的分片 404 是需要覆盖的真实场景。
        write_simple_status(fd, 404);
        record_request(0, 0, 0, false, false, 0, req_path);
        return false;
    }

    // gzip 模式：整份编码后发出（Range 只影响状态码与 Content-Range，不切片）。
    if ((cfg.gzip_always || (cfg.gzip_when_accepted && preq.accepts_gzip)) && cfg.body_file.empty() &&
        cfg.status_code == 0) {
        std::vector<uint8_t> plain = routed_body;
        if (!routed) {
            plain.resize(static_cast<size_t>(std::max<int64_t>(cfg.resource_length, 0)));
            for (size_t i = 0; i < plain.size(); ++i) plain[i] = synthetic_byte(static_cast<int64_t>(i));
        }
        const std::vector<uint8_t> enc = gzip_stored(plain);
        std::ostringstream hdr;
        // 带 Range 的请求回 206 + 描述**编码表示**的 Content-Range（整份），与部分 CDN 一致。
        hdr << (preq.has_range ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n")
            << "Content-Type: " << (routed ? routed_ctype : std::string("application/octet-stream")) << "\r\n"
            << "Content-Encoding: gzip\r\n"
            << "Content-Length: " << enc.size() << "\r\n";
        if (preq.has_range) hdr << "Content-Range: bytes 0-" << (enc.size() - 1) << "/" << enc.size() << "\r\n";
        hdr << "Connection: close\r\n\r\n";
        if (!write_str(fd, hdr.str())) return false;
        if (!write_all(fd, reinterpret_cast<const char*>(enc.data()), enc.size())) return false;
        record_request(0, static_cast<int64_t>(enc.size()), static_cast<int64_t>(enc.size()), false, false, 1,
                       req_path);
        return false;
    }

    // body_file 非空时长度取自文件，配置里的 resource_length 被忽略。
    // 路由模式（routed）下 body_file/synthetic_byte 整体不生效——routes 优先。
    int64_t L = routed ? static_cast<int64_t>(routed_body.size())
                       : std::max<int64_t>(cfg.resource_length, 0);
    std::FILE* body_fp = nullptr;
    if (!routed && !cfg.body_file.empty()) {
        body_fp = std::fopen(cfg.body_file.c_str(), "rb");
        if (body_fp == nullptr) {
            const std::string msg = "HTTP/1.1 500 Internal Server Error\r\n"
                                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
            write_str(fd, msg);
            return false;
        }
        const int seek_rc = std::fseek(body_fp, 0, SEEK_END);
        const long len = std::ftell(body_fp);
        if (seek_rc != 0 || len < 0) {
            std::fclose(body_fp);  // FileCloser 还没构造，这里手动关。
            const std::string msg = "HTTP/1.1 500 Internal Server Error\r\n"
                                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
            write_str(fd, msg);
            return false;
        }
        L = static_cast<int64_t>(len);
    }
    // 提前返回的路径也要关掉它。
    struct FileCloser {
        std::FILE* f;
        ~FileCloser() { if (f != nullptr) std::fclose(f); }
    } body_closer{body_fp};

    int64_t body_start = 0;
    int64_t body_end   = L;  // exclusive
    int32_t status     = 200;

    if (cfg.support_range && preq.has_range) {
        body_start = preq.range_start < 0 ? 0 : preq.range_start;
        if (preq.range_end < 0) body_end = L;
        else {
            if (preq.range_end >= std::numeric_limits<int64_t>::max()) body_end = L;
            else body_end = preq.range_end + 1;
        }
        if (body_start < 0) body_start = 0;
        if (body_end > L) body_end = L;
        if (body_start > L) body_start = L;
        if (body_end < body_start) body_end = body_start;
        status = 206;
        if (body_start >= L && L >= 0) status = 416;
    }

    if (cfg.status_code != 0) status = cfg.status_code;

    if (status != 200 && status != 206) {
        const std::string& body = cfg.error_body;
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << " " << reason_phrase(status) << "\r\n"
            << "Content-Type: text/html\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n";
        if (!write_str(fd, oss.str())) return false;
        if (!body.empty()) write_str(fd, body);
        return false;
    }

    const int64_t body_len = body_end - body_start;

    // C2 修复：从这里开始才算「真的在供体」，无论下面走哪条 return（写失败 /
    // stop_ 信号 / 正常发完），析构时都要把这次请求记一笔——sent 到那时是多少
    // 就是多少，不需要在每个 return 前手动补一句。同时统计并发峰值：
    // E 场景要靠它证「降级后只剩一条连接在跑」，不是靠事后字节对不对推。
    int64_t sent = 0;
    class RequestGuard {
    public:
        RequestGuard(LoopbackServer* self, int64_t start, int64_t end, const int64_t* sent_ptr,
                    int64_t close_after_bytes, std::string path)
            : self_(self), start_(start), end_(end), sent_ptr_(sent_ptr),
              close_after_bytes_(close_after_bytes), path_(std::move(path)) {
            concurrent_at_start_ =
                self_->inflight_requests_.fetch_add(1, std::memory_order_acq_rel) + 1;
            int32_t peak = self_->peak_inflight_requests_.load(std::memory_order_acquire);
            while (concurrent_at_start_ > peak
                   && !self_->peak_inflight_requests_.compare_exchange_weak(
                          peak, concurrent_at_start_, std::memory_order_acq_rel)) {
            }
        }
        ~RequestGuard() {
            self_->inflight_requests_.fetch_sub(1, std::memory_order_acq_rel);
            const int64_t got = *sent_ptr_;
            const bool short_of_promise = got < (end_ - start_);
            // N5 修复：区分「服务端按 close_after_bytes 主动掐断」与「其它任何
            // 提前退出」（写失败 / stop_ 信号 / 客户端主动 cancel() 断开连接）。
            // 前者精确命中配置值本身；混在一起会污染任何靠"服务端真的截断过
            // 几次"做确定性构造的断言（场景 F）——取消也会让 bytes_sent 小于
            // 承诺值，但那不是 close_after_bytes 干的。
            const bool server_capped =
                short_of_promise && close_after_bytes_ >= 0 && got == close_after_bytes_;
            const bool early_close = short_of_promise && !server_capped;
            self_->record_request(start_, end_, got, server_capped, early_close,
                                  concurrent_at_start_, path_);
        }
        RequestGuard(const RequestGuard&) = delete;
        RequestGuard& operator=(const RequestGuard&) = delete;

    private:
        LoopbackServer* self_;
        int64_t start_;
        int64_t end_;
        const int64_t* sent_ptr_;
        int64_t close_after_bytes_;
        std::string path_;
        int32_t concurrent_at_start_ = 0;
    } guard(this, body_start, body_end, &sent, cfg.close_after_bytes, req_path);

    std::ostringstream hdr;
    hdr << "HTTP/1.1 " << status << " " << reason_phrase(status) << "\r\n";
    hdr << "Content-Type: "
        << (routed ? routed_ctype : std::string("application/octet-stream")) << "\r\n";
    hdr << "Content-Length: " << fmt_i64(body_len) << "\r\n";
    if (!cfg.content_range_override.empty()) {
        hdr << "Content-Range: " << cfg.content_range_override << "\r\n";
    } else if (status == 206) {
        if (body_len > 0) {
            hdr << "Content-Range: bytes " << fmt_i64(body_start) << "-"
                << fmt_i64(body_end - 1) << "/" << fmt_i64(L) << "\r\n";
        } else {
            hdr << "Content-Range: bytes */" << fmt_i64(L) << "\r\n";
        }
    }
    if (!cfg.etag.empty()) {
        hdr << "ETag: " << cfg.etag << "\r\n";
    }
    // 保活只给路由/合成公式两条供体分支，且 body 必须能按 Content-Length
    // 完整写出（close_after_bytes 截断时声明的长度与实际不符，只能关连接）。
    const bool keep = cfg.keep_alive && body_fp == nullptr
                      && (cfg.close_after_bytes < 0 || cfg.close_after_bytes >= body_len);
    hdr << (keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
    if (!write_str(fd, hdr.str())) return false;
    if (stop_.load(std::memory_order_acquire)) return false;

    int64_t remaining_limit = body_len;
    if (cfg.close_after_bytes >= 0 && cfg.close_after_bytes < remaining_limit) {
        remaining_limit = cfg.close_after_bytes;
    }

    // 前 slow_first_requests 个供体请求改用 slow_chunk_* 节流（替换，不叠加
    // 全局的 body_chunk_*）。写循环本身不分叉：只换这两个参数。
    const int64_t donor_no = donor_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const bool    slow_this = cfg.slow_first_requests > 0 && cfg.slow_chunk_delay_ms > 0
                              && donor_no <= static_cast<int64_t>(cfg.slow_first_requests);
    const int32_t chunk_cfg = slow_this ? cfg.slow_chunk_bytes : cfg.body_chunk_bytes;
    const int32_t chunk_delay_ms = slow_this ? cfg.slow_chunk_delay_ms : cfg.body_chunk_delay_ms;
    const int32_t chunk = (chunk_cfg > 0) ? chunk_cfg : 4096;
    std::vector<uint8_t> buf(static_cast<size_t>(chunk));

    while (sent < remaining_limit && !stop_.load(std::memory_order_acquire)) {
        if (cfg.pause_after_bytes >= 0 && sent == cfg.pause_after_bytes
            && cfg.pause_ms > 0) {
            sleep_interruptible(cfg.pause_ms);
            if (stop_.load(std::memory_order_acquire)) return false;
        }
        int64_t n = remaining_limit - sent;
        if (n > chunk) n = chunk;
        const int64_t off = body_start + sent;
        if (body_fp != nullptr) {
            if (std::fseek(body_fp, static_cast<long>(off), SEEK_SET) != 0) return false;
            const size_t got = std::fread(buf.data(), 1, static_cast<size_t>(n), body_fp);
            if (got != static_cast<size_t>(n)) return false;
        } else if (routed) {
            for (int64_t i = 0; i < n; ++i) {
                buf[static_cast<size_t>(i)] = routed_body[static_cast<size_t>(off + i)];
            }
        } else {
            for (int64_t i = 0; i < n; ++i) {
                buf[static_cast<size_t>(i)] = synthetic_byte(off + i);
            }
        }
        if (!write_all(fd, reinterpret_cast<const char*>(buf.data()),
                       static_cast<size_t>(n))) {
            return false;
        }
        sent += n;
        if (chunk_delay_ms > 0 && sent < remaining_limit) {
            sleep_interruptible(chunk_delay_ms);
        }
    }
    return keep && sent == body_len;
}

}  // namespace syp::dl::test
