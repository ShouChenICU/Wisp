#include "wisp/core/llm_config.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::core::DeviceRegistry;
using wisp::core::EventBus;
using wisp::core::LlmConfigService;
using wisp::core::LlmConfigUpdate;
using wisp::core::StorageManager;
using wisp::test::FakeStorage;

namespace {

struct Fixture {
    Fixture() : dir(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        svc = std::make_unique<LlmConfigService>(*storage);
        EXPECT_TRUE(svc->init());
    }

    std::string file_path() const { return dir + "/system/secrets/llm.json"; }

    std::string file_contents() const {
        std::string out;
        FILE* f = std::fopen(file_path().c_str(), "rb");
        if (f == nullptr) return out;
        char buf[512];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        return out;
    }

    // Simulates a reboot: a fresh StorageManager and service over the same
    // directory.
    void restart() {
        svc.reset();
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        svc = std::make_unique<LlmConfigService>(*storage);
        EXPECT_TRUE(svc->init());
    }

    std::string dir;
    EventBus bus;
    DeviceRegistry registry;
    std::unique_ptr<StorageManager> storage;
    std::unique_ptr<LlmConfigService> svc;
};

}  // namespace

TEST(LlmConfig, StartsEmpty) {
    Fixture fx;
    auto c = fx.svc->get();
    EXPECT_EQ(c.base_url, "");
    EXPECT_EQ(c.model, "");
    EXPECT_FALSE(c.has_key);
    EXPECT_EQ(fx.svc->authorization_for_request(), "");
}

TEST(LlmConfig, KeyIsWriteOnly) {
    wisp::log::init(64 * 1024);
    Fixture fx;
    ASSERT_TRUE(fx.svc->set(
        {.base_url = "https://api.example.com/v1/", .model = "gpt-x", .api_key = "sk-SECRET123"}));
    auto c = fx.svc->get();
    EXPECT_EQ(c.base_url, "https://api.example.com/v1");
    EXPECT_EQ(c.model, "gpt-x");
    EXPECT_TRUE(c.has_key);
    EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-SECRET123");
    EXPECT_EQ(wisp::log::ring_snapshot().find("SECRET"), std::string::npos);
    // The key is persisted in the secrets file and nowhere else.
    EXPECT_NE(fx.file_contents().find("sk-SECRET123"), std::string::npos);
}

TEST(LlmConfig, OmittedKeyIsKeptEmptyKeyClears) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set({.base_url = "https://a.example", .model = "m1", .api_key = "sk-1"}));

    // api_key omitted: the stored key survives an update of other fields.
    ASSERT_TRUE(fx.svc->set({.model = "m2"}));
    auto c = fx.svc->get();
    EXPECT_EQ(c.base_url, "https://a.example");
    EXPECT_EQ(c.model, "m2");
    EXPECT_TRUE(c.has_key);
    EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-1");

    // api_key "" clears it.
    ASSERT_TRUE(fx.svc->set({.api_key = ""}));
    EXPECT_FALSE(fx.svc->get().has_key);
    EXPECT_EQ(fx.svc->authorization_for_request(), "");
    EXPECT_EQ(fx.file_contents().find("sk-1"), std::string::npos);
}

// The key belongs to the origin (scheme://host:port) it was saved for:
// moving base_url to another origin without a new key forgets the key, so
// it is never sent to a host the owner did not give it to.
TEST(LlmConfig, ChangingTheOriginWithoutAKeyClearsTheKey) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set({.base_url = "https://a.example/v1", .api_key = "sk-old"}));

    ASSERT_TRUE(fx.svc->set({.base_url = "https://b.example/v1"}));
    auto c = fx.svc->get();
    EXPECT_EQ(c.base_url, "https://b.example/v1");
    EXPECT_FALSE(c.has_key);
    EXPECT_EQ(fx.svc->authorization_for_request(), "");
    EXPECT_EQ(fx.file_contents().find("sk-old"), std::string::npos);
    fx.restart();
    EXPECT_FALSE(fx.svc->get().has_key);
}

TEST(LlmConfig, EveryOriginComponentCounts) {
    for (const char* next : {"http://a.example/v1", "https://a.example:8443/v1",
                             "https://a.example.evil/v1", "https://evil@b.example/v1"}) {
        Fixture fx;
        ASSERT_TRUE(fx.svc->set({.base_url = "https://a.example/v1", .api_key = "sk-1"}));
        ASSERT_TRUE(fx.svc->set({.base_url = std::string(next)}));
        EXPECT_FALSE(fx.svc->get().has_key) << next;
    }
}

TEST(LlmConfig, SameOriginKeepsTheKey) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set({.base_url = "https://a.example/v1", .api_key = "sk-1"}));
    // Path, case of the host, an explicit default port and user info do
    // not change where the request goes.
    for (const char* next : {"https://a.example/v2", "https://A.Example/v1",
                             "https://a.example:443/v1", "https://user@a.example/v1"}) {
        ASSERT_TRUE(fx.svc->set({.base_url = std::string(next)}));
        EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-1") << next;
    }
    ASSERT_TRUE(fx.svc->set({.base_url = "http://10.0.0.2/v1", .api_key = "sk-2"}));
    ASSERT_TRUE(fx.svc->set({.base_url = "http://10.0.0.2:80"}));
    EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-2");
    ASSERT_TRUE(fx.svc->set({.base_url = "http://[fd00::2]:8000/v1", .api_key = "sk-3"}));
    ASSERT_TRUE(fx.svc->set({.base_url = "http://[FD00::2]:8000/v2"}));
    EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-3");
    ASSERT_TRUE(fx.svc->set({.base_url = "http://[fd00::3]:8000/v2"}));
    EXPECT_FALSE(fx.svc->get().has_key);
}

TEST(LlmConfig, NewOriginWithANewKeyKeepsTheNewKey) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set({.base_url = "https://a.example/v1", .api_key = "sk-a"}));
    ASSERT_TRUE(fx.svc->set({.base_url = "https://b.example/v1", .api_key = "sk-b"}));
    EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-b");
}

// A key stored before any base_url was saved for no origin at all.
TEST(LlmConfig, FirstBaseUrlWithoutAKeyClearsAnUnboundKey) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set({.api_key = "sk-early"}));
    ASSERT_TRUE(fx.svc->set({.base_url = "https://a.example/v1"}));
    EXPECT_FALSE(fx.svc->get().has_key);
}

TEST(LlmConfig, RejectsNonHttpUrl) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set({.base_url = "http://10.0.0.2:8000/v1", .api_key = "sk-keep"}));
    for (const char* bad : {"ftp://x", "api.example.com", "", "https://", "http:///",
                            "javascript:1", "https://ex ample.com", "https://a.example/\r\nX: y"}) {
        auto r = fx.svc->set({.base_url = std::string(bad), .api_key = "sk-new"});
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << bad;
        EXPECT_EQ(r.error().message.find("sk-new"), std::string::npos);
    }
    // A rejected update changes nothing, including the key that came with it.
    EXPECT_EQ(fx.svc->get().base_url, "http://10.0.0.2:8000/v1");
    EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-keep");
}

TEST(LlmConfig, RejectsOverlongModel) {
    Fixture fx;
    EXPECT_TRUE(fx.svc->set({.model = std::string(128, 'm')}));
    auto r = fx.svc->set({.model = std::string(129, 'm')});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(fx.svc->get().model, std::string(128, 'm'));
}

TEST(LlmConfig, RejectsKeyThatWouldBreakTheHeader) {
    Fixture fx;
    for (const char* bad : {"sk-a\r\nX-Evil: 1", "sk a", "sk-\x01"}) {
        auto r = fx.svc->set({.api_key = std::string(bad)});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(r.error().message.find("sk"), std::string::npos);
    }
    EXPECT_FALSE(fx.svc->get().has_key);
}

TEST(LlmConfig, SurvivesRestart) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set(
        {.base_url = "https://api.example.com/v1", .model = "gpt-x", .api_key = "sk-persist"}));
    fx.restart();
    auto c = fx.svc->get();
    EXPECT_EQ(c.base_url, "https://api.example.com/v1");
    EXPECT_EQ(c.model, "gpt-x");
    EXPECT_TRUE(c.has_key);
    EXPECT_EQ(fx.svc->authorization_for_request(), "Bearer sk-persist");
}

TEST(LlmConfig, CorruptFileStartsEmpty) {
    Fixture fx;
    std::filesystem::create_directories(fx.dir + "/system/secrets");
    FILE* f = std::fopen(fx.file_path().c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("{not json", f);
    std::fclose(f);
    fx.restart();
    EXPECT_FALSE(fx.svc->get().has_key);
    EXPECT_EQ(fx.svc->get().base_url, "");
}

TEST(LlmConfig, ClearForgetsEverythingAcrossRestart) {
    Fixture fx;
    ASSERT_TRUE(fx.svc->set({.base_url = "https://a.example", .model = "m", .api_key = "sk-gone"}));
    fx.svc->clear();
    EXPECT_FALSE(fx.svc->get().has_key);
    EXPECT_EQ(fx.svc->get().base_url, "");
    EXPECT_FALSE(std::filesystem::exists(fx.file_path()));
    fx.restart();
    EXPECT_FALSE(fx.svc->get().has_key);
    EXPECT_EQ(fx.svc->authorization_for_request(), "");
}
