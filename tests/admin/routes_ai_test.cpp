// The model profile routes (spec 6.1 "模型", 5.3).

#include <gtest/gtest.h>

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "wisp/base/json.hpp"
#include "wisp/net/http.hpp"

namespace wisp {
void PrintTo(const Value& v, std::ostream* os) { *os << to_json(v); }
}  // namespace wisp

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::core::ModelKind;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::net::TransportError;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::contains;
using wisp::test::error_code;
using wisp::test::make_req;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

constexpr Method Get = Method::Get;
constexpr Method Post = Method::Post;
constexpr Method Put = Method::Put;
constexpr Method Delete = Method::Delete;

HttpRequest authed(const std::string& token, Method m, const std::string& path,
                   std::string body = {}) {
    HttpRequest req = with_token(make_req(m, path, std::move(body)), token);
    if (m != Get) req = with_csrf(std::move(req));
    return req;
}

const Value& field(const Value& v, const char* key) {
    static const Value null;
    const Value* f = v.find(key);
    EXPECT_NE(f, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return f != nullptr ? *f : null;
}

std::string str(const Value& v) {
    EXPECT_EQ(v.type(), Value::Type::String) << wisp::to_json(v);
    return v.type() == Value::Type::String ? v.as_string() : std::string();
}

TEST(AdminAi, CrudActivateAndTestRoutes) {
    AdminFixture f;
    const std::string token = f.token();

    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/ai/profiles"))),
              Value(Map{{"active",
                         Value(Map{{"llm", Value("")}, {"asr", Value("")}, {"tts", Value("")}})},
                        {"profiles", Value(List{})}}));

    Value presets = body_json(f.srv->handle(authed(token, Get, "/api/v1/ai/presets")));
    const Value& list = field(presets, "presets");
    ASSERT_EQ(list.type(), Value::Type::List);
    ASSERT_EQ(list.as_list().size(), 13u);
    EXPECT_EQ(list.as_list()[1],
              Value(Map{{"id", Value("openai")},
                        {"name", Value("OpenAI")},
                        {"kinds", Value(List{Value("llm"), Value("asr"), Value("tts")})},
                        {"base_url", Value("https://api.openai.com/v1")},
                        {"key_required", Value(true)}}));

    // Create: 201 with the full profile, never the key.
    HttpResponse created = f.srv->handle(authed(
        token, Post, "/api/v1/ai/profiles",
        R"({"kind":"llm","name":"Chat","preset":"openai","model":"gpt-x","api_key":"sk-1"})"));
    ASSERT_EQ(created.status, 201) << created.body;
    Value c = body_json(created);
    const std::string id = str(field(c, "id"));
    EXPECT_EQ(c, Value(Map{{"id", Value(id)},
                           {"kind", Value("llm")},
                           {"name", Value("Chat")},
                           {"preset", Value("openai")},
                           {"base_url", Value("https://api.openai.com/v1")},
                           {"model", Value("gpt-x")},
                           {"options", Value(Map{})},
                           {"has_key", Value(true)}}));
    EXPECT_FALSE(contains(created.body, "sk-1"));

    // A second llm profile and a tts one.
    Value second = body_json(f.srv->handle(authed(
        token, Post, "/api/v1/ai/profiles",
        R"({"kind":"llm","name":"Local","preset":"ollama","base_url":"http://192.168.1.9:11434/v1"})")));
    const std::string id2 = str(field(second, "id"));
    HttpResponse tts = f.srv->handle(authed(
        token, Post, "/api/v1/ai/profiles",
        R"({"kind":"tts","name":"Voice","preset":"openai","options":{"voice":"nova"},"api_key_from":")" +
            id + R"("})"));
    ASSERT_EQ(tts.status, 201) << tts.body;
    EXPECT_EQ(field(body_json(tts), "has_key"), Value(true));
    EXPECT_EQ(field(body_json(tts), "options"), Value(Map{{"voice", Value("nova")}}));

    // Bad creates.
    struct Bad {
        const char* body;
        int status;
        const char* code;
    };
    for (const Bad& b : {
             Bad{R"({"kind":"asr","name":"x","preset":"deepseek"})", 400, "invalid_argument"},
             Bad{R"({"kind":"llm","name":"x","preset":"ollama"})", 400, "invalid_argument"},
             Bad{R"({"kind":"vision","name":"x","preset":"openai"})", 400, "invalid_argument"},
             Bad{R"({"name":"x","preset":"openai"})", 400, "invalid_argument"},
             Bad{R"({"kind":"llm","name":"x","preset":"openai","temperature":"1"})", 400,
                 "invalid_argument"},
             Bad{R"({"kind":"llm","name":5,"preset":"openai"})", 400, "invalid_argument"},
             Bad{R"({"kind":"tts","name":"x","preset":"openai","options":{"voice":1}})", 400,
                 "invalid_argument"},
             Bad{R"({"kind":"asr","name":"x","preset":"groq","api_key_from":"ffffffff"})", 400,
                 "key_origin_mismatch"},
             Bad{R"({"kind":"asr","name":"x","preset":"openai","api_key":"a","api_key_from":"b"})",
                 400, "invalid_argument"},
             Bad{R"([])", 400, "invalid_argument"},
             Bad{"", 400, "invalid_argument"},
         }) {
        HttpResponse r = f.srv->handle(authed(token, Post, "/api/v1/ai/profiles", b.body));
        EXPECT_EQ(r.status, b.status) << b.body;
        EXPECT_EQ(error_code(r), b.code) << b.body;
    }

    // PUT: partial update, full object back; unknown fields and kind 400.
    HttpResponse put = f.srv->handle(
        authed(token, Put, "/api/v1/ai/profiles/" + id, R"({"name":"Chat 2","model":"gpt-y"})"));
    ASSERT_EQ(put.status, 200) << put.body;
    Value pv = body_json(put);
    EXPECT_EQ(field(pv, "name"), Value("Chat 2"));
    EXPECT_EQ(field(pv, "model"), Value("gpt-y"));
    EXPECT_EQ(field(pv, "has_key"), Value(true));
    EXPECT_EQ(field(pv, "base_url"), Value("https://api.openai.com/v1"));
    for (const char* body : {R"({"kind":"asr"})", R"({"id":"x"})", R"({"bogus":1})",
                             R"({"base_url":"ftp://x"})", R"({"model":5})", "[]", ""}) {
        HttpResponse r = f.srv->handle(authed(token, Put, "/api/v1/ai/profiles/" + id, body));
        EXPECT_EQ(r.status, 400) << body;
    }
    // Moving to another origin without a key drops the key and says so.
    HttpResponse moved = f.srv->handle(authed(token, Put, "/api/v1/ai/profiles/" + id,
                                              R"({"base_url":"https://other.example/v1"})"));
    EXPECT_EQ(field(body_json(moved), "has_key"), Value(false));
    HttpResponse mismatch = f.srv->handle(
        authed(token, Put, "/api/v1/ai/profiles/" + id2, R"({"api_key_from":")" + id + R"("})"));
    EXPECT_EQ(mismatch.status, 400);
    EXPECT_EQ(error_code(mismatch), "key_origin_mismatch");
    EXPECT_EQ(
        f.srv->handle(authed(token, Put, "/api/v1/ai/profiles/00000000", R"({"name":"x"})")).status,
        404);

    // Activate: only its own kind moves.
    Value state = body_json(f.srv->handle(authed(token, Get, "/api/v1/ai/profiles")));
    const std::string tts_id = str(field(field(state, "active"), "tts"));
    EXPECT_EQ(str(field(field(state, "active"), "llm")), id);
    HttpResponse act =
        f.srv->handle(authed(token, Post, "/api/v1/ai/active", R"({"id":")" + id2 + R"("})"));
    ASSERT_EQ(act.status, 200) << act.body;
    EXPECT_EQ(field(body_json(act), "active"),
              Value(Map{{"llm", Value(id2)}, {"asr", Value("")}, {"tts", Value(tts_id)}}));
    EXPECT_EQ(field(body_json(act), "profiles").as_list().size(), 3u);
    EXPECT_EQ(
        f.srv->handle(authed(token, Post, "/api/v1/ai/active", R"({"id":"00000000"})")).status,
        404);
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/ai/active", R"({"id":5})")).status, 400);

    // Delete: the active one of a kind that has others is 409.
    HttpResponse busy = f.srv->handle(authed(token, Delete, "/api/v1/ai/profiles/" + id2));
    EXPECT_EQ(busy.status, 409);
    EXPECT_EQ(error_code(busy), "active_profile");
    EXPECT_EQ(f.srv->handle(authed(token, Delete, "/api/v1/ai/profiles/" + id)).status, 204);
    EXPECT_EQ(f.srv->handle(authed(token, Delete, "/api/v1/ai/profiles/" + id)).status, 404);
    // The last of its kind goes, leaving no active.
    EXPECT_EQ(f.srv->handle(authed(token, Delete, "/api/v1/ai/profiles/" + tts_id)).status, 204);
    EXPECT_EQ(f.models->get().active_for(ModelKind::Tts), "");

    // Test: runs against that profile.
    f.http.next = {.status = 200, .body = R"({"data":[{"id":"m1"},{"id":"m2"}]})"};
    HttpResponse ok = f.srv->handle(authed(token, Post, "/api/v1/ai/profiles/" + id2 + "/test"));
    ASSERT_EQ(ok.status, 200) << ok.body;
    Value okv = body_json(ok);
    EXPECT_EQ(field(okv, "ok"), Value(true));
    EXPECT_TRUE(field(okv, "error").is_null());
    EXPECT_EQ(field(okv, "http_status"), Value(int64_t{200}));
    EXPECT_EQ(field(okv, "models"), Value(List{Value("m1"), Value("m2")}));
    EXPECT_EQ(field(okv, "latency_ms").type(), Value::Type::Int);
    EXPECT_EQ(f.http.last()->url, "http://192.168.1.9:11434/v1/models");

    struct Case {
        TransportError transport;
        int status;
        const char* error;
    };
    for (const Case& c :
         {Case{TransportError::Dns, 0, "dns"}, Case{TransportError::Tls, 0, "tls"},
          Case{TransportError::Timeout, 0, "timeout"}, Case{TransportError::None, 503, "http"},
          Case{TransportError::None, 404, "no_model_list"}}) {
        f.http.next = {.transport = c.transport, .status = c.status, .body = "upstream-body"};
        HttpResponse res =
            f.srv->handle(authed(token, Post, "/api/v1/ai/profiles/" + id2 + "/test"));
        EXPECT_EQ(res.status, 200);
        Value v = body_json(res);
        EXPECT_EQ(field(v, "ok"), Value(false));
        EXPECT_EQ(field(v, "error"), Value(c.error));
        EXPECT_EQ(field(v, "http_status"), Value(int64_t{c.status}));
        EXPECT_EQ(field(v, "models"), Value(List{}));
        EXPECT_FALSE(contains(res.body, "upstream-body"));
    }
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/ai/profiles/00000000/test")).status, 404);
    // Method and path mistakes.
    EXPECT_EQ(f.srv->handle(authed(token, Get, "/api/v1/ai/profiles/" + id2 + "/test")).status,
              405);
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/ai/profiles/" + id2 + "/nope")).status,
              404);
}

TEST(AdminAi, NeedsATokenAndCsrf) {
    AdminFixture f;
    for (const char* path : {"/api/v1/ai/profiles", "/api/v1/ai/presets"}) {
        EXPECT_EQ(f.srv->handle(make_req(Get, path)).status, 401) << path;
    }
    EXPECT_EQ(f.srv
                  ->handle(with_csrf(make_req(Post, "/api/v1/ai/profiles",
                                              R"({"kind":"llm","name":"x","preset":"openai"})")))
                  .status,
              401);
    const std::string token = f.token();
    HttpResponse no_csrf = f.srv->handle(with_token(
        make_req(Post, "/api/v1/ai/profiles", R"({"kind":"llm","name":"x","preset":"openai"})"),
        token));
    EXPECT_EQ(no_csrf.status, 403);
    EXPECT_EQ(error_code(no_csrf), "csrf");
    EXPECT_TRUE(f.models->get().profiles.empty());
}

TEST(AdminAi, LimitReachedIs409) {
    AdminFixture f;
    const std::string token = f.token();
    for (int i = 0; i < 16; ++i) {
        ASSERT_EQ(f.srv
                      ->handle(authed(token, Post, "/api/v1/ai/profiles",
                                      R"({"kind":"asr","name":"a","preset":"openai"})"))
                      .status,
                  201);
    }
    HttpResponse r = f.srv->handle(authed(token, Post, "/api/v1/ai/profiles",
                                          R"({"kind":"asr","name":"a","preset":"openai"})"));
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(error_code(r), "limit_reached");
}

TEST(AdminAi, NoResponseEverContainsApiKey) {
    AdminFixture f;
    const std::string token = f.token();
    auto p = f.models->create({.kind = ModelKind::Llm,
                               .name = "x",
                               .preset = "custom",
                               .base_url = "https://x/v1",
                               .api_key = "sk-NEVERLEAK"});
    ASSERT_TRUE(p);
    const std::string id = p.value().id;
    f.http.next = {.status = 401, .body = R"({"error":"bad key sk-NEVERLEAK"})"};

    std::vector<HttpRequest> reqs;
    for (const char* path :
         {"/api/v1/info", "/api/v1/device", "/api/v1/devices", "/api/v1/settings", "/api/v1/wifi",
          "/api/v1/wifi/scan", "/api/v1/clients", "/api/v1/ai/profiles", "/api/v1/ai/presets"}) {
        reqs.push_back(authed(token, Get, path));
    }
    reqs.push_back(authed(token, Post, "/api/v1/ai/profiles/" + id + "/test"));
    reqs.push_back(authed(token, Put, "/api/v1/ai/profiles/" + id, R"({"model":"gpt-x"})"));
    reqs.push_back(authed(token, Post, "/api/v1/ai/active", R"({"id":")" + id + R"("})"));
    reqs.push_back(authed(token, Post, "/api/v1/ai/profiles",
                          R"({"kind":"asr","name":"a","preset":"custom","base_url":"https://x/v2",)"
                          R"("api_key_from":")" +
                              id + R"("})"));
    // A rejected key must not be echoed back in the error either.
    reqs.push_back(
        authed(token, Put, "/api/v1/ai/profiles/" + id, R"({"api_key":"sk-NEVERLEAK x"})"));
    reqs.push_back(
        authed(token, Post, "/api/v1/ai/profiles",
               R"({"kind":"llm","name":"b","preset":"openai","api_key":"sk-NEVERLEAK\n"})"));
    for (const HttpRequest& req : reqs) {
        HttpResponse res = f.srv->handle(req);
        EXPECT_LT(res.status, 500) << req.path;
        EXPECT_EQ(res.body.find("NEVERLEAK"), std::string::npos) << req.path << ": " << res.body;
        for (const auto& [k, v] : res.headers) {
            EXPECT_EQ(v.find("NEVERLEAK"), std::string::npos) << req.path << " header " << k;
        }
    }
    // The probe really ran, and its key went only into the header.
    auto sent = f.http.last();
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(sent->url, "https://x/v1/models");
}

TEST(AdminAi, OldLlmRoutesAreGone) {
    AdminFixture f;
    for (Method m : {Get, Put}) {
        EXPECT_EQ(f.srv->handle(with_csrf(make_req(m, "/api/v1/llm"))).status, 401);
    }
    const std::string token = f.token();
    EXPECT_EQ(f.srv->handle(authed(token, Get, "/api/v1/llm")).status, 404);
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/llm", R"({"model":"x"})")).status, 404);
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/llm/test")).status, 404);
}

TEST(AdminAi, TestWithoutHttpClientIsUnavailable) {
    AdminFixture f;
    f.models->set_prober(nullptr);
    const std::string token = f.token();
    auto p = f.models->create(
        {.kind = ModelKind::Llm, .name = "x", .preset = "openai", .api_key = "sk-1"});
    ASSERT_TRUE(p);
    HttpResponse res =
        f.srv->handle(authed(token, Post, "/api/v1/ai/profiles/" + p.value().id + "/test"));
    EXPECT_EQ(res.status, 503);
    EXPECT_EQ(error_code(res), "unavailable");
}

}  // namespace
