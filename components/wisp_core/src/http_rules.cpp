#include "wisp/core/http_rules.hpp"

#include <cstring>

namespace wisp::core {

namespace {

bool has_control_or_space(std::string_view s) {
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return true;
    }
    return false;
}

bool is_tchar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return c != '\0' && std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

// The authority of an http(s) URL: what lies between "://" and the first
// '/', '?' or '#'. "" when the scheme is neither.
std::string_view authority_of(std::string_view url) {
    std::string_view rest;
    if (url.starts_with("http://")) {
        rest = url.substr(7);
    } else if (url.starts_with("https://")) {
        rest = url.substr(8);
    } else {
        return {};
    }
    return rest.substr(0, rest.find_first_of("/?#"));
}

}  // namespace

bool valid_http_url(std::string_view url) {
    std::string_view rest;
    if (url.starts_with("http://")) {
        rest = url.substr(7);
    } else if (url.starts_with("https://")) {
        rest = url.substr(8);
    } else {
        return false;
    }
    if (has_control_or_space(url)) return false;
    size_t host_end = rest.find_first_of("/?#");
    if (host_end == 0 || rest.empty()) return false;
    // A host must remain once user info and the port are taken off, the
    // same way url_origin() reads it.
    std::string_view host = rest.substr(0, host_end);
    if (size_t at = host.rfind('@'); at != std::string_view::npos) host.remove_prefix(at + 1);
    size_t colon = host.rfind(':');
    size_t bracket = host.rfind(']');
    if (colon != std::string_view::npos && (bracket == std::string_view::npos || colon > bracket)) {
        host = host.substr(0, colon);
    }
    if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
    return !host.empty();
}

std::string url_origin(std::string_view url) {
    if (url.empty()) return {};
    const bool https = url.starts_with("https://");
    std::string_view authority = url.substr(https ? 8 : 7);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (size_t at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);
    }
    std::string_view host = authority;
    std::string_view port;
    size_t colon = authority.rfind(':');
    size_t bracket = authority.rfind(']');
    if (colon != std::string_view::npos && (bracket == std::string_view::npos || colon > bracket)) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (port.empty()) port = https ? "443" : "80";
    std::string out = https ? "https://" : "http://";
    for (char c : host) out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    out += ':';
    out += port;
    return out;
}

bool url_has_userinfo(std::string_view url) {
    return authority_of(url).find('@') != std::string_view::npos;
}

bool http_header_syntax_ok(std::string_view name, std::string_view value) {
    if (name.empty()) return false;
    for (char c : name)
        if (!is_tchar(c)) return false;
    for (char c : value)
        if (c == '\r' || c == '\n' || c == '\0') return false;
    return true;
}

}  // namespace wisp::core
