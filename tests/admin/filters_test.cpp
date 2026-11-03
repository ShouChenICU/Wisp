// AdminServer's request pipeline before routing (spec 6.3): Host check,
// captive-portal probes, static files, authentication, CSRF and body
// limits.

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "wisp/net/http.hpp"
#include "wisp/os/os.hpp"

using wisp::net::HttpResponse;
using wisp::net::Iface;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::contains;
using wisp::test::error_code;
using wisp::test::host;
using wisp::test::make_req;
using wisp::test::response_header;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

constexpr Method Get = Method::Get;
constexpr Method Post = Method::Post;

TEST(Filters, RejectsForeignHostButAllowsPortsAndAp) {
    AdminFixture f;
    auto& srv = *f.srv;
    HttpResponse bad = srv.handle(host(make_req(Get, "/api/v1/info"), "evil.com"));
    EXPECT_EQ(bad.status, 403);
    EXPECT_EQ(error_code(bad), "bad_host");
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), "192.168.1.50:80")).status, 200);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), "localhost:8080")).status, 200);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), "wisp.local")).status, 200);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), "WISP.LOCAL:80")).status, 200);

    // A DNS-rebinding attacker's name, however IP-like, is still a name.
    for (const char* h : {"", "192.168.1.50.evil.com", "192.168.1", "192.168.1.256", "1.2.3.4.5",
                          "wisp.local.evil.com", "localhost.evil.com", "[::1]:8080", ":80",
                          "1.2.3.4:80@evil", "1.2.3.4@evil.com", "wisp.local.", "192.168.1.50:"}) {
        EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), h)).status, 403) << h;
    }
    auto no_host = make_req(Get, "/api/v1/info");
    no_host.headers.erase("host");
    EXPECT_EQ(srv.handle(no_host).status, 403);

    // On the hotspot the hijacked DNS makes any name reach the device, so
    // probes pass with any Host and other foreign-Host GETs are steered
    // to the setup page.
    ASSERT_TRUE(f.wifi->start_provisioning(f.now));
    EXPECT_EQ(srv.handle(host(make_req(Get, "/generate_204", {}, Iface::Ap),
                              "connectivitycheck.gstatic.com"))
                  .status,
              302);
    HttpResponse steered =
        srv.handle(host(make_req(Get, "/api/v1/info", {}, Iface::Ap), "evil.com"));
    EXPECT_EQ(steered.status, 302);
    EXPECT_EQ(response_header(steered, "Location"), "http://192.168.4.1:8900/setup.html");
    EXPECT_EQ(srv.handle(host(make_req(Get, "/", {}, Iface::Ap), "evil.com")).status, 302);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info", {}, Iface::Ap), "192.168.4.1")).status,
              200);
}

// DNS rebinding over the hotspot: a page from evil.com rebound to the
// hotspot address sends same-origin requests that arrive on Iface::Ap
// with Host evil.com. It must not obtain a token or reach the API.
TEST(Filters, HotspotRebindingCannotPairOrCallApi) {
    AdminFixture f;
    auto& srv = *f.srv;
    ASSERT_TRUE(f.wifi->start_provisioning(f.now));

    HttpResponse pair =
        srv.handle(host(with_csrf(make_req(Post, "/api/v1/pair/ap", {}, Iface::Ap)), "evil.com"));
    EXPECT_EQ(pair.status, 403);
    EXPECT_EQ(error_code(pair), "bad_host");
    EXPECT_TRUE(wisp::test::set_cookie(pair).empty());
    EXPECT_TRUE(f.pairing->clients().empty());

    const std::string token = f.token();
    EXPECT_EQ(srv.handle(host(with_csrf(with_token(make_req(Post, "/api/v1/logout", {}, Iface::Ap),
                                                   token)),
                              "evil.com"))
                  .status,
              403);
    EXPECT_EQ(f.pairing->clients().size(), 1u);

    // Once the hotspot is gone there is no setup page to steer to.
    ASSERT_TRUE(f.wifi->stop_provisioning());
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info", {}, Iface::Ap), "evil.com")).status,
              403);
}

TEST(Filters, ProbePathsRedirectFromHotspotOnly) {
    AdminFixture f;
    ASSERT_TRUE(f.wifi->start_provisioning(f.now));
    for (const char* p :
         {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
          "/connecttest.txt", "/ncsi.txt", "/canonical.html", "/success.txt"}) {
        HttpResponse ap = f.srv->handle(host(make_req(Get, p, {}, Iface::Ap), "captive.apple.com"));
        EXPECT_EQ(ap.status, 302) << p;
        EXPECT_EQ(response_header(ap, "Location"), "http://192.168.4.1:8900/setup.html") << p;

        EXPECT_EQ(f.srv->handle(make_req(Get, p)).status, 404) << p;
    }
}

// Port 80 is the URL default, so the Location leaves it out.
TEST(Filters, SetupRedirectOmitsPort80) {
    AdminFixture f;
    ASSERT_TRUE(f.wifi->start_provisioning(f.now));
    auto deps = f.make_deps();
    deps.http_port = 80;
    wisp::admin::AdminServer on_80(std::move(deps));
    HttpResponse res =
        on_80.handle(host(make_req(Get, "/generate_204", {}, Iface::Ap), "captive.apple.com"));
    EXPECT_EQ(res.status, 302);
    EXPECT_EQ(response_header(res, "Location"), "http://192.168.4.1/setup.html");
}

// The hotspot's port-80 server answers every request -- any method, path or
// Host -- with the redirect to the admin port's setup page.
TEST(Filters, CaptiveHandlerRedirectsEverything) {
    AdminFixture f;
    ASSERT_TRUE(f.wifi->start_provisioning(f.now));
    wisp::net::HttpHandler captive = f.srv->captive_handler();
    for (const auto& [method, path, h] :
         std::vector<std::tuple<wisp::net::Method, std::string, std::string>>{
             {Get, "/generate_204", "connectivitycheck.gstatic.com"},
             {Get, "/", "192.168.4.1"},
             {Get, "/api/v1/info", "evil.com"},
             {Post, "/api/v1/pair/ap", "192.168.4.1"},
             {Get, "/some/deep/path?x=1", ""}}) {
        HttpResponse res = captive(host(make_req(method, path, {}, Iface::Ap), h));
        EXPECT_EQ(res.status, 302) << path;
        EXPECT_EQ(response_header(res, "Location"), "http://192.168.4.1:8900/setup.html") << path;
        EXPECT_EQ(response_header(res, "Cache-Control"), "no-store") << path;
    }
    EXPECT_TRUE(f.pairing->clients().empty());

    // Should a request race the hotspot going down, there is no page to
    // point at.
    ASSERT_TRUE(f.wifi->stop_provisioning());
    EXPECT_EQ(captive(make_req(Get, "/", {}, Iface::Ap)).status, 503);
}

// The port-80 server listens on every interface, but only hotspot clients
// may be sent to the hotspot address: a LAN client could not reach it.
TEST(Filters, CaptiveHandlerAnswersOnlyTheHotspot) {
    AdminFixture f;
    ASSERT_TRUE(f.wifi->start_provisioning(f.now));
    wisp::net::HttpHandler captive = f.srv->captive_handler();
    for (Iface iface : {Iface::Sta, Iface::Loopback}) {
        HttpResponse res = captive(host(make_req(Get, "/generate_204", {}, iface), "192.168.1.88"));
        EXPECT_EQ(res.status, 404);
        EXPECT_EQ(response_header(res, "Location"), "");
    }
}

// The router reads the lowercase "x-wisp" key and accepts only the exact
// value "1".
TEST(Filters, CsrfHeaderRequiredAnyCase) {
    AdminFixture f;
    auto& srv = *f.srv;
    const std::string token = f.token();
    auto r = with_token(make_req(Post, "/api/v1/logout"), token);
    HttpResponse refused = srv.handle(r);
    EXPECT_EQ(refused.status, 403);
    EXPECT_EQ(error_code(refused), "csrf");

    // Only the exact value "1" counts.
    r.headers["x-wisp"] = "0";
    EXPECT_EQ(srv.handle(r).status, 403);

    // The browser sends "X-Wisp: 1"; HttpServer implementations lowercase
    // header names, so the router only ever sees "x-wisp".
    r.headers["x-wisp"] = "1";
    EXPECT_EQ(srv.handle(r).status, 200);
}

TEST(Filters, CsrfAppliesToWhitelistedRoutesToo) {
    AdminFixture f;
    HttpResponse res = f.srv->handle(make_req(Post, "/api/v1/pair/start"));
    EXPECT_EQ(res.status, 403);
    EXPECT_EQ(error_code(res), "csrf");
    EXPECT_FALSE(f.pairing->active_code(f.now).has_value());
}

// wisp_token is picked out of a Cookie header carrying several pairs.
TEST(Filters, TokenIsFoundAmongSeveralCookies) {
    AdminFixture f;
    const std::string token = f.token();
    auto r = make_req(Post, "/api/v1/logout");
    r.headers["cookie"] = "theme=dark; wisp_token=" + token + "; lang=zh-CN";
    EXPECT_EQ(f.srv->handle(with_csrf(r)).status, 200);
}

TEST(Filters, UnauthenticatedApiIs401AndOptionsIs405) {
    AdminFixture f;
    auto& srv = *f.srv;

    HttpResponse anon = srv.handle(with_csrf(make_req(Post, "/api/v1/logout")));
    EXPECT_EQ(anon.status, 401);
    EXPECT_EQ(error_code(anon), "unauthorized");
    EXPECT_EQ(srv.handle(with_csrf(with_token(make_req(Post, "/api/v1/logout"), "bogus"))).status,
              401);
    // An unknown API path does not reveal itself to anonymous callers.
    EXPECT_EQ(srv.handle(make_req(Get, "/api/v1/nope")).status, 401);
    EXPECT_EQ(srv.handle(with_token(make_req(Get, "/api/v1/nope"), f.token())).status, 404);

    // The whitelist needs no token.
    EXPECT_EQ(srv.handle(make_req(Get, "/api/v1/info")).status, 200);

    for (const char* path : {"/api/v1/info", "/api/v1/logout", "/", "/index.html"}) {
        HttpResponse opt = srv.handle(make_req(Method::Other, path));
        EXPECT_EQ(opt.status, 405) << path;
        for (const auto& [k, v] : opt.headers) {
            EXPECT_NE(k.rfind("Access-Control", 0), 0u) << path;
        }
    }
}

TEST(Filters, OversizedBodyIs413) {
    AdminFixture f;
    const std::string token = f.token();
    const std::string big = "{\"code\":\"" + std::string(8192, '1') + "\"}";
    HttpResponse res =
        f.srv->handle(with_csrf(with_token(make_req(Post, "/api/v1/logout", big), token)));
    EXPECT_EQ(res.status, 413);

    // Exactly 8 KiB is still accepted (and then judged as JSON).
    std::string edge = "{\"pad\":\"\"}";
    edge.insert(8, 8192 - edge.size(), 'x');
    ASSERT_EQ(edge.size(), 8192u);
    EXPECT_EQ(
        f.srv->handle(with_csrf(with_token(make_req(Post, "/api/v1/logout", edge), token))).status,
        200);
}

TEST(Filters, MalformedJsonIs400) {
    AdminFixture f;
    HttpResponse res =
        f.srv->handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", "{\"code\":")));
    EXPECT_EQ(res.status, 400);
    EXPECT_EQ(error_code(res), "bad_json");
}

TEST(Filters, StaticFileGzipEtagAnd304) {
    AdminFixture f;
    auto& srv = *f.srv;

    HttpResponse index = srv.handle(make_req(Get, "/"));
    ASSERT_EQ(index.status, 200);
    EXPECT_TRUE(contains(index.content_type, "text/html")) << index.content_type;
    EXPECT_EQ(response_header(index, "Content-Encoding"), "gzip");
    const std::string etag = response_header(index, "ETag");
    EXPECT_FALSE(etag.empty());
    ASSERT_FALSE(index.static_body.empty());
    EXPECT_EQ(index.static_body[0], 0x1f);  // gzip magic
    EXPECT_EQ(index.static_body[1], 0x8b);

    // Static files need no token and no Host other than the device's.
    auto again = make_req(Get, "/index.html");
    again.headers["if-none-match"] = etag;
    HttpResponse cached = srv.handle(again);
    EXPECT_EQ(cached.status, 304);
    EXPECT_TRUE(cached.static_body.empty());
    EXPECT_TRUE(cached.body.empty());
    EXPECT_EQ(response_header(cached, "ETag"), etag);

    again.headers["if-none-match"] = "\"0000000000000000\"";
    EXPECT_EQ(srv.handle(again).status, 200);

    // Binary assets are stored as-is.
    HttpResponse logo = srv.handle(make_req(Get, "/logo.png"));
    EXPECT_EQ(logo.status, 200);
    EXPECT_EQ(logo.content_type, "image/png");
    EXPECT_EQ(response_header(logo, "Content-Encoding"), "");

    EXPECT_EQ(srv.handle(make_req(Get, "/missing.js")).status, 404);
    EXPECT_EQ(srv.handle(make_req(Post, "/index.html")).status, 405);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/"), "evil.com")).status, 403);
}

TEST(Filters, NoAssetPackServesApiOnly) {
    AdminFixture f(/*with_assets=*/false);
    EXPECT_EQ(f.srv->handle(make_req(Get, "/")).status, 404);
    EXPECT_EQ(f.srv->handle(make_req(Get, "/api/v1/info")).status, 200);
}

TEST(Filters, MissingClockDefaultsToOsTime) {
    AdminFixture f;
    wisp::admin::AdminDeps defaulted = f.make_deps(/*with_clock=*/false);
    ASSERT_TRUE(defaulted.now_ms);
    const uint64_t before = wisp::os::now_ms();
    const uint64_t got = defaulted.now_ms();
    EXPECT_GE(got, before);
    EXPECT_LE(got, wisp::os::now_ms());

    // Explicitly cleared: the server still uses the OS clock, so a code
    // requested through it is live at the OS's current time.
    wisp::admin::AdminDeps cleared = f.make_deps();
    cleared.now_ms = nullptr;
    wisp::admin::AdminServer srv(std::move(cleared));
    f.now = 1;  // the fixture clock is not consulted
    ASSERT_EQ(srv.handle(with_csrf(make_req(Post, "/api/v1/pair/start"))).status, 202);
    EXPECT_TRUE(f.pairing->active_code(wisp::os::now_ms()).has_value());
}

TEST(Filters, WebSocketUpgradesAreRejectedForNow) {
    AdminFixture f;
    auto ws = f.srv->ws_handlers();
    ASSERT_TRUE(ws.on_open);
    EXPECT_FALSE(ws.on_open(nullptr, with_token(make_req(Get, "/api/v1/ws"), f.token())));
}

}  // namespace
