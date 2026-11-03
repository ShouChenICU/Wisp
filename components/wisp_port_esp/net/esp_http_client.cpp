#include "wisp/port/esp/esp_http_client.hpp"

#include <memory>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_tls_errors.h"
#include "wisp/core/log.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "http_client";
// Room for long Authorization headers (some gateways issue long keys).
constexpr int kTxBufferBytes = 2048;
constexpr int kRxBufferBytes = 1024;

struct ClientDeleter {
    void operator()(esp_http_client* c) const { esp_http_client_cleanup(c); }
};
using ClientPtr = std::unique_ptr<esp_http_client, ClientDeleter>;

// "https://host:port" without user info, path or query, any of which may
// carry a credential.
std::string origin_of(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    const size_t at = url.rfind('@', end);
    const size_t host = at != std::string::npos && at >= start ? at + 1 : start;
    return url.substr(0, start) + url.substr(host, end - host);
}

bool method_of(net::Method m, esp_http_client_method_t& out, const char*& name) {
    switch (m) {
        case net::Method::Get:
            out = HTTP_METHOD_GET;
            name = "GET";
            return true;
        case net::Method::Post:
            out = HTTP_METHOD_POST;
            name = "POST";
            return true;
        case net::Method::Put:
            out = HTTP_METHOD_PUT;
            name = "PUT";
            return true;
        case net::Method::Delete:
            out = HTTP_METHOD_DELETE;
            name = "DELETE";
            return true;
        case net::Method::Other:
            break;
    }
    return false;
}

// What went wrong underneath a failed open/read, from the transport's
// last esp-tls error (also recorded for plain TCP connections).
net::TransportError classify(esp_http_client_handle_t client, esp_err_t fallback_err) {
    int tls_code = 0;
    int tls_flags = 0;
    const esp_err_t tls =
        esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
    switch (tls) {
        case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
            return net::TransportError::Dns;
        case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:
        case ESP_ERR_ESP_TLS_UNSUPPORTED_PROTOCOL_FAMILY:
        case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
        case ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED:
            return net::TransportError::Connect;
        case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
        case ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT:
            return net::TransportError::Timeout;
        case ESP_OK:
            break;
        default:
            // ESP_ERR_MBEDTLS_*: certificate verification, handshake and
            // record-layer failures.
            if (tls >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK && tls < ESP_ERR_ESP_TLS_BASE + 0x100) {
                return net::TransportError::Tls;
            }
            break;
    }
    switch (fallback_err) {
        case ESP_ERR_HTTP_EAGAIN:
        case ESP_ERR_HTTP_READ_TIMEOUT:
        case ESP_ERR_TIMEOUT:
            return net::TransportError::Timeout;
        case ESP_ERR_HTTP_CONNECT:
            return net::TransportError::Connect;
        default:
            return net::TransportError::Other;
    }
}

}  // namespace

net::HttpClientResponse EspHttpClient::request(const net::HttpClientRequest& req) {
    net::HttpClientResponse res;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    const char* method_name = nullptr;
    if (!method_of(req.method, method, method_name)) {
        res.transport = net::TransportError::Other;
        return res;
    }
    const std::string origin = origin_of(req.url);
    const auto failed = [&](net::TransportError e, const char* stage, esp_err_t err) {
        WLOG_W(kTag, "%s %s failed at %s: %s (%s)", method_name, origin.c_str(), stage,
               esp_err_to_name(err), net::to_string(e));
        res.transport = e;
        res.status = 0;
        res.body.clear();
        return res;
    };

    esp_http_client_config_t cfg = {};
    cfg.url = req.url.c_str();
    cfg.method = method;
    cfg.timeout_ms = static_cast<int>(req.timeout_ms);
    cfg.disable_auto_redirect = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // Explicit, so no default can turn off the host name check that keeps
    // the API key from reaching an impostor.
    cfg.skip_cert_common_name_check = false;
    cfg.buffer_size = kRxBufferBytes;
    cfg.buffer_size_tx = kTxBufferBytes;
    ClientPtr client(esp_http_client_init(&cfg));
    if (!client) return failed(net::TransportError::Other, "init", ESP_ERR_INVALID_ARG);

    for (const auto& [name, value] : req.headers) {
        esp_http_client_set_header(client.get(), name.c_str(), value.c_str());
    }

    esp_err_t err = esp_http_client_open(client.get(), static_cast<int>(req.body.size()));
    if (err != ESP_OK) return failed(classify(client.get(), err), "connect", err);

    size_t sent = 0;
    while (sent < req.body.size()) {
        const int n = esp_http_client_write(client.get(), req.body.data() + sent,
                                            static_cast<int>(req.body.size() - sent));
        if (n <= 0) {
            return failed(classify(client.get(), ESP_ERR_HTTP_WRITE_DATA), "send",
                          ESP_ERR_HTTP_WRITE_DATA);
        }
        sent += static_cast<size_t>(n);
    }

    const int64_t length = esp_http_client_fetch_headers(client.get());
    if (length == -ESP_ERR_HTTP_EAGAIN) {
        return failed(net::TransportError::Timeout, "headers", ESP_ERR_HTTP_EAGAIN);
    }
    if (length < 0) {
        return failed(classify(client.get(), ESP_ERR_HTTP_FETCH_HEADER), "headers",
                      ESP_ERR_HTTP_FETCH_HEADER);
    }
    if (static_cast<uint64_t>(length) > kMaxResponseBytes) {
        return failed(net::TransportError::Other, "body (too large)", ESP_ERR_INVALID_SIZE);
    }
    res.status = esp_http_client_get_status_code(client.get());

    char buf[512];
    for (;;) {
        const int n = esp_http_client_read(client.get(), buf, sizeof(buf));
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            return failed(net::TransportError::Timeout, "body", ESP_ERR_HTTP_EAGAIN);
        }
        if (n < 0) {
            return failed(classify(client.get(), ESP_FAIL), "body", ESP_FAIL);
        }
        if (n == 0) break;
        if (res.body.size() + static_cast<size_t>(n) > kMaxResponseBytes) {
            return failed(net::TransportError::Other, "body (too large)", ESP_ERR_INVALID_SIZE);
        }
        res.body.append(buf, static_cast<size_t>(n));
    }
    // A body delimited by the connection closing has nothing to check.
    const bool delimited = length > 0 || esp_http_client_is_chunked_response(client.get());
    if (delimited && !esp_http_client_is_complete_data_received(client.get())) {
        return failed(net::TransportError::Other, "body (incomplete)",
                      ESP_ERR_HTTP_INCOMPLETE_DATA);
    }
    esp_http_client_close(client.get());
    return res;
}

}  // namespace wisp::port::esp
