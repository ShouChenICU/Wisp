#include "wisp/net/llm_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/temp_dir.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/llm_config.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::core::LlmConfigService;
using wisp::net::LlmProbe;
using wisp::net::Method;
using wisp::net::probe_llm;
using wisp::net::TransportError;
using wisp::test::FakeHttpClient;

namespace {

struct Fixture {
    Fixture() : dir(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", dir, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        llm = std::make_unique<LlmConfigService>(*storage);
        EXPECT_TRUE(llm->init());
    }

    // Each call advances the clock by 120 ms, so a probe that reads it
    // once before and once after the request measures 120 ms.
    LlmProbe probe() {
        return probe_llm(*llm, client, [this] {
            uint64_t t = clock;
            clock += 120;
            return t;
        });
    }

    std::string dir;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<LlmConfigService> llm;
    FakeHttpClient client;
    uint64_t clock = 5000;
};

std::string header(const wisp::net::HttpClientRequest& req, const std::string& name) {
    for (const auto& [k, v] : req.headers) {
        if (k == name) return v;
    }
    return {};
}

TEST(LlmProbe, ParsesModelsAndSendsBearer) {
    Fixture f;
    ASSERT_TRUE(f.llm->set({.base_url = "https://api.example.com/v1/", .api_key = "sk-test123"}));
    f.client.next = {.status = 200,
                     .body = R"({"object":"list","data":[{"id":"gpt-a","object":"model"},)"
                             R"({"id":"gpt-b"},{"object":"model"}]})"};

    LlmProbe p = f.probe();

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
    EXPECT_EQ(p.transport, TransportError::None);
    EXPECT_EQ(p.http_status, 200);
    EXPECT_EQ(p.latency_ms, 120u);
    EXPECT_EQ(p.models, (std::vector<std::string>{"gpt-a", "gpt-b"}));
}

TEST(LlmProbe, MapsTransportAndHttpErrors) {
    Fixture f;
    ASSERT_TRUE(f.llm->set({.base_url = "https://x/v1", .api_key = "sk-abc"}));

    f.client.next = {.transport = TransportError::Tls, .status = 0, .body = {}};
    LlmProbe tls = f.probe();
    EXPECT_FALSE(tls.ok);
    EXPECT_EQ(tls.transport, TransportError::Tls);
    EXPECT_EQ(tls.http_status, 0);
    EXPECT_TRUE(tls.models.empty());

    f.client.next = {.status = 401, .body = R"({"error":"bad key"})"};
    LlmProbe unauthorized = f.probe();
    EXPECT_FALSE(unauthorized.ok);
    EXPECT_EQ(unauthorized.transport, TransportError::None);
    EXPECT_EQ(unauthorized.http_status, 401);

    // A 200 whose body is not a model list is not a working endpoint.
    f.client.next = {.status = 200, .body = "<html>captive portal</html>"};
    LlmProbe html = f.probe();
    EXPECT_FALSE(html.ok);
    EXPECT_EQ(html.http_status, 200);

    f.client.next = {.status = 200, .body = R"({"data":"nope"})"};
    EXPECT_FALSE(f.probe().ok);
}

TEST(LlmProbe, WithoutBaseUrlSendsNothing) {
    Fixture f;
    LlmProbe p = f.probe();
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.transport, TransportError::Other);
    EXPECT_EQ(f.client.calls(), 0u);
}

TEST(LlmProbe, WithoutKeySendsNoAuthorization) {
    Fixture f;
    ASSERT_TRUE(f.llm->set({.base_url = "http://192.168.1.9:11434/v1"}));
    f.client.next = {.status = 200, .body = R"({"data":[{"id":"qwen"}]})"};
    LlmProbe p = f.probe();
    EXPECT_TRUE(p.ok);
    auto req = f.client.last();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(header(*req, "Authorization"), "");
}

TEST(LlmProbe, DropsModelIdsThatEchoTheKey) {
    Fixture f;
    ASSERT_TRUE(f.llm->set({.base_url = "https://x/v1", .api_key = "sk-SECRETVALUE"}));
    f.client.next = {.status = 200,
                     .body = R"({"data":[{"id":"ok-model"},{"id":"echo-sk-SECRETVALUE"}]})"};
    LlmProbe p = f.probe();
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.models, (std::vector<std::string>{"ok-model"}));
}

}  // namespace
