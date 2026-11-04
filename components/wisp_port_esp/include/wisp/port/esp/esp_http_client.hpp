#pragma once

// net::HttpClient on ESP-IDF's esp_http_client (connectivity spec 2.1):
// the model connectivity test's outgoing requests. HTTPS servers are
// verified against the IDF certificate bundle (esp_crt_bundle), host
// name included; there is no way to turn verification off. Plain http
// is allowed (a gateway on the LAN). Redirects are not followed: one
// could carry the Authorization header to another host.
//
// Each request() uses its own client handle, so concurrent calls are
// fine. The response body is capped at HttpClientRequest::
// max_response_bytes, or kMaxResponseBytes when that is 0 (a larger one
// fails the request with TooLarge), so a misbehaving endpoint cannot
// exhaust memory. Response headers are collected from
// HTTP_EVENT_ON_HEADER; with on_body the body goes there 512 bytes at a
// time instead of into `body`. cancelled is asked before each write and
// read: a read blocked on a silent server notices only when it times out
// (HttpClientRequest::timeout_ms).
//
// Errors map to TransportError from the esp_http_client result and the
// last esp-tls error: name resolution -> Dns, refused/unreachable ->
// Connect, certificate or handshake failure -> Tls, timeouts -> Timeout,
// anything else -> Other.
//
// Secrets: request bodies and headers (the Authorization header carries
// the API key) are never logged; a failure logs the method, the
// scheme://host:port part of the URL and the IDF error names only.

#include <cstddef>

#include "wisp/net/http.hpp"

namespace wisp::port::esp {

class EspHttpClient : public net::HttpClient {
public:
    static constexpr size_t kMaxResponseBytes = 64 * 1024;

    net::HttpClientResponse request(const net::HttpClientRequest& req) override;
};

}  // namespace wisp::port::esp
