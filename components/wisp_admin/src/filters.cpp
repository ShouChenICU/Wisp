#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "admin_internal.hpp"

namespace wisp::admin::detail {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Four decimal octets 0-255, each 1-3 digits. Anything else -- including
// names that merely start with an address -- is a hostname, and a
// hostname other than ours is what a DNS-rebinding page would send.
bool is_ipv4(std::string_view s) {
    int parts = 0;
    while (true) {
        size_t dot = s.find('.');
        std::string_view part = s.substr(0, dot);
        if (part.empty() || part.size() > 3) return false;
        int value = 0;
        for (char c : part) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        if (value > 255) return false;
        ++parts;
        if (dot == std::string_view::npos) break;
        s.remove_prefix(dot + 1);
    }
    return parts == 4;
}

// "name" or "name:port" -> "name"; nullopt for a malformed port. A Host
// with more than one ':' (an IPv6 literal) is left whole and so never
// matches an accepted name.
std::optional<std::string_view> strip_port(std::string_view host) {
    size_t colon = host.find(':');
    if (colon == std::string_view::npos || host.find(':', colon + 1) != std::string_view::npos) {
        return host;
    }
    std::string_view port = host.substr(colon + 1);
    if (port.empty() || port.size() > 5) return std::nullopt;
    for (char c : port) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    return host.substr(0, colon);
}

// The URLs phones, laptops and browsers fetch to detect a captive portal
// (Android, Apple, Windows, Firefox, Ubuntu).
constexpr std::string_view kProbePaths[] = {
    "/generate_204",    "/gen_204",  "/hotspot-detect.html", "/library/test/success.html",
    "/connecttest.txt", "/ncsi.txt", "/canonical.html",      "/success.txt",
};

}  // namespace

bool host_allowed(const net::HttpRequest& req) {
    auto name = strip_port(req.header("host"));
    if (!name || name->empty()) return false;
    return is_ipv4(*name) || iequals(*name, "wisp.local") || iequals(*name, "localhost");
}

bool is_probe_path(std::string_view path) {
    for (std::string_view p : kProbePaths) {
        if (path == p) return true;
    }
    return false;
}

std::optional<net::HttpResponse> setup_redirect(const AdminDeps& deps) {
    // ap_session() also carries the hotspot password; only the IP is
    // used here.
    auto session = deps.wifi.ap_session();
    if (!session || session->ip.empty()) return std::nullopt;
    net::HttpResponse res;
    res.status = 302;
    res.content_type = "text/plain";
    // 80 is the URL default; any other port must be spelled out.
    std::string authority = session->ip;
    if (deps.http_port != 80) authority += ":" + std::to_string(deps.http_port);
    res.headers.emplace_back("Location", "http://" + authority + "/setup.html");
    res.headers.emplace_back("Cache-Control", "no-store");
    return res;
}

net::HttpResponse probe_response(const AdminDeps& deps, const net::HttpRequest& req) {
    if (req.iface == net::Iface::Ap) {
        if (auto redirect = setup_redirect(deps)) return std::move(*redirect);
    }
    return net::HttpResponse::error(404, "not_found", "no such resource");
}

bool is_mutating(net::Method m) {
    return m == net::Method::Post || m == net::Method::Put || m == net::Method::Delete;
}

bool csrf_ok(const net::HttpRequest& req) { return req.header("x-wisp") == "1"; }

}  // namespace wisp::admin::detail
