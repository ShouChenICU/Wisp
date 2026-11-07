// wisp::voice::TtsClient (wisp/voice/tts_client.hpp) against the scripted
// FakeModelServer.

#include "wisp/voice/tts_client.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_models.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/request_watch.hpp"

using wisp::Map;
using wisp::Value;
using wisp::core::ModelKind;
using wisp::core::ModelOptions;
using wisp::test::FakeModelServer;
using wisp::test::FakeTtsReply;
using wisp::voice::TtsClient;
using wisp::voice::TtsResult;

namespace {

constexpr const char* kKey = "sk-tts";
constexpr const char* kBase = "https://tts.example.com/v1";

struct Fixture {
    explicit Fixture(bool with_profile = true, ModelOptions options = {})
        : dir(wisp::test::temp_dir().string()) {
        wisp::log::reset_for_test();
        wisp::log::init();
        registry.add(
            std::make_shared<wisp::test::FakeStorage>("storage.int", dir, /*removable=*/false));
        bus.start();
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        models = std::make_unique<wisp::core::ModelProfileService>(*storage, bus);
        EXPECT_TRUE(models->init());
        if (with_profile) {
            auto r = models->create({.kind = ModelKind::Tts,
                                     .name = "tts",
                                     .preset = "custom",
                                     .base_url = std::string(kBase),
                                     .model = std::string("tts-1"),
                                     .options = options,
                                     .api_key = std::string(kKey)});
            EXPECT_TRUE(r);
        }
        client = std::make_unique<TtsClient>(server, *models);
    }

    ~Fixture() {
        client.reset();
        models.reset();
        bus.stop();
    }

    std::string dir;
    wisp::core::EventBus bus;
    wisp::core::DeviceRegistry registry;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<wisp::core::ModelProfileService> models;
    FakeModelServer server;
    std::unique_ptr<TtsClient> client;
};

}  // namespace

TEST(TtsClient, RequestBodyShape) {
    {
        Fixture f(/*with_profile=*/true, ModelOptions{{"voice", "alex"}});
        f.server.push_tts({.status = 200});
        TtsResult r = f.client->synthesize(
            "hello", [](std::span<const int16_t>) { return true; }, [] { return false; });
        EXPECT_EQ(r.error, "");
        Value body = f.server.body_json(0);
        EXPECT_EQ(*body.find("model"), Value("tts-1"));
        EXPECT_EQ(*body.find("input"), Value("hello"));
        EXPECT_EQ(*body.find("voice"), Value("alex"));
        EXPECT_EQ(*body.find("response_format"), Value("pcm"));
        EXPECT_EQ(*body.find("sample_rate"), Value(int64_t{24000}));
        EXPECT_EQ(body.find("speed"), nullptr);
        EXPECT_EQ(f.server.header(0, "authorization"), "Bearer sk-tts");
        EXPECT_EQ(f.server.requests()[0].timeout_ms, 15000u);
    }
    {
        // speed only when set.
        Fixture f(/*with_profile=*/true, ModelOptions{{"voice", "alex"}, {"speed", "1.5"}});
        f.server.push_tts({.status = 200});
        f.client->synthesize(
            "hi", [](std::span<const int16_t>) { return true; }, [] { return false; });
        Value body = f.server.body_json(0);
        EXPECT_EQ(*body.find("speed"), Value(1.5));
    }
}

TEST(TtsClient, StreamsWholeSamplesAcrossOddChunks) {
    Fixture f;
    uint8_t bytes[12];
    for (int i = 0; i < 12; ++i) bytes[i] = static_cast<uint8_t>(i);
    std::vector<std::vector<uint8_t>> chunks;
    size_t off = 0;
    for (size_t sz : {3, 1, 5, 3}) {
        chunks.emplace_back(bytes + off, bytes + off + sz);
        off += sz;
    }
    f.server.push_tts({.status = 200, .chunks = chunks});

    std::vector<int16_t> got;
    TtsResult r = f.client->synthesize(
        "hi",
        [&](std::span<const int16_t> s) {
            got.insert(got.end(), s.begin(), s.end());
            return true;
        },
        [] { return false; });

    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.bytes, 12u);
    ASSERT_EQ(got.size(), 6u);
    int16_t expected[6];
    std::memcpy(expected, bytes, sizeof(expected));
    for (size_t i = 0; i < 6; ++i) EXPECT_EQ(got[i], expected[i]) << i;
}

TEST(TtsClient, SampleRateFromProfile) {
    {
        Fixture f(/*with_profile=*/true, ModelOptions{{"sample_rate", "16000"}});
        auto rate = f.client->sample_rate();
        ASSERT_TRUE(rate.has_value());
        EXPECT_EQ(*rate, 16000u);
    }
    {
        Fixture f(/*with_profile=*/true);
        auto rate = f.client->sample_rate();
        ASSERT_TRUE(rate.has_value());
        EXPECT_EQ(*rate, 24000u);
    }
    {
        Fixture f(/*with_profile=*/false);
        EXPECT_FALSE(f.client->sample_rate().has_value());
    }
}

TEST(TtsClient, ErrorsAndCancel) {
    {
        Fixture f;
        f.server.push_tts({.status = 500});
        TtsResult r = f.client->synthesize(
            "hi", [](std::span<const int16_t>) { return true; }, [] { return false; });
        EXPECT_EQ(r.error, "server");
        EXPECT_EQ(f.server.requests().size(), 1u);
    }
    {
        Fixture f;
        std::vector<std::vector<uint8_t>> chunks = {{1, 2, 3, 4}};
        f.server.push_tts({.status = 200, .chunks = chunks});
        TtsResult r = f.client->synthesize(
            "hi", [](std::span<const int16_t>) { return false; }, [] { return false; });
        EXPECT_EQ(r.error, "cancelled");
    }
    {
        Fixture f;
        f.server.push_tts({.hold_after = 0});
        auto watch = wisp::net::RequestWatch::create();
        ASSERT_NE(watch, nullptr);
        TtsResult r;
        std::thread t([&] {
            r = f.client->synthesize(
                "hi", [](std::span<const int16_t>) { return true; }, [] { return false; },
                watch.get());
        });
        ASSERT_TRUE(wisp::test::wait_until([&] { return f.server.held() == 1; }, 2000));
        // A fresh RequestWatch's first arm() always returns ticket 1.
        watch->abort(1);
        t.join();
        EXPECT_EQ(r.error, "cancelled");
    }
}
