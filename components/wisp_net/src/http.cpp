#include "wisp/net/http.hpp"

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

}  // namespace wisp::net
