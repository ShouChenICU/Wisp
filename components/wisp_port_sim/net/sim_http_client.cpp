#include "wisp/port/sim/sim_http_client.hpp"

#include <curl/curl.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "wisp/core/log.hpp"

namespace wisp::port::sim {

namespace {

constexpr const char* kTag = "http_client";

// What the curl callbacks share for one transfer.
struct Transfer {
    const net::HttpClientRequest* req = nullptr;
    net::HttpClientResponse* res = nullptr;
    size_t limit = 0;
    size_t received = 0;
    bool too_large = false;
    bool truncated = false;
    bool cancelled = false;
    CURL* curl = nullptr;
    bool status_sent = false;  // req->on_status has been called
};

// The final response's status to req->on_status, once: from the first body
// piece (curl knows it by then), or after a transfer without a body.
void send_status(Transfer* t) {
    if (t->status_sent) return;
    t->status_sent = true;
    if (!t->req->on_status) return;
    long status = 0;
    curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
    t->req->on_status(static_cast<int>(status));
}

// The caller cancelled, or its watch fired (curl keeps the deadline by
// itself too, with CURLOPT_TIMEOUT_MS).
bool stop_requested(const Transfer* t) {
    return (t->req->cancelled && t->req->cancelled()) ||
           (t->req->watch != nullptr && t->req->watch->reason() != net::RequestWatch::Reason::None);
}

size_t on_data(char* data, size_t size, size_t nmemb, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * nmemb;
    // Returning less than n makes curl fail with CURLE_WRITE_ERROR.
    if (stop_requested(t)) {
        t->cancelled = true;
        return 0;
    }
    if (n > t->limit - t->received) {
        if (!t->req->truncate_at_limit) {
            t->too_large = true;
            return 0;
        }
        // Keep exactly up to the limit, then stop the transfer: a
        // successful, truncated response, not an error (the Agent
        // principal's cap, agent-tools spec 3.1/3.2).
        const size_t take = t->limit - t->received;
        t->truncated = true;
        if (take > 0) {
            t->received += take;
            send_status(t);
            if (t->req->on_body) {
                if (!t->req->on_body({reinterpret_cast<const uint8_t*>(data), take})) {
                    t->cancelled = true;
                }
            } else {
                t->res->body.append(data, take);
            }
        }
        return 0;
    }
    t->received += n;
    send_status(t);
    if (t->req->on_body) {
        if (!t->req->on_body({reinterpret_cast<const uint8_t*>(data), n})) {
            t->cancelled = true;
            return 0;
        }
    } else {
        t->res->body.append(data, n);
    }
    return n;
}

// One header line ("Name: value\r\n"), or a status line, which starts a
// new response (an interim 1xx came before it), or the blank line.
size_t on_header(char* data, size_t size, size_t nmemb, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * nmemb;
    std::string_view line(data, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (line.rfind("HTTP/", 0) == 0) {
        t->res->headers.clear();
    } else if (size_t colon = line.find(':'); colon != std::string_view::npos) {
        net::keep_response_header(t->res->headers, line.substr(0, colon), line.substr(colon + 1));
    }
    return n;
}

// Called about once a second while curl waits, and as data moves.
int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(user);
    if (stop_requested(t)) {
        t->cancelled = true;
        return 1;  // CURLE_ABORTED_BY_CALLBACK
    }
    return 0;
}

// "https://host:port" without user info, path or query, any of which may
// carry a credential.
std::string origin_of(const std::string& url) {
    size_t scheme = url.find("://");
    size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    size_t at = url.rfind('@', end);
    size_t host = at != std::string::npos && at >= start ? at + 1 : start;
    return url.substr(0, start) + url.substr(host, end - host);
}

const char* method_name(net::Method m) {
    switch (m) {
        case net::Method::Get:
            return "GET";
        case net::Method::Post:
            return "POST";
        case net::Method::Put:
            return "PUT";
        case net::Method::Delete:
            return "DELETE";
        case net::Method::Patch:
            return "PATCH";
        case net::Method::Head:
            return "HEAD";
        case net::Method::Other:
            break;
    }
    return nullptr;
}

}  // namespace

SimHttpClient::SimHttpClient() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

net::TransportError SimHttpClient::transport_error_for(int curl_code) {
    switch (static_cast<CURLcode>(curl_code)) {
        case CURLE_OK:
            return net::TransportError::None;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return net::TransportError::Dns;
        case CURLE_COULDNT_CONNECT:
            return net::TransportError::Connect;
        case CURLE_OPERATION_TIMEDOUT:
            return net::TransportError::Timeout;
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_ENGINE_NOTFOUND:
        case CURLE_SSL_ENGINE_SETFAILED:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_ENGINE_INITFAILED:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_SHUTDOWN_FAILED:
        case CURLE_SSL_CRL_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        case CURLE_SSL_INVALIDCERTSTATUS:
        case CURLE_SSL_CLIENTCERT:
            return net::TransportError::Tls;
        default:
            return net::TransportError::Other;
    }
}

net::HttpClientResponse SimHttpClient::request(const net::HttpClientRequest& req) {
    net::HttpClientResponse res;
    const char* method = method_name(req.method);
    if (method == nullptr) {
        res.transport = net::TransportError::Other;
        return res;
    }
    if (req.cancelled && req.cancelled()) {
        res.transport = net::TransportError::Cancelled;
        return res;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        WLOG_E(kTag, "curl_easy_init failed");
        res.transport = net::TransportError::Other;
        return res;
    }

    curl_slist* headers = nullptr;
    for (const auto& [name, value] : req.headers) {
        std::string line = name + ": " + value;
        headers = curl_slist_append(headers, line.c_str());
    }
    // Without this curl adds "Expect: 100-continue" to larger POST bodies
    // and stalls a second waiting for a reply some servers never send.
    headers = curl_slist_append(headers, "Expect:");

    Transfer t;
    t.req = &req;
    t.res = &res;
    t.curl = curl;
    t.limit = req.max_response_bytes != 0 ? req.max_response_bytes : kMaxResponseBytes;
    const char* body =
        req.body_ref.empty() ? req.body.data() : reinterpret_cast<const char*>(req.body_ref.data());
    const size_t body_size = req.body_ref.empty() ? req.body.size() : req.body_ref.size();

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (req.method == net::Method::Get) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (req.method == net::Method::Head) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (req.method != net::Method::Delete || body_size != 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body_size));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.timeout_ms));
    // Signals cannot be used for timeouts in a multi-threaded process.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    if (req.cancelled || req.watch != nullptr) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    // Refuses a too-large Content-Length before any of the body arrives;
    // with truncate_at_limit the body itself is capped instead (on_data),
    // so a Content-Length past the limit is not refused up front.
    curl_easy_setopt(
        curl, CURLOPT_MAXFILESIZE_LARGE,
        req.truncate_at_limit ? static_cast<curl_off_t>(0) : static_cast<curl_off_t>(t.limit));
    // Same scope as the device's client: plain http and https only, no
    // redirects (a redirect could carry the Authorization header elsewhere).
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    // Explicit, so no build or environment default can turn off the
    // certificate checks that keep the API key from reaching an impostor.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    CURLcode rc = curl_easy_perform(curl);
    if (req.watch != nullptr && req.watch->reason() == net::RequestWatch::Reason::Deadline &&
        rc != CURLE_OK) {
        res.transport = net::TransportError::Timeout;
    } else if (t.cancelled || rc == CURLE_ABORTED_BY_CALLBACK) {
        res.transport = net::TransportError::Cancelled;
    } else if (t.truncated) {
        // on_data() stopped the transfer deliberately, after keeping the
        // first max_response_bytes of the body (truncate_at_limit): a
        // success, not CURLE_WRITE_ERROR.
        res.transport = net::TransportError::None;
    } else if (t.too_large || rc == CURLE_FILESIZE_EXCEEDED) {
        res.transport = net::TransportError::TooLarge;
    } else {
        res.transport = transport_error_for(rc);
    }
    if (res.transport == net::TransportError::None) {
        send_status(&t);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.status = static_cast<int>(status);
        res.truncated = t.truncated;
    } else {
        res.body.clear();
        res.headers.clear();
        if (res.transport != net::TransportError::Cancelled) {
            WLOG_W(kTag, "%s %s failed: curl error %d (%s)", method, origin_of(req.url).c_str(),
                   static_cast<int>(rc), net::to_string(res.transport));
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

}  // namespace wisp::port::sim
