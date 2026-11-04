#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/net/request_watch.hpp"

namespace wisp::net {

// Platform-neutral HTTP types. The server and client are platform services
// injected through app::Platform (spec 2.2): wisp_port_esp wraps
// esp_http_server/esp_http_client, wisp_port_sim uses POSIX sockets and
// libcurl. Routing lives above this layer, behind a single HttpHandler.

enum class Method { Get, Post, Put, Delete, Other };

// Which network interface a request arrived on; routes use it to restrict
// provisioning endpoints to the setup hotspot.
enum class Iface { Sta, Ap, Loopback };

struct HttpRequest {
    Method method = Method::Get;
    std::string path;  // without the query string
    std::string query;
    std::map<std::string, std::string> headers;  // keys are always lowercase
    std::string body;
    // The Content-Length the client announced (0 without one). Mostly for
    // streamed requests, whose `body` stays empty.
    uint64_t content_length = 0;
    std::string remote_ip;
    Iface iface = Iface::Sta;

    // Value of header `lower_name`, or "" when absent. The name is
    // lowercased before lookup, so either spelling works.
    std::string header(std::string_view lower_name) const;
    // Value of cookie `name` from the Cookie header (whitespace around
    // each pair trimmed), or "" when absent.
    std::string cookie(std::string_view name) const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // When non-empty it is sent instead of `body` without copying (asset-
    // pack data that lives in mapped flash, or a buffer `body_owner`
    // keeps alive); the bytes must outlive the response.
    std::span<const uint8_t> static_body;
    // Keeps whatever static_body points into alive until the response has
    // been sent (a scene export zip in PSRAM). Null for mapped data.
    std::shared_ptr<const void> body_owner;

    static HttpResponse json(int status, const Value& v);
    // {"error": {"code": ..., "message": ...}} (spec 6.1).
    static HttpResponse error(int status, std::string_view code, std::string_view message);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// Streamed request bodies (spec 6.5: the firmware upload). A request the
// server's StreamHandler takes is not buffered and has no size limit of its
// own: its body reaches a BodySink in pieces as it arrives. Both servers
// (SimHttpServer, EspHttpServer) give the same guarantees:
//   - the StreamHandler sees the request head (method, path, headers,
//     content_length; `body` empty) before any of the body is read;
//   - a chunked request never gets there: the server answers 411 as for
//     any other route. One without a Content-Length arrives with
//     content_length 0 (the handler decides whether that is an error);
//   - on_data() gets the body in order, at most kBodyChunkBytes at a time;
//     then exactly one of on_end() (every byte arrived) or on_abort() (the
//     connection closed, stalled past the server's receive timeout, or the
//     server is stopping) -- unless on_data() ended the request early.
//     Stopping aborts at once on the simulator; on the device stop() cannot
//     interrupt the httpd task, so it waits until the body is complete or a
//     receive fails (see EspHttpServer);
//   - every callback runs on the connection's own thread (the httpd task on
//     the device), one at a time, so a slow write delays only that client.
// An early response (StreamStart::response, or one returned from on_data)
// is sent at once; the server then reads and discards the rest of the body
// (up to kBodyDrainBytes, so the client sees the response rather than a
// reset) and closes the connection. Past kBodyDrainBytes the device does
// not drain: the client may see a reset instead of the response.
inline constexpr size_t kBodyChunkBytes = 4096;
inline constexpr size_t kBodyDrainBytes = 8 * 1024 * 1024;

class BodySink {
public:
    virtual ~BodySink() = default;
    // The next piece of the body. A response ends the request with it.
    virtual std::optional<HttpResponse> on_data(std::span<const uint8_t> chunk) = 0;
    // The whole body arrived; what to answer.
    virtual HttpResponse on_end() = 0;
    // The body will never be complete; nothing is sent.
    virtual void on_abort() = 0;
};

// What the StreamHandler decided for a request it takes: stream the body
// into `sink`, or (sink null) answer `response` without reading the body.
struct StreamStart {
    std::unique_ptr<BodySink> sink;
    HttpResponse response;
};

// Consulted for every request once its head is parsed. nullopt: not a
// streamed request; it is buffered and handed to the HttpHandler as usual
// (8 KiB body limit).
using StreamHandler = std::function<std::optional<StreamStart>(const HttpRequest& head)>;

class WsConnection {
public:
    virtual ~WsConnection() = default;
    virtual int id() const = 0;
    // Never blocks: returns false when the connection's send queue is full.
    // Must stay safe to call after close() and after the server has
    // reported the connection closed (on_close): it then returns false.
    // Callers may hold their reference past either point.
    virtual bool send_text(std::string_view text) = 0;
    // Starts closing the connection; callable from any thread, never
    // blocks. The server reports the close through on_close as usual
    // (possibly from within this call).
    virtual void close() = 0;
};

struct WsHandlers {
    // Returning false rejects the upgrade.
    std::function<bool(std::shared_ptr<WsConnection>, const HttpRequest&)> on_open;
    std::function<void(int id, std::string_view text)> on_message;
    std::function<void(int id)> on_close;
};

class HttpServer {
public:
    virtual ~HttpServer() = default;
    // `stream` may be empty: no request is streamed.
    virtual Result<void> start(uint16_t port, HttpHandler handler, WsHandlers ws,
                               StreamHandler stream = {}) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    // The port actually listened on (assigned by the OS after start(0, ...)).
    virtual uint16_t port() const = 0;
};

enum class TransportError { None, Dns, Connect, Tls, Timeout, TooLarge, Cancelled, Other };

// "dns"/"connect"/"tls"/"timeout"/"too_large"/"cancelled"/"other"; "" for
// None.
const char* to_string(TransportError e);

// What a client keeps of a response's headers (HttpClientResponse::headers):
// names lowercased, at most kMaxResponseHeaders of them, none whose value
// is over kMaxResponseHeaderValue bytes, and at most kMaxResponseHeaderBytes
// of names and values together; anything past those is skipped. Header
// strings are ordinary heap strings, which on the device land in internal
// RAM, so the total is bounded no matter what a server sends.
inline constexpr size_t kMaxResponseHeaders = 32;
inline constexpr size_t kMaxResponseHeaderValue = 1024;
inline constexpr size_t kMaxResponseHeaderBytes = 8 * 1024;

// Adds one received header to `headers` under the limits above (name
// lowercased, surrounding blanks trimmed from both). False when it was
// skipped.
bool keep_response_header(std::vector<std::pair<std::string, std::string>>& headers,
                          std::string_view name, std::string_view value);

struct HttpClientRequest {
    Method method = Method::Get;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // When non-empty it is sent instead of `body`, without copying (a body
    // that lives in PSRAM); the bytes must outlive request().
    std::span<const uint8_t> body_ref;
    // The deadline for the whole request, from the call to request() to
    // its return: waiting for the TLS gate, connect, handshake, sending
    // and every read (Timeout when it passes).
    uint32_t timeout_ms = 10000;
    // A response body over this many bytes fails the request with
    // TooLarge (checked against Content-Length first, then as it
    // arrives). 0: the client's own default cap.
    size_t max_response_bytes = 0;
    // When set, the body is handed over in pieces as it arrives and
    // HttpClientResponse::body stays empty; returning false stops the
    // transfer (Cancelled).
    std::function<bool(std::span<const uint8_t>)> on_body;
    // When set, asked between pieces of the transfer (and while waiting
    // for the server, as often as the client can); true stops it
    // (Cancelled). On its own it cannot reach a client blocked in a
    // single read; `watch` can.
    std::function<bool()> cancelled;
    // When set, the caller has armed it with the deadline (and may abort()
    // it): the client attaches what interrupts a call blocked inside it
    // and disarms it before releasing the connection, and reports a
    // request it ended as Timeout (Deadline) or Cancelled (Aborted).
    // Without one, a client keeps the deadline as well as it can on its
    // own (see each client).
    RequestWatch* watch = nullptr;
};

struct HttpClientResponse {
    TransportError transport = TransportError::None;
    int status = 0;
    std::string body;
    // The final response's headers, under keep_response_header()'s limits.
    // Empty when the transfer failed.
    std::vector<std::pair<std::string, std::string>> headers;
};

// Every client: no redirects (a 3xx is the response; following one could
// carry the Authorization header to another host), and request headers,
// bodies and anything past the URL's host are never logged.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpClientResponse request(const HttpClientRequest& req) = 0;
};

// HTTP status for a service error (spec 6.1's table, plus 429 for
// RateLimited); anything unlisted maps to 500.
int http_status_for(ErrorCode code);

}  // namespace wisp::net
