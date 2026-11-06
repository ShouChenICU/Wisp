#pragma once

// net::HttpServer on ESP-IDF's esp_http_server (connectivity spec 2.1,
// 6.3). Two instances run on the device: the admin backend (port 8900,
// with the /api/v1/ws WebSocket) and, while the setup hotspot is up, the
// captive-portal redirect on port 80 (no WebSocket, fewer sockets, a
// small stack). Each httpd instance is one task that runs every handler
// in turn, so a slow handler (a scan, the LLM test) delays the other
// requests on that instance, never the rest of the system.
//
// Requests, matching the simulator's server (SimHttpServer):
//   - the handler sees header names in lowercase. Only the headers the
//     router reads are collected (Host, Cookie, X-Wisp, Content-Type,
//     If-None-Match, User-Agent, plus the WebSocket upgrade headers);
//     Host's value is trimmed of surrounding whitespace;
//   - a body over kMaxBodyBytes is refused with 413 from Content-Length
//     alone, before any of it is read; a chunked body (Transfer-Encoding)
//     is refused with 411. Either way the connection is then closed, as
//     what is left of the body cannot be parsed as the next request;
//   - a request the StreamHandler takes (net::BodySink) is exempt from
//     the 413: its body is read in kBodyChunkBytes pieces on the httpd
//     task, a connection that drops or stays silent for two of httpd's
//     5-second receive timeouts aborts it, and an early answer is sent
//     with Connection: close after which httpd discards the rest of the
//     body (up to kBodyDrainBytes) before closing;
//   - known limits of the single httpd task, accepted as they are
//     (docs/architecture.md "真机的网络"): a body larger than
//     kBodyDrainBytes is answered but not drained, so the client usually
//     sees a connection reset rather than the 413; requests are served one
//     at a time, so a second upload waits for the first to finish and
//     only then gets its 409 (after its own body is drained), and stop()
//     returns only once a running stream has ended (at most two receive
//     timeouts after its client goes quiet);
//   - iface is decided from the connection's addresses: loopback means
//     Loopback; the hotspot's own address reached from a peer inside the
//     hotspot subnet means Ap; anything else Sta (net_iface.hpp).
//
// WebSocket (only with Config::websocket): an upgrade on /api/v1/ws is
// offered to WsHandlers::on_open before the handshake, so a refusal is an
// ordinary 403. Each accepted connection gets an id that is never reused
// within a boot (not even across stop()/start()), and is reported through
// on_close exactly once -- when the peer closes, the server closes it, or
// stop(). send_text() queues up to kWsQueueDepth messages and hands them
// to the httpd task; it returns false when that queue is full, after
// close(), and once the connection is gone. close() never blocks and
// takes no lock that a server callback holds while calling the
// application. Text messages up to kMaxWsMessageBytes (fragments
// reassembled) reach on_message; a larger message or a binary frame
// closes the connection.
//
// httpd's own least-recently-used purge (lru_purge_enable) frees a socket
// for a new connection when all are taken. A WebSocket frame sent or
// received marks its own connection as used, and every plain HTTP request
// marks all open WebSockets, so the purge picks an idle keep-alive socket
// before a log stream.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "wisp/base/result.hpp"
#include "wisp/net/http.hpp"

namespace wisp::port::esp {

struct EspHttpServerConfig {
    // httpd's control socket (a UDP port on loopback). Every instance
    // running at the same time needs its own.
    uint16_t ctrl_port = 32768;
    // Client sockets; httpd adds three of its own, and the sum over all
    // instances must fit CONFIG_LWIP_MAX_SOCKETS with the other users.
    uint16_t max_open_sockets = 7;
    // The httpd task's stack. Handlers run on it, including the model
    // connectivity test's TLS handshake; Settings reads and writes go
    // through the cache (core::CachedSettingsBackend) and the firmware
    // upload and scene file downloads hand their flash I/O to wisp.fs
    // (run_sync, IoLane::Bulk) instead of touching it here.
    size_t stack_bytes = 10240;
    // Whether the handlers may touch internal flash, reads included (the
    // definition is at os::TaskConfig::flash_access). Defaults to false:
    // no httpd instance touches flash directly. The port places the
    // stack from it, as for any task: an instance declaring false may
    // get a PSRAM stack.
    //
    // Wherever the stack goes, stopping an instance needs about 2 KB of internal RAM
    // at that moment: the httpd task deletes itself with
    // vTaskDeleteWithCaps(), which starts a helper task with an internal
    // stack and aborts if it cannot (ESP-IDF's idf_additions.c). The
    // internal-RAM budget's floor (wisp/app/ram_budget.hpp) covers it.
    bool flash_access = false;
    // Whether this instance's stack should stay in internal RAM for
    // speed rather than for flash safety (os::TaskConfig::hot_stack's
    // same meaning). No httpd instance declares this true today.
    bool hot_stack = false;
    // Below the UI task (wisp::ui::UiRuntime, 4) so serving a page never
    // costs frames (connectivity spec 10); the other core and idle time
    // leave it plenty.
    int task_priority = 3;
    // Serve the /api/v1/ws WebSocket.
    bool websocket = true;
};

class EspHttpServer : public net::HttpServer {
public:
    static constexpr size_t kMaxBodyBytes = 8192;
    static constexpr size_t kWsQueueDepth = 64;
    static constexpr size_t kMaxWsMessageBytes = 64 * 1024;
    static constexpr const char* kWsPath = "/api/v1/ws";

    explicit EspHttpServer(EspHttpServerConfig config = {});
    ~EspHttpServer() override;

    EspHttpServer(const EspHttpServer&) = delete;
    EspHttpServer& operator=(const EspHttpServer&) = delete;

    Result<void> start(uint16_t port, net::HttpHandler handler, net::WsHandlers ws,
                       net::StreamHandler stream = {}) override;
    // Closes every connection (reporting WebSocket closes) and stops the
    // task; must not be called from a handler.
    void stop() override;
    bool running() const override;
    uint16_t port() const override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::port::esp
