#pragma once

// ShellFixture: a ConversationController over a real AgentService (the
// AgentHarness, its model client moved to a FakeModelServer) and a real
// VoiceService with scripted audio, the three model kinds all served by
// that one FakeModelServer. The storage worker runs, so anything that
// touches flash off wisp.fs aborts the test.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/agent_fixture.hpp"
#include "support/fake_audio.hpp"
#include "support/fake_llm.hpp"
#include "support/fake_models.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/tone_player.hpp"
#include "wisp/net/llm_client.hpp"
#include "wisp/os/os.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/voice/asr_client.hpp"
#include "wisp/voice/pcm_player.hpp"
#include "wisp/voice/tts_client.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::test {

inline constexpr const char* kAsrPath = "/audio/transcriptions";
inline constexpr const char* kTtsPath = "/audio/speech";
inline constexpr const char* kLlmPath = "/chat/completions";

inline FakeReply asr_text(const std::string& text) {
    return FakeReply{.status = 200, .chunks = {R"({"text":")" + text + R"("})"}};
}

inline FakeReply llm_text(const std::string& text, int hold_after = -1) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_text(text), FakeLlmServer::sse_finish("stop"),
                       FakeLlmServer::sse_done()},
            .hold_after = hold_after};
}

// `text` and then a call of the tool named `wire`.
inline FakeReply llm_text_then_tool(const std::string& text, const std::string& id,
                                    const std::string& wire) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_text(text), FakeLlmServer::sse_tool(0, id, wire, "{}"),
                       FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}};
}

// `ms` of 24 kHz PCM.
inline FakeTtsReply tts_pcm(uint32_t ms = 200) {
    std::vector<int16_t> pcm(24 * ms);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<int16_t>((i * 37) % 4000 - 2000);
    FakeTtsReply r{.status = 200, .chunks = {}};
    const auto* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    const size_t total = pcm.size() * sizeof(int16_t);
    for (size_t off = 0; off < total; off += 4096) {
        const size_t n = std::min<size_t>(4096, total - off);
        r.chunks.emplace_back(bytes + off, bytes + off + n);
    }
    return r;
}

class ShellFixture : public ::testing::Test {
protected:
    using Kind = agent::AgentEvent::Kind;

    void SetUp() override {
        log::reset_for_test();
        log::init();
    }

    void TearDown() override {
        out->open_gate();
        server.release();
        if (h && voice_sub != 0) h->bus.unsubscribe(voice_sub);
        if (conv) conv->stop();
        conv.reset();
        tones.set_output_guard(nullptr);
        voice.reset();
        if (agent) agent->stop();
        agent.reset();
        tones.stop();
        player.stop_task();
        h.reset();
        log::reset_for_test();
    }

    // Builds and starts everything; with_llm false leaves out the llm
    // profile (asr and tts are always there).
    void start(bool with_llm = true, std::function<void(agent::ToolRegistry&)> add_tools = {}) {
        h = std::make_unique<AgentHarness>(with_llm);
        h->llm = std::make_unique<net::LlmClient>(server, *h->models, [](uint32_t) {});
        ASSERT_TRUE(h->worker.run_sync(
            [&] {
                ASSERT_TRUE(
                    h->models->create({.kind = core::ModelKind::Asr,
                                       .name = "asr",
                                       .preset = "custom",
                                       .base_url = std::string("https://asr.example.com/v1"),
                                       .model = std::string("whisper-1"),
                                       .api_key = std::string("sk-asr")}));
                ASSERT_TRUE(
                    h->models->create({.kind = core::ModelKind::Tts,
                                       .name = "tts",
                                       .preset = "custom",
                                       .base_url = std::string("https://tts.example.com/v1"),
                                       .model = std::string("tts-1"),
                                       .api_key = std::string("sk-tts")}));
            },
            os::kForever));
        if (add_tools) add_tools(*h->tools);
        voice_sub = h->bus.subscribe("voice.state", [this](const core::Event& e) {
            auto it = e.payload.find("state");
            if (it == e.payload.end()) return;
            std::lock_guard<std::mutex> lock(mu);
            if (states_.empty() || states_.back() != it->second.as_string())
                states_.push_back(it->second.as_string());
        });
        asr = std::make_unique<voice::AsrClient>(server, *h->models, [](uint32_t) {});
        tts = std::make_unique<voice::TtsClient>(server, *h->models);
        ASSERT_TRUE(player.start());
        ASSERT_TRUE(tones.start());

        agent = std::make_unique<agent::AgentService>(h->deps());
        agent_listener = agent->add_listener([this](const agent::AgentEvent& e) {
            std::lock_guard<std::mutex> lock(mu);
            agent_events.push_back(e);
        });
        ASSERT_TRUE(agent->start());

        voice = std::make_unique<voice::VoiceService>(voice::VoiceDeps{
            .bus = h->bus,
            .mic = mic,
            .player = player,
            .asr = *asr,
            .tts = *tts,
            .models = *h->models,
            .tones = &tones,
            .locks = h->locks.get(),
            .network_up = [] { return true; },
            .volume = [] { return uint8_t{100}; },
            .now_ms = [this] { return os::now_ms() + virtual_ms.load(); },
        });
        ASSERT_TRUE(voice->start());
        tones.set_output_guard([v = voice.get()](core::ToneKind k) { return v->may_play_tone(k); });

        conv = std::make_unique<shell::ConversationController>(shell::ConversationDeps{
            .bus = h->bus,
            .agent = *agent,
            .voice = *voice,
            .tts_mute = [this] { return mute.load(); },
            .readiness = [this] { return ready.load(); },
        });
        conv->start();
    }

    // --- observations ---

    std::vector<agent::AgentEvent> events() {
        std::lock_guard<std::mutex> lock(mu);
        return agent_events;
    }
    size_t count(Kind k) {
        size_t n = 0;
        for (const auto& e : events()) n += e.kind == k;
        return n;
    }
    std::vector<std::string> statuses() {
        std::vector<std::string> out;
        for (const auto& e : events())
            if (e.kind == Kind::TurnEnd) out.push_back(e.status);
        return out;
    }
    bool wait_turns(size_t n, uint32_t ms = 8000) {
        return wait_until([&] { return count(Kind::TurnEnd) >= n; }, ms);
    }
    size_t requests(const char* path) const {
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
    size_t locks_of(const char* holder) const {
        size_t n = 0;
        for (const auto& l : h->locks->list()) n += l.holder == holder;
        return n;
    }
    size_t voice_locks() const { return locks_of(voice::VoiceService::kHolder); }
    // Everything back at rest: no capture, no speech, no turn, no lock.
    bool settled(uint32_t ms = 8000) {
        return wait_until(
            [&] {
                return voice->state() == voice::VoiceState::Idle && !voice->capturing() &&
                       !player.active() && agent->state() == agent::AgentState::Idle &&
                       !agent->view().running_turn && !agent->view().queued &&
                       !conv->current().running && voice_locks() == 0 &&
                       locks_of("system.agent") == 0;
            },
            ms);
    }
    // voice.state in order, repeats folded.
    std::vector<std::string> voice_states() {
        h->bus.wait_idle(1000);
        std::lock_guard<std::mutex> lock(mu);
        return states_;
    }
    bool saw_state(const std::string& s) {
        auto all = voice_states();
        return std::find(all.begin(), all.end(), s) != all.end();
    }
    std::string text(const os::LargeString& s) const { return std::string(s.data(), s.size()); }

    // A spoken utterance on the mic: noise floor, speech, then silence
    // long enough for the VAD to end it.
    void say_into_mic() {
        mic->set_script({audio::silence(300), audio::sine(600), audio::silence(900)});
    }

    FakeModelServer server;
    std::unique_ptr<AgentHarness> h;
    std::unique_ptr<voice::AsrClient> asr;
    std::unique_ptr<voice::TtsClient> tts;
    std::shared_ptr<audio::OrderLog> order = std::make_shared<audio::OrderLog>();
    std::atomic<uint64_t> virtual_ms{0};
    std::shared_ptr<audio::FakeAudioIn> mic =
        std::make_shared<audio::FakeAudioIn>(std::vector<audio::Segment>{}, order, &virtual_ms);
    std::shared_ptr<audio::FakeAudioOut> out = std::make_shared<audio::FakeAudioOut>(order);
    voice::PcmPlayer player{out};
    core::TonePlayer tones{out, nullptr};
    std::unique_ptr<agent::AgentService> agent;
    uint32_t agent_listener = 0;
    std::unique_ptr<voice::VoiceService> voice;
    std::unique_ptr<shell::ConversationController> conv;
    std::atomic<bool> mute{false};
    std::atomic<shell::Readiness> ready{shell::Readiness::Ready};

    std::mutex mu;
    std::vector<agent::AgentEvent> agent_events;
    std::vector<std::string> states_;
    core::SubscriptionId voice_sub = 0;
};

}  // namespace wisp::test
