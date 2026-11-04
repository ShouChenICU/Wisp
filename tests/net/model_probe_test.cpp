#include "wisp/net/model_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "support/fake_http.hpp"
#include "wisp/core/model_profiles.hpp"

using wisp::core::ResolvedProfile;
using wisp::net::Method;
using wisp::net::probe_models;
using wisp::net::TransportError;
using wisp::test::FakeHttpClient;

namespace {

struct Fixture {
    // Each call advances the clock by 120 ms, so a probe that reads it
    // once before and once after the request measures 120 ms.
    wisp::core::ModelProbe probe(const std::string& base_url, const std::string& key) {
        ResolvedProfile target{.id = "0000abcd", .base_url = base_url, .api_key = key};
        return probe_models(target, client, [this] {
            uint64_t t = clock;
            clock += 120;
            return t;
        });
    }

    FakeHttpClient client;
    uint64_t clock = 5000;
};

std::string header(const wisp::net::HttpClientRequest& req, const std::string& name) {
    for (const auto& [k, v] : req.headers) {
        if (k == name) return v;
    }
    return {};
}

TEST(ModelProbe, ParsesModelsAndSendsBearer) {
    Fixture f;
    f.client.next = {.status = 200,
                     .body = R"({"object":"list","data":[{"id":"gpt-a","object":"model"},)"
                             R"({"id":"gpt-b"},{"object":"model"}]})"};

    auto p = f.probe("https://api.example.com/v1", "sk-test123");

    auto req = f.client.last();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, Method::Get);
    EXPECT_EQ(req->url, "https://api.example.com/v1/models");
    EXPECT_EQ(header(*req, "Authorization"), "Bearer sk-test123");
    EXPECT_EQ(req->timeout_ms, 10000u);
    // The key goes only into the Authorization header.
    EXPECT_EQ(req->url.find("sk-test123"), std::string::npos);
    EXPECT_TRUE(req->body.empty());

    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.error, "");
    EXPECT_EQ(p.http_status, 200);
    EXPECT_EQ(p.latency_ms, 120u);
    EXPECT_EQ(p.models, (std::vector<std::string>{"gpt-a", "gpt-b"}));
}

TEST(ModelProbe, MapsTransportAndHttpErrors) {
    Fixture f;
    struct Case {
        TransportError transport;
        int status;
        const char* body;
        const char* error;
    };
    for (const Case& c :
         {Case{TransportError::Dns, 0, "", "dns"}, Case{TransportError::Connect, 0, "", "connect"},
          Case{TransportError::Tls, 0, "", "tls"}, Case{TransportError::Timeout, 0, "", "timeout"},
          Case{TransportError::Other, 0, "", "other"},
          Case{TransportError::TooLarge, 0, "", "other"},
          Case{TransportError::None, 401, R"({"error":"bad key"})", "http"},
          Case{TransportError::None, 503, "", "http"},
          // A 200 whose body is not a model list is not a
          // working endpoint.
          Case{TransportError::None, 200, "<html>captive portal</html>", "http"},
          Case{TransportError::None, 200, R"({"data":"nope"})", "http"}}) {
        f.client.next = {.transport = c.transport, .status = c.status, .body = c.body};
        auto p = f.probe("https://x/v1", "sk-abc");
        EXPECT_FALSE(p.ok) << c.error;
        EXPECT_EQ(p.error, c.error);
        EXPECT_EQ(p.http_status, c.status) << c.error;
        EXPECT_TRUE(p.models.empty());
    }
}

// Several platforms (Anthropic's OpenAI layer, some ASR/TTS services) have
// no /models; the page then asks for the model name by hand (spec 5.3).
TEST(ModelProbe, NotFoundIsNoModelList) {
    Fixture f;
    f.client.next = {.status = 404, .body = R"({"error":"not found"})"};
    auto p = f.probe("https://api.anthropic.com/v1", "sk-ant");
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.error, "no_model_list");
    EXPECT_EQ(p.http_status, 404);
}

TEST(ModelProbe, WithoutBaseUrlSendsNothing) {
    Fixture f;
    auto p = f.probe("", "sk-1");
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.error, "other");
    EXPECT_EQ(f.client.calls(), 0u);
}

TEST(ModelProbe, WithoutKeySendsNoAuthorization) {
    Fixture f;
    f.client.next = {.status = 200, .body = R"({"data":[{"id":"qwen"}]})"};
    auto p = f.probe("http://192.168.1.9:11434/v1", "");
    EXPECT_TRUE(p.ok);
    auto req = f.client.last();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(header(*req, "Authorization"), "");
}

TEST(ModelProbe, DropsModelIdsThatEchoTheKey) {
    Fixture f;
    f.client.next = {.status = 200,
                     .body = R"({"data":[{"id":"ok-model"},{"id":"echo-sk-SECRETVALUE"}]})"};
    auto p = f.probe("https://x/v1", "sk-SECRETVALUE");
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.models, (std::vector<std::string>{"ok-model"}));
}

TEST(ModelProbe, ProberAdaptsTheService) {
    Fixture f;
    f.client.next = {.status = 200, .body = R"({"data":[{"id":"m"}]})"};
    auto prober = wisp::net::make_model_prober(f.client, [] { return uint64_t{0}; });
    auto p = prober(ResolvedProfile{.base_url = "https://y/v1"});
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(f.client.last()->url, "https://y/v1/models");
}

}  // namespace
