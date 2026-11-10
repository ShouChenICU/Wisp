// wisp::voice::TtsClient (wisp/voice/tts_client.hpp) against the scripted
// FakeModelServer.

#include "wisp/voice/tts_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
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
        // Not part of OpenAI's request: servers such as speech.cpp refuse
        // unknown fields with 400, so it is sent only when set.
        EXPECT_EQ(body.find("sample_rate"), nullptr);
        EXPECT_EQ(body.find("speed"), nullptr);
        EXPECT_EQ(f.server.header(0, "authorization"), "Bearer sk-tts");
        // "hello" is 5 code points: 15000 + 5 * 400 (see
        // TtsClient.TimeoutGrowsWithSentenceLength for the general case).
        EXPECT_EQ(f.server.requests()[0].timeout_ms, 17000u);
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
    {
        // sample_rate only when set.
        Fixture f(/*with_profile=*/true, ModelOptions{{"sample_rate", "16000"}});
        f.server.push_tts({.status = 200});
        f.client->synthesize(
            "hi", [](std::span<const int16_t>) { return true; }, [] { return false; });
        Value body = f.server.body_json(0);
        EXPECT_EQ(*body.find("sample_rate"), Value(int64_t{16000}));
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
        std::atomic<uint32_t> ticket{0};
        TtsResult r;
        std::thread t([&] {
            r = f.client->synthesize(
                "hi", [](std::span<const int16_t>) { return true; }, [] { return false; },
                watch.get(), [&](uint32_t tk) { ticket = tk; });
        });
        ASSERT_TRUE(wisp::test::wait_until([&] { return f.server.held() == 1; }, 2000));
        ASSERT_NE(ticket.load(), 0u);
        watch->abort(ticket.load());
        t.join();
        EXPECT_EQ(r.error, "cancelled");
    }
}

TEST(TtsClient, FailedResponseBodyNeverReachesOnPcm) {
    Fixture f;
    std::vector<std::vector<uint8_t>> chunks = {{1, 2, 3, 4}};
    f.server.push_tts({.status = 500, .chunks = chunks});
    bool called = false;

    TtsResult r = f.client->synthesize(
        "hi",
        [&](std::span<const int16_t>) {
            called = true;
            return true;
        },
        [] { return false; });

    EXPECT_EQ(r.error, "server");
    EXPECT_FALSE(called);
    EXPECT_EQ(r.bytes, 0u);
}

TEST(TtsClient, MaxResponseBytesSizedFromTheDeadline) {
    Fixture f;
    f.server.push_tts({.status = 200});

    f.client->synthesize("hi", [](std::span<const int16_t>) { return true; }, [] { return false; });

    auto reqs = f.server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    // Default rate 24000 Hz, "hi" is 2 code points: timeout = 15000 + 2 *
    // 400 = 15800 ms; cap = 24000 * 2 * 15800 / 1000.
    EXPECT_EQ(reqs[0].timeout_ms, 15800u);
    EXPECT_EQ(reqs[0].max_response_bytes, 758400u);
}

TEST(TtsClient, TimeoutGrowsWithSentenceLength) {
    Fixture f;
    f.server.push_tts({.status = 200});
    const std::string text(200, 'a');  // 200 code points (ASCII)

    f.client->synthesize(text, [](std::span<const int16_t>) { return true; }, [] { return false; });

    auto reqs = f.server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].timeout_ms, 15000u + 80000u);
}
