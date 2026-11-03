#pragma once

// Simulator net::HttpClient on libcurl (the only place in the project that
// uses it; portable code reaches it through app::Platform::http_client).
// Each request() uses its own easy handle, so concurrent calls from
// several HTTP server threads are fine.
//
// Secrets: request bodies and headers (an Authorization header carries
// the LLM API key) are never logged; a failed request logs only the
// method, scheme://host part of the URL and curl's error code.

#include <cstddef>

#include "wisp/net/http.hpp"

namespace wisp::port::sim {

class SimHttpClient : public net::HttpClient {
public:
    // A larger response body aborts the transfer (transport Other), so a
    // misbehaving endpoint cannot exhaust memory.
    static constexpr size_t kMaxResponseBytes = 1024 * 1024;

    // Initializes libcurl's global state once per process; construct the
    // first client before spawning threads that use it.
    SimHttpClient();

    net::HttpClientResponse request(const net::HttpClientRequest& req) override;

    // Maps a CURLcode (passed as int so this header needs no curl/ include)
    // to the transport error the admin page reports: resolve failures ->
    // Dns, refused/unreachable -> Connect, any CURLE_SSL_* or certificate
    // verification failure -> Tls, timeouts -> Timeout, CURLE_OK -> None,
    // everything else -> Other.
    static net::TransportError transport_error_for(int curl_code);
};

}  // namespace wisp::port::sim
