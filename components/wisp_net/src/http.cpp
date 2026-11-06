#include "wisp/net/http.hpp"

#include <algorithm>
#include <cctype>

#include "wisp/base/json.hpp"

namespace wisp::net {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::string HttpRequest::header(std::string_view lower_name) const {
    std::string key(lower_name);
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = headers.find(key);
    return it == headers.end() ? std::string() : it->second;
}

std::string HttpRequest::cookie(std::string_view name) const {
    auto it = headers.find("cookie");
    if (it == headers.end() || name.empty()) return {};
    std::string_view rest = it->second;
    while (!rest.empty()) {
        size_t semi = rest.find(';');
        std::string_view pair = trim(rest.substr(0, semi));
        rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
        size_t eq = pair.find('=');
        if (eq == std::string_view::npos) continue;
        if (trim(pair.substr(0, eq)) == name) return std::string(trim(pair.substr(eq + 1)));
    }
    return {};
}

HttpResponse HttpResponse::json(int status, const Value& v) {
    HttpResponse r;
    r.status = status;
    r.body = to_json(v);
    return r;
}

HttpResponse HttpResponse::error(int status, std::string_view code, std::string_view message) {
    Map err{{"code", Value(std::string(code))}, {"message", Value(std::string(message))}};
    return json(status, Value(Map{{"error", Value(std::move(err))}}));
}

bool pump_stream(const HttpResponse& res, std::span<uint8_t> buf,
                 const std::function<bool(std::span<const uint8_t>)>& write) {
    if (!res.stream || buf.empty()) return false;
    if (buf.size() > kStreamChunkBytes) buf = buf.first(kStreamChunkBytes);
    const bool sized = res.stream_length > 0;
    uint64_t left = res.stream_length;
    while (!sized || left > 0) {
        std::span<uint8_t> piece = buf;
        if (sized && left < piece.size()) piece = piece.first(static_cast<size_t>(left));
        Result<size_t> got = res.stream(piece);
        if (!got) return false;
        const size_t n = std::min(got.value(), piece.size());
        if (n == 0) return !sized;  // dry: the end, or short of the promise
        if (!write(piece.first(n))) return false;
        if (sized) left -= n;
    }
    return true;
}

int http_status_for(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidArgument:
            return 400;
        case ErrorCode::PermissionDenied:
            return 403;
        case ErrorCode::NotFound:
            return 404;
        case ErrorCode::AlreadyExists:
        case ErrorCode::Busy:
        case ErrorCode::QuotaExceeded:
            return 409;
        case ErrorCode::RateLimited:
            return 429;
        case ErrorCode::Unsupported:
            return 501;
        case ErrorCode::Timeout:
            return 504;
        case ErrorCode::StorageUnavailable:
        case ErrorCode::NoMemory:
            return 503;
        default:
            return 500;
    }
}

const char* to_string(TransportError e) {
    switch (e) {
        case TransportError::None:
            return "";
        case TransportError::Dns:
            return "dns";
        case TransportError::Connect:
            return "connect";
        case TransportError::Tls:
            return "tls";
        case TransportError::Timeout:
            return "timeout";
        case TransportError::TooLarge:
            return "too_large";
        case TransportError::Cancelled:
            return "cancelled";
        case TransportError::Other:
            break;
    }
    return "other";
}

bool keep_response_header(std::vector<std::pair<std::string, std::string>>& headers,
                          std::string_view name, std::string_view value) {
    name = trim(name);
    value = trim(value);
    if (name.empty() || headers.size() >= kMaxResponseHeaders ||
        value.size() > kMaxResponseHeaderValue)
        return false;
    size_t used = 0;
    for (const auto& [n, v] : headers) used += n.size() + v.size();
    if (used + name.size() + value.size() > kMaxResponseHeaderBytes) return false;
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    headers.emplace_back(std::move(lower), std::string(value));
    return true;
}

}  // namespace wisp::net
