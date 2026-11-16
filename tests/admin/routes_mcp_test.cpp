// The MCP server routes (spec 4.3 of
// docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "support/secret_pieces.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/mcp_servers.hpp"
#include "wisp/net/http.hpp"

namespace wisp {
void PrintTo(const Value& v, std::ostream* os) { *os << to_json(v); }
}  // namespace wisp

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::body_text;
using wisp::test::error_code;
using wisp::test::holds_secret_piece;
using wisp::test::make_req;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

constexpr Method Get = Method::Get;
constexpr Method Post = Method::Post;
constexpr Method Put = Method::Put;
constexpr Method Delete = Method::Delete;

constexpr const char* kSecret = "tok-SECRET-123456";

HttpRequest authed(const std::string& token, Method m, const std::string& path,
                   std::string body = {}) {
    HttpRequest req = with_token(make_req(m, path, std::move(body)), token);
    if (m != Get) req = with_csrf(std::move(req));
    return req;
}

// A create or update the way the HTTP server sends it: streamed through
// AdminServer::stream_handler(), as every POST and PUT with a body is.
// nullopt when the stream handler did not take the request.
std::optional<HttpResponse> streamed(AdminFixture& f, const std::string& token, Method m,
                                     const std::string& path, const std::string& body) {
    HttpRequest head = with_csrf(with_token(make_req(m, path), token));
    head.content_length = body.size();
    auto start = f.srv->stream_handler()(head);
    if (!start) return std::nullopt;
    if (!start->sink) return start->response;
    std::span<const uint8_t> rest(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    while (!rest.empty()) {
        const size_t n = std::min(rest.size(), wisp::net::kBodyChunkBytes);
        if (auto early = start->sink->on_data(rest.first(n))) return *early;
        rest = rest.subspan(n);
    }
    return start->sink->on_end();
}

// A JSON string whose content is `bytes` bytes, every one of them escaped
// as a browser's JSON.stringify escapes a quote: the worst case per byte
// the service accepts in a value.
std::string escaped_string(size_t bytes) {
    std::string s = "\"";
    for (size_t i = 0; i < bytes; ++i) s += "\\\"";
    return s + "\"";
}

std::string with_secret_body() {
    return std::string(R"({"name":"Files","url":"https://files.example/mcp",)") +
           R"("headers":[{"name":"Authorization","value":"Bearer )" + kSecret + R"("}]})";
}

TEST(AdminMcp, CrudRoutes) {
    AdminFixture f;
    const std::string token = f.token();

    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/mcp/servers"))),
              Value(Map{{"servers", Value(List{})}, {"max", Value(8)}}));

    HttpResponse created =
        f.srv->handle(authed(token, Post, "/api/v1/mcp/servers", with_secret_body()));
    ASSERT_EQ(created.status, 201) << body_text(created);
    const Value expected(Map{
        {"id", Value("files")},
        {"name", Value("Files")},
        {"url", Value("https://files.example/mcp")},
        {"enabled", Value(true)},
        {"timeout_ms", Value(60000)},
        {"headers",
         Value(List{Value(Map{{"name", Value("Authorization")}, {"has_value", Value(true)}})})},
    });
    EXPECT_EQ(body_json(created), expected);

    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/mcp/servers"))),
              Value(Map{{"servers", Value(List{expected})}, {"max", Value(8)}}));

    // Optional fields on create.
    HttpResponse second = f.srv->handle(authed(
        token, Post, "/api/v1/mcp/servers",
        R"({"name":"Wiki","url":"http://wiki.local/mcp","enabled":false,"timeout_ms":5000})"));
    ASSERT_EQ(second.status, 201) << body_text(second);
    EXPECT_EQ(*body_json(second).find("enabled"), Value(false));
    EXPECT_EQ(*body_json(second).find("timeout_ms"), Value(5000));

    // Update: a header without a value keeps the stored one on the same
    // origin.
    HttpResponse updated = f.srv->handle(
        authed(token, Put, "/api/v1/mcp/servers/files",
               R"({"name":"Docs","enabled":false,"timeout_ms":3000,)"
               R"("url":"https://files.example/other","headers":[{"name":"authorization"}]})"));
    ASSERT_EQ(updated.status, 200) << body_text(updated);
    Value u = body_json(updated);
    EXPECT_EQ(*u.find("id"), Value("files"));
    EXPECT_EQ(*u.find("name"), Value("Docs"));
    EXPECT_EQ(*u.find("url"), Value("https://files.example/other"));
    EXPECT_EQ(*u.find("enabled"), Value(false));
    EXPECT_EQ(*u.find("timeout_ms"), Value(3000));
    EXPECT_EQ(
        *u.find("headers"),
        Value(List{Value(Map{{"name", Value("authorization")}, {"has_value", Value(true)}})}));
    auto target = f.mcp->resolve_for_request("files");
    ASSERT_TRUE(target);
    ASSERT_EQ(target->headers.size(), 1u);
    EXPECT_EQ(std::string(target->headers[0].second.c_str()), std::string("Bearer ") + kSecret);

    // A service-level refusal passes through as 400.
    HttpResponse bad_url = f.srv->handle(
        authed(token, Put, "/api/v1/mcp/servers/files", R"({"url":"ftp://files.example/"})"));
    EXPECT_EQ(bad_url.status, 400);
    EXPECT_EQ(error_code(bad_url), "invalid_argument");

    EXPECT_EQ(f.srv->handle(authed(token, Delete, "/api/v1/mcp/servers/files")).status, 204);
    EXPECT_EQ(f.mcp->list().size(), 1u);

    for (auto [m, path, body] : {
             std::tuple{Put, "/api/v1/mcp/servers/files", R"({"name":"x"})"},
             std::tuple{Delete, "/api/v1/mcp/servers/files", ""},
             std::tuple{Post, "/api/v1/mcp/servers/files/test", ""},
         }) {
        HttpResponse r = f.srv->handle(authed(token, m, path, body));
        EXPECT_EQ(r.status, 404) << path;
        EXPECT_EQ(error_code(r), "not_found") << path;
    }
}

TEST(AdminMcp, NeedsATokenAndCsrf) {
    AdminFixture f;
    EXPECT_EQ(f.srv->handle(make_req(Get, "/api/v1/mcp/servers")).status, 401);
    EXPECT_EQ(f.srv
                  ->handle(with_csrf(make_req(Post, "/api/v1/mcp/servers",
                                              R"({"name":"x","url":"http://x.local/"})")))
                  .status,
              401);
    const std::string token = f.token();
    HttpResponse no_csrf = f.srv->handle(with_token(
        make_req(Post, "/api/v1/mcp/servers", R"({"name":"x","url":"http://x.local/"})"), token));
    EXPECT_EQ(no_csrf.status, 403);
    EXPECT_EQ(error_code(no_csrf), "csrf");
    EXPECT_TRUE(f.mcp->list().empty());

    ASSERT_TRUE(f.mcp->create({.name = "x", .url = "http://x.local/"}));
    for (auto [m, path] :
         {std::pair{Put, "/api/v1/mcp/servers/x"}, std::pair{Delete, "/api/v1/mcp/servers/x"},
          std::pair{Post, "/api/v1/mcp/servers/x/test"}}) {
        EXPECT_EQ(f.srv->handle(with_csrf(make_req(m, path, R"({"name":"y"})"))).status, 401)
            << path;
        HttpResponse r = f.srv->handle(with_token(make_req(m, path, R"({"name":"y"})"), token));
        EXPECT_EQ(r.status, 403) << path;
    }
    EXPECT_EQ(f.mcp->list().size(), 1u);
    EXPECT_TRUE(f.probed_targets.empty());
}

TEST(AdminMcp, UnknownFieldIs400) {
    AdminFixture f;
    const std::string token = f.token();
    for (const char* body : {
             R"({"name":"x","url":"http://x.local/","extra":1})",
             R"({"name":"x","url":"http://x.local/","headers":[{"name":"A","value":"v","x":1}]})",
             R"({"name":"x","url":"http://x.local/","headers":{"A":"v"}})",
             R"({"name":"x","url":"http://x.local/","timeout_ms":"5000"})",
             R"({"name":"x","url":"http://x.local/","timeout_ms":-1})",
             R"({"name":"x","url":"http://x.local/","enabled":"yes"})",
             R"({"name":"x"})",
             R"(["x"])",
             "",
         }) {
        HttpResponse r = f.srv->handle(authed(token, Post, "/api/v1/mcp/servers", body));
        EXPECT_EQ(r.status, 400) << body;
        EXPECT_EQ(error_code(r), "invalid_argument") << body;
    }
    EXPECT_TRUE(f.mcp->list().empty());

    ASSERT_TRUE(f.mcp->create({.name = "x", .url = "http://x.local/"}));
    for (const char* body : {
             R"({"name":"y","id":"z"})",
             R"({"headers":[{"value":"v"}]})",
             R"({"headers":[{"name":"A","value":5}]})",
             R"({"url":null})",
         }) {
        HttpResponse r = f.srv->handle(authed(token, Put, "/api/v1/mcp/servers/x", body));
        EXPECT_EQ(r.status, 400) << body;
        EXPECT_EQ(error_code(r), "invalid_argument") << body;
    }
    EXPECT_EQ(f.mcp->get("x")->name, "x");
}

TEST(AdminMcp, LimitReachedIs409) {
    AdminFixture f;
    const std::string token = f.token();
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(f.srv
                      ->handle(authed(token, Post, "/api/v1/mcp/servers",
                                      R"({"name":"s","url":"http://s.local/"})"))
                      .status,
                  201);
    }
    HttpResponse r = f.srv->handle(
        authed(token, Post, "/api/v1/mcp/servers", R"({"name":"s","url":"http://s.local/"})"));
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(error_code(r), "limit_reached");
    EXPECT_EQ(f.mcp->list().size(), 8u);
}

TEST(AdminMcp, NoResponseEverContainsAHeaderValue) {
    AdminFixture f;
    const std::string token = f.token();
    std::vector<std::pair<std::string, HttpResponse>> responses;
    auto run = [&](Method m, const std::string& path, std::string body = {}) {
        responses.emplace_back(path, f.srv->handle(authed(token, m, path, std::move(body))));
        return responses.back().second.status;
    };

    EXPECT_EQ(run(Post, "/api/v1/mcp/servers", with_secret_body()), 201);
    run(Get, "/api/v1/mcp/servers");
    // Refusals echo nothing of the value either: a bad header name next to
    // it, an empty value, a reserved name, an unknown field.
    EXPECT_EQ(run(Post, "/api/v1/mcp/servers",
                  std::string(R"({"name":"Two","url":"https://two.example/",)") +
                      R"("headers":[{"name":"Bad Name","value":")" + kSecret + R"("}]})"),
              400);
    EXPECT_EQ(run(Put, "/api/v1/mcp/servers/files",
                  std::string(R"({"headers":[{"name":"Accept","value":")") + kSecret + R"("}]})"),
              400);
    EXPECT_EQ(run(Put, "/api/v1/mcp/servers/files",
                  std::string(R"({"headers":[{"name":"X","value":")") + kSecret +
                      R"("}],"surprise":")" + kSecret + R"("})"),
              400);
    EXPECT_EQ(run(Put, "/api/v1/mcp/servers/files",
                  std::string(R"({"headers":[{"name":"Authorization"},{"name":"X-Key","value":")") +
                      kSecret + R"(2"}]})"),
              200);
    // The test route runs the prober with the values; its answer carries
    // the probe's fields only.
    f.mcp_probe = {.ok = false, .error = "http", .http_status = 401, .latency_ms = 12};
    EXPECT_EQ(run(Post, "/api/v1/mcp/servers/files/test"), 200);
    EXPECT_EQ(run(Delete, "/api/v1/mcp/servers/files"), 204);
    run(Get, "/api/v1/mcp/servers");

    for (const auto& [path, res] : responses) {
        const std::string text = body_text(res);
        EXPECT_FALSE(holds_secret_piece(text, kSecret)) << path << ": " << text;
        for (const auto& [name, value] : res.headers) {
            EXPECT_FALSE(holds_secret_piece(value, kSecret)) << path << " " << name;
        }
    }
    // The values did reach the prober, which is where they belong.
    ASSERT_EQ(f.probed_targets.size(), 1u);
    ASSERT_EQ(f.probed_targets[0].headers.size(), 2u);
}

TEST(AdminMcp, OriginChangeDropsValues) {
    AdminFixture f;
    const std::string token = f.token();
    ASSERT_EQ(f.srv->handle(authed(token, Post, "/api/v1/mcp/servers", with_secret_body())).status,
              201);
    HttpResponse r = f.srv->handle(
        authed(token, Put, "/api/v1/mcp/servers/files",
               R"({"url":"https://evil.example/mcp","headers":[{"name":"Authorization"}]})"));
    ASSERT_EQ(r.status, 200) << body_text(r);
    // The answer shows the web card the header is gone, so it can ask for
    // the value again.
    EXPECT_EQ(*body_json(r).find("headers"), Value(List{}));
    EXPECT_EQ(*body_json(r).find("url"), Value("https://evil.example/mcp"));
    auto target = f.mcp->resolve_for_request("files");
    ASSERT_TRUE(target);
    EXPECT_TRUE(target->headers.empty());

    // Moving to another origin without a headers field at all drops the
    // stored values too.
    ASSERT_EQ(f.srv
                  ->handle(authed(token, Put, "/api/v1/mcp/servers/files",
                                  R"({"url":"https://files.example/mcp","headers":[)"
                                  R"({"name":"Authorization","value":"Bearer )" +
                                      std::string(kSecret) + R"("}]})"))
                  .status,
              200);
    ASSERT_EQ(f.mcp->resolve_for_request("files")->headers.size(), 1u);
    HttpResponse moved = f.srv->handle(
        authed(token, Put, "/api/v1/mcp/servers/files", R"({"url":"https://evil.example/mcp"})"));
    ASSERT_EQ(moved.status, 200) << body_text(moved);
    EXPECT_EQ(*body_json(moved).find("headers"), Value(List{}));
    target = f.mcp->resolve_for_request("files");
    ASSERT_TRUE(target);
    EXPECT_TRUE(target->headers.empty());
}

// The largest configuration the service accepts fits one request, on
// create and on update, so the web card never has to split it; a body
// past that is refused before any of it is read.
TEST(AdminMcp, LargestConfigurationFitsOneRequest) {
    using namespace wisp::core;
    AdminFixture f;
    const std::string token = f.token();
    auto biggest = [](const std::string& host) {
        std::string url = "https://" + host + "/";
        url.append(kMcpMaxUrlBytes - url.size(), 'p');
        std::string body = R"({ "name": )" + escaped_string(kMcpMaxNameBytes) + R"(, "url": ")" +
                           url + R"(", "enabled": false, "timeout_ms": 120000, "headers": [)";
        for (size_t i = 0; i < kMcpMaxHeaders; ++i) {
            std::string name = "X-" + std::to_string(i) + "-";
            name.append(kMcpMaxHeaderNameBytes - name.size(), 'h');
            if (i > 0) body += ", ";
            body += R"({ "name": ")" + name + R"(", "value": )" +
                    escaped_string(kMcpMaxHeaderValueBytes) + " }";
        }
        return body + "] }";
    };

    const std::string create = biggest("files.example");
    ASSERT_GT(create.size(), 8192u);  // the cap on a buffered API body
    auto created = streamed(f, token, Post, "/api/v1/mcp/servers", create);
    ASSERT_TRUE(created) << "not streamed";
    ASSERT_EQ(created->status, 201) << body_text(*created);
    const std::string id = body_json(*created).find("id")->as_string();
    auto target = f.mcp->resolve_for_request(id);
    ASSERT_TRUE(target);
    ASSERT_EQ(target->headers.size(), kMcpMaxHeaders);
    EXPECT_EQ(target->headers[7].second.size(), kMcpMaxHeaderValueBytes);

    auto updated = streamed(f, token, Put, "/api/v1/mcp/servers/" + id, biggest("other.example"));
    ASSERT_TRUE(updated) << "not streamed";
    ASSERT_EQ(updated->status, 200) << body_text(*updated);
    EXPECT_EQ(f.mcp->list()[0].headers.size(), kMcpMaxHeaders);

    // Past the cap: 413 from the Content-Length alone, nothing changed.
    auto too_big =
        streamed(f, token, Post, "/api/v1/mcp/servers", create + std::string(64 * 1024, ' '));
    ASSERT_TRUE(too_big);
    EXPECT_EQ(too_big->status, 413);
    EXPECT_EQ(error_code(*too_big), "payload_too_large");
    EXPECT_EQ(f.mcp->list().size(), 1u);

    // The stream path keeps the routes' checks: token, CSRF, shape.
    HttpRequest head = with_token(make_req(Post, "/api/v1/mcp/servers"), token);
    head.content_length = 2;
    auto no_csrf = f.srv->stream_handler()(head);
    ASSERT_TRUE(no_csrf);
    EXPECT_EQ(no_csrf->response.status, 403);
    auto bad = streamed(f, token, Put, "/api/v1/mcp/servers/" + id, R"({"bogus":1})");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
    auto missing = streamed(f, token, Put, "/api/v1/mcp/servers/nope", R"({"name":"x"})");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
}

TEST(AdminMcp, TestRouteReportsTheProbe) {
    AdminFixture f;
    const std::string token = f.token();
    ASSERT_TRUE(f.mcp->create({.name = "Files", .url = "http://files.local/mcp"}));

    f.mcp_probe = {.ok = true, .latency_ms = 42, .protocol = "2025-06-18"};
    f.mcp_probe.tools.emplace_back("read_file");
    f.mcp_probe.tools.emplace_back("list_dir");
    HttpResponse ok = f.srv->handle(authed(token, Post, "/api/v1/mcp/servers/files/test"));
    ASSERT_EQ(ok.status, 200) << body_text(ok);
    EXPECT_EQ(body_json(ok), Value(Map{
                                 {"ok", Value(true)},
                                 {"error", Value()},
                                 {"latency_ms", Value(42)},
                                 {"protocol", Value("2025-06-18")},
                                 {"tools", Value(List{Value("read_file"), Value("list_dir")})},
                             }));
    ASSERT_EQ(f.probed_targets.size(), 1u);
    EXPECT_EQ(f.probed_targets[0].id, "files");

    for (const char* category :
         {"dns", "connect", "tls", "timeout", "http", "unsupported_protocol", "bad_response"}) {
        f.mcp_probe = {.ok = false, .error = category, .latency_ms = 7};
        HttpResponse r = f.srv->handle(authed(token, Post, "/api/v1/mcp/servers/files/test"));
        ASSERT_EQ(r.status, 200) << category;
        EXPECT_EQ(body_json(r), Value(Map{
                                    {"ok", Value(false)},
                                    {"error", Value(category)},
                                    {"latency_ms", Value(7)},
                                    {"protocol", Value("")},
                                    {"tools", Value(List{})},
                                }))
            << category;
    }

    // No prober: no HTTP client on this platform.
    f.mcp->set_prober(nullptr);
    HttpResponse none = f.srv->handle(authed(token, Post, "/api/v1/mcp/servers/files/test"));
    EXPECT_EQ(none.status, 503);
    EXPECT_EQ(error_code(none), "unavailable");
}

TEST(AdminMcp, UnavailableWithoutTheService) {
    AdminFixture f;
    const std::string token = f.token();
    wisp::admin::AdminDeps deps = f.make_deps();
    deps.mcp = nullptr;
    f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
    for (auto [m, path, body] : {
             std::tuple{Get, "/api/v1/mcp/servers", ""},
             std::tuple{Post, "/api/v1/mcp/servers", R"({"name":"x","url":"http://x.local/"})"},
             std::tuple{Put, "/api/v1/mcp/servers/x", R"({"name":"y"})"},
             std::tuple{Delete, "/api/v1/mcp/servers/x", ""},
             std::tuple{Post, "/api/v1/mcp/servers/x/test", ""},
         }) {
        HttpResponse r = f.srv->handle(authed(token, m, path, body));
        EXPECT_EQ(r.status, 503) << path;
        EXPECT_EQ(error_code(r), "unavailable") << path;
    }
}

}  // namespace
