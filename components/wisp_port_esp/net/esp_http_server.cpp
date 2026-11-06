#include "wisp/port/esp/esp_http_server.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wisp/core/log.hpp"
#include "wisp/os/large.hpp"
#include "wisp/port/esp/net_iface.hpp"
#include "wisp/port/esp/stack_placement.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "http_server";

// A refused body up to this size is read and discarded by httpd after the
// error response (keeping the connection); a larger one closes the
// connection instead of streaming it in.
constexpr size_t kDrainLimit = 4 * EspHttpServer::kMaxBodyBytes;
// httpd_req_recv() gives up after recv_wait_timeout (5 s) of silence;
// this many in a row end the request.
constexpr int kBodyRecvTimeouts = 2;

constexpr uint16_t kCloseNormal = 1000;
constexpr uint16_t kCloseProtocolError = 1002;
constexpr uint16_t kCloseUnsupportedData = 1003;
constexpr uint16_t kCloseTooBig = 1009;

// Control frame payloads are at most 125 bytes (RFC 6455 section 5.5).
constexpr size_t kMaxControlPayload = 125;

// Request headers the router reads (or that the WebSocket upgrade and the
// 411 check need), as delivered to the handler: lowercase.
constexpr const char* kHeaders[] = {
    "host",
    "cookie",
    "x-wisp",
    "content-type",
    "if-none-match",
    "user-agent",
    "origin",
    "transfer-encoding",
    "upgrade",
    "connection",
    "sec-websocket-key",
    "sec-websocket-version",
};

const char* status_text(int status) {
    switch (status) {
        case 200:
            return "200 OK";
        case 201:
            return "201 Created";
        case 202:
            return "202 Accepted";
        case 204:
            return "204 No Content";
        case 301:
            return "301 Moved Permanently";
        case 302:
            return "302 Found";
        case 304:
            return "304 Not Modified";
        case 400:
            return "400 Bad Request";
        case 401:
            return "401 Unauthorized";
        case 403:
            return "403 Forbidden";
        case 404:
            return "404 Not Found";
        case 405:
            return "405 Method Not Allowed";
        case 409:
            return "409 Conflict";
        case 411:
            return "411 Length Required";
        case 413:
            return "413 Content Too Large";
        case 426:
            return "426 Upgrade Required";
        case 429:
            return "429 Too Many Requests";
        case 431:
            return "431 Request Header Fields Too Large";
        case 500:
            return "500 Internal Server Error";
        case 501:
            return "501 Not Implemented";
        case 503:
            return "503 Service Unavailable";
        case 504:
            return "504 Gateway Timeout";
        default:
            return nullptr;
    }
}

bool has_crlf(std::string_view s) { return s.find_first_of("\r\n") != std::string_view::npos; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

net::Method method_of(int m) {
    switch (m) {
        case HTTP_GET:
            return net::Method::Get;
        case HTTP_POST:
            return net::Method::Post;
        case HTTP_PUT:
            return net::Method::Put;
        case HTTP_DELETE:
            return net::Method::Delete;
        default:
            return net::Method::Other;
    }
}

// IPv4 address (host byte order) of a socket address, unwrapping the
// IPv4-mapped IPv6 form httpd's dual-stack listener reports; 0 otherwise.
uint32_t ipv4_of(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET) {
        return lwip_ntohl(reinterpret_cast<const sockaddr_in&>(ss).sin_addr.s_addr);
    }
#if CONFIG_LWIP_IPV6
    if (ss.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(ss).sin6_addr;
        const uint8_t* b = a.s6_addr;
        for (int i = 0; i < 10; ++i) {
            if (b[i] != 0) return 0;
        }
        if (b[10] != 0xff || b[11] != 0xff) return 0;
        return ipv4(b[12], b[13], b[14], b[15]);
    }
#endif
    return 0;
}

std::string dotted(uint32_t ip) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24),
                  static_cast<unsigned>((ip >> 16) & 0xff), static_cast<unsigned>((ip >> 8) & 0xff),
                  static_cast<unsigned>(ip & 0xff));
    return buf;
}

void read_netif(const char* key, uint32_t& ip, uint32_t* mask) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(key);
    if (netif == nullptr || !esp_netif_is_netif_up(netif)) return;
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return;
    ip = lwip_ntohl(info.ip.addr);
    if (mask != nullptr) *mask = lwip_ntohl(info.netmask.addr);
}

LocalAddresses local_addresses() {
    LocalAddresses own;
    // IDF's keys for the default Wi-Fi interfaces (EspWifi creates both).
    read_netif("WIFI_AP_DEF", own.ap_ip, &own.ap_mask);
    read_netif("WIFI_STA_DEF", own.sta_ip, nullptr);
    return own;
}

void fill_request(httpd_req_t* r, net::HttpRequest& req) {
    req.method = method_of(r->method);
    std::string_view uri(r->uri);
    const size_t q = uri.find('?');
    req.path = std::string(uri.substr(0, q));
    if (q != std::string_view::npos) req.query = std::string(uri.substr(q + 1));

    for (const char* name : kHeaders) {
        const size_t len = httpd_req_get_hdr_value_len(r, name);
        if (len == 0) continue;
        std::string value(len + 1, '\0');
        if (httpd_req_get_hdr_value_str(r, name, value.data(), value.size()) != ESP_OK) continue;
        value.resize(len);
        if (std::strcmp(name, "host") == 0) value = std::string(trim(value));
        req.headers.emplace(name, std::move(value));
    }

    const int fd = httpd_req_to_sockfd(r);
    sockaddr_storage peer = {};
    sockaddr_storage local = {};
    socklen_t peer_len = sizeof(peer);
    socklen_t local_len = sizeof(local);
    uint32_t peer_ip = 0;
    uint32_t local_ip = 0;
    if (lwip_getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
        peer_ip = ipv4_of(peer);
    }
    if (lwip_getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
        local_ip = ipv4_of(local);
    }
    if (peer_ip != 0) req.remote_ip = dotted(peer_ip);
    req.iface = iface_for(local_ip, peer_ip, local_addresses());
}

esp_err_t send_streamed(httpd_req_t* r, const net::HttpResponse& res, const char* status,
                        os::LargeBuffer& buf);

esp_err_t send_response(httpd_req_t* r, const net::HttpResponse& res) {
    std::string custom;
    const char* status = status_text(res.status);
    if (status == nullptr) {
        custom = std::to_string(res.status) + " Status";
        status = custom.c_str();
    }
    // 1xx, 204 and 304 never carry a body (RFC 9110 section 6.4.1).
    const bool bodyless =
        (res.status >= 100 && res.status < 200) || res.status == 204 || res.status == 304;
    // A streamed body's buffer, taken before any of the response's head is
    // set: without it the 503 must not go out under the download's
    // Content-Disposition, or the browser would save it as the file.
    os::LargeBuffer stream_buf;
    if (res.stream && !bodyless) {
        stream_buf = os::LargeBuffer::allocate(net::kStreamChunkBytes);
        if (!stream_buf.ok()) {
            WLOG_W(kTag, "no memory to stream a response");
            net::HttpResponse err =
                net::HttpResponse::error(503, "no_memory", "no memory for the response");
            err.headers.emplace_back("Cache-Control", "no-store");
            return send_response(r, err);
        }
    }
    httpd_resp_set_status(r, status);
    if (!res.content_type.empty() && !has_crlf(res.content_type)) {
        httpd_resp_set_type(r, res.content_type.c_str());
    }
    for (const auto& [name, value] : res.headers) {
        // A stray CR/LF would let a value inject headers.
        if (name.empty() || has_crlf(name) || has_crlf(value)) continue;
        // httpd keeps the pointers until the response is sent below.
        if (httpd_resp_set_hdr(r, name.c_str(), value.c_str()) != ESP_OK) {
            WLOG_W(kTag, "response header dropped (too many): %s", name.c_str());
        }
    }
    // The head set above is what a chunked stream goes out with.
    if (stream_buf.ok()) return send_streamed(r, res, status, stream_buf);
    const char* data = nullptr;
    size_t len = 0;
    if (!bodyless) {
        if (!res.static_body.empty()) {
            // No copy: mapped asset data, or a buffer body_owner keeps alive.
            data = reinterpret_cast<const char*>(res.static_body.data());
            len = res.static_body.size();
        } else {
            data = res.body.data();
            len = res.body.size();
        }
    }
    return httpd_resp_send(r, data, static_cast<ssize_t>(len));
}

// httpd_send() may take only part of a piece.
bool send_raw(httpd_req_t* r, const char* data, size_t len) {
    while (len > 0) {
        const int n = httpd_send(r, data, len);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// A body pulled from HttpResponse::stream through `buf` (Mem::Large,
// kStreamChunkBytes, taken by send_response() and held only while this
// response is sent). Chunked (httpd's own
// framing, with the head send_response() set) without a stream_length;
// with one, the head is written here so it can carry a Content-Length,
// which httpd_resp_send_chunk() cannot and httpd_resp_send() only up to an
// int. ESP_FAIL once anything fails midway: httpd then closes the session,
// so the client never takes a short body for a whole one.
esp_err_t send_streamed(httpd_req_t* r, const net::HttpResponse& res, const char* status,
                        os::LargeBuffer& buf) {
    if (res.stream_length == 0) {
        const bool ok = net::pump_stream(res, buf.span(), [r](std::span<const uint8_t> piece) {
            return httpd_resp_send_chunk(r, reinterpret_cast<const char*>(piece.data()),
                                         static_cast<ssize_t>(piece.size())) == ESP_OK;
        });
        if (!ok) return ESP_FAIL;
        return httpd_resp_send_chunk(r, nullptr, 0);
    }
    std::string head = std::string("HTTP/1.1 ") + status + "\r\n";
    if (!res.content_type.empty() && !has_crlf(res.content_type)) {
        head += "Content-Type: " + res.content_type + "\r\n";
    }
    head += "Content-Length: " + std::to_string(res.stream_length) + "\r\n";
    for (const auto& [name, value] : res.headers) {
        if (name.empty() || has_crlf(name) || has_crlf(value)) continue;
        head += name + ": " + value + "\r\n";
    }
    head += "\r\n";
    if (!send_raw(r, head.data(), head.size())) return ESP_FAIL;
    head = std::string();
    const bool ok = net::pump_stream(res, buf.span(), [r](std::span<const uint8_t> piece) {
        return send_raw(r, reinterpret_cast<const char*>(piece.data()), piece.size());
    });
    return ok ? ESP_OK : ESP_FAIL;
}

bool read_body(httpd_req_t* r, std::string& body) {
    body.resize(r->content_len);
    size_t off = 0;
    int timeouts = 0;
    while (off < body.size()) {
        const int n = httpd_req_recv(r, body.data() + off, body.size() - off);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kBodyRecvTimeouts) continue;
        if (n <= 0) return false;
        timeouts = 0;
        off += static_cast<size_t>(n);
    }
    return true;
}

void noop_free(void*) {}

}  // namespace

// One accepted WebSocket. The application holds it as a net::WsConnection
// (possibly past the end of the connection); the server holds it in
// Impl::conns, keyed by socket, while the session lives.
//
// Threads: send_text()/close() come from any thread; everything else runs
// on the httpd task. mutex_ guards the outgoing queue and the flags, and
// is never held while calling the application. `gone_` is set (under
// mutex_) by the session's close callback before the socket is closed, so
// while it is false under mutex_ fd_ still belongs to this connection.
class WsConn final : public net::WsConnection, public std::enable_shared_from_this<WsConn> {
public:
    WsConn(int id, httpd_handle_t hd, int fd) : id_(id), hd_(hd), fd_(fd) {}

    int id() const override { return id_; }

    bool send_text(std::string_view text) override {
        // Never logs: the log sink feeding the WebSocket calls this.
        std::lock_guard<std::mutex> lock(mutex_);
        if (gone_ || closing_ || queue_.size() >= EspHttpServer::kWsQueueDepth) return false;
        queue_.emplace_back(text);
        if (!flush_queued_) {
            auto* self = new std::shared_ptr<WsConn>(shared_from_this());
            if (httpd_queue_work(hd_, &WsConn::flush_work, self) != ESP_OK) {
                // httpd's control queue is full; the caller retries later.
                delete self;
                queue_.pop_back();
                return false;
            }
            flush_queued_ = true;
        }
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gone_ || closing_) return;
        closing_ = true;
        queue_.clear();
        auto* self = new std::shared_ptr<WsConn>(shared_from_this());
        if (httpd_queue_work(hd_, &WsConn::close_work, self) != ESP_OK) {
            delete self;
            // No room to ask the httpd task: shutting the socket down makes
            // it notice the end of the connection and close the session.
            lwip_shutdown(fd_, SHUT_RDWR);
        }
    }

    // httpd task: the session is closing; nothing more may touch fd_.
    void mark_gone() {
        std::lock_guard<std::mutex> lock(mutex_);
        gone_ = true;
        queue_.clear();
    }

    // httpd task: message reassembly state.
    std::string fragments;
    bool in_fragmented_message = false;

private:
    static void flush_work(void* arg) {
        std::unique_ptr<std::shared_ptr<WsConn>> self(static_cast<std::shared_ptr<WsConn>*>(arg));
        (*self)->flush();
    }

    static void close_work(void* arg) {
        std::unique_ptr<std::shared_ptr<WsConn>> self(static_cast<std::shared_ptr<WsConn>*>(arg));
        WsConn& c = **self;
        {
            std::lock_guard<std::mutex> lock(c.mutex_);
            if (c.gone_) return;
        }
        send_close(c.hd_, c.fd_, kCloseNormal);
        httpd_sess_trigger_close(c.hd_, c.fd_);
    }

    void flush() {
        for (;;) {
            std::string msg;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (gone_ || closing_ || queue_.empty()) {
                    flush_queued_ = false;
                    return;
                }
                msg = std::move(queue_.front());
                queue_.pop_front();
            }
            httpd_ws_frame_t frame = {};
            frame.type = HTTPD_WS_TYPE_TEXT;
            frame.payload = reinterpret_cast<uint8_t*>(msg.data());
            frame.len = msg.size();
            if (httpd_ws_send_frame_async(hd_, fd_, &frame) != ESP_OK) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    closing_ = true;
                    flush_queued_ = false;
                    queue_.clear();
                }
                httpd_sess_trigger_close(hd_, fd_);
                return;
            }
            // Keeps a streaming connection away from the LRU purge.
            httpd_sess_update_lru_counter(hd_, fd_);
        }
    }

public:
    static void send_close(httpd_handle_t hd, int fd, uint16_t code) {
        uint8_t payload[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code & 0xff)};
        httpd_ws_frame_t frame = {};
        frame.type = HTTPD_WS_TYPE_CLOSE;
        frame.payload = payload;
        frame.len = sizeof(payload);
        httpd_ws_send_frame_async(hd, fd, &frame);
    }

private:
    const int id_;
    const httpd_handle_t hd_;
    const int fd_;

    std::mutex mutex_;
    std::deque<std::string> queue_;
    bool gone_ = false;
    bool closing_ = false;
    bool flush_queued_ = false;
};

struct EspHttpServer::Impl {
    explicit Impl(EspHttpServerConfig c) : config(c) {}

    const EspHttpServerConfig config;
    // start()/stop() run on the owner's thread; running() may be asked
    // from anywhere.
    httpd_handle_t hd = nullptr;
    std::atomic<bool> running{false};
    std::atomic<uint16_t> port{0};
    net::HttpHandler handler;
    net::WsHandlers ws;
    net::StreamHandler stream;
    // Never reset, not even across restarts: a late on_close(id) must
    // never name a newer connection.
    std::atomic<int> next_id{1};
    // httpd task only (and stop(), once the task has exited).
    std::map<int, std::shared_ptr<WsConn>> conns;

    static esp_err_t on_request(httpd_req_t* r);
    static esp_err_t on_ws_frame(httpd_req_t* r);
    static esp_err_t on_ws_upgrade(httpd_req_t* r);
    static void on_session_close(httpd_handle_t hd, int fd);

    esp_err_t serve(httpd_req_t* r);
    esp_err_t reject(httpd_req_t* r, int status, const char* code, const char* message);
};

namespace {

// An answer before the end of the body (`left` bytes still unread): sent
// with Connection: close, and the session closes once the handler returns.
// Returning ESP_OK lets httpd read and discard what is left first, so the
// client gets to read the answer instead of a reset; past kBodyDrainBytes
// that is not worth waiting for.
esp_err_t answer_early(httpd_req_t* r, const net::HttpResponse& res, size_t left) {
    httpd_resp_set_hdr(r, "Connection", "close");
    send_response(r, res);
    if (left > net::kBodyDrainBytes) return ESP_FAIL;
    httpd_sess_trigger_close(r->handle, httpd_req_to_sockfd(r));
    return ESP_OK;
}

esp_err_t stream_body(httpd_req_t* r, net::BodySink& sink) {
    // On the heap: the httpd stack also carries the sink's flash writes.
    std::vector<uint8_t> chunk(net::kBodyChunkBytes);
    size_t left = r->content_len;
    int timeouts = 0;
    while (left > 0) {
        const size_t want = left < chunk.size() ? left : chunk.size();
        const int n = httpd_req_recv(r, reinterpret_cast<char*>(chunk.data()), want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kBodyRecvTimeouts) continue;
        if (n <= 0) {
            sink.on_abort();
            return ESP_FAIL;
        }
        timeouts = 0;
        left -= static_cast<size_t>(n);
        if (auto early =
                sink.on_data(std::span<const uint8_t>(chunk.data(), static_cast<size_t>(n)))) {
            return answer_early(r, *early, left);
        }
    }
    return send_response(r, sink.on_end());
}

}  // namespace

esp_err_t EspHttpServer::Impl::reject(httpd_req_t* r, int status, const char* code,
                                      const char* message) {
    send_response(r, net::HttpResponse::error(status, code, message));
    return ESP_OK;
}

esp_err_t EspHttpServer::Impl::serve(httpd_req_t* r) {
    // When every socket is taken, httpd's LRU purge closes the session
    // touched least recently. httpd marks this request's session once the
    // handler returns; marking the live WebSockets now puts them right
    // behind it and ahead of every other socket, so an idle keep-alive
    // socket (which the browser silently reopens) goes before a quiet log
    // stream. conns holds only the open WebSockets (WsHub admits few) and
    // is only touched on this task.
    for (const auto& [fd, conn] : conns) httpd_sess_update_lru_counter(r->handle, fd);

    net::HttpRequest req;
    fill_request(r, req);

    if (req.headers.count("transfer-encoding") != 0) {
        reject(r, 411, "length_required", "chunked request bodies are not supported");
        // The chunked body is still on the wire and cannot be skipped.
        return ESP_FAIL;
    }
    if (stream) {
        req.content_length = r->content_len;
        if (auto start = stream(req)) {
            if (!start->sink) return answer_early(r, start->response, r->content_len);
            return stream_body(r, *start->sink);
        }
    }
    // Refused from the header alone: the body is never buffered.
    if (r->content_len > EspHttpServer::kMaxBodyBytes) {
        reject(r, 413, "payload_too_large", "request body exceeds 8192 bytes");
        // httpd discards up to kDrainLimit unread bytes itself before the
        // next request; a bigger body is not worth waiting for.
        return r->content_len <= kDrainLimit ? ESP_OK : ESP_FAIL;
    }
    if (r->content_len > 0 && !read_body(r, req.body)) return ESP_FAIL;

    const net::HttpResponse res =
        handler ? handler(req) : net::HttpResponse::error(404, "not_found", "no handler");
    return send_response(r, res);
}

esp_err_t EspHttpServer::Impl::on_request(httpd_req_t* r) {
    return static_cast<Impl*>(r->user_ctx)->serve(r);
}

// Runs before httpd answers the upgrade, so a refusal is still a plain
// HTTP error. Returning ESP_FAIL makes httpd close the connection.
esp_err_t EspHttpServer::Impl::on_ws_upgrade(httpd_req_t* r) {
    auto* self = static_cast<Impl*>(r->user_ctx);
    net::HttpRequest req;
    fill_request(r, req);
    // The key is base64 of 16 random bytes (RFC 6455 section 4.1).
    if (req.header("sec-websocket-key").size() != 24) {
        self->reject(r, 400, "bad_request", "malformed WebSocket upgrade");
        return ESP_FAIL;
    }
    if (req.header("sec-websocket-version") != "13") {
        httpd_resp_set_hdr(r, "Sec-WebSocket-Version", "13");
        self->reject(r, 426, "upgrade_required", "only WebSocket version 13 is supported");
        return ESP_FAIL;
    }
    const int fd = httpd_req_to_sockfd(r);
    auto conn = std::make_shared<WsConn>(self->next_id.fetch_add(1), r->handle, fd);
    if (!self->ws.on_open || !self->ws.on_open(conn, req)) {
        conn->mark_gone();
        self->reject(r, 403, "forbidden", "WebSocket upgrade refused");
        return ESP_FAIL;
    }
    // From here on the session's close reports on_close, whether or not
    // httpd manages to complete the handshake. Messages on_open queued go
    // out after the handshake response: the httpd task runs them later.
    self->conns[fd] = std::move(conn);
    return ESP_OK;
}

esp_err_t EspHttpServer::Impl::on_ws_frame(httpd_req_t* r) {
    auto* self = static_cast<Impl*>(r->user_ctx);
    const int fd = httpd_req_to_sockfd(r);
    auto it = self->conns.find(fd);
    // A plain GET of the WebSocket path (no upgrade) is an ordinary request.
    if (it == self->conns.end()) return self->serve(r);
    std::shared_ptr<WsConn> conn = it->second;

    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(r, &frame, 0) != ESP_OK) return ESP_FAIL;
    httpd_sess_update_lru_counter(r->handle, fd);

    switch (frame.type) {
        case HTTPD_WS_TYPE_PING:
        case HTTPD_WS_TYPE_PONG:
        case HTTPD_WS_TYPE_CLOSE: {
            uint8_t payload[kMaxControlPayload] = {};
            if (frame.len > kMaxControlPayload) {
                WsConn::send_close(r->handle, fd, kCloseProtocolError);
                return ESP_FAIL;
            }
            if (frame.len > 0) {
                frame.payload = payload;
                if (httpd_ws_recv_frame(r, &frame, frame.len) != ESP_OK) return ESP_FAIL;
            }
            if (frame.type == HTTPD_WS_TYPE_PING) {
                frame.type = HTTPD_WS_TYPE_PONG;
                return httpd_ws_send_frame(r, &frame);
            }
            if (frame.type == HTTPD_WS_TYPE_CLOSE) {
                // Answer the close, then end the session (which reports
                // on_close).
                httpd_ws_frame_t reply = {};
                reply.type = HTTPD_WS_TYPE_CLOSE;
                httpd_ws_send_frame(r, &reply);
                return ESP_FAIL;
            }
            return ESP_OK;
        }
        case HTTPD_WS_TYPE_TEXT:
        case HTTPD_WS_TYPE_CONTINUE: {
            const bool continuation = frame.type == HTTPD_WS_TYPE_CONTINUE;
            if (continuation != conn->in_fragmented_message) {
                WsConn::send_close(r->handle, fd, kCloseProtocolError);
                return ESP_FAIL;
            }
            const size_t have = continuation ? conn->fragments.size() : 0;
            if (frame.len > EspHttpServer::kMaxWsMessageBytes - have) {
                WsConn::send_close(r->handle, fd, kCloseTooBig);
                return ESP_FAIL;
            }
            if (!continuation) conn->fragments.clear();
            const size_t start = conn->fragments.size();
            conn->fragments.resize(start + frame.len);
            if (frame.len > 0) {
                frame.payload = reinterpret_cast<uint8_t*>(conn->fragments.data() + start);
                if (httpd_ws_recv_frame(r, &frame, frame.len) != ESP_OK) return ESP_FAIL;
            }
            if (!frame.final) {
                conn->in_fragmented_message = true;
                return ESP_OK;
            }
            conn->in_fragmented_message = false;
            std::string message = std::move(conn->fragments);
            conn->fragments.clear();
            if (self->ws.on_message) self->ws.on_message(conn->id(), message);
            return ESP_OK;
        }
        default:
            // Binary frames: the admin protocol is JSON text only.
            WsConn::send_close(r->handle, fd, kCloseUnsupportedData);
            return ESP_FAIL;
    }
}

void EspHttpServer::Impl::on_session_close(httpd_handle_t hd, int fd) {
    auto* self = static_cast<Impl*>(httpd_get_global_user_ctx(hd));
    if (self != nullptr) {
        auto it = self->conns.find(fd);
        if (it != self->conns.end()) {
            std::shared_ptr<WsConn> conn = std::move(it->second);
            self->conns.erase(it);
            // Before the socket closes: send_text() starts refusing and
            // nothing touches this fd again once another session reuses it.
            conn->mark_gone();
            if (self->ws.on_close) self->ws.on_close(conn->id());
        }
    }
    // With a close callback installed, closing the socket is ours to do.
    lwip_close(fd);
}

EspHttpServer::EspHttpServer(EspHttpServerConfig config) : impl_(std::make_unique<Impl>(config)) {}

EspHttpServer::~EspHttpServer() { stop(); }

Result<void> EspHttpServer::start(uint16_t port, net::HttpHandler handler, net::WsHandlers ws,
                                  net::StreamHandler stream) {
    if (impl_->hd != nullptr) return Result<void>::err(ErrorCode::Busy, "already running");
    impl_->handler = std::move(handler);
    impl_->ws = std::move(ws);
    impl_->stream = std::move(stream);

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = port;
    c.ctrl_port = impl_->config.ctrl_port;
    c.max_open_sockets = impl_->config.max_open_sockets;
    c.stack_size = impl_->config.stack_bytes;
    if (place_stack_in_psram(impl_->config.flash_access, impl_->config.hot_stack,
                             impl_->config.stack_bytes)) {
        c.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    c.task_priority = impl_->config.task_priority;
    c.lru_purge_enable = true;
    // A peer that vanished without a FIN (left Wi-Fi range, a lid shut)
    // would otherwise keep its session -- and an admin WebSocket's wake
    // lock, which keeps Wi-Fi out of deep standby -- until reboot: probe
    // after 30 s of silence, every 10 s, and drop it after 3 unanswered
    // probes (lwIP is built with LWIP_TCP_KEEPALIVE).
    c.keep_alive_enable = true;
    c.keep_alive_idle = 30;
    c.keep_alive_interval = 10;
    c.keep_alive_count = 3;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.max_uri_handlers = 6;
    // Static files carry ETag, Cache-Control, Content-Encoding and the
    // security headers; API responses Set-Cookie and Location.
    c.max_resp_headers = 16;
    c.global_user_ctx = impl_.get();
    // httpd free()s the global context on stop unless told otherwise.
    c.global_user_ctx_free_fn = &noop_free;
    c.close_fn = &Impl::on_session_close;

    httpd_handle_t hd = nullptr;
    esp_err_t err = httpd_start(&hd, &c);
    if (err != ESP_OK) {
        impl_->handler = nullptr;
        impl_->ws = {};
        impl_->stream = nullptr;
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("httpd_start: ") + esp_err_to_name(err));
    }

    // Registration order matters: httpd picks the first match, and "/*"
    // matches the WebSocket path too.
    if (impl_->config.websocket) {
        httpd_uri_t ws_uri = {};
        ws_uri.uri = kWsPath;
        ws_uri.method = HTTP_GET;
        ws_uri.handler = &Impl::on_ws_frame;
        ws_uri.user_ctx = impl_.get();
        ws_uri.is_websocket = true;
        ws_uri.handle_ws_control_frames = true;
        ws_uri.ws_pre_handshake_cb = &Impl::on_ws_upgrade;
        err = httpd_register_uri_handler(hd, &ws_uri);
    }
    const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_OPTIONS};
    for (httpd_method_t m : methods) {
        if (err != ESP_OK) break;
        httpd_uri_t uri = {};
        uri.uri = "/*";
        uri.method = m;
        uri.handler = &Impl::on_request;
        uri.user_ctx = impl_.get();
        err = httpd_register_uri_handler(hd, &uri);
    }
    if (err != ESP_OK) {
        httpd_stop(hd);
        impl_->conns.clear();
        impl_->handler = nullptr;
        impl_->ws = {};
        impl_->stream = nullptr;
        return Result<void>::err(
            ErrorCode::IoError, std::string("httpd_register_uri_handler: ") + esp_err_to_name(err));
    }
    impl_->hd = hd;
    impl_->port.store(port);
    impl_->running.store(true);
    WLOG_I(kTag, "listening on port %u", static_cast<unsigned>(port));
    return Result<void>::ok();
}

void EspHttpServer::stop() {
    if (impl_->hd == nullptr) return;
    // Closes every session on the httpd task (reporting WebSocket closes
    // through on_session_close) and waits for the task to exit.
    impl_->running.store(false);
    httpd_stop(impl_->hd);
    impl_->hd = nullptr;
    impl_->conns.clear();
    impl_->port.store(0);
    impl_->handler = nullptr;
    impl_->ws = {};
    impl_->stream = nullptr;
}

bool EspHttpServer::running() const { return impl_->running.load(); }

uint16_t EspHttpServer::port() const { return impl_->port.load(); }

}  // namespace wisp::port::esp
