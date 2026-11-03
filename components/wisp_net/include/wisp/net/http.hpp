#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"

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
    // pack data that lives in mapped flash); the bytes must outlive the
    // response.
    std::span<const uint8_t> static_body;

    static HttpResponse json(int status, const Value& v);
    // {"error": {"code": ..., "message": ...}} (spec 6.1).
    static HttpResponse error(int status, std::string_view code, std::string_view message);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class WsConnection {
public:
    virtual ~WsConnection() = default;
    virtual int id() const = 0;
    // Never blocks: returns false when the connection's send queue is full.
    virtual bool send_text(std::string_view text) = 0;
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
    virtual Result<void> start(uint16_t port, HttpHandler handler, WsHandlers ws) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    // The port actually listened on (assigned by the OS after start(0, ...)).
    virtual uint16_t port() const = 0;
};

enum class TransportError { None, Dns, Connect, Tls, Timeout, Other };

// "dns"/"connect"/"tls"/"timeout"/"other"; "" for None.
const char* to_string(TransportError e);

struct HttpClientRequest {
    Method method = Method::Get;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    uint32_t timeout_ms = 10000;
};

struct HttpClientResponse {
    TransportError transport = TransportError::None;
    int status = 0;
    std::string body;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpClientResponse request(const HttpClientRequest& req) = 0;
};

// HTTP status for a service error (spec 6.1's table, plus 429 for
// RateLimited); anything unlisted maps to 500.
int http_status_for(ErrorCode code);

}  // namespace wisp::net
