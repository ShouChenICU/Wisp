#include "wisp/port/sim/sim_http_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sha1.hpp"
#include "wisp/base/base64.hpp"

namespace wisp::port::sim {

namespace {

// An idle keep-alive connection (or one trickling in a request head) is
// dropped after this long, so idle browser sockets do not hold the
// connection slots forever.
constexpr int kIdleTimeoutMs = 10000;
// After an error response the request's remaining bytes are read and
// discarded for at most this long / this much before closing: closing
// with unread data makes the kernel send a reset, which can destroy the
// error response before the client reads it.
constexpr int kLingerMs = 1000;
constexpr size_t kLingerBytes = 64 * 1024;

constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

constexpr uint16_t kCloseProtocolError = 1002;
constexpr uint16_t kCloseUnsupportedData = 1003;
constexpr uint16_t kCloseTooBig = 1009;

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

// MSG_NOSIGNAL: a peer that went away yields EPIPE instead of SIGPIPE
// killing the simulator.
bool send_all(int fd, const void* data, size_t size) {
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int fd, std::string_view s) { return send_all(fd, s.data(), s.size()); }

// Waits up to `timeout_ms` (-1: forever) for data and appends one read's
// worth to `buf`. False on timeout, EOF, error or a shut-down socket.
bool recv_some(int fd, std::string& buf, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    int r;
    do {
        r = ::poll(&p, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) return false;
    char tmp[4096];
    ssize_t n;
    do {
        n = ::recv(fd, tmp, sizeof tmp, 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    buf.append(tmp, static_cast<size_t>(n));
    return true;
}

// Half-closes and drains what the peer still sends (bounded), so the
// final close does not reset the connection under an unread response.
void linger_close(int fd) {
    ::shutdown(fd, SHUT_WR);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLingerMs);
    size_t discarded = 0;
    std::string sink;
    while (discarded < kLingerBytes) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;
        sink.clear();
        if (!recv_some(fd, sink, static_cast<int>(left))) break;
        discarded += sink.size();
    }
}

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Whether the comma-separated header value `list` contains `token`
// (case-insensitive), e.g. Connection: keep-alive, Upgrade.
bool has_token(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        const size_t comma = list.find(',');
        if (lower(trim(list.substr(0, comma))) == token) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

const char* reason_phrase(int status) {
    switch (status) {
        case 100:
            return "Continue";
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 409:
            return "Conflict";
        case 411:
            return "Length Required";
        case 413:
            return "Content Too Large";
        case 426:
            return "Upgrade Required";
        case 429:
            return "Too Many Requests";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        default:
            return "";
    }
}

net::Method parse_method(std::string_view m) {
    if (m == "GET") return net::Method::Get;
    if (m == "POST") return net::Method::Post;
    if (m == "PUT") return net::Method::Put;
    if (m == "DELETE") return net::Method::Delete;
    return net::Method::Other;
}

bool has_crlf(std::string_view s) { return s.find_first_of("\r\n") != std::string_view::npos; }

bool write_response(int fd, const net::HttpResponse& res, bool keep_alive) {
    // 1xx, 204 and 304 never carry a body (RFC 9110 section 6.4.1).
    const bool bodyless =
        (res.status >= 100 && res.status < 200) || res.status == 204 || res.status == 304;
    std::span<const uint8_t> body = res.static_body;
    if (body.empty()) {
        body = {reinterpret_cast<const uint8_t*>(res.body.data()), res.body.size()};
    }

    std::string head =
        "HTTP/1.1 " + std::to_string(res.status) + " " + reason_phrase(res.status) + "\r\n";
    if (!bodyless) {
        if (!res.content_type.empty() && !has_crlf(res.content_type)) {
            head += "Content-Type: " + res.content_type + "\r\n";
        }
        head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    for (const auto& [name, value] : res.headers) {
        // A stray CR/LF would let a value inject headers.
        if (name.empty() || has_crlf(name) || has_crlf(value)) continue;
        head += name + ": " + value + "\r\n";
    }
    head += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    if (!send_all(fd, head)) return false;
    // static_body goes straight from its (mapped) storage to the socket.
    if (!bodyless && !body.empty()) return send_all(fd, body.data(), body.size());
    return true;
}

std::string ws_frame_header(uint8_t opcode, size_t size) {
    std::string h;
    h.push_back(static_cast<char>(0x80 | opcode));
    if (size < 126) {
        h.push_back(static_cast<char>(size));
    } else if (size <= 0xffff) {
        h.push_back(static_cast<char>(126));
        h.push_back(static_cast<char>(size >> 8));
        h.push_back(static_cast<char>(size & 0xff));
    } else {
        h.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            h.push_back(static_cast<char>((static_cast<uint64_t>(size) >> shift) & 0xff));
        }
    }
    return h;
}

bool send_ws_frame(int fd, uint8_t opcode, std::string_view payload) {
    return send_all(fd, ws_frame_header(opcode, payload.size())) &&
           (payload.empty() || send_all(fd, payload));
}

bool send_ws_close(int fd, uint16_t code) {
    const char payload[2] = {static_cast<char>(code >> 8), static_cast<char>(code & 0xff)};
    return send_ws_frame(fd, kOpClose, std::string_view(payload, 2));
}

struct WsFrame {
    bool fin = false;
    uint8_t opcode = 0;
    std::string payload;
};

// Takes one complete frame off the front of `in`. Returns 0 with `out`
// filled, -1 when more bytes are needed, or a close code for a frame
// that must not be accepted. Size limits are checked from the header,
// before the payload is buffered: `continuation_room` is what is left of
// the message limit for a continuation frame.
int take_ws_frame(std::string& in, WsFrame& out, size_t continuation_room) {
    if (in.size() < 2) return -1;
    const auto b0 = static_cast<uint8_t>(in[0]);
    const auto b1 = static_cast<uint8_t>(in[1]);
    // No extensions are negotiated, so RSV bits must be clear; clients
    // must mask every frame (RFC 6455 section 5.1).
    if ((b0 & 0x70) != 0 || (b1 & 0x80) == 0) return kCloseProtocolError;
    const uint8_t opcode = b0 & 0x0f;
    const bool fin = (b0 & 0x80) != 0;
    uint64_t size = b1 & 0x7f;
    size_t pos = 2;
    if (size == 126 || size == 127) {
        const size_t width = size == 126 ? 2 : 8;
        if (in.size() < pos + width) return -1;
        size = 0;
        for (size_t i = 0; i < width; ++i) size = (size << 8) | static_cast<uint8_t>(in[pos + i]);
        pos += width;
    }
    if (opcode >= 0x8 && (!fin || size > 125)) return kCloseProtocolError;
    const size_t limit =
        opcode == kOpContinuation ? continuation_room : SimHttpServer::kMaxWsMessageBytes;
    if (size > limit) return kCloseTooBig;
    if (in.size() < pos + 4 + size) return -1;
    const char* mask = in.data() + pos;
    pos += 4;
    out.fin = fin;
    out.opcode = opcode;
    out.payload.assign(in, pos, static_cast<size_t>(size));
    for (size_t i = 0; i < out.payload.size(); ++i) out.payload[i] ^= mask[i % 4];
    in.erase(0, pos + static_cast<size_t>(size));
    return 0;
}

// One accepted socket. For a WebSocket it is also the net::WsConnection
// handed to the application, which may keep it after the connection (or
// the whole server) is gone, so it owns everything send_text()/close()
// touch.
//
// Locking: mutex_ guards fd_, closing_ and queue_, and is never held
// across a blocking call or an application callback, so send_text() and
// close() never wait on the connection thread, whatever locks the
// application holds while calling them.
class Connection final : public net::WsConnection {
public:
    Connection(int id, int fd, int wake_rd, int wake_wr)
        : id_(id), fd_(fd), wake_rd_(wake_rd), wake_wr_(wake_wr) {}

    ~Connection() override {
        close_fd(fd_);
        close_fd(wake_rd_);
        close_fd(wake_wr_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    int id() const override { return id_; }

    bool send_text(std::string_view text) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_ || queue_.size() >= SimHttpServer::kWsQueueDepth) return false;
            queue_.emplace_back(text);
        }
        wake();
        return true;
    }

    // Flag and shut down the socket; the connection thread notices,
    // finishes and reports on_close itself.
    void close() override { abort(); }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        queue_.clear();
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    }

    // Connection-thread side. fd_ only ever changes in finish(), on that
    // same thread, so reading it there needs no lock.
    int fd() const { return fd_; }
    int wake_fd() const { return wake_rd_; }

    bool closing() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closing_;
    }

    // Idle: an HTTP connection between requests, nothing of the next one
    // read yet. Only idle connections may be evicted to admit a new one
    // (spec 6.3); a WebSocket or a connection mid-request never is.
    void set_idle(bool idle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idle && !idle_) idle_since_ = std::chrono::steady_clock::now();
        idle_ = idle;
    }

    // When idle (and not already closing), the time it became idle.
    std::optional<std::chrono::steady_clock::time_point> idle_since() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idle_ || closing_) return std::nullopt;
        return idle_since_;
    }

    // Shuts the socket down if the connection is still idle; checked and
    // done under one lock so a connection that just started reading a
    // request is left alone.
    bool evict_if_idle() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idle_ || closing_) return false;
        closing_ = true;
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
        return true;
    }

    std::deque<std::string> take_queue() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(queue_, {});
    }

    void drain_wake() {
        char buf[64];
        while (::read(wake_rd_, buf, sizeof buf) > 0) {
        }
    }

    // Marks the connection closed and releases the socket; send_text()
    // returns false from here on.
    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        queue_.clear();
        close_fd(fd_);
    }

private:
    void wake() {
        const char b = 1;
        // A full (non-blocking) pipe already holds a pending wake-up.
        [[maybe_unused]] const ssize_t n = ::write(wake_wr_, &b, 1);
    }

    const int id_;
    std::mutex mutex_;
    int fd_;
    bool closing_ = false;
    bool idle_ = false;
    std::chrono::steady_clock::time_point idle_since_{};
    std::deque<std::string> queue_;
    int wake_rd_;
    int wake_wr_;
};

}  // namespace

struct SimHttpServer::Impl {
    struct Slot {
        std::shared_ptr<Connection> conn;
        std::thread thread;
        std::atomic<bool> finished{false};
        bool evicted = false;  // guarded by slots_mutex
    };

    explicit Impl(std::string bind_addr) : bind(std::move(bind_addr)) {}

    void accept_loop();
    void reap_locked();
    // Makes room for one more connection when every slot is taken: evicts
    // the least recently used idle keep-alive connection. False when none
    // is idle.
    bool make_room_locked();
    void serve(const std::shared_ptr<Connection>& conn_ptr, const std::string& remote_ip);
    void serve_ws(const std::shared_ptr<Connection>& conn_ptr, const net::HttpRequest& req,
                  std::string leftover);
    void ws_loop(Connection& conn, std::string in);
    // Sends a close frame with `code` and closes once the peer hangs up.
    void ws_close(Connection& conn, uint16_t code);
    // Sends an error response and closes the connection gracefully.
    void reject(Connection& conn, int status, const char* code, const char* message,
                std::vector<std::pair<std::string, std::string>> headers = {});

    const std::string bind;

    std::mutex lifecycle;  // serialises start()/stop()
    std::atomic<bool> is_running{false};
    std::atomic<uint16_t> bound_port{0};
    // Written by start() before any thread exists and cleared by stop()
    // after all are joined, so connection threads read them unlocked.
    net::HttpHandler handler;
    net::WsHandlers ws;
    int listen_fd = -1;
    int wake_rd = -1;
    int wake_wr = -1;
    std::thread acceptor;

    std::mutex slots_mutex;
    std::list<std::unique_ptr<Slot>> slots;
    // Never reset, not even across restarts: a late on_close(id) can
    // never name a newer connection.
    int next_id = 1;
};

void SimHttpServer::Impl::reap_locked() {
    for (auto it = slots.begin(); it != slots.end();) {
        if ((*it)->finished.load()) {
            (*it)->thread.join();
            it = slots.erase(it);
        } else {
            ++it;
        }
    }
}

bool SimHttpServer::Impl::make_room_locked() {
    size_t live = 0;
    Slot* lru = nullptr;
    std::chrono::steady_clock::time_point lru_since{};
    for (auto& slot : slots) {
        if (slot->finished.load() || slot->evicted) continue;
        ++live;
        if (auto since = slot->conn->idle_since();
            since && (lru == nullptr || *since < lru_since)) {
            lru = slot.get();
            lru_since = *since;
        }
    }
    if (live < kMaxConnections) return true;
    // The evicted connection's thread finishes on its own and is reaped
    // later; until then it no longer counts toward the limit.
    if (lru == nullptr || !lru->conn->evict_if_idle()) return false;
    lru->evicted = true;
    return true;
}

void SimHttpServer::Impl::accept_loop() {
    for (;;) {
        pollfd p[2] = {{listen_fd, POLLIN, 0}, {wake_rd, POLLIN, 0}};
        const int r = ::poll(p, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (p[1].revents != 0) return;  // stop()
        if ((p[0].revents & POLLIN) == 0) continue;

        sockaddr_in peer{};
        socklen_t peer_len = sizeof peer;
        const int fd =
            ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                // Out of descriptors: back off instead of spinning on a
                // listen socket that stays readable; stop() still wakes us.
                pollfd w{wake_rd, POLLIN, 0};
                if (::poll(&w, 1, 100) > 0) return;
            }
            continue;
        }

        std::lock_guard<std::mutex> lock(slots_mutex);
        reap_locked();
        int pipe_fds[2] = {-1, -1};
        if (!make_room_locked() || ::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            ::close(fd);
            continue;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);

        auto slot = std::make_unique<Slot>();
        slot->conn = std::make_shared<Connection>(next_id++, fd, pipe_fds[0], pipe_fds[1]);
        Slot* s = slot.get();
        s->thread = std::thread([this, s, remote_ip = std::string(ip)] {
            serve(s->conn, remote_ip);
            s->conn->finish();
            s->finished.store(true);
        });
        slots.push_back(std::move(slot));
    }
}

void SimHttpServer::Impl::reject(Connection& conn, int status, const char* code,
                                 const char* message,
                                 std::vector<std::pair<std::string, std::string>> headers) {
    net::HttpResponse res = net::HttpResponse::error(status, code, message);
    res.headers = std::move(headers);
    if (write_response(conn.fd(), res, /*keep_alive=*/false)) linger_close(conn.fd());
    conn.finish();
}

void SimHttpServer::Impl::serve(const std::shared_ptr<Connection>& conn_ptr,
                                const std::string& remote_ip) {
    Connection& conn = *conn_ptr;
    const int fd = conn.fd();
    std::string buf;
    for (;;) {
        // Request line and headers, capped before they are complete so a
        // client cannot make us buffer without bound.
        size_t head_end;
        while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
            if (buf.size() > kMaxHeadBytes) {
                return reject(conn, 431, "headers_too_large", "request head exceeds 8192 bytes");
            }
            if (buf.empty()) conn.set_idle(true);
            const bool got = recv_some(fd, buf, kIdleTimeoutMs);
            conn.set_idle(false);
            if (!got) return conn.finish();
        }
        if (head_end + 4 > kMaxHeadBytes) {
            return reject(conn, 431, "headers_too_large", "request head exceeds 8192 bytes");
        }

        net::HttpRequest req;
        req.remote_ip = remote_ip;
        req.iface = net::Iface::Loopback;
        std::string_view head(buf.data(), head_end);
        size_t line_end = head.find("\r\n");
        const std::string_view request_line = head.substr(0, line_end);
        const size_t sp1 = request_line.find(' ');
        const size_t sp2 = request_line.rfind(' ');
        if (sp1 == std::string_view::npos || sp1 == sp2) {
            return reject(conn, 400, "bad_request", "malformed request line");
        }
        const std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string_view version = request_line.substr(sp2 + 1);
        if (target.empty() || target.front() != '/' ||
            (version != "HTTP/1.1" && version != "HTTP/1.0")) {
            return reject(conn, 400, "bad_request", "malformed request line");
        }
        req.method = parse_method(request_line.substr(0, sp1));
        const size_t qmark = target.find('?');
        req.path = std::string(target.substr(0, qmark));
        if (qmark != std::string_view::npos) req.query = std::string(target.substr(qmark + 1));

        bool bad_header = false;
        while (line_end != std::string_view::npos) {
            head.remove_prefix(line_end + 2);
            line_end = head.find("\r\n");
            const std::string_view line = head.substr(0, line_end);
            const size_t colon = line.find(':');
            // Obsolete line folding and whitespace before the colon are
            // both rejected (RFC 9112 section 5).
            if (colon == std::string_view::npos || colon == 0 || line.front() == ' ' ||
                line.front() == '\t' || line[colon - 1] == ' ' || line[colon - 1] == '\t') {
                bad_header = true;
                break;
            }
            const std::string name = lower(line.substr(0, colon));
            const std::string_view value = trim(line.substr(colon + 1));
            auto [it, inserted] = req.headers.try_emplace(name, value);
            if (!inserted) {
                it->second += name == "cookie" ? "; " : ", ";
                it->second += value;
            }
        }
        buf.erase(0, head_end + 4);
        if (bad_header) return reject(conn, 400, "bad_request", "malformed header line");

        const bool http11 = version == "HTTP/1.1";
        const std::string connection = req.header("connection");
        const bool keep_alive =
            http11 ? !has_token(connection, "close") : has_token(connection, "keep-alive");

        if (req.headers.count("transfer-encoding") != 0) {
            return reject(conn, 411, "length_required", "chunked request bodies are not supported");
        }
        size_t content_length = 0;
        if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
            const std::string& v = it->second;
            if (v.empty() || v.size() > 9 ||
                v.find_first_not_of("0123456789") != std::string::npos) {
                return reject(conn, 400, "bad_request", "invalid Content-Length");
            }
            content_length = std::stoul(v);
        }
        // Refused from the header alone: the body is never read.
        if (content_length > kMaxBodyBytes) {
            return reject(conn, 413, "body_too_large", "request body exceeds 8192 bytes");
        }
        if (content_length > 0) {
            if (http11 && has_token(req.header("expect"), "100-continue") &&
                buf.size() < content_length) {
                if (!send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n")) return conn.finish();
            }
            while (buf.size() < content_length) {
                if (!recv_some(fd, buf, kIdleTimeoutMs)) return conn.finish();
            }
            req.body = buf.substr(0, content_length);
            buf.erase(0, content_length);
        }

        if (req.path == kWsPath && has_token(req.header("upgrade"), "websocket")) {
            return serve_ws(conn_ptr, req, std::move(buf));
        }

        const net::HttpResponse res =
            handler ? handler(req) : net::HttpResponse::error(404, "not_found", "no handler");
        if (!write_response(fd, res, keep_alive) || !keep_alive) return conn.finish();
    }
}

void SimHttpServer::Impl::serve_ws(const std::shared_ptr<Connection>& conn_ptr,
                                   const net::HttpRequest& req, std::string leftover) {
    Connection& conn = *conn_ptr;
    const std::string key = req.header("sec-websocket-key");
    // The key is base64 of 16 random bytes (RFC 6455 section 4.1).
    if (req.method != net::Method::Get || !has_token(req.header("connection"), "upgrade") ||
        key.size() != 24) {
        return reject(conn, 400, "bad_request", "malformed WebSocket upgrade");
    }
    if (req.header("sec-websocket-version") != "13") {
        return reject(conn, 426, "upgrade_required", "only WebSocket version 13 is supported",
                      {{"Sec-WebSocket-Version", "13"}});
    }

    // on_open runs before the handshake completes so a refusal is still
    // an ordinary HTTP error; messages it queues go out right after the
    // 101.
    if (!ws.on_open || !ws.on_open(conn_ptr, req)) {
        return reject(conn, 403, "forbidden", "WebSocket upgrade refused");
    }

    const std::string accept_input = key + kWsGuid;
    const auto digest =
        sha1({reinterpret_cast<const uint8_t*>(accept_input.data()), accept_input.size()});
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        base64_encode(digest) + "\r\n\r\n";
    if (send_all(conn.fd(), response)) ws_loop(conn, std::move(leftover));
    // Every connection on_open accepted is reported closed exactly once,
    // after send_text() has started refusing.
    conn.finish();
    if (ws.on_close) ws.on_close(conn.id());
}

void SimHttpServer::Impl::ws_close(Connection& conn, uint16_t code) {
    // Wait (bounded) for the peer to answer and hang up, so unread input
    // does not turn our close into a reset.
    if (send_ws_close(conn.fd(), code)) linger_close(conn.fd());
}

void SimHttpServer::Impl::ws_loop(Connection& conn, std::string in) {
    const int fd = conn.fd();
    std::string message;  // a fragmented text message being reassembled
    bool in_message = false;
    WsFrame frame;
    for (;;) {
        for (const std::string& text : conn.take_queue()) {
            if (!send_ws_frame(fd, kOpText, text)) return;
        }
        if (conn.closing()) return;  // close(): the socket is already shut down

        for (;;) {
            const int r = take_ws_frame(in, frame, kMaxWsMessageBytes - message.size());
            if (r < 0) break;
            if (r > 0) return ws_close(conn, static_cast<uint16_t>(r));
            switch (frame.opcode) {
                case kOpText:
                case kOpBinary:
                    if (in_message) return ws_close(conn, kCloseProtocolError);
                    // The admin protocol is JSON text only.
                    if (frame.opcode == kOpBinary) return ws_close(conn, kCloseUnsupportedData);
                    if (frame.fin) {
                        if (ws.on_message) ws.on_message(conn.id(), frame.payload);
                    } else {
                        message = std::move(frame.payload);
                        in_message = true;
                    }
                    break;
                case kOpContinuation:
                    if (!in_message) return ws_close(conn, kCloseProtocolError);
                    message += frame.payload;
                    if (frame.fin) {
                        in_message = false;
                        if (ws.on_message) ws.on_message(conn.id(), message);
                        message.clear();
                    }
                    break;
                case kOpPing:
                    if (!send_ws_frame(fd, kOpPong, frame.payload)) return;
                    break;
                case kOpPong:
                    break;
                case kOpClose:
                    // Answer with the peer's status code (RFC 6455 section
                    // 5.5.1); the peer then closes the TCP connection.
                    if (frame.payload.size() == 1) return ws_close(conn, kCloseProtocolError);
                    send_ws_frame(fd, kOpClose, std::string_view(frame.payload).substr(0, 2));
                    return;
                default:
                    return ws_close(conn, kCloseProtocolError);
            }
            if (conn.closing()) return;
        }

        pollfd p[2] = {{fd, POLLIN, 0}, {conn.wake_fd(), POLLIN, 0}};
        const int r = ::poll(p, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (p[1].revents != 0) conn.drain_wake();
        if (p[0].revents != 0 && !recv_some(fd, in, 0)) return;
    }
}

SimHttpServer::SimHttpServer(std::string bind) : impl_(std::make_unique<Impl>(std::move(bind))) {}

SimHttpServer::~SimHttpServer() { stop(); }

Result<void> SimHttpServer::start(uint16_t port, net::HttpHandler handler, net::WsHandlers ws) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.lifecycle);
    if (s.is_running.load()) return Result<void>::err(ErrorCode::Busy, "server already running");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, s.bind.c_str(), &addr.sin_addr) != 1) {
        return Result<void>::err(ErrorCode::InvalidArgument, "bind address must be dotted IPv4");
    }
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return Result<void>::err(ErrorCode::IoError, std::strerror(errno));
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 16) != 0) {
        std::string message =
            "cannot listen on " + s.bind + ":" + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return Result<void>::err(ErrorCode::IoError, std::move(message));
    }
    sockaddr_in bound{};
    socklen_t bound_len = sizeof bound;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len);

    int pipe_fds[2];
    if (::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        ::close(fd);
        return Result<void>::err(ErrorCode::IoError, std::strerror(errno));
    }

    s.listen_fd = fd;
    s.wake_rd = pipe_fds[0];
    s.wake_wr = pipe_fds[1];
    s.handler = std::move(handler);
    s.ws = std::move(ws);
    s.bound_port.store(ntohs(bound.sin_port));
    s.is_running.store(true);
    s.acceptor = std::thread([&s] { s.accept_loop(); });
    return Result<void>::ok();
}

void SimHttpServer::stop() {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.lifecycle);
    if (!s.is_running.load()) return;

    const char b = 1;
    [[maybe_unused]] const ssize_t n = ::write(s.wake_wr, &b, 1);
    s.acceptor.join();
    close_fd(s.listen_fd);

    // No new slots can appear now that the acceptor is gone. Shutting
    // every socket down wakes threads blocked in poll/recv/send; each
    // then finishes (reporting on_close for an open WebSocket) and exits.
    std::list<std::unique_ptr<Impl::Slot>> slots;
    {
        std::lock_guard<std::mutex> slots_lock(s.slots_mutex);
        slots = std::move(s.slots);
        s.slots.clear();
    }
    for (auto& slot : slots) slot->conn->abort();
    for (auto& slot : slots) slot->thread.join();

    close_fd(s.wake_rd);
    close_fd(s.wake_wr);
    s.handler = nullptr;
    s.ws = {};
    s.bound_port.store(0);
    s.is_running.store(false);
}

bool SimHttpServer::running() const { return impl_->is_running.load(); }

uint16_t SimHttpServer::port() const { return impl_->bound_port.load(); }

}  // namespace wisp::port::sim
