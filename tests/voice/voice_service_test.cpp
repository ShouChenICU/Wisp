// wisp::voice::VoiceService (wisp/voice/voice_service.hpp) with a scripted
// mic, a recording output and the scripted FakeModelServer.

#include "wisp/voice/voice_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_audio.hpp"
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
#include "wisp/core/tone_player.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/os.hpp"

using wisp::Map;
using wisp::Value;
using wisp::core::ModelKind;
using wisp::core::ModelOptions;
using wisp::core::ToneKind;
using wisp::core::WakeLockKind;
using wisp::test::FakeModelServer;
using wisp::test::FakeReply;
using wisp::test::FakeTtsReply;
using wisp::test::wait_until;
using wisp::test::audio::FakeAudioIn;
using wisp::test::audio::FakeAudioOut;
using wisp::test::audio::noise;
using wisp::test::audio::OrderLog;
using wisp::test::audio::silence;
using wisp::test::audio::sine;
using wisp::voice::CaptureMode;
using wisp::voice::VoiceConfig;
using wisp::voice::VoiceService;
using wisp::voice::VoiceState;

static_assert(wisp::voice::kMaxRecordMs == 30000);

namespace {

constexpr const char* kAsrPath = "/audio/transcriptions";
constexpr const char* kTtsPath = "/audio/speech";

std::vector<int16_t> ramp(size_t n, int seed = 0) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<int16_t>(((i + seed) * 13) % 16000 - 8000);
    }
    return v;
}

FakeTtsReply pcm_reply(const std::vector<int16_t>& pcm, int hold_after = -1) {
    FakeTtsReply r{.status = 200, .chunks = {}, .hold_after = hold_after};
    const auto* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    const size_t total = pcm.size() * sizeof(int16_t);
    for (size_t off = 0; off < total; off += 4096) {
        const size_t n = std::min<size_t>(4096, total - off);
        r.chunks.emplace_back(bytes + off, bytes + off + n);
    }
    return r;
}

FakeReply asr_text(const std::string& text) {
    return FakeReply{.status = 200, .chunks = {R"({"text":")" + text + R"("})"}};
}

std::string str(const Map& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second.as_string();
}

struct VoiceServiceTest : ::testing::Test {
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
        dir = wisp::test::temp_dir().string();
        registry.add(
            std::make_shared<wisp::test::FakeStorage>("storage.int", dir, /*removable=*/false));
        ASSERT_TRUE(bus.start());
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        ASSERT_TRUE(storage->init());
        models = std::make_unique<wisp::core::ModelProfileService>(*storage, bus);
        ASSERT_TRUE(models->init());
        sub = bus.subscribe("voice.state", [this](const wisp::core::Event& e) {
            std::lock_guard<std::mutex> lock(mu);
            events_.push_back(e.payload);
        });
        asr = std::make_unique<wisp::voice::AsrClient>(server, *models, [](uint32_t) {});
        tts = std::make_unique<wisp::voice::TtsClient>(server, *models);
        ASSERT_TRUE(player.start());
        ASSERT_TRUE(tones.start());
    }

    void TearDown() override {
        server.release();
        tones.set_output_guard(nullptr);
        voice.reset();
        tones.stop();
        player.stop_task();
        bus.unsubscribe(sub);
        bus.stop();
    }

    void add_asr() {
        ASSERT_TRUE(models->create({.kind = ModelKind::Asr,
                                    .name = "asr",
                                    .preset = "custom",
                                    .base_url = std::string("https://asr.example.com/v1"),
                                    .model = std::string("whisper-1"),
                                    .api_key = std::string("sk-asr")}));
    }

    std::string add_tts(ModelOptions options = {}) {
        auto r = models->create({.kind = ModelKind::Tts,
                                 .name = "tts",
                                 .preset = "custom",
                                 .base_url = std::string("https://tts.example.com/v1"),
                                 .model = std::string("tts-1"),
                                 .options = options,
                                 .api_key = std::string("sk-tts")});
        EXPECT_TRUE(r);
        return r ? r.value().id : std::string();
    }

    void make(VoiceConfig config = {}, bool with_mic = true) {
        voice.reset();
        voice = std::make_unique<VoiceService>(
            wisp::voice::VoiceDeps{
                .bus = bus,
                .mic = with_mic ? mic : nullptr,
                .player = player,
                .asr = *asr,
                .tts = *tts,
                .models = *models,
                .tones = &tones,
                .locks = &locks,
                .network_up = [this] { return net_up.load(); },
                .volume = [] { return uint8_t{100}; },
                .now_ms = [this] { return wisp::os::now_ms() + virtual_ms.load(); },
            },
            config);
        voice->set_on_transcript([this](std::string text) {
            std::lock_guard<std::mutex> lock(mu);
            transcripts.push_back(std::move(text));
        });
        ASSERT_TRUE(voice->start());
    }

    std::vector<Map> events() const {
        std::lock_guard<std::mutex> lock(mu);
        return events_;
    }
    void clear_events() {
        bus.wait_idle(1000);
        std::lock_guard<std::mutex> lock(mu);
        events_.clear();
    }
    // The states in order, repeats (level updates) folded.
    std::vector<std::string> states() const {
        std::vector<std::string> out;
        for (const Map& m : events()) {
            const std::string s = str(m, "state");
            if (out.empty() || out.back() != s) out.push_back(s);
        }
        return out;
    }
    std::vector<std::string> reasons() const {
        std::vector<std::string> out;
        for (const Map& m : events()) {
            if (str(m, "state") == "error") out.push_back(str(m, "reason"));
        }
        return out;
    }
    bool saw(const std::string& state, const char* key = nullptr, const std::string& value = {}) {
        for (const Map& m : events()) {
            if (str(m, "state") != state) continue;
            if (key == nullptr || str(m, key) == value) return true;
        }
        return false;
    }
    // Waits for the service to settle back at idle, its last event idle.
    bool wait_idle(uint32_t timeout_ms = 3000) {
        return wait_until(
            [&] {
                if (voice->state() != VoiceState::Idle) return false;
                bus.wait_idle(100);
                auto e = events();
                return !e.empty() && str(e.back(), "state") == "idle";
            },
            timeout_ms);
    }
    std::vector<std::string> got_transcripts() const {
        std::lock_guard<std::mutex> lock(mu);
        return transcripts;
    }
    size_t count(const char* path) const {
        size_t n = 0;
        for (const auto& r : server.requests()) n += r.url.find(path) != std::string::npos;
        return n;
    }
    std::vector<std::string> tts_inputs() const {
        std::vector<std::string> out;
        const auto reqs = server.requests();
        for (size_t i = 0; i < reqs.size(); ++i) {
            if (reqs[i].url.find(kTtsPath) == std::string::npos) continue;
            out.push_back(server.body_json(i).find("input")->as_string());
        }
        return out;
    }
    size_t voice_locks() const {
        size_t n = 0;
        for (const auto& l : locks.list()) n += l.holder == VoiceService::kHolder;
        return n;
    }
    // Plays a recording through to its transcript.
    void capture_turn(const std::string& text = "你好") {
        const size_t before = got_transcripts().size();
        server.push_asr(asr_text(text));
        mic->set_script({silence(300), sine(600), silence(900)});
        ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
        ASSERT_TRUE(wait_until([&] { return got_transcripts().size() > before; }, 3000));
    }

    std::string dir;
    wisp::core::EventBus bus;
    wisp::core::DeviceRegistry registry;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<wisp::core::ModelProfileService> models;
    FakeModelServer server;
    std::unique_ptr<wisp::voice::AsrClient> asr;
    std::unique_ptr<wisp::voice::TtsClient> tts;
    std::shared_ptr<OrderLog> order = std::make_shared<OrderLog>();
    std::atomic<uint64_t> virtual_ms{0};
    std::shared_ptr<FakeAudioIn> mic = std::make_shared<FakeAudioIn>(
        std::vector<wisp::test::audio::Segment>{}, order, &virtual_ms);
    std::shared_ptr<FakeAudioOut> out = std::make_shared<FakeAudioOut>(order);
    wisp::voice::PcmPlayer player{out};
    wisp::core::WakeLockRegistry locks{bus};
    wisp::core::TonePlayer tones{out, nullptr};
    std::atomic<bool> net_up{true};
    std::unique_ptr<VoiceService> voice;

    mutable std::mutex mu;
    std::vector<Map> events_;
    std::vector<std::string> transcripts;
    wisp::core::SubscriptionId sub = 0;
};

}  // namespace

TEST_F(VoiceServiceTest, HoldCaptureSendsOnRelease) {
    add_asr();
    make();
    server.push_asr(asr_text("你好"));
    mic->set_script({silence(300), sine(1000)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    // Held: it keeps listening past the speech.
    wisp::os::sleep_ms(50);
    EXPECT_EQ(voice->state(), VoiceState::Listening);
    EXPECT_EQ(count(kAsrPath), 0u);
    voice->end_capture();
    ASSERT_TRUE(wait_until([&] { return !got_transcripts().empty(); }, 3000));
    EXPECT_EQ(got_transcripts(), std::vector<std::string>{"你好"});
    EXPECT_EQ(count(kAsrPath), 1u);
    // The whole recording went: 1.3 s scripted, less the settle time, plus
    // the live tail.
    EXPECT_GT(server.requests()[0].body.size(), (1300u - wisp::voice::kMicSettleMs) * 32);
    EXPECT_FALSE(mic->is_open());
    EXPECT_EQ(mic->format().sample_rate, 16000u);
    EXPECT_TRUE(wait_idle());
    // No VAD, but the level meter still runs.
    bool loud = false;
    for (const Map& m : events()) {
        if (str(m, "state") == "listening" && m.count("level")) {
            loud = loud || m.at("level").as_int() > 50;
        }
    }
    EXPECT_TRUE(loud);
    // Busy while capturing is the only refusal.
    EXPECT_TRUE(voice->begin_capture(CaptureMode::Hold));
    EXPECT_EQ(voice->begin_capture(CaptureMode::Hold).error().code, wisp::ErrorCode::Busy);
    voice->cancel_capture();
}

TEST_F(VoiceServiceTest, VadEndsOnSilenceOrSecondTap) {
    add_asr();
    make();
    // On its own: 800 ms of silence after the speech.
    capture_turn("一");
    EXPECT_EQ(count(kAsrPath), 1u);
    EXPECT_TRUE(wait_idle());

    // A second tap ends it before the silence would.
    server.push_asr(asr_text("二"));
    mic->set_script({silence(300), sine(1000)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    voice->end_capture();
    ASSERT_TRUE(wait_until([&] { return got_transcripts().size() == 2; }, 3000));
    EXPECT_EQ(got_transcripts()[1], "二");
    // Well short of the 800 ms of live silence the VAD would have waited.
    EXPECT_LT(server.requests()[1].body.size(), (1300u + 600) * 32);
}

TEST_F(VoiceServiceTest, NotHeardCases) {
    add_asr();
    make();
    // Nothing said for 5 s in Vad mode.
    mic->set_script({silence(5000)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_idle());
    EXPECT_TRUE(saw("idle", "result", "not_heard"));
    EXPECT_GE(virtual_ms.load(), 5000u);

    // A touch on Hold: under kMinSpeechMs recorded once the mic's settle
    // time (kMicSettleMs, the leading silence) is dropped.
    clear_events();
    mic->set_script({silence(100), sine(100)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    voice->end_capture();
    ASSERT_TRUE(wait_idle());
    EXPECT_TRUE(saw("idle", "result", "not_heard"));
    EXPECT_EQ(count(kAsrPath), 0u);

    // An empty transcript.
    clear_events();
    server.push_asr(asr_text(""));
    mic->set_script({silence(300), sine(600), silence(900)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_until([&] { return count(kAsrPath) == 1; }, 3000));
    ASSERT_TRUE(wait_idle());
    EXPECT_TRUE(saw("idle", "result", "not_heard"));
    EXPECT_TRUE(got_transcripts().empty());
    EXPECT_TRUE(reasons().empty());
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(VoiceServiceTest, HoldSendsWithoutVadSpeech) {
    add_asr();
    make();
    // Nothing the VAD calls speech, yet held for over kMinSpeechMs: sent.
    server.push_asr(asr_text("嗯"));
    mic->set_script({silence(100), noise(600)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    voice->end_capture();
    ASSERT_TRUE(wait_until([&] { return !got_transcripts().empty(); }, 3000));
    EXPECT_EQ(got_transcripts(), std::vector<std::string>{"嗯"});
    EXPECT_TRUE(wait_idle());

    // An empty transcript is still not heard.
    clear_events();
    server.push_asr(asr_text(""));
    mic->set_script({silence(100), noise(600)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    voice->end_capture();
    ASSERT_TRUE(wait_until([&] { return count(kAsrPath) == 2; }, 3000));
    ASSERT_TRUE(wait_idle());
    EXPECT_TRUE(saw("idle", "result", "not_heard"));
    EXPECT_EQ(got_transcripts().size(), 1u);
}

TEST_F(VoiceServiceTest, MicSettleTimeIsDropped) {
    add_asr();
    make();
    // The first kMicSettleMs after open (a loud pop here) never reach the
    // recording.
    server.push_asr(asr_text("好"));
    mic->set_script({sine(wisp::voice::kMicSettleMs, 30000), silence(500)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    voice->end_capture();
    ASSERT_TRUE(wait_until([&] { return !got_transcripts().empty(); }, 3000));
    const std::string body = server.requests().at(0).body;
    const size_t data = body.find("data", body.find("WAVE"));
    ASSERT_NE(data, std::string::npos);
    uint32_t bytes = 0;
    std::memcpy(&bytes, body.data() + data + 4, 4);
    ASSERT_GE(bytes, 500u * 32);
    ASSERT_LE(data + 8 + bytes, body.size());
    std::vector<int16_t> pcm(bytes / 2);
    std::memcpy(pcm.data(), body.data() + data + 8, bytes);
    EXPECT_TRUE(std::all_of(pcm.begin(), pcm.end(), [](int16_t v) { return v == 0; }));
}

TEST_F(VoiceServiceTest, RecordingCapStopsAndSends) {
    add_asr();
    make(VoiceConfig{.max_record_ms = 1000});
    server.push_asr(asr_text("满了"));
    mic->set_script({silence(300), sine(2000)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return !got_transcripts().empty(); }, 3000));
    const std::string body = server.requests().at(0).body;
    // The WAV's data chunk: exactly 16000 frames.
    const size_t data = body.find("data", body.find("WAVE"));
    ASSERT_NE(data, std::string::npos);
    uint32_t bytes = 0;
    std::memcpy(&bytes, body.data() + data + 4, 4);
    EXPECT_EQ(bytes, 16000u * 2);
}

TEST_F(VoiceServiceTest, CancelDropsTheRecording) {
    add_asr();
    make();
    mic->set_script({silence(300), sine(1000)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    EXPECT_EQ(voice_locks(), 3u);
    voice->cancel_capture();
    ASSERT_TRUE(wait_idle());
    EXPECT_FALSE(mic->is_open());
    EXPECT_EQ(count(kAsrPath), 0u);
    EXPECT_FALSE(saw("transcribing"));
    EXPECT_EQ(voice_locks(), 0u);

    // Cancelled while the request is in flight: aborted, not waited out.
    server.push_asr(FakeReply{.status = 200, .chunks = {R"({"text":"x"})"}, .hold_after = 0});
    mic->set_script({silence(300), sine(600), silence(900)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_until([&] { return server.held() == 1; }, 3000));
    const uint64_t t0 = wisp::os::now_ms();
    voice->cancel_capture();
    ASSERT_TRUE(wait_idle());
    EXPECT_LT(wisp::os::now_ms() - t0, 500u);
    EXPECT_EQ(server.held(), 0);
    EXPECT_TRUE(got_transcripts().empty());
    EXPECT_TRUE(reasons().empty());
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(VoiceServiceTest, StartToneBeforeTheMic) {
    add_asr();
    make();
    mic->set_script({silence(300), sine(1000)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->opens() == 1; }, 2000));
    voice->cancel_capture();
    ASSERT_TRUE(wait_idle());
    const auto log = order->entries();
    ASSERT_GE(log.size(), 3u);
    EXPECT_EQ(log[0], "out.open");
    EXPECT_EQ(log[1], "out.close");
    EXPECT_EQ(log[2], "in.open");
    EXPECT_GE(out->frames(), 2400u);  // the whole 150 ms tone
}

TEST_F(VoiceServiceTest, StatesAndThrottledLevels) {
    add_asr();
    make();
    capture_turn();
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(states(), (std::vector<std::string>{"listening", "transcribing", "idle"}));
    size_t listening = 0;
    bool loud = false;
    for (const Map& m : events()) {
        if (str(m, "state") != "listening") continue;
        ++listening;
        ASSERT_NE(m.find("level"), m.end());
        const int64_t level = m.at("level").as_int();
        EXPECT_GE(level, 0);
        EXPECT_LE(level, 100);
        loud = loud || level > 50;
    }
    EXPECT_TRUE(loud);
    // 1.8 s of audio at most 20 Hz (the first event included).
    EXPECT_LE(listening, 1u + 1800 / 50 + 1);
    EXPECT_GE(listening, 2u);
}

TEST_F(VoiceServiceTest, MissingPiecesAreErrors) {
    // No asr profile.
    make();
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"no_asr_profile"});
    EXPECT_EQ(states(), (std::vector<std::string>{"error", "idle"}));
    EXPECT_EQ(voice_locks(), 0u);

    // Network down.
    add_asr();
    clear_events();
    net_up = false;
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"offline"});

    // No mic.
    net_up = true;
    make(VoiceConfig{}, /*with_mic=*/false);
    clear_events();
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"mic_unavailable"});

    // A mic that will not open.
    make();
    clear_events();
    mic->set_fail_open(true);
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"mic_unavailable"});

    EXPECT_EQ(count(kAsrPath), 0u);
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(VoiceServiceTest, TransportErrorDuringAsrIsReported) {
    add_asr();
    make();
    server.push_asr(
        FakeReply{.status = 0, .chunks = {}, .fail = wisp::net::TransportError::Connect});
    mic->set_script({silence(300), sine(600), silence(900)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_until([&] { return !reasons().empty(); }, 3000));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"asr_failed"});
    EXPECT_EQ(voice_locks(), 0u);

    // The network went away while the request was out: offline.
    clear_events();
    server.push_asr(FakeReply{
        .status = 0, .chunks = {}, .fail = wisp::net::TransportError::Connect, .hold_after = 0});
    mic->set_script({silence(300), sine(600), silence(900)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_until([&] { return server.held() == 1; }, 3000));
    net_up = false;
    server.release();
    ASSERT_TRUE(wait_until([&] { return !reasons().empty(); }, 3000));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"offline"});
    EXPECT_EQ(voice_locks(), 0u);
    EXPECT_TRUE(got_transcripts().empty());
}

TEST_F(VoiceServiceTest, SpeaksSentencesInOrderAndPipelines) {
    add_tts();
    make();
    // 0.5 s of sentence 1 takes about 250 ms to play out.
    out->set_write_ms(10);
    const auto s1 = ramp(12000, 1), s2 = ramp(4800, 2);
    server.push_tts(pcm_reply(s1));
    server.push_tts(pcm_reply(s2, /*hold_after=*/0));
    voice->begin_speech();
    voice->speak("第一句。第");
    voice->speak("二句。");
    voice->end_speech();
    // Sentence 2's request is out while sentence 1 still plays.
    ASSERT_TRUE(wait_until([&] { return server.held() == 1; }, 2000));
    EXPECT_LT(out->frames(), s1.size());
    EXPECT_TRUE(voice->speaking());
    server.release();
    ASSERT_TRUE(wait_until([&] { return saw("speaking") && !player.active(); }, 3000));
    ASSERT_TRUE(wait_idle());

    EXPECT_EQ(tts_inputs(), (std::vector<std::string>{"第一句。", "第二句。"}));
    std::vector<int16_t> want = s1;
    want.insert(want.end(), s2.begin(), s2.end());
    EXPECT_EQ(out->samples(), want);
    EXPECT_EQ(states(), (std::vector<std::string>{"speaking", "idle"}));
    EXPECT_EQ(server.max_concurrent(), 1);
}

TEST_F(VoiceServiceTest, FirstSentenceMayOverlapTheLlm) {
    add_tts();
    make();
    server.push_tts(pcm_reply(ramp(480)));
    server.push_tts(pcm_reply(ramp(480)));
    voice->set_llm_busy(true);
    voice->begin_speech();
    voice->speak("一。二。");
    ASSERT_TRUE(wait_until([&] { return count(kTtsPath) == 1; }, 2000));
    wisp::os::sleep_ms(100);
    EXPECT_EQ(count(kTtsPath), 1u);
    voice->set_llm_busy(false);
    ASSERT_TRUE(wait_until([&] { return count(kTtsPath) == 2; }, 2000));
    voice->end_speech();
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(tts_inputs(), (std::vector<std::string>{"一。", "二。"}));
}

TEST_F(VoiceServiceTest, NoOverlapWhenConfiguredSerial) {
    add_tts();
    make(VoiceConfig{.overlap_first_sentence = false});
    server.push_tts(pcm_reply(ramp(480)));
    server.push_tts(pcm_reply(ramp(480)));
    voice->set_llm_busy(true);
    voice->begin_speech();
    voice->speak("一。二。");
    wisp::os::sleep_ms(100);
    EXPECT_EQ(count(kTtsPath), 0u);
    voice->set_llm_busy(false);
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return count(kTtsPath) == 2; }, 2000));
    ASSERT_TRUE(wait_idle());
}

TEST_F(VoiceServiceTest, CapsSpeechAt200Chars) {
    add_tts();
    make();
    for (int i = 0; i < 8; ++i) server.push_tts(pcm_reply(ramp(48)));
    // 300 code points in sentences of 50 (49 + the terminator).
    std::string say;
    for (int s = 0; s < 6; ++s) {
        for (int i = 0; i < 49; ++i) say += "字";
        say += "。";
    }
    voice->begin_speech();
    for (size_t off = 0; off < say.size(); off += 7) voice->speak(say.substr(off, 7));
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return count(kTtsPath) == 4; }, 3000));
    ASSERT_TRUE(wait_idle());
    wisp::os::sleep_ms(50);
    size_t code_points = 0;
    for (const std::string& in : tts_inputs()) {
        for (unsigned char c : in) code_points += (c & 0xC0) != 0x80;
    }
    EXPECT_EQ(code_points, 200u);
    EXPECT_EQ(count(kTtsPath), 4u);
}

TEST_F(VoiceServiceTest, StopSpeakingCancelsAndClears) {
    add_tts();
    make();
    server.push_tts(pcm_reply(ramp(4800), /*hold_after=*/1));
    voice->begin_speech();
    voice->speak("一。二。三。");
    ASSERT_TRUE(wait_until([&] { return voice->speaking() && server.held() == 1; }, 2000));
    ASSERT_TRUE(wait_until([&] { return out->frames() == 4096 / 2; }, 2000));
    const uint64_t t0 = wisp::os::now_ms();
    voice->stop_speaking();
    ASSERT_TRUE(wait_idle(1000));
    EXPECT_LT(wisp::os::now_ms() - t0, 200u);
    EXPECT_EQ(server.held(), 0);
    EXPECT_FALSE(player.active());
    const size_t played = out->frames();
    wisp::os::sleep_ms(50);
    EXPECT_EQ(out->frames(), played);
    EXPECT_EQ(count(kTtsPath), 1u);
    EXPECT_TRUE(reasons().empty());
    EXPECT_EQ(voice_locks(), 0u);
    // What comes after the stop is not spoken.
    voice->speak("四。");
    voice->end_speech();
    wisp::os::sleep_ms(50);
    EXPECT_EQ(count(kTtsPath), 1u);
}

TEST_F(VoiceServiceTest, TtsFailureOnlyStopsSpeech) {
    add_tts();
    make();
    server.push_tts(FakeTtsReply{.status = 500, .chunks = {}});
    voice->begin_speech();
    voice->speak("一。二。");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return !reasons().empty(); }, 2000));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"tts_failed"});
    wisp::os::sleep_ms(50);
    EXPECT_EQ(count(kTtsPath), 1u);
    EXPECT_EQ(voice_locks(), 0u);

    // The next turn speaks.
    clear_events();
    server.push_tts(pcm_reply(ramp(960)));
    voice->begin_speech();
    voice->speak("三。");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return saw("speaking"); }, 2000));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(count(kTtsPath), 2u);
    EXPECT_EQ(out->frames(), 960u);
    EXPECT_TRUE(reasons().empty());
}

TEST_F(VoiceServiceTest, NoTtsProfileIsAnError) {
    make();
    voice->begin_speech();
    voice->speak("一。二。三。");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return !reasons().empty(); }, 2000));
    ASSERT_TRUE(wait_idle());
    wisp::os::sleep_ms(50);
    EXPECT_EQ(reasons(), std::vector<std::string>{"no_tts_profile"});
    EXPECT_EQ(count(kTtsPath), 0u);
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(VoiceServiceTest, NetworkLossMidSpeechEndsCleanly) {
    add_tts();
    make();
    FakeTtsReply r = pcm_reply(ramp(4800), /*hold_after=*/1);
    r.fail = wisp::net::TransportError::Connect;
    server.push_tts(r);
    voice->begin_speech();
    voice->speak("一。二。");
    ASSERT_TRUE(wait_until([&] { return voice->speaking() && server.held() == 1; }, 2000));
    EXPECT_EQ(voice_locks(), 3u);
    net_up = false;
    server.release();
    ASSERT_TRUE(wait_until([&] { return !reasons().empty(); }, 2000));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(reasons(), std::vector<std::string>{"offline"});
    EXPECT_TRUE(wait_until([&] { return !player.active(); }, 500));
    EXPECT_FALSE(out->is_open());
    EXPECT_EQ(voice_locks(), 0u);
    EXPECT_EQ(count(kTtsPath), 1u);
}

TEST_F(VoiceServiceTest, HoldsThreeLocksForTheRound) {
    add_asr();
    add_tts();
    make();
    server.push_asr(asr_text("你好"));
    mic->set_script({silence(300), sine(600), silence(900)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    std::vector<WakeLockKind> kinds;
    for (const auto& l : locks.list()) {
        EXPECT_EQ(l.holder, "system.voice");
        EXPECT_EQ(l.reason, "voice_turn");
        kinds.push_back(l.kind);
    }
    EXPECT_EQ(kinds, (std::vector<WakeLockKind>{WakeLockKind::Screen, WakeLockKind::Network,
                                                WakeLockKind::Cpu}));
    ASSERT_TRUE(wait_until([&] { return !got_transcripts().empty(); }, 3000));
    // The round goes on into the reply.
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(voice_locks(), 3u);

    out->set_write_ms(5);
    server.push_tts(pcm_reply(ramp(4800)));
    voice->begin_speech();
    voice->speak("好的。");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return voice->speaking(); }, 2000));
    EXPECT_EQ(voice_locks(), 3u);
    ASSERT_TRUE(wait_idle());
    EXPECT_TRUE(wait_until([&] { return voice_locks() == 0; }, 500));
    EXPECT_EQ(out->frames(), 4800u + 2400u);  // the speech and the start tone

    // A turn the reply never spoke ends at end_speech() too.
    capture_turn("再来");
    EXPECT_EQ(voice_locks(), 3u);
    voice->end_speech();
    EXPECT_TRUE(wait_until([&] { return voice_locks() == 0; }, 1000));
}

TEST_F(VoiceServiceTest, PlaysAtTheProfileRate) {
    add_tts();
    make();
    server.push_tts(pcm_reply(ramp(480)));
    voice->begin_speech();
    voice->speak("一。");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return saw("speaking"); }, 2000));
    ASSERT_TRUE(wait_idle());
    ASSERT_EQ(out->formats().size(), 1u);
    EXPECT_EQ(out->formats()[0].sample_rate, 24000u);

    const std::string id = add_tts(ModelOptions{{"sample_rate", "16000"}});
    ASSERT_TRUE(models->activate(id));
    clear_events();
    server.push_tts(pcm_reply(ramp(480)));
    voice->begin_speech();
    voice->speak("二。");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return saw("speaking"); }, 2000));
    ASSERT_TRUE(wait_idle());
    ASSERT_EQ(out->formats().size(), 2u);
    EXPECT_EQ(out->formats()[1].sample_rate, 16000u);
}

TEST_F(VoiceServiceTest, AlarmPreemptsSpeech) {
    add_tts();
    make();
    tones.set_output_guard([this](ToneKind k) { return voice->may_play_tone(k); });
    server.push_tts(pcm_reply(ramp(4800), /*hold_after=*/1));
    voice->begin_speech();
    voice->speak("一。二。");
    ASSERT_TRUE(wait_until([&] { return voice->speaking() && server.held() == 1; }, 2000));
    ASSERT_EQ(out->opens(), 1);

    // A notification beep while speech has the output: skipped.
    tones.play(ToneKind::Tone, 60);
    ASSERT_TRUE(wait_until([&] { return !tones.playing(); }, 1000));
    EXPECT_EQ(out->opens(), 1);
    EXPECT_TRUE(voice->speaking());

    // An alarm: the speech stops, then the alarm plays.
    out->set_write_ms(5);  // the alarm plays on for a while
    tones.play(ToneKind::Alarm, 60);
    ASSERT_TRUE(wait_until([&] { return out->opens() == 2; }, 1000));
    EXPECT_FALSE(player.active());
    EXPECT_TRUE(wait_until([&] { return server.held() == 0; }, 200));
    ASSERT_TRUE(wait_idle());
    EXPECT_TRUE(tones.playing());
    tones.silence();
    ASSERT_TRUE(wait_until([&] { return !tones.playing(); }, 1000));
    EXPECT_EQ(out->overlapping_opens(), 0);
    EXPECT_EQ(count(kTtsPath), 1u);
}

TEST_F(VoiceServiceTest, TaskAndLogs) {
    const wisp::os::TaskConfig c = VoiceService::task_config(VoiceConfig{});
    EXPECT_STREQ(c.name, "wisp.voice");
    EXPECT_FALSE(c.flash_access);
    EXPECT_EQ(c.stack_bytes, 8u * 1024);
    EXPECT_EQ(c.priority, 3);

    add_asr();
    add_tts();
    make();
    capture_turn("a secret transcript");
    ASSERT_TRUE(wait_idle());
    server.push_tts(pcm_reply(ramp(480)));
    voice->begin_speech();
    voice->speak("a secret reply.\n");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return saw("speaking"); }, 2000));
    ASSERT_TRUE(wait_until([&] { return voice_locks() == 0; }, 2000));
    ASSERT_TRUE(wait_idle());
    std::vector<std::string> lines;
    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_EQ(e.msg.find("secret"), std::string::npos) << e.msg;
        if (e.tag == "voice" && e.msg.rfind("state ", 0) == 0) lines.push_back(e.msg);
    }
    EXPECT_EQ(lines, (std::vector<std::string>{"state listening", "state transcribing",
                                               "state idle", "state speaking", "state idle"}));
    for (const Map& m : events()) {
        for (const auto& [k, v] : m) EXPECT_TRUE(k == "state" || k == "level") << k;
    }
}

TEST_F(VoiceServiceTest, AlarmWinsTheOutputWhileSpeechOpensIt) {
    add_tts();
    make();
    tones.set_output_guard([this](ToneKind k) { return voice->may_play_tone(k); });
    // Speech's open() is held half-way, as if the codec were slow.
    std::atomic<bool> in_open{false}, let_go{false};
    out->set_open_hook([&](int index) {
        if (index != 0) return;
        in_open = true;
        while (!let_go.load()) wisp::os::sleep_ms(1);
    });
    server.push_tts(pcm_reply(ramp(4800), /*hold_after=*/1));
    voice->begin_speech();
    voice->speak("一。二。");
    ASSERT_TRUE(wait_until([&] { return in_open.load(); }, 2000));

    // The alarm comes while speech is opening the output: its guard waits
    // for that decision instead of racing it.
    tones.play(ToneKind::Alarm, 60);
    out->set_write_ms(5);
    wisp::os::sleep_ms(50);
    EXPECT_EQ(out->open_calls(), 1);
    let_go = true;

    // Speech opened first, so it is stopped, and the alarm opens after.
    ASSERT_TRUE(wait_until([&] { return out->opens() == 2; }, 2000));
    EXPECT_EQ(out->overlapping_opens(), 0);
    EXPECT_TRUE(tones.playing());
    ASSERT_TRUE(wait_idle());
    EXPECT_FALSE(player.active());
    tones.silence();
    ASSERT_TRUE(wait_until([&] { return !tones.playing(); }, 1000));
    EXPECT_EQ(out->overlapping_opens(), 0);
    EXPECT_EQ(out->formats().size(), 2u);
}

TEST_F(VoiceServiceTest, ToneAskedFirstKeepsSpeechOff) {
    add_tts();
    make();
    tones.set_output_guard([this](ToneKind k) { return voice->may_play_tone(k); });
    // An alarm owns the output; speech that arrives meanwhile gives way.
    out->set_write_ms(5);
    tones.play(ToneKind::Alarm, 60);
    ASSERT_TRUE(wait_until([&] { return out->opens() == 1; }, 1000));
    server.push_tts(pcm_reply(ramp(480)));
    voice->begin_speech();
    voice->speak("一。");
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return voice_locks() == 0; }, 2000));
    EXPECT_EQ(out->opens(), 1);
    EXPECT_EQ(out->overlapping_opens(), 0);
    EXPECT_FALSE(saw("speaking"));
    EXPECT_TRUE(reasons().empty());
    tones.silence();
}

TEST_F(VoiceServiceTest, QueueFullRollsBackBeginCapture) {
    add_asr();
    make(VoiceConfig{.queue_depth = 1});
    // The transcript's consumer stalls wisp.voice.
    std::atomic<bool> stall{true};
    voice->set_on_transcript([&](std::string) {
        while (stall.load()) wisp::os::sleep_ms(1);
    });
    server.push_asr(asr_text("你好"));
    mic->set_script({silence(300), sine(600), silence(900)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
    ASSERT_TRUE(wait_until([&] { return count(kAsrPath) == 1 && voice_locks() == 3; }, 3000));
    ASSERT_TRUE(wait_idle());
    // One wake-up fills the queue.
    voice->set_llm_busy(true);
    const int tones_before = out->opens();

    auto r = voice->begin_capture(CaptureMode::Vad);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::Busy);
    // Nothing changed: no tone, no mic, the round's locks as they were.
    stall = false;
    wisp::os::sleep_ms(100);
    EXPECT_EQ(out->opens(), tones_before);
    EXPECT_EQ(mic->opens(), 1);
    EXPECT_EQ(voice->state(), VoiceState::Idle);
    EXPECT_EQ(voice_locks(), 3u);
    voice->set_llm_busy(false);
    voice->end_speech();
    EXPECT_TRUE(wait_until([&] { return voice_locks() == 0; }, 1000));

    // Not stuck at Busy.
    mic->set_script({silence(300), sine(1000)});
    ASSERT_TRUE(voice->begin_capture(CaptureMode::Hold));
    ASSERT_TRUE(wait_until([&] { return mic->opens() == 2; }, 2000));
    voice->cancel_capture();
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(VoiceServiceTest, RoundBackstopEndsAForgottenRound) {
    add_asr();
    make();
    capture_turn();
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(voice_locks(), 3u);

    // Nearly the backstop, then speech activity: the clock starts again.
    virtual_ms += wisp::voice::kVoiceRoundBackstopMs - 10000;
    wisp::os::sleep_ms(300);
    EXPECT_EQ(voice_locks(), 3u);
    voice->begin_speech();
    virtual_ms += wisp::voice::kVoiceRoundBackstopMs - 10000;
    wisp::os::sleep_ms(300);
    EXPECT_EQ(voice_locks(), 3u);

    // Past it with nothing more: the round ends itself.
    clear_events();
    virtual_ms += 20000;
    ASSERT_TRUE(wait_until([&] { return voice_locks() == 0; }, 1000));
    ASSERT_TRUE(wait_idle());
    bool logged = false;
    for (const auto& e : wisp::log::ring_entries()) {
        logged = logged || (e.tag == "voice" && e.msg == "round backstop");
    }
    EXPECT_TRUE(logged);
    // A speech turn that ended normally leaves nothing for it.
    voice->set_llm_busy(false);
    voice->end_speech();
    EXPECT_EQ(voice->state(), VoiceState::Idle);
}

TEST_F(VoiceServiceTest, BusyTurnKeepsItsRound) {
    add_asr();
    make();
    capture_turn();
    ASSERT_TRUE(wait_idle());
    // The agent works (tools, thinking) for longer than the backstop
    // without speaking; its state changes keep the round alive.
    for (int i = 0; i < 4; ++i) {
        virtual_ms += wisp::voice::kVoiceRoundBackstopMs - 10000;
        voice->set_llm_busy(i % 2 == 0);
        wisp::os::sleep_ms(200);
        EXPECT_EQ(voice_locks(), 3u);
    }
    voice->set_llm_busy(false);
    voice->end_speech();
    ASSERT_TRUE(wait_until([&] { return voice_locks() == 0; }, 2000));
    bool logged = false;
    for (const auto& e : wisp::log::ring_entries()) {
        logged = logged || (e.tag == "voice" && e.msg == "round backstop");
    }
    EXPECT_FALSE(logged);
}

TEST_F(VoiceServiceTest, LocksAreGoneWhenIdleArrives) {
    add_asr();
    make();
    // Every idle that ends a round comes after its locks went.
    std::atomic<int> idle_with_locks{0};
    auto check = bus.subscribe("voice.state", [&](const wisp::core::Event& e) {
        auto it = e.payload.find("state");
        if (it != e.payload.end() && it->second.as_string() == "idle" && voice_locks() != 0 &&
            voice->state() == VoiceState::Idle && got_transcripts().empty()) {
            ++idle_with_locks;
        }
    });
    for (int i = 0; i < 5; ++i) {
        clear_events();
        mic->set_script({silence(5000)});
        ASSERT_TRUE(voice->begin_capture(CaptureMode::Vad));
        ASSERT_TRUE(wait_idle());
        EXPECT_EQ(voice_locks(), 0u);
    }
    bus.wait_idle(1000);
    bus.unsubscribe(check);
    EXPECT_EQ(idle_with_locks.load(), 0);
}
