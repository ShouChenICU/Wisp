#include "wisp/port/sim/sim_http_client.hpp"

#include <curl/curl.h>

#include <mutex>
#include <string>

#include "wisp/core/log.hpp"

namespace wisp::port::sim {

namespace {

constexpr const char* kTag = "http_client";

size_t append_body(char* data, size_t size, size_t nmemb, void* user) {
    static_cast<std::string*>(user)->append(data, size * nmemb);
    return size * nmemb;
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

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (req.method == net::Method::Get) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (req.method != net::Method::Delete || !req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(req.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.timeout_ms));
    // Signals cannot be used for timeouts in a multi-threaded process.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    // Same scope as the device's client: plain http and https only, no
    // redirects (a redirect could carry the Authorization header elsewhere).
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    CURLcode rc = curl_easy_perform(curl);
    res.transport = transport_error_for(rc);
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.status = static_cast<int>(status);
    } else {
        res.body.clear();
        WLOG_W(kTag, "%s %s failed: curl error %d (%s)", method, origin_of(req.url).c_str(),
               static_cast<int>(rc), net::to_string(res.transport));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

}  // namespace wisp::port::sim
