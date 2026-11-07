// wisp::voice::AsrClient (wisp/voice/asr_client.hpp) against the scripted
// FakeModelServer.

#include "wisp/voice/asr_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_models.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/voice/capture_buffer.hpp"

using wisp::Result;
using wisp::core::ModelKind;
using wisp::test::FakeModelServer;
using wisp::voice::AsrClient;
using wisp::voice::AsrResult;
using wisp::voice::CaptureBuffer;

namespace {

constexpr const char* kKey = "sk-asr";
constexpr const char* kBase = "https://asr.example.com/v1";

Result<CaptureBuffer> make_audio(size_t frames = 100) {
    auto r = CaptureBuffer::create();
    if (!r) return r;
    auto space = r.value().free_space();
    std::fill(space.begin(), space.begin() + frames, int16_t{0});
    r.value().commit(frames);
    return r;
}

struct Fixture {
    explicit Fixture(bool with_profile = true) : dir(wisp::test::temp_dir().string()) {
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
            auto r = models->create({.kind = ModelKind::Asr,
                                     .name = "asr",
                                     .preset = "custom",
                                     .base_url = std::string(kBase),
                                     .model = std::string("whisper-1"),
                                     .api_key = std::string(kKey)});
            EXPECT_TRUE(r);
        }
        client = std::make_unique<AsrClient>(server, *models, [this](uint32_t ms) {
            sleeps.push_back(ms);
            if (on_sleep) on_sleep();
        });
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
    std::vector<uint32_t> sleeps;
    std::function<void()> on_sleep;
    std::unique_ptr<AsrClient> client;
};

}  // namespace

TEST(AsrClient, RequestShape) {
    Fixture f;
    f.server.push_asr({.status = 200, .chunks = {R"({"text":"hi"})"}});
    auto audio = make_audio();
    ASSERT_TRUE(audio);

    AsrResult r = f.client->transcribe(audio.value(), [] { return false; });

    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.text, "hi");
    EXPECT_EQ(r.attempts, 1u);
    auto reqs = f.server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].url, std::string(kBase) + "/audio/transcriptions");
    EXPECT_EQ(f.server.header(0, "authorization"), "Bearer sk-asr");
    EXPECT_EQ(reqs[0].timeout_ms, 15000u);
    EXPECT_NE(reqs[0].body.find("name=\"model\""), std::string::npos);
    EXPECT_NE(reqs[0].body.find("whisper-1"), std::string::npos);
    EXPECT_NE(reqs[0].body.find("name=\"file\""), std::string::npos);
}

TEST(AsrClient, ParsesTextEmptyIsNotAnError) {
    Fixture f;
    f.server.push_asr({.status = 200, .chunks = {R"({"text":""})"}});
    auto audio = make_audio();
    ASSERT_TRUE(audio);

    AsrResult r = f.client->transcribe(audio.value(), [] { return false; });

    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.text, "");
}

TEST(AsrClient, RetriesOnceOn429And5xx) {
    {
        Fixture f;
        f.server.push_asr({.status = 503});
        f.server.push_asr({.status = 200, .chunks = {R"({"text":"ok"})"}});
        auto audio = make_audio();
        ASSERT_TRUE(audio);
        AsrResult r = f.client->transcribe(audio.value(), [] { return false; });
        EXPECT_EQ(r.error, "");
        EXPECT_EQ(r.text, "ok");
        EXPECT_EQ(r.attempts, 2u);
        EXPECT_EQ(f.sleeps, (std::vector<uint32_t>{1000}));
        EXPECT_EQ(f.server.requests().size(), 2u);
    }
    {
        Fixture f;
        f.server.push_asr({.status = 500});
        f.server.push_asr({.status = 500});
        auto audio = make_audio();
        ASSERT_TRUE(audio);
        AsrResult r = f.client->transcribe(audio.value(), [] { return false; });
        EXPECT_EQ(r.error, "server");
        EXPECT_EQ(r.attempts, 2u);
    }
    {
        Fixture f;
        f.server.push_asr({.status = 401});
        auto audio = make_audio();
        ASSERT_TRUE(audio);
        AsrResult r = f.client->transcribe(audio.value(), [] { return false; });
        EXPECT_EQ(r.error, "unauthorized");
        EXPECT_EQ(r.attempts, 1u);
        EXPECT_TRUE(f.sleeps.empty());
    }
}

TEST(AsrClient, NoProfileMakesNoRequest) {
    Fixture f(/*with_profile=*/false);
    auto audio = make_audio();
    ASSERT_TRUE(audio);

    AsrResult r = f.client->transcribe(audio.value(), [] { return false; });

    EXPECT_EQ(r.error, "no_profile");
    EXPECT_EQ(r.attempts, 0u);
    EXPECT_EQ(f.server.requests().size(), 0u);
}

TEST(AsrClient, CancelStopsTheUpload) {
    Fixture f;
    f.server.push_asr({.hold_after = 0});
    auto audio = make_audio();
    ASSERT_TRUE(audio);
    std::atomic<bool> stop{false};
    AsrResult r;

    std::thread t([&] { r = f.client->transcribe(audio.value(), [&] { return stop.load(); }); });
    ASSERT_TRUE(wisp::test::wait_until([&] { return f.server.held() == 1; }, 2000));
    stop = true;
    t.join();

    EXPECT_EQ(r.error, "cancelled");
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_EQ(f.server.requests().size(), 1u);
}

TEST(AsrClient, LogsOnlyMetadata) {
    Fixture f;
    f.server.push_asr({.status = 200, .chunks = {R"({"text":"a secret transcript"})"}});
    auto audio = make_audio();
    ASSERT_TRUE(audio);

    AsrResult r = f.client->transcribe(audio.value(), [] { return false; });
    EXPECT_EQ(r.text, "a secret transcript");

    std::vector<std::string> lines;
    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_EQ(e.msg.find("secret transcript"), std::string::npos) << e.msg;
        EXPECT_EQ(e.msg.find(kKey), std::string::npos) << e.msg;
        if (e.tag == "voice") lines.push_back(e.msg);
    }
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("asr ms=", 0), 0u) << lines[0];
}
