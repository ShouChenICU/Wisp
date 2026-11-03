// Wi-Fi status, scanning and saved networks (spec 6.1). The hotspot
// password is never part of a response: the status comes from
// WifiService::ap_status(), which does not carry it.

#include <optional>
#include <string>
#include <vector>

#include "admin_internal.hpp"

namespace wisp::admin::detail {

namespace {

net::HttpResponse wifi_status(AdminDeps& deps, const RequestContext&) {
    const net::WifiStatus st = deps.wifi.status();
    const net::ApStatus ap = deps.wifi.ap_status();
    List saved;
    for (std::string& ssid : deps.wifi.saved_ssids()) saved.push_back(Value(std::move(ssid)));
    return net::HttpResponse::json(200, Value(Map{
                                            {"state", Value(net::to_string(st.state))},
                                            {"ssid", Value(st.ssid)},
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
            {"rssi", Value(n.rssi)},
            {"secured", Value(n.secured)},
            {"saved", Value(n.saved)},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{{"networks", Value(std::move(networks))}}));
}

// {ssid, password?}; a missing password means an open network. 202: the
// connection attempt happens later, on the net task.
net::HttpResponse add_network(AdminDeps& deps, const RequestContext& ctx) {
    const Value* ssid = ctx.body.find("ssid");
    const Value* password = ctx.body.find("password");
    if (ssid == nullptr || ssid->type() != Value::Type::String ||
        (password != nullptr && password->type() != Value::Type::String)) {
        return bad_request("body must be {\"ssid\": string, \"password\": string}");
    }
    auto r = deps.wifi.add_network(
        ssid->as_string(), password != nullptr ? password->as_string() : std::string(), ctx.now_ms);
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
