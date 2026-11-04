#include "wisp/port/esp/esp_http_client.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_tls_errors.h"
#include "sys/socket.h"
#include "wisp/core/log.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/net/tls_gate.hpp"
#include "wisp/os/os.hpp"

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

// Disarms the watch (if any) when it goes out of scope. Declared after
// the ClientPtr, so it runs before esp_http_client_cleanup() closes the
// socket the watch's stop() shuts down: the descriptor cannot be closed,
// and reused by another connection, under a stop() still to come.
struct Disarm {
    net::RequestWatch* watch;
    ~Disarm() {
        if (watch != nullptr) watch->disarm();
    }
};

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

bool is_https(const std::string& url) {
    constexpr char kScheme[] = "https:";
    if (url.size() < sizeof(kScheme) - 1) return false;
    for (size_t i = 0; i < sizeof(kScheme) - 1; ++i)
        if (std::tolower(static_cast<unsigned char>(url[i])) != kScheme[i]) return false;
    return true;
}

// HTTP_EVENT_ON_HEADER: one response header. Runs inside the client's
// own calls, on the calling task.
esp_err_t on_event(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data != nullptr &&
        evt->header_key != nullptr && evt->header_value != nullptr) {
        auto* res = static_cast<net::HttpClientResponse*>(evt->user_data);
        net::keep_response_header(res->headers, evt->header_key, evt->header_value);
    }
    return ESP_OK;
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
    // The caller's watch, armed with the deadline; without one, only the
    // per-call clamping below bounds the request.
    net::RequestWatch* const watch = req.watch;
    const auto fired = [watch] {
        return watch != nullptr ? watch->reason() : net::RequestWatch::Reason::None;
    };
    const auto failed = [&](net::TransportError e, const char* stage, esp_err_t err) {
        // Once the watch has fired, whatever broke was its doing (a read
        // on the shut-down socket fails as EOF or a socket error).
        switch (fired()) {
            case net::RequestWatch::Reason::Deadline:
                e = net::TransportError::Timeout;
                err = ESP_ERR_TIMEOUT;
                break;
            case net::RequestWatch::Reason::Aborted:
                e = net::TransportError::Cancelled;
                break;
            case net::RequestWatch::Reason::None:
                break;
        }
        // A cancel is the caller's own doing, not a failure worth a warning.
        if (e != net::TransportError::Cancelled) {
            WLOG_W(kTag, "%s %s failed at %s: %s (%s)", method_name, origin.c_str(), stage,
                   esp_err_to_name(err), net::to_string(e));
        }
        res.transport = e;
        res.status = 0;
        res.body.clear();
        res.headers.clear();
        return res;
    };
    const auto cancelled = [&] {
        return (req.cancelled && req.cancelled()) || fired() == net::RequestWatch::Reason::Aborted;
    };
    // req.timeout_ms bounds the whole request. esp_http_client applies its
    // timeout to each low-level operation, so each call is also given only
    // what is left (left(), at least 1 ms while any is); that alone cannot
    // stop the header fetch or a body read, which loop over many socket
    // reads inside one call. After open() the caller's watch does: at the
    // deadline or on abort it shuts the socket down and the call fails.
    const uint64_t deadline = os::now_ms() + req.timeout_ms;
    const auto left = [deadline]() -> int {
        const uint64_t now = os::now_ms();
        if (now >= deadline) return 0;
        return static_cast<int>(std::min<uint64_t>(deadline - now, INT_MAX));
    };
    const size_t limit = req.max_response_bytes != 0 ? req.max_response_bytes : kMaxResponseBytes;
    const char* body =
        req.body_ref.empty() ? req.body.data() : reinterpret_cast<const char*>(req.body_ref.data());
    const size_t body_size = req.body_ref.empty() ? req.body.size() : req.body_ref.size();
    if (cancelled()) return failed(net::TransportError::Cancelled, "start", ESP_OK);

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
    cfg.event_handler = on_event;
    cfg.user_data = &res;
    ClientPtr client(esp_http_client_init(&cfg));
    if (!client) return failed(net::TransportError::Other, "init", ESP_ERR_INVALID_ARG);
    const Disarm disarm{watch};

    for (const auto& [name, value] : req.headers) {
        esp_http_client_set_header(client.get(), name.c_str(), value.c_str());
    }

    // False once the deadline has passed; otherwise the next operation's
    // timeout becomes what is left of it.
    const auto arm = [&] {
        const int ms = left();
        if (ms > 0) esp_http_client_set_timeout_ms(client.get(), ms);
        return ms > 0;
    };

    esp_err_t err = ESP_OK;
    {
        // Connect and handshake under the process-wide TLS gate (one
        // handshake at a time, wisp/net/tls_gate.hpp), released before
        // the request is sent. Waiting for it counts against the deadline
        // and ends early on a cancel.
        std::optional<net::TlsGate::Hold> gate;
        if (is_https(req.url)) {
            gate.emplace(net::TlsGate::process(), static_cast<uint32_t>(left()),
                         [&] { return cancelled() || fired() != net::RequestWatch::Reason::None; });
            if (!gate->held()) {
                if (cancelled()) return failed(net::TransportError::Cancelled, "tls gate", ESP_OK);
                return failed(net::TransportError::Timeout, "tls gate", ESP_ERR_TIMEOUT);
            }
        }
        if (!arm()) return failed(net::TransportError::Timeout, "connect", ESP_ERR_TIMEOUT);
        err = esp_http_client_open(client.get(), static_cast<int>(body_size));
    }
    if (err != ESP_OK) return failed(classify(client.get(), err), "connect", err);
    // From here until the Disarm above runs, the transport keeps this
    // socket open (esp_http_client closes it only in close(), cleanup(),
    // set_url(), perform() and a failed open()). If the watch has already
    // fired, attach() shuts it down at once.
    //
    // SHUT_RD only. On lwIP (IDF v6.0.3, LWIP_NETCONN_FULLDUPLEX) it marks
    // the netconn's receive mailbox invalid, which wakes a task blocked in
    // recv(), and raises NETCONN_EVT_RCVPLUS, which wakes select() (the
    // transports poll with it before each read); every read then fails or
    // sees EOF. It does not wait: lwip_netconn_do_close() just runs
    // tcp_shutdown(pcb, 1, 0), which queues no FIN, whereas a full close
    // (SHUT_RDWR) can wait out an ERR_MEM retry loop, on the timer task
    // every os::Timer shares. Writes need no interrupt: each is bounded by
    // the clamped send timeout.
    const int fd = esp_http_client_get_socket(client.get());
    if (watch != nullptr && fd >= 0) watch->attach([fd] { shutdown(fd, SHUT_RD); });
    if (fired() != net::RequestWatch::Reason::None) {
        return failed(net::TransportError::Other, "connect", ESP_FAIL);
    }

    size_t sent = 0;
    while (sent < body_size) {
        if (cancelled()) return failed(net::TransportError::Cancelled, "send", ESP_OK);
        if (!arm()) return failed(net::TransportError::Timeout, "send", ESP_ERR_TIMEOUT);
        const int n =
            esp_http_client_write(client.get(), body + sent, static_cast<int>(body_size - sent));
        if (n <= 0) {
            return failed(classify(client.get(), ESP_ERR_HTTP_WRITE_DATA), "send",
                          ESP_ERR_HTTP_WRITE_DATA);
        }
        sent += static_cast<size_t>(n);
    }

    if (!arm()) return failed(net::TransportError::Timeout, "headers", ESP_ERR_TIMEOUT);
    const int64_t length = esp_http_client_fetch_headers(client.get());
    if (length == -ESP_ERR_HTTP_EAGAIN) {
        return failed(net::TransportError::Timeout, "headers", ESP_ERR_HTTP_EAGAIN);
    }
    if (length < 0) {
        return failed(classify(client.get(), ESP_ERR_HTTP_FETCH_HEADER), "headers",
                      ESP_ERR_HTTP_FETCH_HEADER);
    }
    if (static_cast<uint64_t>(length) > limit) {
        return failed(net::TransportError::TooLarge, "body (too large)", ESP_ERR_INVALID_SIZE);
    }
    res.status = esp_http_client_get_status_code(client.get());

    // On the caller's stack: the scene proxy's task has its stack in PSRAM,
    // so this costs no internal RAM there.
    char buf[512];
    size_t received = 0;
    for (;;) {
        if (cancelled()) return failed(net::TransportError::Cancelled, "body", ESP_OK);
        if (!arm()) return failed(net::TransportError::Timeout, "body", ESP_ERR_TIMEOUT);
        const int n = esp_http_client_read(client.get(), buf, sizeof(buf));
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            return failed(net::TransportError::Timeout, "body", ESP_ERR_HTTP_EAGAIN);
        }
        if (n < 0) {
            return failed(classify(client.get(), ESP_FAIL), "body", ESP_FAIL);
        }
        if (n == 0) break;
        if (static_cast<size_t>(n) > limit - received) {
            return failed(net::TransportError::TooLarge, "body (too large)", ESP_ERR_INVALID_SIZE);
        }
        received += static_cast<size_t>(n);
        if (req.on_body) {
            if (!req.on_body({reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n)})) {
                return failed(net::TransportError::Cancelled, "body", ESP_OK);
            }
        } else {
            res.body.append(buf, static_cast<size_t>(n));
        }
    }
    // A read on a socket the watch shut down can end like a clean EOF.
    if (fired() != net::RequestWatch::Reason::None) {
        return failed(net::TransportError::Other, "body", ESP_FAIL);
    }
    // A body delimited by the connection closing has nothing to check.
    const bool delimited = length > 0 || esp_http_client_is_chunked_response(client.get());
    if (delimited && !esp_http_client_is_complete_data_received(client.get())) {
        return failed(net::TransportError::Other, "body (incomplete)",
                      ESP_ERR_HTTP_INCOMPLETE_DATA);
    }
    if (watch != nullptr) watch->disarm();
    esp_http_client_close(client.get());
    return res;
}

}  // namespace wisp::port::esp
