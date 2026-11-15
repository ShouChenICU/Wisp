// The voice wired into the App (mvp spec 4, 6.3; components/wisp_app/src/
// voice_wiring.hpp): built and started with the agent, the talk key on the
// button marked "talk", and booting without a mic.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "support/bus_flood.hpp"
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
    // `tweak` changes the options before the harness is built.
    explicit VoiceApp(bool with_mic, const std::function<void(UiNetOptions&)>& tweak = {}) {
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
        if (tweak) tweak(opts);
        h = std::make_unique<UiNetHarness>(opts);
    }
    ~VoiceApp() {
        if (server != nullptr) server->release();
        h.reset();
        wisp::log::reset_for_test();
    }

    // Boots, then creates (and so activates) a profile of each kind asked
    // for.
    bool start(bool llm = true, bool asr = true, bool tts = true) {
        if (!h->start("voice.")) return false;
        wisp::core::StorageWorker* worker = h->app->storage_worker();
        if (worker == nullptr) return false;
        bool ok = true;
        auto made = worker->run_sync(
            [&] {
                wisp::core::ModelProfileService& m = h->app->models();
                if (llm) {
                    ok = ok && m.create({.kind = wisp::core::ModelKind::Llm,
                                         .name = "llm",
                                         .preset = "custom",
                                         .base_url = std::string("https://llm.example.com/v1"),
                                         .model = std::string("m"),
                                         .api_key = std::string("sk-llm")})
                                   .is_ok();
                }
                if (asr) {
                    ok = ok && m.create({.kind = wisp::core::ModelKind::Asr,
                                         .name = "asr",
                                         .preset = "custom",
                                         .base_url = std::string("https://asr.example.com/v1"),
                                         .model = std::string("whisper-1"),
                                         .api_key = std::string("sk-asr")})
                                   .is_ok();
                }
                if (tts) {
                    ok = ok && m.create({.kind = wisp::core::ModelKind::Tts,
                                         .name = "tts",
                                         .preset = "custom",
                                         .base_url = std::string("https://tts.example.com/v1"),
                                         .model = std::string("tts-1"),
                                         .api_key = std::string("sk-tts")})
                                   .is_ok();
                }
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

// Stores `changes` while wisp.bus is held and floods it, so the
// system.settings event they publish is dropped before the bus runs again:
// what follows sees the change only through the settings cell.
void set_flooded(UiNetHarness& h, const wisp::Map& changes) {
    const uint64_t dropped = h.app->bus().dropped();
    {
        wisp::test::BusFlood flood(h.app->bus());
        flood.hold();
        EXPECT_TRUE(h.app->system_settings().set(changes));
        flood.flood(200);
        flood.release();
    }
    EXPECT_TRUE(h.app->bus().wait_idle(2000));
    EXPECT_GT(h.app->bus().dropped(), dropped);
}

// Runs one voice turn from recognised text, its reply "<say>好的。</say>"
// with `tts` queued as its speech; true once the turn has ended.
bool voice_reply(VoiceApp& v, const FakeTtsReply& tts) {
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    if (conv == nullptr) return false;
    v.server->llm().push({.status = 200,
                          .chunks = {wisp::test::FakeLlmServer::sse_text("<say>好的。</say>"),
                                     wisp::test::FakeLlmServer::sse_finish("stop"),
                                     wisp::test::FakeLlmServer::sse_done()}});
    v.server->push_tts(tts);
    if (!conv->voice_text("你好")) return false;
    return wait_until(
        [&] {
            auto t = conv->current();
            return t.turn != 0 && !t.running;
        },
        10000);
}

size_t tts_requests(VoiceApp& v) {
    size_t n = 0;
    for (const auto& r : v.server->requests()) {
        if (r.url.find("/audio/speech") != std::string::npos) ++n;
    }
    return n;
}

}  // namespace

TEST(AppVoice, BootWiresVoiceAndTalkKey) {
    VoiceApp v(/*with_mic=*/true);
    ASSERT_TRUE(v.start());
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    ASSERT_TRUE(wait_until([&] { return conv->readiness().complete(); }, 10000));

    v.server->push_asr(FakeReply{.status = 200, .chunks = {R"({"text":"你好"})"}});
    v.server->llm().push({.status = 200,
                          .chunks = {wisp::test::FakeLlmServer::sse_text("<say>你好呀。</say>"),
                                     wisp::test::FakeLlmServer::sse_finish("stop"),
                                     wisp::test::FakeLlmServer::sse_done()}});
    v.server->push_tts(pcm(200));
    v.mic->set_script({silence(300), sine(1000)});
    // The press primes the mic, and the fake reads its script as fast as
    // it is read: the words come once the hold has begun.
    v.mic->hold_at(0);

    // Held past 300 ms: wisp.voice records.
    v.key->down = true;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 3000));
    v.mic->release();
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

// The talk key's inbox has its line in the dispatch report, after the
// power key's; it goes with the voice.
TEST(AppVoice, DispatchReportListsTheTalkKeyInbox) {
    VoiceApp v(/*with_mic=*/true);
    ASSERT_TRUE(v.start());
    const std::string report = v.h->app->dispatch_report();
    EXPECT_TRUE(std::regex_match(report, std::regex("^bus dropped=[0-9]+ cell_subs=[0-9]+\n"
                                                    "inbox scene_lifecycle overflows=0\n"
                                                    "inbox power_key overflows=0\n"
                                                    "inbox talk_key overflows=0$")))
        << report;
    v.h->app->stop();
    EXPECT_EQ(v.h->app->dispatch_report().find("talk_key"), std::string::npos);
}

TEST(AppVoice, NoMicStillBoots) {
    VoiceApp v(/*with_mic=*/false);
    ASSERT_TRUE(v.start());
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    ASSERT_TRUE(wait_until([&] { return conv->readiness().complete(); }, 10000));
    v.key->down = true;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 3000));
    ASSERT_TRUE(wait_until(
        [&] { return conv->current().error == wisp::voice::VoiceError::MicUnavailable; }, 3000));
    v.key->down = false;
    ASSERT_TRUE(wait_until([&] { return !conv->holding(); }, 3000));
    EXPECT_EQ(v.voice_reasons(), std::vector<std::string>{"mic_unavailable"});
    EXPECT_TRUE(v.server->requests().empty());
}

// Spec 2.3: the mic is found by role, whatever its id.
TEST(AppVoice, FindsTheMicByRole) {
    VoiceApp v(/*with_mic=*/true);
    v.mic->set_id("audio_in.mic");
    ASSERT_TRUE(v.start());
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    ASSERT_TRUE(wait_until([&] { return conv->readiness().complete(); }, 10000));
    v.mic->set_script({silence(300)});
    v.key->down = true;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 3000));
    EXPECT_TRUE(wait_until([&] { return v.mic->opens() > 0; }, 3000));
    v.key->down = false;
    ASSERT_TRUE(wait_until([&] { return !conv->holding(); }, 3000));
    for (const auto& reason : v.voice_reasons()) EXPECT_NE(reason, "mic_unavailable");
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

// The checklist from the services: each profile kind, and the network.
TEST(AppVoice, ReadinessListsWhatIsMissing) {
    VoiceApp v(/*with_mic=*/true);
    ASSERT_TRUE(v.start(/*llm=*/true, /*asr=*/false, /*tts=*/false));
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    using wisp::shell::Readiness;
    EXPECT_TRUE(wait_until(
        [&] { return conv->readiness() == Readiness{.asr = false, .tts = false}; }, 10000));
    EXPECT_FALSE(conv->readiness().can_talk());
}

// Muted, speech is not needed: no tts profile is not missing.
TEST(AppVoice, MutedDeviceDoesNotAskForTts) {
    VoiceApp v(/*with_mic=*/true);
    ASSERT_TRUE(v.start(/*llm=*/true, /*asr=*/true, /*tts=*/false));
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    using wisp::shell::Readiness;
    ASSERT_TRUE(wait_until([&] { return conv->readiness() == Readiness{.tts = false}; }, 10000));
    EXPECT_TRUE(conv->readiness().can_talk());
    ASSERT_TRUE(v.h->app->system_settings().set({{"tts_mute", true}}));
    EXPECT_TRUE(wait_until([&] { return conv->readiness().complete(); }, 3000));
}

// Offline and no connection being made -- a saved network that is out of
// reach waits in backoff, or none is saved -- is down: a hold would only
// wait and fail (WifiService::connection_expected()).
TEST(AppVoice, OfflineIsDownWhileNoConnectionIsBeingMade) {
    using wisp::shell::Readiness;
    {
        VoiceApp v(/*with_mic=*/true, [](UiNetOptions& o) { o.wifi_reachable = false; });
        ASSERT_TRUE(v.start());
        wisp::shell::ConversationController* conv = v.h->app->conversation();
        ASSERT_NE(conv, nullptr);
        EXPECT_TRUE(wait_until(
            [&] { return conv->readiness() == Readiness{.net = Readiness::Net::Down}; }, 10000));
        EXPECT_FALSE(conv->readiness().can_talk());
    }
    {
        VoiceApp v(/*with_mic=*/true, [](UiNetOptions& o) { o.saved.clear(); });
        ASSERT_TRUE(v.start());
        wisp::shell::ConversationController* conv = v.h->app->conversation();
        ASSERT_NE(conv, nullptr);
        EXPECT_TRUE(wait_until(
            [&] { return conv->readiness() == Readiness{.net = Readiness::Net::Down}; }, 10000));
        EXPECT_FALSE(conv->readiness().can_talk());
    }
}

// Mute is read from the settings cell when a reply would be spoken: muted
// while its system.settings event is lost, the next reply is not spoken.
TEST(AppVoice, MuteFollowsStoredSettingsFlooded) {
    VoiceApp v(/*with_mic=*/true);
    ASSERT_TRUE(v.start());
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    ASSERT_TRUE(wait_until([&] { return conv->readiness().complete(); }, 10000));

    set_flooded(*v.h, {{"tts_mute", wisp::Value(true)}});
    ASSERT_TRUE(voice_reply(v, pcm(200)));
    EXPECT_EQ(conv->current().say, "好的。");
    EXPECT_FALSE(conv->current().spoken);
    EXPECT_EQ(tts_requests(v), 0u);
    EXPECT_EQ(v.out->frames(), 0u);
}

// The reply's volume is read from the settings cell when its speech
// starts: samples of 1000 at volume 30 are written as 300, though the
// change's system.settings event was lost.
TEST(AppVoice, VolumeFollowsStoredSettingsFlooded) {
    VoiceApp v(/*with_mic=*/true);
    ASSERT_TRUE(v.start());
    wisp::shell::ConversationController* conv = v.h->app->conversation();
    ASSERT_NE(conv, nullptr);
    ASSERT_TRUE(wait_until([&] { return conv->readiness().complete(); }, 10000));

    set_flooded(*v.h, {{"volume", wisp::Value(30)}});
    ASSERT_TRUE(voice_reply(v, pcm(200)));
    ASSERT_TRUE(wait_until([&] { return conv->current().spoken; }, 5000));
    ASSERT_TRUE(wait_until([&] { return v.out->frames() >= 24u * 200; }, 5000));
    int peak = 0;
    for (int16_t s : v.out->samples()) peak = std::max(peak, std::abs(int{s}));
    EXPECT_EQ(peak, 300);
}
