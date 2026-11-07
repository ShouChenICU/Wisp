// The voice wired into the App (mvp spec 4, 6.3; components/wisp_app/src/
// voice_wiring.hpp): built and started with the agent, the talk key on the
// button marked "talk", and booting without a mic.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "support/fake_audio.hpp"
#include "support/fake_devices.hpp"
#include "support/fake_models.hpp"
#include "support/wait.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/os.hpp"
#include "wisp/shell/conversation.hpp"

using wisp::test::FakeButton;
using wisp::test::FakeModelServer;
using wisp::test::FakeReply;
using wisp::test::FakeTtsReply;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::test::audio::silence;
using wisp::test::audio::sine;

namespace {

struct VoiceApp {
    explicit VoiceApp(bool with_mic) {
        wisp::log::reset_for_test();
        wisp::log::init();
        opts.saved = {{"Home", "password1"}};
        opts.audio_out = out;
        opts.extra_devices.push_back(key);
        if (with_mic) opts.extra_devices.push_back(mic);
        opts.http_client = [this] {
            auto s = std::make_unique<FakeModelServer>();
            server = s.get();
            return std::unique_ptr<wisp::net::HttpClient>(std::move(s));
        };
        h = std::make_unique<UiNetHarness>(opts);
    }
    ~VoiceApp() {
        if (server != nullptr) server->release();
        h.reset();
        wisp::log::reset_for_test();
    }

    bool start() {
        if (!h->start("voice.")) return false;
        wisp::core::StorageWorker* worker = h->app->storage_worker();
        if (worker == nullptr) return false;
        bool ok = true;
        auto made = worker->run_sync(
            [&] {
                wisp::core::ModelProfileService& m = h->app->models();
                ok = m.create({.kind = wisp::core::ModelKind::Llm,
                               .name = "llm",
                               .preset = "custom",
                               .base_url = std::string("https://llm.example.com/v1"),
                               .model = std::string("m"),
                               .api_key = std::string("sk-llm")})
                         .is_ok() &&
                     m.create({.kind = wisp::core::ModelKind::Asr,
                               .name = "asr",
                               .preset = "custom",
                               .base_url = std::string("https://asr.example.com/v1"),
                               .model = std::string("whisper-1"),
                               .api_key = std::string("sk-asr")})
                         .is_ok() &&
                     m.create({.kind = wisp::core::ModelKind::Tts,
                               .name = "tts",
                               .preset = "custom",
                               .base_url = std::string("https://tts.example.com/v1"),
                               .model = std::string("tts-1"),
                               .api_key = std::string("sk-tts")})
                         .is_ok();
            },
            wisp::os::kForever);
        return made.is_ok() && ok;
    }

    std::vector<std::string> voice_reasons() {
        std::vector<std::string> outv;
        for (const auto& e : h->topic("voice.state")) {
            auto it = e.payload.find("reason");
            if (it != e.payload.end()) outv.push_back(it->second.as_string());
        }
        return outv;
    }
    bool saw_voice(const std::string& state) {
        for (const auto& e : h->topic("voice.state")) {
            auto it = e.payload.find("state");
            if (it != e.payload.end() && it->second.as_string() == state) return true;
        }
        return false;
    }

    UiNetOptions opts;
    std::shared_ptr<wisp::test::audio::FakeAudioOut> out =
        std::make_shared<wisp::test::audio::FakeAudioOut>();
    std::shared_ptr<wisp::test::audio::FakeAudioIn> mic =
        std::make_shared<wisp::test::audio::FakeAudioIn>();
    std::shared_ptr<FakeButton> key = std::make_shared<FakeButton>(
        "button.vol_down", wisp::Properties{{"role", wisp::Value("talk")}});
    FakeModelServer* server = nullptr;  // owned by the App's Platform
    std::unique_ptr<UiNetHarness> h;
};

FakeTtsReply pcm(uint32_t ms) {
    std::vector<int16_t> samples(24 * ms, 1000);
    FakeTtsReply r{.status = 200, .chunks = {}};
    const auto* bytes = reinterpret_cast<const uint8_t*>(samples.data());
    r.chunks.emplace_back(bytes, bytes + samples.size() * sizeof(int16_t));
    return r;
}

}  // namespace

TEST(AppVoice, BootWiresVoiceAndTalkKey) {
    VoiceApp v(/*with_mic=*/true);
    ASSERT_TRUE(v.start());
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    ASSERT_TRUE(
        wait_until([&] { return conv->readiness() == wisp::shell::Readiness::Ready; }, 10000));

    v.server->push_asr(FakeReply{.status = 200, .chunks = {R"({"text":"你好"})"}});
    v.server->llm().push({.status = 200,
                          .chunks = {wisp::test::FakeLlmServer::sse_text("<say>你好呀。</say>"),
                                     wisp::test::FakeLlmServer::sse_finish("stop"),
                                     wisp::test::FakeLlmServer::sse_done()}});
    v.server->push_tts(pcm(200));
    v.mic->set_script({silence(300), sine(1000)});

    // Held past 300 ms: wisp.voice records.
    v.key->down = true;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 3000));
    ASSERT_TRUE(wait_until([&] { return v.saw_voice("listening"); }, 3000));
    ASSERT_TRUE(wait_until([&] { return v.mic->script_done(); }, 3000));
    v.key->down = false;
    ASSERT_TRUE(wait_until([&] { return !conv->holding(); }, 3000));

    // The reply is spoken through wisp.play.
    ASSERT_TRUE(wait_until(
        [&] {
            auto t = conv->current();
            return t.turn != 0 && !t.running && t.spoken;
        },
        10000));
    ASSERT_TRUE(wait_until([&] { return v.out->frames() >= 24u * 200; }, 5000));
    EXPECT_EQ(conv->current().heard, "你好");
    EXPECT_EQ(conv->current().source, wisp::agent::Source::Voice);
    EXPECT_TRUE(v.voice_reasons().empty());
    EXPECT_TRUE(wait_until(
        [&] {
            for (const auto& l : v.h->app->wakelocks().list())
                if (l.holder == "system.voice") return false;
            return true;
        },
        5000));
}

TEST(AppVoice, NoMicStillBoots) {
    VoiceApp v(/*with_mic=*/false);
    ASSERT_TRUE(v.start());
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    ASSERT_TRUE(
        wait_until([&] { return conv->readiness() == wisp::shell::Readiness::Ready; }, 10000));
    v.key->down = true;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 3000));
    ASSERT_TRUE(wait_until(
        [&] { return conv->current().error == wisp::voice::VoiceError::MicUnavailable; }, 3000));
    v.key->down = false;
    ASSERT_TRUE(wait_until([&] { return !conv->holding(); }, 3000));
    EXPECT_EQ(v.voice_reasons(), std::vector<std::string>{"mic_unavailable"});
    EXPECT_TRUE(v.server->requests().empty());
}

TEST(AppVoice, AgentNotRunningLeavesVoiceOff) {
    VoiceApp v(/*with_mic=*/true);
    // An unreadable conversation file: the agent does not start.
    std::filesystem::create_directories(v.h->root + "/system/agent/current.jsonl");
    ASSERT_TRUE(v.start());
    EXPECT_EQ(v.h->app->conversation(), nullptr);
    v.key->down = true;
    wisp::os::sleep_ms(500);
    v.key->down = false;
    EXPECT_FALSE(v.saw_voice("listening"));
}
