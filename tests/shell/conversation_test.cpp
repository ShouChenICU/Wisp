// wisp::shell::ConversationController (wisp/shell/conversation.hpp): voice
// and agent together, over the real services with scripted models and
// audio (shell_fixture.hpp).

#include "wisp/shell/conversation.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "shell/shell_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_registry.hpp"

using wisp::ErrorCode;
using wisp::agent::Role;
using wisp::agent::Source;
using wisp::shell::Readiness;
using wisp::test::asr_text;
using wisp::test::FakeLlmServer;
using wisp::test::FakeReply;
using wisp::test::kAsrPath;
using wisp::test::kLlmPath;
using wisp::test::kTtsPath;
using wisp::test::llm_text;
using wisp::test::llm_text_then_tool;
using wisp::test::tts_pcm;
using wisp::test::wait_until;
using wisp::test::audio::silence;
using wisp::test::audio::sine;
using wisp::voice::VoiceState;

namespace {

class ConversationTest : public wisp::test::ShellFixture {
protected:
    // A recording that the VAD ends, transcribed as `heard`.
    void tap_and_say(const std::string& heard = "你好") {
        server.push_asr(asr_text(heard));
        say_into_mic();
        conv->tap();
    }

    std::optional<Source> stored_user_source() {
        auto history = h->store->history();
        for (auto it = history->rbegin(); it != history->rend(); ++it)
            if (it->role == Role::User) return it->source;
        return std::nullopt;
    }

    std::vector<wisp::agent::AgentEvent> begins() {
        std::vector<wisp::agent::AgentEvent> out;
        for (const auto& e : events())
            if (e.kind == Kind::TurnBegin) out.push_back(e);
        return out;
    }
};

}  // namespace

TEST_F(ConversationTest, VoiceTurnSubmitsWithVoiceSource) {
    start();
    server.llm().push(llm_text("<say>好的。</say>"));
    server.push_tts(tts_pcm());
    tap_and_say("你好");
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());

    auto b = begins();
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].source, Source::Voice);
    EXPECT_EQ(b[0].text, "你好");
    EXPECT_EQ(stored_user_source(), Source::Voice);
    EXPECT_EQ(requests(kAsrPath), 1u);
    EXPECT_EQ(requests(kLlmPath), 1u);

    const wisp::shell::TurnView v = conv->current();
    EXPECT_EQ(v.turn, b[0].turn);
    EXPECT_EQ(v.source, Source::Voice);
    EXPECT_EQ(v.heard, "你好");
    EXPECT_FALSE(v.running);
}

TEST_F(ConversationTest, SayIsSpokenBodyIsNot) {
    start();
    server.llm().push(llm_text("<say>好的。</say>已建好待办"));
    server.push_tts(tts_pcm());
    tap_and_say();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());

    EXPECT_EQ(tts_inputs(), std::vector<std::string>{"好的。"});
    const wisp::shell::TurnView v = conv->current();
    EXPECT_EQ(text(v.say), "好的。");
    EXPECT_EQ(text(v.body), "已建好待办");
    EXPECT_TRUE(v.spoken);
    EXPECT_TRUE(saw_state("speaking"));
    EXPECT_GT(out->frames(), 0u);
}

TEST_F(ConversationTest, WebTurnIsShownNotSpoken) {
    start();
    std::vector<std::string> says;
    std::mutex says_mu;
    conv->add_listener([&](const wisp::shell::TurnView& v) {
        std::lock_guard<std::mutex> lock(says_mu);
        says.push_back(text(v.say));
    });
    server.llm().push(llm_text("<say>嗯。</say>正文"));
    ASSERT_TRUE(agent->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());

    EXPECT_EQ(requests(kTtsPath), 0u);
    const wisp::shell::TurnView v = conv->current();
    EXPECT_EQ(v.source, Source::Web);
    EXPECT_EQ(v.heard, "hi");
    EXPECT_EQ(text(v.say), "嗯。");
    EXPECT_EQ(text(v.body), "正文");
    EXPECT_FALSE(v.spoken);
    EXPECT_FALSE(v.running);
    std::lock_guard<std::mutex> lock(says_mu);
    ASSERT_FALSE(says.empty());
    EXPECT_EQ(says.back(), "嗯。");
}

TEST_F(ConversationTest, TtsMuteShowsWithoutSpeaking) {
    start();
    mute = true;
    server.llm().push(llm_text("<say>好的。</say>正文"));
    tap_and_say();
    ASSERT_TRUE(wait_turns(1));
    // The voice round ends with the turn: its three locks go.
    ASSERT_TRUE(settled());
    EXPECT_EQ(voice_locks(), 0u);
    EXPECT_EQ(requests(kTtsPath), 0u);
    const wisp::shell::TurnView v = conv->current();
    EXPECT_EQ(text(v.say), "好的。");
    EXPECT_FALSE(v.spoken);
    EXPECT_FALSE(saw_state("speaking"));
}

TEST_F(ConversationTest, RefusedSubmitEndsTheRound) {
    start();
    // Over kMaxUserChars: submit() says too_long.
    std::string longest;
    for (size_t i = 0; i <= wisp::agent::kMaxUserChars; ++i) longest += "啊";
    tap_and_say(longest);
    ASSERT_TRUE(wait_until([&] { return requests(kAsrPath) == 1 && !voice->capturing(); }, 5000));
    ASSERT_TRUE(settled());
    EXPECT_EQ(voice_locks(), 0u);
    EXPECT_EQ(requests(kLlmPath), 0u);
    EXPECT_EQ(count(Kind::TurnBegin), 0u);
}

TEST_F(ConversationTest, NoSayFallsBackToFirstSentence) {
    start();
    server.llm().push(llm_text("今天晴。明天雨。"));
    server.push_tts(tts_pcm());
    tap_and_say();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(tts_inputs(), std::vector<std::string>{"今天晴。"});
    const wisp::shell::TurnView v = conv->current();
    EXPECT_EQ(text(v.say), "");
    EXPECT_EQ(text(v.body), "今天晴。明天雨。");
    EXPECT_TRUE(v.spoken);
}

TEST_F(ConversationTest, TapCycle) {
    start();
    out->set_write_ms(20);  // plays in real time

    // Idle: a tap starts a VAD capture.
    mic->set_script({silence(300), sine(1000)});
    server.push_asr(asr_text("讲个故事"));
    // The model thinks until released.
    server.llm().push({.status = 200,
                       .chunks = {FakeLlmServer::sse_text("<say>从前有座山，山里有座庙。</say>"),
                                  FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()},
                       .hold_after = 0});
    server.push_tts(tts_pcm(3000));
    conv->tap();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));

    // Listening: a tap ends the recording and sends it.
    conv->tap();
    ASSERT_TRUE(wait_until([&] { return server.llm().held() == 1; }, 5000));
    EXPECT_TRUE(saw_state("transcribing"));
    EXPECT_EQ(requests(kAsrPath), 1u);

    // Thinking: a tap does nothing.
    const int opens = mic->opens();
    conv->tap();
    wisp::os::sleep_ms(150);
    EXPECT_EQ(mic->opens(), opens);
    EXPECT_FALSE(voice->capturing());

    // Speaking: a tap stops the speech.
    server.release();
    ASSERT_TRUE(wait_until([&] { return voice->speaking(); }, 5000));
    conv->tap();
    ASSERT_TRUE(settled());
    EXPECT_LT(out->frames(), 24u * 3000);
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(ConversationTest, HoldInterruptsSpeechAndRunningTurns) {
    start();
    // A web turn waiting for the model: stopped, then the hold listens.
    server.llm().push(llm_text("never", /*hold_after=*/0));
    ASSERT_TRUE(agent->submit("hi", Source::Web));
    ASSERT_TRUE(wait_until([&] { return server.llm().held() == 1; }, 3000));
    mic->set_script({silence(300), sine(1000)});
    conv->hold_begin();
    EXPECT_TRUE(conv->holding());
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(statuses(), std::vector<std::string>{"stopped"});
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    conv->hold_cancel();
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(settled());

    // Speaking: the speech stops and the hold listens.
    out->set_write_ms(20);
    server.llm().push(llm_text("<say>从前有座山，山里有座庙。</say>"));
    server.push_tts(tts_pcm(3000));
    ASSERT_TRUE(conv->voice_text("讲个故事"));
    ASSERT_TRUE(wait_until([&] { return voice->speaking(); }, 5000));
    mic->set_script({silence(300), sine(1000)});
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 3000));
    EXPECT_TRUE(wait_until([&] { return !player.active(); }, 1000));
    EXPECT_LT(out->frames(), 24u * 3000);
    conv->hold_cancel();
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(kAsrPath), 0u);
}

TEST_F(ConversationTest, HoldOverATranscriptionRecordsAnew) {
    start();
    server.push_asr(FakeReply{.status = 200, .chunks = {R"({"text":"x"})"}, .hold_after = 0});
    say_into_mic();
    conv->tap();
    ASSERT_TRUE(wait_until([&] { return server.held() == 1; }, 5000));
    // The transcription is dropped; the hold records once it has ended.
    mic->set_script({silence(300), sine(1000)});
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return mic->opens() == 2; }, 3000));
    EXPECT_EQ(server.held(), 0);
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    server.push_asr(asr_text("你好"));
    server.llm().push(llm_text("<say>好。</say>"));
    server.push_tts(tts_pcm());
    conv->hold_end();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    auto b = begins();
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].text, "你好");
}

TEST_F(ConversationTest, HoldCancelDropsTheRecording) {
    start();
    mic->set_script({silence(300), sine(1000)});
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    EXPECT_EQ(voice_locks(), 3u);
    conv->hold_cancel();
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(kAsrPath), 0u);
    EXPECT_EQ(count(Kind::TurnBegin), 0u);
    EXPECT_FALSE(saw_state("transcribing"));

    // A hold that is released sends what it heard.
    server.push_asr(asr_text("你好"));
    server.llm().push(llm_text("<say>好。</say>"));
    server.push_tts(tts_pcm());
    mic->set_script({silence(300), sine(1000)});
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    conv->hold_end();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(kAsrPath), 1u);
    EXPECT_EQ(tts_inputs(), std::vector<std::string>{"好。"});
}

TEST_F(ConversationTest, NotReadyMakesNoRequest) {
    start(/*with_llm=*/false);
    ready = Readiness::NoLlm;
    EXPECT_EQ(conv->readiness(), Readiness::NoLlm);
    say_into_mic();
    conv->tap();
    conv->hold_begin();
    EXPECT_FALSE(conv->holding());
    conv->hold_end();
    auto r = conv->voice_text("你好");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(r.error().message, "no_llm");
    ready = Readiness::NoWifi;
    EXPECT_EQ(conv->voice_text("你好").error().message, "no_wifi");
    wisp::os::sleep_ms(200);
    EXPECT_TRUE(server.requests().empty());
    EXPECT_EQ(mic->opens(), 0);
    EXPECT_EQ(count(Kind::TurnBegin), 0u);
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(ConversationTest, VoiceTextSkipsAsr) {
    start();
    server.llm().push(llm_text("<say>好。</say>"));
    server.push_tts(tts_pcm());
    ASSERT_TRUE(conv->voice_text("你好"));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(kAsrPath), 0u);
    EXPECT_EQ(mic->opens(), 0);
    EXPECT_EQ(stored_user_source(), Source::Voice);
    EXPECT_EQ(tts_inputs(), std::vector<std::string>{"好。"});
    EXPECT_TRUE(conv->current().spoken);

    // Muted, it shows and its round still ends.
    mute = true;
    server.llm().push(llm_text("<say>好。</say>"));
    ASSERT_TRUE(conv->voice_text("再来"));
    ASSERT_TRUE(wait_turns(2));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(kTtsPath), 1u);
    EXPECT_EQ(voice_locks(), 0u);
}

TEST_F(ConversationTest, RapidTriggersNeverOverlapTurns) {
    start();
    out->set_write_ms(20);
    // The first transcription waits until the burst cancels it.
    server.push_asr(FakeReply{.status = 200, .chunks = {R"({"text":"x"})"}, .hold_after = 0});
    for (int i = 0; i < 8; ++i) {
        server.push_asr(asr_text("你好"));
        server.llm().push(llm_text("<say>好的，这是很长的一句话。</say>正文"));
        server.push_tts(tts_pcm(1500));
    }
    auto burst = [&] {
        conv->tap();
        conv->tap();
        conv->hold_begin();
        wisp::os::sleep_ms(20);
        conv->hold_end();
        conv->tap();
    };

    // During transcription.
    say_into_mic();
    conv->tap();
    ASSERT_TRUE(wait_until([&] { return server.held() == 1; }, 5000));
    EXPECT_EQ(voice->state(), VoiceState::Transcribing);
    burst();
    ASSERT_TRUE(settled(20000));

    // During speech.
    ASSERT_TRUE(conv->voice_text("讲"));
    ASSERT_TRUE(wait_until([&] { return voice->speaking(); }, 5000));
    burst();
    ASSERT_TRUE(settled(20000));

    // Turns never overlapped, nor did captures or outputs.
    bool in_turn = false;
    for (const auto& e : events()) {
        if (e.kind == Kind::TurnBegin) {
            EXPECT_FALSE(in_turn);
            in_turn = true;
        } else if (e.kind == Kind::TurnEnd) {
            EXPECT_TRUE(in_turn);
            in_turn = false;
        }
    }
    EXPECT_FALSE(in_turn);
    bool mic_open = false;
    for (const std::string& o : order->entries()) {
        if (o == "in.open") {
            EXPECT_FALSE(mic_open);
            mic_open = true;
        } else if (o == "in.close") {
            mic_open = false;
        }
    }
    EXPECT_EQ(out->overlapping_opens(), 0);
    EXPECT_FALSE(conv->holding());
    EXPECT_EQ(voice->state(), VoiceState::Idle);
    EXPECT_TRUE(h->locks->list().empty());
}

TEST_F(ConversationTest, AtMostTwoModelConnectionsInAVoiceTurn) {
    std::atomic<int> tool_runs{0};
    start(true, [&](wisp::agent::ToolRegistry& tools) {
        wisp::agent::ToolDef def;
        def.name = "test.wait";
        def.description = "test tool";
        def.run = [&](const wisp::Value&, wisp::agent::ToolCtx&) {
            ++tool_runs;
            wisp::os::sleep_ms(100);
            return wisp::agent::ToolOutput{.ok = true, .result = wisp::Map{{"ok", true}}};
        };
        ASSERT_TRUE(tools.add(std::move(def)));
    });
    out->set_write_ms(20);
    server.llm().push(llm_text_then_tool("<say>第一句。</say>", "c1", "test_wait"));
    server.llm().push(llm_text_then_tool("<say>第二句。</say>", "c2", "test_wait"));
    server.llm().push(llm_text("<say>第三句。</say>完成"));
    for (int i = 0; i < 3; ++i) server.push_tts(tts_pcm(400));
    tap_and_say();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled(15000));
    EXPECT_EQ(tool_runs.load(), 2);
    EXPECT_EQ(tts_inputs(), (std::vector<std::string>{"第一句。", "第二句。", "第三句。"}));
    EXPECT_LE(server.max_concurrent(), 2);
}
