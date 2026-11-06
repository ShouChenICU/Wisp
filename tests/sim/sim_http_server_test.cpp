// Drives SimHttpServer over real loopback sockets. Every wait polls with a
// deadline (client sockets carry receive timeouts, callbacks are awaited
// on a condition variable), so a regression fails a case instead of
// hanging it, and nothing relies on fixed sleeps.

#include "wisp/port/sim/sim_http_server.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "net/sha1.hpp"
#include "wisp/base/sha256.hpp"

namespace {

using namespace std::chrono_literals;
using wisp::net::HttpHandler;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Iface;
using wisp::net::Method;
using wisp::net::WsConnection;
using wisp::net::WsHandlers;
using wisp::port::sim::SimHttpServer;
using Clock = std::chrono::steady_clock;

constexpr auto kWait = 5s;
constexpr const char* kSampleKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr const char* kSampleAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::span<const uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;  // lowercase names
    std::string body;
};

struct Frame {
    bool fin = false;
    uint8_t opcode = 0;
    std::string payload;
};

// A blocking loopback client whose reads give up after kWait.
class Client {
public:
    explicit Client(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        timeval tv{};
        tv.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(kWait).count();
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    }
    ~Client() { close(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connected() const { return connected_; }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool send(std::string_view data) {
        while (!data.empty()) {
            ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            data.remove_prefix(static_cast<size_t>(n));
        }
        return true;
    }

    // True once the server has closed the connection (EOF or reset),
    // discarding anything still unread; false if it stays open for kWait.
    bool closed_by_peer() {
        buf_.clear();
        char tmp[4096];
        for (;;) {
            ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n > 0) continue;
            if (n == 0) return true;
            if (errno == EINTR) continue;
            return errno == ECONNRESET;
        }
    }

    std::optional<Response> read_response() {
        size_t end;
        while ((end = buf_.find("\r\n\r\n")) == std::string::npos) {
            if (!fill()) return std::nullopt;
        }
        Response res;
        std::string_view head(buf_.data(), end);
        size_t line_end = head.find("\r\n");
        std::string_view status_line = head.substr(0, line_end);
        if (status_line.size() < 12) return std::nullopt;
        res.status = std::stoi(std::string(status_line.substr(9, 3)));
        while (line_end != std::string_view::npos) {
            head.remove_prefix(line_end + 2);
            line_end = head.find("\r\n");
            std::string_view line = head.substr(0, line_end);
            size_t colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            res.headers[lower(line.substr(0, colon))] = std::string(value);
        }
        buf_.erase(0, end + 4);
        if (auto te = res.headers.find("transfer-encoding");
            te != res.headers.end() && lower(te->second) == "chunked") {
            if (!read_chunked(res.body)) return std::nullopt;
            return res;
        }
        size_t length = 0;
        if (auto it = res.headers.find("content-length"); it != res.headers.end()) {
            length = std::stoul(it->second);
        }
        while (buf_.size() < length) {
            if (!fill()) return std::nullopt;
        }
        res.body = buf_.substr(0, length);
        buf_.erase(0, length);
        return res;
    }

    std::optional<Frame> read_frame() {
        while (buf_.size() < 2) {
            if (!fill()) return std::nullopt;
        }
        const auto b0 = static_cast<uint8_t>(buf_[0]);
        const auto b1 = static_cast<uint8_t>(buf_[1]);
        if (b1 & 0x80) return std::nullopt;  // servers never mask
        size_t header = 2;
        uint64_t length = b1 & 0x7f;
        if (length >= 126) {
            header = length == 126 ? 4 : 10;
            while (buf_.size() < header) {
                if (!fill()) return std::nullopt;
            }
            length = 0;
            for (size_t i = 2; i < header; ++i) {
                length = (length << 8) | static_cast<uint8_t>(buf_[i]);
            }
        }
        while (buf_.size() < header + length) {
            if (!fill()) return std::nullopt;
        }
        Frame f;
        f.fin = (b0 & 0x80) != 0;
        f.opcode = b0 & 0x0f;
        f.payload = buf_.substr(header, length);
        buf_.erase(0, header + length);
        return f;
    }

private:
    // A chunked body (no trailers) into `body`.
    bool read_chunked(std::string& body) {
        for (;;) {
            size_t eol;
            while ((eol = buf_.find("\r\n")) == std::string::npos) {
                if (!fill()) return false;
            }
            const size_t size = std::stoul(buf_.substr(0, eol), nullptr, 16);
            buf_.erase(0, eol + 2);
            while (buf_.size() < size + 2) {
                if (!fill()) return false;
            }
            if (buf_.compare(size, 2, "\r\n") != 0) return false;
            body.append(buf_, 0, size);
            buf_.erase(0, size + 2);
            if (size == 0) return true;
        }
    }

    bool fill() {
        char tmp[4096];
        for (;;) {
            ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            buf_.append(tmp, static_cast<size_t>(n));
            return true;
        }
    }

    int fd_ = -1;
    bool connected_ = false;
    std::string buf_;
};

// A client-to-server frame: always masked unless `masked` is false.
std::string client_frame(uint8_t first_byte, std::string_view payload, bool masked = true) {
    std::string out;
    out.push_back(static_cast<char>(first_byte));
    const uint8_t mask_bit = masked ? 0x80 : 0x00;
    if (payload.size() < 126) {
        out.push_back(static_cast<char>(mask_bit | payload.size()));
    } else if (payload.size() <= 0xffff) {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>(payload.size() >> 8));
        out.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(
                static_cast<char>((static_cast<uint64_t>(payload.size()) >> shift) & 0xff));
        }
    }
    const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    if (masked) out.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i) {
        out.push_back(static_cast<char>(masked ? payload[i] ^ mask[i % 4] : payload[i]));
    }
    return out;
}

std::string text_frame(std::string_view text) { return client_frame(0x81, text); }

uint16_t close_code(const Frame& f) {
    if (f.payload.size() < 2) return 0;
    return static_cast<uint16_t>((static_cast<uint8_t>(f.payload[0]) << 8) |
                                 static_cast<uint8_t>(f.payload[1]));
}

std::string get_request(std::string_view path, std::string_view extra_headers = {}) {
    std::string req = "GET ";
    req += path;
    req += " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    req += extra_headers;
    req += "\r\n";
    return req;
}

std::string upgrade_request(std::string_view key = kSampleKey) {
    std::string req =
        "GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ";
    req += key;
    req += "\r\n\r\n";
    return req;
}

// Records what the server reports and lets the test wait for it.
class Recorder {
public:
    template <class Pred>
    bool wait(Pred pred) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, kWait, pred);
    }

    HttpHandler handler() {
        return [this](const HttpRequest& req) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests.push_back(req);
            }
            cv_.notify_all();
            HttpResponse res;
            res.content_type = "text/plain";
            res.body = "path=" + req.path;
            return res;
        };
    }

    WsHandlers ws(bool accept = true) {
        WsHandlers ws;
        ws.on_open = [this, accept](std::shared_ptr<WsConnection> conn, const HttpRequest& req) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ws_requests.push_back(req);
                if (accept) opened.push_back(std::move(conn));
            }
            cv_.notify_all();
            return accept;
        };
        ws.on_message = [this](int id, std::string_view text) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                messages.emplace_back(id, std::string(text));
            }
            cv_.notify_all();
        };
        ws.on_close = [this](int id) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                closed.push_back(id);
            }
            cv_.notify_all();
        };
        return ws;
    }

    size_t opened_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return opened.size();
    }

    std::shared_ptr<WsConnection> conn(size_t i) {
        std::lock_guard<std::mutex> lock(mutex_);
        return opened.at(i);
    }

    std::vector<int> closed_ids() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed;
    }

    std::vector<std::pair<int, std::string>> received() {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages;
    }

    HttpRequest request(size_t i) {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests.at(i);
    }

    size_t request_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests.size();
    }

    HttpRequest ws_request(size_t i) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ws_requests.at(i);
    }

    size_t ws_request_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ws_requests.size();
    }

    // Guarded by mutex_; read through the accessors above.
    std::vector<HttpRequest> requests;
    std::vector<HttpRequest> ws_requests;
    std::vector<std::shared_ptr<WsConnection>> opened;
    std::vector<std::pair<int, std::string>> messages;
    std::vector<int> closed;

    void notify() { cv_.notify_all(); }
    std::mutex& mutex() { return mutex_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
};

// Opens a WebSocket on `client` and waits until the server reports it.
bool open_ws(Client& client, Recorder& rec, size_t expected_opened) {
    if (!client.send(upgrade_request())) return false;
    auto res = client.read_response();
    if (!res || res->status != 101) return false;
    return rec.wait([&] { return rec.opened.size() == expected_opened; });
}

class SimHttpServerTest : public ::testing::Test {
protected:
    void start(WsHandlers ws) { start(rec.handler(), std::move(ws)); }
    void start(HttpHandler handler, WsHandlers ws) {
        auto r = server.start(0, std::move(handler), std::move(ws));
        ASSERT_TRUE(r.is_ok()) << r.error().message;
        ASSERT_NE(server.port(), 0);
    }

    Recorder rec;
    SimHttpServer server;
};

}  // namespace

TEST(SimHttpServerSha1, StandardVectors) {
    using wisp::to_hex;
    using wisp::port::sim::sha1;
    EXPECT_EQ(to_hex(sha1(bytes(""))), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(to_hex(sha1(bytes("abc"))), "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes: the length no longer fits, so padding spills into a second block.
    EXPECT_EQ(to_hex(sha1(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(to_hex(sha1(bytes("The quick brown fox jumps over the lazy dog"))),
              "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    const std::string million(1000000, 'a');
    EXPECT_EQ(to_hex(sha1(bytes(million))), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // Exactly one block of input plus a whole padding block.
    const std::string block(64, 'a');
    EXPECT_EQ(to_hex(sha1(bytes(block))), "0098ba824b5c16427bd7a1122a5a442a25ec644d");
}

TEST_F(SimHttpServerTest, ServesHandlerResponseWithKeepAlive) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(c.connected());

    ASSERT_TRUE(c.send(get_request("/a?x=1&y=2")));
    auto first = c.read_response();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, 200);
    EXPECT_EQ(first->body, "path=/a");
    EXPECT_EQ(first->headers["content-type"], "text/plain");

    ASSERT_TRUE(c.send(get_request("/b")));
    auto second = c.read_response();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 200);
    EXPECT_EQ(second->body, "path=/b");

    ASSERT_EQ(rec.request_count(), 2u);
    HttpRequest req = rec.request(0);
    EXPECT_EQ(req.method, Method::Get);
    EXPECT_EQ(req.path, "/a");
    EXPECT_EQ(req.query, "x=1&y=2");
    EXPECT_EQ(req.iface, Iface::Loopback);
    EXPECT_EQ(req.remote_ip, "127.0.0.1");
}

TEST_F(SimHttpServerTest, ReadsBodyAndClosesWhenAsked) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(
        c.send("POST /api/v1/x HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 7\r\n"
               "Connection: close\r\n\r\n{\"a\":1}"));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->headers["connection"], "close");
    EXPECT_TRUE(c.closed_by_peer());
    HttpRequest req = rec.request(0);
    EXPECT_EQ(req.method, Method::Post);
    EXPECT_EQ(req.body, "{\"a\":1}");
}

TEST_F(SimHttpServerTest, LowercasesHeaderNames) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(
        c.send("GET /h HTTP/1.1\r\nHoSt:   127.0.0.1:8080  \r\nX-Wisp: 1\r\n"
               "COOKIE: wisp_token=abc\r\n\r\n"));
    ASSERT_TRUE(c.read_response());
    HttpRequest req = rec.request(0);
    EXPECT_EQ(req.headers["x-wisp"], "1");
    EXPECT_EQ(req.headers["host"], "127.0.0.1:8080");
    EXPECT_EQ(req.cookie("wisp_token"), "abc");
    EXPECT_EQ(req.headers.count("X-Wisp"), 0u);
}

TEST_F(SimHttpServerTest, AcceptsBodyAtTheLimit) {
    start(rec.ws());
    Client c(server.port());
    const std::string body(SimHttpServer::kMaxBodyBytes, 'x');
    ASSERT_TRUE(c.send("POST /big HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(rec.request(0).body.size(), body.size());
}

TEST_F(SimHttpServerTest, RejectsOversizedBody) {
    start(rec.ws());
    Client c(server.port());
    // The body is never sent: the server must answer from Content-Length
    // alone, without waiting for (or buffering) the body.
    ASSERT_TRUE(c.send("POST /big HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 8193\r\n\r\n"));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 413);
    // The same code the admin router uses, so the web page can say it.
    EXPECT_NE(res->body.find("\"payload_too_large\""), std::string::npos) << res->body;
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_EQ(rec.request_count(), 0u);
}

TEST_F(SimHttpServerTest, RejectsChunkedBody) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(
        c.send("POST /x HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n"));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 411);
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_EQ(rec.request_count(), 0u);
}

TEST_F(SimHttpServerTest, RejectsOversizedHead) {
    start(rec.ws());
    Client c(server.port());
    std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ";
    req.append(SimHttpServer::kMaxHeadBytes, 'p');
    req += "\r\n\r\n";
    ASSERT_TRUE(c.send(req));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 431);
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_EQ(rec.request_count(), 0u);
}

TEST_F(SimHttpServerTest, RejectsMalformedRequestLine) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(c.send("NONSENSE\r\n\r\n"));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_TRUE(c.closed_by_peer());
}

TEST_F(SimHttpServerTest, SendsStaticBodyAndExtraHeaders) {
    static const uint8_t kAsset[] = {'<', 'h', '1', '>', 0x00, 0xff, '<', '/', 'h', '1', '>'};
    start(
        [](const HttpRequest&) {
            HttpResponse res;
            res.content_type = "text/html";
            res.headers.emplace_back("Content-Encoding", "gzip");
            res.static_body = std::span<const uint8_t>(kAsset, sizeof kAsset);
            return res;
        },
        rec.ws());
    Client c(server.port());
    ASSERT_TRUE(c.send(get_request("/")));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->headers["content-encoding"], "gzip");
    EXPECT_EQ(res->body, std::string(reinterpret_cast<const char*>(kAsset), sizeof kAsset));
}

TEST_F(SimHttpServerTest, NotModifiedHasNoBody) {
    start(
        [](const HttpRequest&) {
            HttpResponse res;
            res.status = 304;
            res.headers.emplace_back("ETag", "\"abc\"");
            return res;
        },
        rec.ws());
    Client c(server.port());
    ASSERT_TRUE(c.send(get_request("/")));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 304);
    EXPECT_EQ(res->headers.count("content-length"), 0u);
    // The connection is still usable: no stray body bytes precede the
    // next response.
    ASSERT_TRUE(c.send(get_request("/")));
    auto again = c.read_response();
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 304);
}

TEST_F(SimHttpServerTest, WebSocketHandshakeAndEcho) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(c.send(upgrade_request()));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 101);
    EXPECT_EQ(lower(res->headers["upgrade"]), "websocket");
    EXPECT_EQ(res->headers["sec-websocket-accept"], kSampleAccept);
    ASSERT_TRUE(rec.wait([&] { return rec.opened.size() == 1; }));
    auto conn = rec.conn(0);
    EXPECT_EQ(rec.ws_request(0).path, "/api/v1/ws");
    EXPECT_EQ(rec.ws_request(0).iface, Iface::Loopback);

    ASSERT_TRUE(c.send(text_frame("hello")));
    ASSERT_TRUE(rec.wait([&] { return rec.messages.size() == 1; }));
    EXPECT_EQ(rec.received().at(0), std::make_pair(conn->id(), std::string("hello")));

    ASSERT_TRUE(conn->send_text("world"));
    auto frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->fin);
    EXPECT_EQ(frame->opcode, 0x1);
    EXPECT_EQ(frame->payload, "world");

    // A 64 KB message exercises the 64-bit length encoding both ways.
    const std::string big(SimHttpServer::kMaxWsMessageBytes, 'b');
    ASSERT_TRUE(c.send(text_frame(big)));
    ASSERT_TRUE(rec.wait([&] { return rec.messages.size() == 2; }));
    EXPECT_EQ(rec.received().at(1).second, big);
    ASSERT_TRUE(conn->send_text(big));
    frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->payload, big);

    // Client-initiated close: the server answers with a close frame
    // echoing the code and reports the close exactly once.
    ASSERT_TRUE(c.send(client_frame(0x88, std::string("\x03\xe8", 2))));
    frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->opcode, 0x8);
    EXPECT_EQ(close_code(*frame), 1000);
    ASSERT_TRUE(rec.wait([&] { return rec.closed.size() == 1; }));
    EXPECT_EQ(rec.closed_ids(), std::vector<int>{conn->id()});
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_FALSE(conn->send_text("late"));
}

TEST_F(SimHttpServerTest, WebSocketMessagesQueuedInOnOpenFollowTheHandshake) {
    WsHandlers ws = rec.ws();
    auto inner = ws.on_open;
    ws.on_open = [inner](std::shared_ptr<WsConnection> conn, const HttpRequest& req) {
        conn->send_text("greeting");
        return inner(std::move(conn), req);
    };
    start(std::move(ws));
    Client c(server.port());
    ASSERT_TRUE(c.send(upgrade_request()));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 101);
    auto frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->payload, "greeting");
}

TEST_F(SimHttpServerTest, WebSocketRefusedByOnOpenGets403) {
    start(rec.ws(/*accept=*/false));
    Client c(server.port());
    ASSERT_TRUE(c.send(upgrade_request()));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_EQ(rec.ws_request_count(), 1u);
    EXPECT_TRUE(rec.closed_ids().empty());
}

TEST_F(SimHttpServerTest, WebSocketBadHandshakeIsRejected) {
    start(rec.ws());
    {
        Client c(server.port());
        ASSERT_TRUE(
            c.send("GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Version: 8\r\nSec-WebSocket-Key: " +
                   std::string(kSampleKey) + "\r\n\r\n"));
        auto res = c.read_response();
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 426);
        EXPECT_EQ(res->headers["sec-websocket-version"], "13");
    }
    {
        Client c(server.port());
        ASSERT_TRUE(
            c.send("GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n"));
        auto res = c.read_response();
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 400);
    }
    EXPECT_EQ(rec.ws_request_count(), 0u);
}

TEST_F(SimHttpServerTest, WebSocketUnmaskedFrameClosesWith1002) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    ASSERT_TRUE(c.send(client_frame(0x81, "plain", /*masked=*/false)));
    auto frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->opcode, 0x8);
    EXPECT_EQ(close_code(*frame), 1002);
    // The server half-closes after its close frame; a browser then hangs
    // up, which ends the server's bounded wait for it.
    EXPECT_TRUE(c.closed_by_peer());
    c.close();
    ASSERT_TRUE(rec.wait([&] { return rec.closed.size() == 1; }));
    EXPECT_TRUE(rec.received().empty());
}

TEST_F(SimHttpServerTest, WebSocketPingGetsPong) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    ASSERT_TRUE(c.send(client_frame(0x89, "beat")));
    auto frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->opcode, 0xA);
    EXPECT_EQ(frame->payload, "beat");
}

TEST_F(SimHttpServerTest, WebSocketReassemblesFragments) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    // A ping between fragments is allowed and answered.
    ASSERT_TRUE(c.send(client_frame(0x01, "hel") + client_frame(0x89, "") +
                       client_frame(0x00, "lo, ") + client_frame(0x80, "world")));
    auto frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->opcode, 0xA);
    ASSERT_TRUE(rec.wait([&] { return rec.messages.size() == 1; }));
    EXPECT_EQ(rec.received().at(0).second, "hello, world");
}

TEST_F(SimHttpServerTest, WebSocketOversizedMessageClosesWith1009) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    const std::string half(SimHttpServer::kMaxWsMessageBytes / 2 + 1, 'h');
    ASSERT_TRUE(c.send(client_frame(0x01, half) + client_frame(0x80, half)));
    auto frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->opcode, 0x8);
    EXPECT_EQ(close_code(*frame), 1009);
    EXPECT_TRUE(c.closed_by_peer());
    c.close();
    ASSERT_TRUE(rec.wait([&] { return rec.closed.size() == 1; }));
    EXPECT_TRUE(rec.received().empty());
}

TEST_F(SimHttpServerTest, WebSocketSingleOversizedFrameClosesWith1009) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    // Only the header is sent: the limit applies before the payload is
    // buffered.
    const std::string frame_bytes =
        client_frame(0x81, std::string(SimHttpServer::kMaxWsMessageBytes + 1, 'x'));
    ASSERT_TRUE(c.send(std::string_view(frame_bytes).substr(0, 14)));
    auto frame = c.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(close_code(*frame), 1009);
    EXPECT_TRUE(c.closed_by_peer());
    c.close();
    ASSERT_TRUE(rec.wait([&] { return rec.closed.size() == 1; }));
}

TEST_F(SimHttpServerTest, WebSocketCloseFromAnotherThreadReportsOnce) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    auto conn = rec.conn(0);
    conn->close();
    EXPECT_FALSE(conn->send_text("after close"));
    ASSERT_TRUE(rec.wait([&] { return rec.closed.size() == 1; }));
    EXPECT_TRUE(c.closed_by_peer());
    conn->close();  // repeated close is harmless
    server.stop();
    EXPECT_EQ(rec.closed_ids(), std::vector<int>{conn->id()});
}

// The admin layer calls close() while holding a mutex that its own
// WebSocket callbacks take; close() must not wait for the connection
// thread, even while that thread is blocked inside a callback.
TEST_F(SimHttpServerTest, CloseNeverWaitsForTheConnectionThread) {
    std::mutex app;
    std::atomic<bool> in_callback{false};
    WsHandlers ws = rec.ws();
    auto inner_message = ws.on_message;
    ws.on_message = [&, inner_message](int id, std::string_view text) {
        in_callback = true;
        rec.notify();
        std::lock_guard<std::mutex> lock(app);
        inner_message(id, text);
    };
    auto inner_close = ws.on_close;
    ws.on_close = [&, inner_close](int id) {
        std::lock_guard<std::mutex> lock(app);
        inner_close(id);
    };
    start(std::move(ws));
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    auto conn = rec.conn(0);
    {
        std::lock_guard<std::mutex> lock(app);
        ASSERT_TRUE(c.send(text_frame("block")));
        ASSERT_TRUE(rec.wait([&] { return in_callback.load(); }));
        const auto t0 = Clock::now();
        conn->close();
        EXPECT_LT(Clock::now() - t0, 500ms);
        EXPECT_FALSE(conn->send_text("late"));
    }
    ASSERT_TRUE(rec.wait([&] { return rec.closed.size() == 1; }));
    EXPECT_EQ(rec.closed_ids(), std::vector<int>{conn->id()});
}

TEST_F(SimHttpServerTest, WebSocketIdsAreNeverReused) {
    start(rec.ws());
    std::vector<int> ids;
    for (size_t i = 0; i < 3; ++i) {
        Client c(server.port());
        ASSERT_TRUE(open_ws(c, rec, i + 1));
        ids.push_back(rec.conn(i)->id());
        c.close();
        ASSERT_TRUE(rec.wait([&] { return rec.closed.size() == i + 1; }));
    }
    EXPECT_LT(ids[0], ids[1]);
    EXPECT_LT(ids[1], ids[2]);
}

TEST_F(SimHttpServerTest, SendQueueFillsWhenThePeerStopsReading) {
    start(rec.ws());
    Client c(server.port());
    ASSERT_TRUE(open_ws(c, rec, 1));
    auto conn = rec.conn(0);
    // The client never reads: once the socket buffers are full the
    // connection thread blocks in send() and the queue fills up.
    const std::string chunk(SimHttpServer::kMaxWsMessageBytes, 'q');
    size_t accepted = 0;
    const auto deadline = Clock::now() + kWait;
    while (conn->send_text(chunk)) {
        ++accepted;
        ASSERT_LT(Clock::now(), deadline) << "send_text never refused";
    }
    EXPECT_GE(accepted, SimHttpServer::kWsQueueDepth);
    // stop() still returns promptly with the connection thread stuck in
    // send().
    const auto t0 = Clock::now();
    server.stop();
    EXPECT_LT(Clock::now() - t0, 1s);
    EXPECT_EQ(rec.closed_ids(), std::vector<int>{conn->id()});
}

TEST_F(SimHttpServerTest, EighthConnectionClosedWhenNoneIsIdle) {
    start(rec.ws());
    std::vector<std::unique_ptr<Client>> clients;
    // Six connections in the middle of a request head: none is idle.
    for (size_t i = 0; i < SimHttpServer::kMaxConnections - 1; ++i) {
        clients.push_back(std::make_unique<Client>(server.port()));
        ASSERT_TRUE(clients.back()->send("GET /partial HTTP/1.1\r\nHost: 127.0.0.1\r\n"));
    }
    // Until the server has read a partial head, that connection still
    // looks idle; wait for all six.
    const auto read_by = Clock::now() + kWait;
    while (server.busy_connections_for_test() < SimHttpServer::kMaxConnections - 1 &&
           Clock::now() < read_by) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(server.busy_connections_for_test(), SimHttpServer::kMaxConnections - 1);
    // The seventh is a WebSocket: both kinds count toward the limit, and a
    // WebSocket is never evicted.
    clients.push_back(std::make_unique<Client>(server.port()));
    ASSERT_TRUE(open_ws(*clients.back(), rec, 1));

    Client eighth(server.port());
    ASSERT_TRUE(eighth.connected());
    EXPECT_TRUE(eighth.closed_by_peer());

    // The connections already admitted were left alone: each can finish
    // its request.
    ASSERT_TRUE(clients.front()->send("\r\n"));
    auto res = clients.front()->read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    // Once a slot frees up, new connections are served again.
    clients.front()->close();
    bool served = false;
    const auto deadline = Clock::now() + kWait;
    while (!served && Clock::now() < deadline) {
        Client retry(server.port());
        if (!retry.send(get_request("/again"))) continue;
        auto again = retry.read_response();
        served = again && again->status == 200;
    }
    EXPECT_TRUE(served);
}

TEST_F(SimHttpServerTest, EighthConnectionEvictsTheLeastRecentlyUsedIdleOne) {
    start(rec.ws());
    std::vector<std::unique_ptr<Client>> clients;
    // Six keep-alive connections, each idle after one request; the sleeps
    // give them distinct idle-since times, oldest first.
    for (size_t i = 0; i < SimHttpServer::kMaxConnections - 1; ++i) {
        clients.push_back(std::make_unique<Client>(server.port()));
        ASSERT_TRUE(clients.back()->send(get_request("/keep")));
        auto res = clients.back()->read_response();
        ASSERT_TRUE(res);
        ASSERT_EQ(res->status, 200);
        std::this_thread::sleep_for(10ms);
    }
    // Using the oldest again makes the second one the least recently used.
    ASSERT_TRUE(clients[0]->send(get_request("/reuse")));
    ASSERT_TRUE(clients[0]->read_response());
    std::this_thread::sleep_for(10ms);
    // A WebSocket fills the last slot.
    Client ws(server.port());
    ASSERT_TRUE(open_ws(ws, rec, 1));

    Client eighth(server.port());
    ASSERT_TRUE(eighth.send(get_request("/eighth")));
    auto res = eighth.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    EXPECT_TRUE(clients[1]->closed_by_peer());
    // Everyone else is still served, the WebSocket included.
    for (size_t i : {size_t{0}, size_t{2}, size_t{5}}) {
        ASSERT_TRUE(clients[i]->send(get_request("/still")));
        auto again = clients[i]->read_response();
        ASSERT_TRUE(again) << i;
        EXPECT_EQ(again->status, 200) << i;
    }
    EXPECT_TRUE(rec.closed_ids().empty());
}

TEST_F(SimHttpServerTest, StopJoinsCleanly) {
    start(rec.ws());
    Client idle(server.port());
    ASSERT_TRUE(idle.send(get_request("/idle")));
    ASSERT_TRUE(idle.read_response());
    Client partial(server.port());
    ASSERT_TRUE(partial.send("GET /partial HTTP/1.1\r\nHost: 127.0.0.1\r\n"));
    Client ws(server.port());
    ASSERT_TRUE(open_ws(ws, rec, 1));
    auto conn = rec.conn(0);

    const auto t0 = Clock::now();
    server.stop();
    EXPECT_LT(Clock::now() - t0, 1s);
    EXPECT_FALSE(server.running());
    EXPECT_EQ(server.port(), 0);
    EXPECT_EQ(rec.closed_ids(), std::vector<int>{conn->id()});
    EXPECT_TRUE(idle.closed_by_peer());
    EXPECT_TRUE(partial.closed_by_peer());
    EXPECT_TRUE(ws.closed_by_peer());
    EXPECT_FALSE(conn->send_text("after stop"));
    conn->close();
    server.stop();  // idempotent
}

TEST(SimHttpServer, ConnectionOutlivesServer) {
    Recorder rec;
    std::shared_ptr<WsConnection> conn;
    {
        SimHttpServer server;
        ASSERT_TRUE(server.start(0, rec.handler(), rec.ws()).is_ok());
        Client c(server.port());
        ASSERT_TRUE(open_ws(c, rec, 1));
        conn = rec.conn(0);
    }
    EXPECT_EQ(rec.closed_ids(), std::vector<int>{conn->id()});
    EXPECT_FALSE(conn->send_text("orphan"));
    conn->close();
}

TEST(SimHttpServer, StartErrors) {
    Recorder rec;
    SimHttpServer first;
    ASSERT_TRUE(first.start(0, rec.handler(), rec.ws()).is_ok());
    EXPECT_TRUE(first.running());
    EXPECT_FALSE(first.start(0, rec.handler(), rec.ws()).is_ok());

    SimHttpServer second;
    EXPECT_FALSE(second.start(first.port(), rec.handler(), rec.ws()).is_ok());
    EXPECT_FALSE(second.running());

    SimHttpServer bad("not-an-address");
    EXPECT_FALSE(bad.start(0, rec.handler(), rec.ws()).is_ok());

    // A stopped server can be started again.
    first.stop();
    ASSERT_TRUE(first.start(0, rec.handler(), rec.ws()).is_ok());
    Client c(first.port());
    ASSERT_TRUE(c.send(get_request("/again")));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

namespace {

// Streams POST /upload bodies into a shared record; everything else is
// left to the buffered handler. `reject_after` > 0 answers 400 once that
// many bytes arrived.
struct StreamRecord {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<HttpRequest> heads;
    std::string body;
    size_t chunks = 0;
    size_t largest_chunk = 0;
    int ends = 0;
    int aborts = 0;

    template <class Pred>
    bool wait(Pred pred) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kWait, pred);
    }
};

class RecordingSink : public wisp::net::BodySink {
public:
    RecordingSink(StreamRecord& rec, size_t reject_after)
        : rec_(rec), reject_after_(reject_after) {}

    std::optional<HttpResponse> on_data(std::span<const uint8_t> chunk) override {
        std::lock_guard<std::mutex> lock(rec_.mutex);
        rec_.body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        ++rec_.chunks;
        rec_.largest_chunk = std::max(rec_.largest_chunk, chunk.size());
        if (reject_after_ > 0 && rec_.body.size() >= reject_after_) {
            return HttpResponse::error(400, "wrong_board", "not for this device");
        }
        return std::nullopt;
    }
    HttpResponse on_end() override {
        {
            std::lock_guard<std::mutex> lock(rec_.mutex);
            ++rec_.ends;
        }
        rec_.cv.notify_all();
        HttpResponse res;
        res.status = 202;
        res.body = "{\"received\":" + std::to_string(rec_.body.size()) + "}";
        return res;
    }
    void on_abort() override {
        {
            std::lock_guard<std::mutex> lock(rec_.mutex);
            ++rec_.aborts;
        }
        rec_.cv.notify_all();
    }

private:
    StreamRecord& rec_;
    size_t reject_after_;
};

wisp::net::StreamHandler upload_streams(StreamRecord& rec, size_t reject_after = 0) {
    return [&rec, reject_after](const HttpRequest& head) -> std::optional<wisp::net::StreamStart> {
        if (head.method != Method::Post || head.path != "/upload") return std::nullopt;
        {
            std::lock_guard<std::mutex> lock(rec.mutex);
            rec.heads.push_back(head);
        }
        if (head.header("x-refuse") == "1") {
            return wisp::net::StreamStart{nullptr, HttpResponse::error(409, "busy", "busy")};
        }
        return wisp::net::StreamStart{std::make_unique<RecordingSink>(rec, reject_after), {}};
    };
}

std::string post_head(std::string_view path, size_t length, std::string_view extra = {}) {
    std::string req = "POST ";
    req += path;
    req += " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " + std::to_string(length) + "\r\n";
    req += extra;
    req += "\r\n";
    return req;
}

std::string pattern(size_t size) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xff);
    return s;
}

}  // namespace

TEST(SimHttpServer, StreamsLargeRequestBody) {
    Recorder rec;
    StreamRecord streamed;
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, rec.handler(), rec.ws(), upload_streams(streamed)).is_ok());

    const std::string body = pattern(1024 * 1024);
    Client c(server.port());
    ASSERT_TRUE(c.send(post_head("/upload", body.size(), "X-Wisp: 1\r\n") + body));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202);
    EXPECT_EQ(res->body, "{\"received\":1048576}");
    {
        std::lock_guard<std::mutex> lock(streamed.mutex);
        ASSERT_EQ(streamed.heads.size(), 1u);
        EXPECT_EQ(streamed.heads[0].content_length, body.size());
        EXPECT_EQ(streamed.heads[0].header("x-wisp"), "1");
        EXPECT_TRUE(streamed.heads[0].body.empty());
        EXPECT_EQ(streamed.body, body);
        EXPECT_GT(streamed.chunks, 1u);
        EXPECT_LE(streamed.largest_chunk, wisp::net::kBodyChunkBytes);
        EXPECT_EQ(streamed.ends, 1);
        EXPECT_EQ(streamed.aborts, 0);
    }
    // The buffered handler never saw it.
    EXPECT_EQ(rec.request_count(), 0u);

    // The connection stays usable, and every other route keeps its limit.
    ASSERT_TRUE(c.send(post_head("/other", 8193)));
    auto other = c.read_response();
    ASSERT_TRUE(other);
    EXPECT_EQ(other->status, 413);
    EXPECT_EQ(rec.request_count(), 0u);
}

TEST(SimHttpServer, StreamedRequestCanBeAnsweredEarly) {
    Recorder rec;
    StreamRecord streamed;
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, rec.handler(), rec.ws(), upload_streams(streamed, 6000)).is_ok());

    // Refused from the head: no body byte reaches a sink.
    {
        Client c(server.port());
        const std::string body = pattern(64 * 1024);
        ASSERT_TRUE(c.send(post_head("/upload", body.size(), "X-Refuse: 1\r\n") + body));
        auto res = c.read_response();
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 409);
        EXPECT_EQ(res->headers["connection"], "close");
        EXPECT_TRUE(c.closed_by_peer());
    }
    // Refused by the sink part way: the client can still send the rest
    // and read the answer.
    {
        Client c(server.port());
        const std::string body = pattern(512 * 1024);
        ASSERT_TRUE(c.send(post_head("/upload", body.size()) + body));
        auto res = c.read_response();
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 400);
        EXPECT_NE(res->body.find("wrong_board"), std::string::npos);
        EXPECT_TRUE(c.closed_by_peer());
    }
    std::lock_guard<std::mutex> lock(streamed.mutex);
    // Answered on the piece that crossed 6000 bytes; nothing after it.
    EXPECT_GE(streamed.body.size(), 6000u);
    EXPECT_LT(streamed.body.size(), 6000u + wisp::net::kBodyChunkBytes);
    EXPECT_EQ(streamed.ends, 0);
    EXPECT_EQ(streamed.aborts, 0);
}

TEST(SimHttpServer, DroppedUploadAborts) {
    Recorder rec;
    StreamRecord streamed;
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, rec.handler(), rec.ws(), upload_streams(streamed)).is_ok());
    {
        Client c(server.port());
        ASSERT_TRUE(c.send(post_head("/upload", 100000) + pattern(30000)));
        ASSERT_TRUE(streamed.wait([&] { return streamed.body.size() == 30000; }));
    }  // the client goes away mid-body
    ASSERT_TRUE(streamed.wait([&] { return streamed.aborts == 1; }));
    EXPECT_EQ(streamed.ends, 0);

    // Stopping the server mid-body aborts too.
    Client c(server.port());
    ASSERT_TRUE(c.send(post_head("/upload", 100000) + pattern(1000)));
    ASSERT_TRUE(streamed.wait([&] { return streamed.body.size() == 31000; }));
    server.stop();
    std::lock_guard<std::mutex> lock(streamed.mutex);
    EXPECT_EQ(streamed.aborts, 2);
    EXPECT_EQ(streamed.ends, 0);
}

TEST(SimHttpServer, StreamedRouteNeedsContentLength) {
    Recorder rec;
    StreamRecord streamed;
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, rec.handler(), rec.ws(), upload_streams(streamed)).is_ok());
    Client c(server.port());
    ASSERT_TRUE(
        c.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n"));
    auto res = c.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 411);
    std::lock_guard<std::mutex> lock(streamed.mutex);
    EXPECT_TRUE(streamed.heads.empty());
}

// A body pulled from HttpResponse::stream: with a stream_length it goes out
// under that Content-Length, without one chunked; either way every byte
// arrives in order and the connection stays usable.
TEST(SimHttpServer, StreamedResponseBody) {
    const std::string body = pattern(3 * 1024 * 1024);
    std::atomic<size_t> largest{0};
    auto handler = [&](const HttpRequest& req) {
        HttpResponse res;
        res.content_type = "application/octet-stream";
        auto offset = std::make_shared<size_t>(0);
        res.stream = [&body, &largest, offset](std::span<uint8_t> buf) -> wisp::Result<size_t> {
            if (buf.size() > largest.load()) largest.store(buf.size());
            const size_t n = std::min(buf.size(), body.size() - *offset);
            std::memcpy(buf.data(), body.data() + *offset, n);
            *offset += n;
            return wisp::Result<size_t>::ok(n);
        };
        if (req.path == "/sized") res.stream_length = body.size();
        return res;
    };
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, handler, WsHandlers{}).is_ok());
    Client c(server.port());
    ASSERT_TRUE(c.connected());

    ASSERT_TRUE(c.send(get_request("/sized")));
    auto sized = c.read_response();
    ASSERT_TRUE(sized);
    EXPECT_EQ(sized->status, 200);
    EXPECT_EQ(sized->headers["content-length"], std::to_string(body.size()));
    EXPECT_EQ(sized->headers.count("transfer-encoding"), 0u);
    EXPECT_EQ(sized->headers["content-type"], "application/octet-stream");
    EXPECT_TRUE(sized->body == body) << "sized body differs, " << sized->body.size() << " bytes";
    EXPECT_LE(largest.load(), wisp::net::kStreamChunkBytes);

    ASSERT_TRUE(c.send(get_request("/chunked")));
    auto chunked = c.read_response();
    ASSERT_TRUE(chunked);
    EXPECT_EQ(chunked->status, 200);
    EXPECT_EQ(chunked->headers["transfer-encoding"], "chunked");
    EXPECT_EQ(chunked->headers.count("content-length"), 0u);
    EXPECT_TRUE(chunked->body == body)
        << "chunked body differs, " << chunked->body.size() << " bytes";
}

// A stream that fails, or runs dry before its stream_length, ends the
// connection: the client must not take a short body for a whole one.
TEST(SimHttpServer, StreamedResponseFailureClosesTheConnection) {
    auto handler = [](const HttpRequest& req) {
        HttpResponse res;
        auto sent = std::make_shared<bool>(false);
        const bool fail = req.path == "/fail";
        res.stream = [sent, fail](std::span<uint8_t> buf) -> wisp::Result<size_t> {
            if (*sent) {
                if (fail) return wisp::Result<size_t>::err(wisp::ErrorCode::IoError, "gone");
                return wisp::Result<size_t>::ok(0);
            }
            *sent = true;
            std::memset(buf.data(), 'x', 10);
            return wisp::Result<size_t>::ok(10);
        };
        res.stream_length = 100;
        return res;
    };
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, handler, WsHandlers{}).is_ok());
    for (const char* path : {"/short", "/fail"}) {
        Client c(server.port());
        ASSERT_TRUE(c.send(get_request(path)));
        EXPECT_FALSE(c.read_response()) << path;
        EXPECT_TRUE(c.closed_by_peer()) << path;
    }
}

// A client that hangs up in the middle of a long body: the server stops
// pulling, lets the response go, and keeps serving new connections.
TEST(SimHttpServer, ClientDisconnectMidStreamStopsIt) {
    struct Probe {
        std::mutex mu;
        std::condition_variable cv;
        bool released = false;
        std::atomic<size_t> pulled{0};
    };
    auto probe = std::make_shared<Probe>();
    struct Release {
        std::shared_ptr<Probe> p;
        ~Release() {
            if (!p) return;
            std::lock_guard<std::mutex> lock(p->mu);
            p->released = true;
            p->cv.notify_all();
        }
    };
    constexpr uint64_t kLength = 1024ull * 1024 * 1024;  // never sent in full
    auto handler = [probe](const HttpRequest& req) {
        HttpResponse res;
        if (req.path != "/endless") {
            res.body = "ok";
            return res;
        }
        Probe* p = probe.get();
        res.stream = [p](std::span<uint8_t> buf) -> wisp::Result<size_t> {
            std::memset(buf.data(), 'z', buf.size());
            p->pulled += buf.size();
            return wisp::Result<size_t>::ok(buf.size());
        };
        res.stream_length = kLength;
        auto owner = std::make_shared<Release>();
        owner->p = probe;
        res.body_owner = std::move(owner);
        return res;
    };
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, handler, WsHandlers{}).is_ok());
    {
        Client c(server.port());
        ASSERT_TRUE(c.send(get_request("/endless")));
        // Wait until the body is under way, then hang up.
        const auto deadline = Clock::now() + kWait;
        while (probe->pulled.load() < 256 * 1024 && Clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_GE(probe->pulled.load(), 256u * 1024);
    }
    {
        std::unique_lock<std::mutex> lock(probe->mu);
        EXPECT_TRUE(probe->cv.wait_for(lock, kWait, [&] { return probe->released; }));
    }
    EXPECT_LT(probe->pulled.load(), kLength);

    Client next(server.port());
    ASSERT_TRUE(next.send(get_request("/plain")));
    auto res = next.read_response();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, "ok");
}
