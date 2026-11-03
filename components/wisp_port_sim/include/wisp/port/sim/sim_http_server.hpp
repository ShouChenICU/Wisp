#pragma once

// Simulator net::HttpServer: a small HTTP/1.1 + WebSocket (RFC 6455)
// server on POSIX sockets, listening on the loopback address only, so the
// admin backend (wisp_admin) can be driven from a browser or a test on the
// development machine. Every request is reported with Iface::Loopback.
//
// Threads: one accept thread plus one thread per connection; stop() (and
// the destructor) shuts down every socket and joins all of them, so it
// returns promptly even with idle keep-alive or WebSocket connections
// open. Handlers run on the connection threads, concurrently.
//
// Limits mirror the device (connectivity spec 6.3): at most
// kMaxConnections connections in total (HTTP and WebSocket together; a
// connection beyond that evicts the least recently used idle keep-alive
// HTTP connection, or is accepted and closed at once when none is idle,
// as ESP-IDF httpd's lru_purge_enable does), request bodies
// up to kMaxBodyBytes (checked against Content-Length before anything is
// read), and no chunked request bodies.
//
// Streamed requests (net::StreamHandler, see wisp/net/http.hpp): the
// handler is asked once a request's head is parsed; a request it takes has
// no body limit and its body goes to the BodySink in pieces of at most
// net::kBodyChunkBytes, read straight off the socket. A client that stays
// silent for the idle timeout (10 s), hangs up or is cut off by stop()
// mid-body ends in on_abort().

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/net/http.hpp"

namespace wisp::port::sim {

class SimHttpServer : public net::HttpServer {
public:
    static constexpr size_t kMaxConnections = 7;
    static constexpr size_t kMaxBodyBytes = 8192;
    // Request line plus headers; a larger head is answered with 431.
    static constexpr size_t kMaxHeadBytes = 8192;
    // Messages a WebSocket connection queues before send_text() refuses.
    static constexpr size_t kWsQueueDepth = 64;
    // Largest WebSocket message accepted, fragments included (close 1009
    // beyond it).
    static constexpr size_t kMaxWsMessageBytes = 64 * 1024;
    // The only path that accepts a WebSocket upgrade.
    static constexpr const char* kWsPath = "/api/v1/ws";

    // `bind` must be a dotted IPv4 address.
    explicit SimHttpServer(std::string bind = "127.0.0.1");
    ~SimHttpServer() override;

    SimHttpServer(const SimHttpServer&) = delete;
    SimHttpServer& operator=(const SimHttpServer&) = delete;

    Result<void> start(uint16_t port, net::HttpHandler handler, net::WsHandlers ws,
                       net::StreamHandler stream = {}) override;
    void stop() override;
    bool running() const override;
    // 0 while not running.
    uint16_t port() const override;

    // Connections that have read part of a request (or became a
    // WebSocket), i.e. that eviction leaves alone. Lets a test wait until
    // the server has actually seen what its clients sent.
    size_t busy_connections_for_test() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::port::sim
