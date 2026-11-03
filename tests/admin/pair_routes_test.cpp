// The unauthenticated pairing routes and logout (spec 6.1, 6.3).

#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "wisp/base/json.hpp"
#include "wisp/net/http.hpp"

using wisp::Value;
using wisp::net::HttpResponse;
using wisp::net::Iface;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::contains;
using wisp::test::error_code;
using wisp::test::make_req;
using wisp::test::set_cookie;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

constexpr Method Get = Method::Get;
constexpr Method Post = Method::Post;

std::string confirm_body(const std::string& code) { return R"({"code":")" + code + "\"}"; }

// "wisp_token=<value>; ..." -> "<value>".
std::string cookie_token(const std::string& set_cookie_value) {
    const std::string prefix = "wisp_token=";
    if (set_cookie_value.rfind(prefix, 0) != 0) return {};
    size_t end = set_cookie_value.find(';');
    return set_cookie_value.substr(prefix.size(), end - prefix.size());
}

// Asks for a code through the route and reads it off the "screen".
std::string start_pairing(AdminFixture& f) {
    HttpResponse res = f.srv->handle(with_csrf(make_req(Post, "/api/v1/pair/start")));
    EXPECT_EQ(res.status, 202);
    auto code = f.pairing->active_code(f.now);
    EXPECT_TRUE(code.has_value());
    return code.value_or("");
}

TEST(PairRoutes, ConfirmSetsHttpOnlyCookie) {
    AdminFixture f;
    auto& srv = *f.srv;
    const std::string code = start_pairing(f);

    auto res = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", confirm_body(code))));
    EXPECT_EQ(res.status, 200);
    const std::string cookie = set_cookie(res);
    EXPECT_TRUE(contains(cookie, "HttpOnly; SameSite=Strict; Path=/; Max-Age=31536000")) << cookie;
    const std::string token = cookie_token(cookie);
    ASSERT_FALSE(token.empty());
    // The token travels only in the HttpOnly cookie, never in the body.
    EXPECT_FALSE(contains(res.body, token));
    EXPECT_FALSE(contains(res.body, code));

    // The cookie authenticates later requests, and /info reports it.
    Value anon = body_json(srv.handle(make_req(Get, "/api/v1/info")));
    EXPECT_EQ(anon,
              Value(wisp::Map{
                  {"name", Value("Wisp")}, {"fw", Value("0.2.0-test")}, {"paired", Value(false)}}));
    Value paired = body_json(srv.handle(with_token(make_req(Get, "/api/v1/info"), token)));
    ASSERT_NE(paired.find("paired"), nullptr);
    EXPECT_TRUE(paired.find("paired")->as_bool());
    EXPECT_FALSE(body_json(srv.handle(with_token(make_req(Get, "/api/v1/info"), "bogus")))
                     .find("paired")
                     ->as_bool());

    // The code is single-use.
    auto replay = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", confirm_body(code))));
    EXPECT_EQ(replay.status, 403);
}

TEST(PairRoutes, LogoutClearsCookieAndRevokesToken) {
    AdminFixture f;
    auto& srv = *f.srv;
    const std::string token = f.token();
    ASSERT_EQ(f.pairing->clients().size(), 1u);

    auto res = srv.handle(with_csrf(with_token(make_req(Post, "/api/v1/logout"), token)));
    EXPECT_EQ(res.status, 200);
    EXPECT_TRUE(contains(set_cookie(res), "wisp_token=;")) << set_cookie(res);
    EXPECT_TRUE(contains(set_cookie(res), "Max-Age=0")) << set_cookie(res);
    EXPECT_TRUE(f.pairing->clients().empty());

    EXPECT_EQ(srv.handle(with_csrf(with_token(make_req(Post, "/api/v1/logout"), token))).status,
              401);
}

TEST(PairRoutes, ConfirmNeedsACodeString) {
    AdminFixture f;
    start_pairing(f);
    for (const char* body : {"", "{}", R"({"code":123456})", "[1]"}) {
        auto res = f.srv->handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", body)));
        EXPECT_EQ(res.status, 400) << body;
        EXPECT_EQ(error_code(res), "invalid_argument") << body;
    }
}

TEST(PairRoutes, WrongCodeIs403AndLockIs429) {
    AdminFixture f;
    auto& srv = *f.srv;
    const std::string code = start_pairing(f);
    const std::string wrong = code == "000000" ? "111111" : "000000";

    for (int i = 0; i < 4; ++i) {
        auto res =
            srv.handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", confirm_body(wrong))));
        EXPECT_EQ(res.status, 403) << i;
        EXPECT_EQ(error_code(res), "permission_denied") << i;
        EXPECT_TRUE(set_cookie(res).empty());
    }
    // The fifth miss locks pairing; it is itself still a wrong code.
    EXPECT_EQ(
        srv.handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", confirm_body(wrong)))).status,
        403);

    auto locked = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", confirm_body(code))));
    EXPECT_EQ(locked.status, 429);
    EXPECT_EQ(error_code(locked), "rate_limited");
    auto restart = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/start")));
    EXPECT_EQ(restart.status, 429);

    // The router's clock is deps.now_ms: five minutes later it unlocks.
    f.now += 300000;
    EXPECT_EQ(srv.handle(with_csrf(make_req(Post, "/api/v1/pair/start"))).status, 202);
}

TEST(PairRoutes, ApPairOnlyFromApInterface) {
    AdminFixture f;
    auto& srv = *f.srv;

    auto sta = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/ap")));
    EXPECT_EQ(sta.status, 403);
    EXPECT_EQ(error_code(sta), "permission_denied");
    EXPECT_TRUE(set_cookie(sta).empty());
    auto loop = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/ap", {}, Iface::Loopback)));
    EXPECT_EQ(loop.status, 403);
    EXPECT_TRUE(f.pairing->clients().empty());

    auto ap = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/ap", {}, Iface::Ap)));
    EXPECT_EQ(ap.status, 200);
    EXPECT_TRUE(contains(set_cookie(ap), "HttpOnly; SameSite=Strict; Path=/; Max-Age=31536000"))
        << set_cookie(ap);
    ASSERT_EQ(f.pairing->clients().size(), 1u);

    // The token works from the home network afterwards.
    const std::string token = cookie_token(set_cookie(ap));
    EXPECT_EQ(srv.handle(with_csrf(with_token(make_req(Post, "/api/v1/logout"), token))).status,
              200);
}

TEST(PairRoutes, ErrorEnvelopeShape) {
    AdminFixture f;
    auto res = f.srv->handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", confirm_body("1"))));
    EXPECT_EQ(res.status, 403);
    EXPECT_EQ(res.content_type, "application/json");
    Value v = body_json(res);
    ASSERT_EQ(v.type(), Value::Type::Map);
    ASSERT_EQ(v.as_map().size(), 1u);
    const Value* err = v.find("error");
    ASSERT_NE(err, nullptr);
    ASSERT_EQ(err->type(), Value::Type::Map);
    EXPECT_EQ(err->as_map().size(), 2u);
    ASSERT_NE(err->find("code"), nullptr);
    ASSERT_NE(err->find("message"), nullptr);
    EXPECT_EQ(err->find("code")->type(), Value::Type::String);
    EXPECT_EQ(err->find("message")->type(), Value::Type::String);
    EXPECT_FALSE(err->find("message")->as_string().empty());

    // The filters use the same envelope.
    for (const HttpResponse& r :
         {f.srv->handle(with_csrf(make_req(Post, "/api/v1/logout"))),
          f.srv->handle(make_req(Post, "/api/v1/pair/start")),
          f.srv->handle(wisp::test::host(make_req(Get, "/api/v1/info"), "evil.com"))}) {
        Value e = body_json(r);
        ASSERT_NE(e.find("error"), nullptr) << r.body;
        EXPECT_NE(e.find("error")->find("code"), nullptr) << r.body;
        EXPECT_NE(e.find("error")->find("message"), nullptr) << r.body;
    }
}

// A server-side failure is reported without its internal detail: the
// storage error behind it names on-device paths.
TEST(PairRoutes, ServerErrorsHideInternalMessages) {
    AdminFixture f;
    const std::string code = start_pairing(f);
    // A regular file where the net/ directory belongs makes saving the
    // paired-client list fail.
    {
        std::ofstream blocker(f.dir + "/system/net");
        blocker << "x";
    }
    auto res = f.srv->handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", confirm_body(code))));
    EXPECT_GE(res.status, 500);
    EXPECT_TRUE(set_cookie(res).empty());
    Value v = body_json(res);
    ASSERT_NE(v.find("error"), nullptr) << res.body;
    ASSERT_NE(v.find("error")->find("message"), nullptr) << res.body;
    EXPECT_EQ(v.find("error")->find("message")->as_string(), "internal error");
    EXPECT_FALSE(contains(res.body, "system")) << res.body;
    EXPECT_FALSE(contains(res.body, f.dir)) << res.body;
}

}  // namespace
