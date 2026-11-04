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
// read.
//
// Deadline: HttpClientRequest::timeout_ms is the whole request's.
// esp_http_client applies its timeout to each low-level operation, so
// every call is given only what is left of it; but its header fetch and
// body read loop over many socket reads inside one call, so a server that
// trickles bytes would keep one call alive past any deadline checked
// between calls. With HttpClientRequest::watch (the scene HTTP proxy
// passes one), the watch ends that: once the connection is open it shuts
// the socket's read side down (shutdown(SHUT_RD), not close: the
// descriptor stays ours until the watch is disarmed, just before the
// client closes it) at the deadline or on abort, the blocked call fails
// at once and the request reports Timeout or Cancelled. Without a watch
// (the model connectivity test) only the per-call clamping applies. The
// connect phase can overrun the deadline softly either way: DNS
// resolution has lwIP's own timeout and esp-tls checks its handshake
// timeout between socket reads, so a handshake can overrun what was left
// by up to one read timeout; a watch then ends the request right after
// open().
//
// TLS gate: an https request holds net::TlsGate::process() around
// esp_http_client_open() (connect and handshake) and nowhere else, so
// only one handshake runs at a time in the process (the internal-RAM
// budget, wisp/app/ram_budget.hpp). Waiting for it counts against the
// deadline (a wait that runs out is a Timeout) and ends early when the
// request is cancelled or its watch fires.
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
