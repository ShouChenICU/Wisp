// Wi-Fi status, scanning and saved networks (spec 6.1). The hotspot
// password is never part of a response: the status comes from
// WifiService::ap_status(), which does not carry it.
//
// SSIDs are up to 32 raw bytes and need not be UTF-8, while JSON output
// replaces ill-formed UTF-8 with U+FFFD. Every SSID in a response is
// therefore paired with "ssid_hex", its bytes in lowercase hex, and
// POST /wifi/networks accepts that form back.

#include <optional>
#include <string>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/base/sha256.hpp"

namespace wisp::admin::detail {

namespace {

Value hex_of(const std::string& ssid) {
    return Value(to_hex({reinterpret_cast<const uint8_t*>(ssid.data()), ssid.size()}));
}

// Either case; nullopt for an odd length or a non-hex digit.
std::optional<std::string> from_hex(const std::string& s) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.size() % 2 != 0) return std::nullopt;
    std::string out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = digit(s[i]);
        const int lo = digit(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

net::HttpResponse wifi_status(AdminDeps& deps, const RequestContext&) {
    const net::WifiStatus st = deps.wifi.status();
    const net::ApStatus ap = deps.wifi.ap_status();
    List saved;
    for (const net::SavedNetwork& n : deps.wifi.saved_networks()) {
        saved.push_back(Value(Map{
            {"ssid", Value(n.ssid)},
            {"ssid_hex", hex_of(n.ssid)},
            {"auth_failed", Value(n.auth_failed)},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{
                                            {"state", Value(net::to_string(st.state))},
                                            {"ssid", Value(st.ssid)},
                                            {"ssid_hex", hex_of(st.ssid)},
                                            {"ip", Value(st.ip)},
                                            {"rssi", Value(st.rssi)},
                                            {"reason", Value(st.reason)},
                                            {"saved", Value(std::move(saved))},
                                            {"ap", Value(Map{
                                                       {"active", Value(ap.active)},
                                                       {"ssid", Value(ap.ssid)},
                                                       {"clients", Value(ap.clients)},
                                                   })},
                                        }));
}

// Blocks the HTTP thread for the duration of the scan (a few seconds).
net::HttpResponse wifi_scan(AdminDeps& deps, const RequestContext&) {
    auto scan = deps.wifi.scan();
    if (!scan) return service_error(scan.error());
    List networks;
    for (const net::ScanResult& n : scan.value()) {
        networks.push_back(Value(Map{
            {"ssid", Value(n.ssid)},
            {"ssid_hex", hex_of(n.ssid)},
            {"rssi", Value(n.rssi)},
            {"secured", Value(n.secured)},
            {"saved", Value(n.saved)},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{{"networks", Value(std::move(networks))}}));
}

// {ssid | ssid_hex, password?}: exactly one SSID form; a missing password
// means an open network. 202: the connection attempt happens later, on the
// net task.
net::HttpResponse add_network(AdminDeps& deps, const RequestContext& ctx) {
    const Value* ssid = ctx.body.find("ssid");
    const Value* ssid_hex = ctx.body.find("ssid_hex");
    const Value* password = ctx.body.find("password");
    const Value* given = ssid != nullptr ? ssid : ssid_hex;
    if ((ssid == nullptr) == (ssid_hex == nullptr) || given->type() != Value::Type::String ||
        (password != nullptr && password->type() != Value::Type::String)) {
        return bad_request("body must be {\"ssid\" or \"ssid_hex\": string, \"password\": string}");
    }
    std::optional<std::string> name = ssid != nullptr
                                          ? std::optional<std::string>(ssid->as_string())
                                          : from_hex(given->as_string());
    if (!name) return bad_request("ssid_hex must be an even number of hex digits");
    auto r = deps.wifi.add_network(
        *name, password != nullptr ? password->as_string() : std::string(), ctx.now_ms);
    if (!r) return service_error(r.error());
    return ok_json(202);
}

net::HttpResponse forget_network(AdminDeps& deps, const RequestContext& ctx) {
    std::optional<std::string> ssid = percent_decode(ctx.param);
    if (!ssid) return bad_request("malformed %-escape in SSID");
    auto r = deps.wifi.forget_network(*ssid);
    if (!r) return service_error(r.error());
    return no_content();
}

}  // namespace

void add_wifi_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/wifi", false, wifi_status});
    routes.push_back({Method::Get, "/wifi/scan", false, wifi_scan});
    routes.push_back({Method::Post, "/wifi/networks", false, add_network});
    routes.push_back({Method::Delete, "/wifi/networks/*", false, forget_network});
}

}  // namespace wisp::admin::detail
