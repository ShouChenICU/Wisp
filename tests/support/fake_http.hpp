#pragma once

// Test-only builders for in-memory net::HttpRequest values, for driving
// wisp::admin::AdminServer::handle() without a socket. Header names are
// written lowercase, as every HttpServer implementation delivers them.
// Each helper takes and returns the request by value so calls nest:
//   srv.handle(with_csrf(with_token(make_req(Method::Post, "/x"), t)))

#include <string>
#include <string_view>
#include <utility>

#include "wisp/net/http.hpp"

namespace wisp::test {

// A request from a browser on the home network (or, with Iface::Ap, on
// the setup hotspot) addressed to the device's IP.
inline net::HttpRequest make_req(net::Method method, std::string path, std::string body = {},
                                 net::Iface iface = net::Iface::Sta) {
    net::HttpRequest req;
    req.method = method;
    req.path = std::move(path);
    req.body = std::move(body);
    req.iface = iface;
    req.remote_ip = iface == net::Iface::Ap ? "192.168.4.2" : "192.168.1.23";
    req.headers["host"] = "192.168.1.50";
    req.headers["user-agent"] =
        "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/130.0.0.0 Mobile Safari/537.36";
    return req;
}

inline net::HttpRequest host(net::HttpRequest req, std::string value) {
    req.headers["host"] = std::move(value);
    return req;
}

// Appends wisp_token=<token> to the Cookie header, keeping any pairs
// already there.
inline net::HttpRequest with_token(net::HttpRequest req, std::string_view token) {
    std::string& cookie = req.headers["cookie"];
    if (!cookie.empty()) cookie += "; ";
    cookie += "wisp_token=";
    cookie += token;
    return req;
}

inline net::HttpRequest with_csrf(net::HttpRequest req) {
    req.headers["x-wisp"] = "1";
    return req;
}

// Value of the first response header named `name` (case-insensitive), or
// "" when absent.
inline std::string response_header(const net::HttpResponse& res, std::string_view name) {
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    };
    const std::string want = lower(name);
    for (const auto& [k, v] : res.headers) {
        if (lower(k) == want) return v;
    }
    return {};
}

inline bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

inline std::string set_cookie(const net::HttpResponse& res) {
    return response_header(res, "set-cookie");
}

}  // namespace wisp::test
