// wisp::shell::ConversationController (wisp/shell/conversation.hpp): voice
// and agent together, over the real services with scripted models and
// audio (shell_fixture.hpp).

#include "wisp/shell/conversation.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

#include "shell/shell_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/ui/strings.hpp"

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
    // A held recording, transcribed as `heard`.
    void hold_and_say(const std::string& heard = "你好") {
        server.push_asr(asr_text(heard));
        say_into_mic();
        conv->hold_begin();
        ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
        conv->hold_end();
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
    hold_and_say("你好");
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
    hold_and_say();
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
    hold_and_say();
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
    hold_and_say(longest);
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
    hold_and_say();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(tts_inputs(), std::vector<std::string>{"今天晴。"});
    const wisp::shell::TurnView v = conv->current();
    EXPECT_EQ(text(v.say), "");
    EXPECT_EQ(text(v.body), "今天晴。明天雨。");
    EXPECT_TRUE(v.spoken);
}

TEST_F(ConversationTest, TapHasNoVoiceEffect) {
    start();
    out->set_write_ms(20);  // plays in real time

    // Idle: a tap changes nothing.
    conv->tap();
    wisp::os::sleep_ms(50);
    EXPECT_EQ(mic->opens(), 0);
    EXPECT_FALSE(voice->capturing());
    EXPECT_EQ(requests(kAsrPath), 0u);

    // Listening (a hold capture): a tap changes nothing.
    mic->set_script({silence(300), sine(1000)});
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    const int opens_listening = mic->opens();
    conv->tap();
    wisp::os::sleep_ms(50);
    EXPECT_EQ(mic->opens(), opens_listening);
    EXPECT_TRUE(voice->capturing());
    EXPECT_EQ(requests(kAsrPath), 0u);

    // The hold is released normally so the turn can move on to thinking.
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    server.push_asr(asr_text("讲个故事"));
    // The model thinks until released.
    server.llm().push({.status = 200,
                       .chunks = {FakeLlmServer::sse_text("<say>从前有座山，山里有座庙。</say>"),
                                  FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()},
                       .hold_after = 0});
    server.push_tts(tts_pcm(3000));
    conv->hold_end();

    // Thinking: a tap does nothing.
    ASSERT_TRUE(wait_until([&] { return server.llm().held() == 1; }, 5000));
    EXPECT_TRUE(saw_state("transcribing"));
    EXPECT_EQ(requests(kAsrPath), 1u);
    const int opens_thinking = mic->opens();
    conv->tap();
    wisp::os::sleep_ms(150);
    EXPECT_EQ(mic->opens(), opens_thinking);
    EXPECT_FALSE(voice->capturing());

    // Speaking: a tap does not stop the speech.
    const size_t frames_before_speech = out->frames();  // the start tone already wrote some
    server.release();
    ASSERT_TRUE(wait_until([&] { return voice->speaking(); }, 5000));
    conv->tap();
    wisp::os::sleep_ms(150);
    EXPECT_TRUE(voice->speaking());
    ASSERT_TRUE(settled());
    EXPECT_EQ(out->frames() - frames_before_speech, 24u * 3000);
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

TEST_F(ConversationTest, DroppedVoiceTextFreesTheTalkKey) {
    start();
    // The recording starts, then a web turn takes the agent; the
    // transcript waits behind it.
    hold_and_say("你好");
    ASSERT_TRUE(wait_until([&] { return voice->capturing(); }, 2000));
    server.llm().push(llm_text("never", /*hold_after=*/0));
    ASSERT_TRUE(agent->submit("hi", Source::Web));
    ASSERT_TRUE(wait_until([&] { return server.llm().held() == 1; }, 3000));
    ASSERT_TRUE(wait_until([&] { return agent->view().queued; }, 8000));
    EXPECT_EQ(voice_locks(), 3u);

    // A stop from the web page drops the waiting voice text as well.
    agent->stop_turn();
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(statuses(), std::vector<std::string>{"stopped"});
    // Its round ends now, not at the backstop.
    ASSERT_TRUE(wait_until([&] { return voice_locks() == 0; }, 3000));
    EXPECT_FALSE(conv->current().running);

    // The talk key works again.
    mic->set_script({silence(300), sine(1000)});
    conv->hold_begin();
    EXPECT_TRUE(voice->capturing());
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 2000));
    conv->hold_end();
    ASSERT_TRUE(settled());
}

TEST_F(ConversationTest, HoldOverATranscriptionRecordsAnew) {
    start();
    server.push_asr(FakeReply{.status = 200, .chunks = {R"({"text":"x"})"}, .hold_after = 0});
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    conv->hold_end();
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
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    conv->hold_end();
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
    hold_and_say();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled(15000));
    EXPECT_EQ(tool_runs.load(), 2);
    EXPECT_EQ(tts_inputs(), (std::vector<std::string>{"第一句。", "第二句。", "第三句。"}));
    EXPECT_LE(server.max_concurrent(), 2);
}

// --- the agent's steps (mvp spec 6.2) ---------------------------------------

namespace {

using wisp::shell::TurnStep;
using wisp::shell::TurnView;

std::string label(const TurnStep& s) { return std::string(s.label.data(), s.label.size()); }

FakeReply tool_call(const std::string& id, const std::string& wire, const std::string& args) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_tool(0, id, wire, args),
                       FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}};
}

// A tool under a dotted name: ok unless `fail`, and (with `gate`) running
// until the gate opens.
void add_test_tool(wisp::agent::ToolRegistry& tools, const std::string& name, bool fail = false,
                   std::atomic<bool>* gate = nullptr) {
    wisp::agent::ToolDef def;
    def.name = name;
    def.description = "test tool";
    def.run = [fail, gate](const wisp::Value&, wisp::agent::ToolCtx&) {
        while (gate != nullptr && !gate->load()) wisp::os::sleep_ms(5);
        if (fail)
            return wisp::agent::ToolOutput{.ok = false,
                                           .result = wisp::Map{{"ok", false}, {"error", "failed"}}};
        return wisp::agent::ToolOutput{.ok = true, .result = wisp::Map{{"ok", true}}};
    };
    ASSERT_TRUE(tools.add(std::move(def)));
}

}  // namespace

TEST_F(ConversationTest, StepsRunThenEndDoneOrFailed) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    std::atomic<bool> gate{false};
    start(true, [&](wisp::agent::ToolRegistry& tools) {
        add_test_tool(tools, "scene.write_file", false, &gate);
        add_test_tool(tools, "todo.add", /*fail=*/true);
    });
    server.llm().push(tool_call("c1", "scene_write_file",
                                R"({"id":"clock","path":"main.lua","content":"x = 1"})"));
    server.llm().push(tool_call("c2", "todo_add", R"({"title":"买牛奶"})"));
    server.llm().push(llm_text("好了"));
    ASSERT_TRUE(agent->submit("做个时钟", Source::Web));

    ASSERT_TRUE(wait_until(
        [&] {
            const TurnView v = conv->current();
            return v.running_step() != nullptr && label(*v.running_step()) == "写入文件 main.lua";
        },
        5000));
    const uint64_t started = conv->current().started_ms;
    EXPECT_NE(started, 0u);
    EXPECT_EQ(conv->current().steps_total, 1);
    EXPECT_EQ(conv->current().ended_ms, 0u);
    virtual_ms += 42000;
    gate = true;
    ASSERT_TRUE(wait_turns(1));

    const TurnView v = conv->current();
    EXPECT_FALSE(v.running);
    ASSERT_EQ(v.step_count, 2);
    EXPECT_EQ(v.steps_total, 2);
    EXPECT_EQ(label(v.steps[0]), "写入文件 main.lua");
    EXPECT_EQ(v.steps[0].st, TurnStep::State::Done);
    EXPECT_EQ(label(v.steps[1]), "添加待办 买牛奶");
    EXPECT_EQ(v.steps[1].st, TurnStep::State::Failed);
    EXPECT_EQ(v.started_ms, started);
    EXPECT_GE(v.ended_ms, started + 42000);
    EXPECT_FALSE(v.thinking);
    EXPECT_EQ(v.running_step(), nullptr);
}

TEST_F(ConversationTest, AFifthStepDropsTheOldest) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    start(true, [&](wisp::agent::ToolRegistry& tools) { add_test_tool(tools, "scene.open"); });
    for (int i = 1; i <= 5; ++i) {
        const std::string n = std::to_string(i);
        server.llm().push(tool_call("c" + n, "scene_open", R"({"id":"s)" + n + R"("})"));
    }
    server.llm().push(llm_text("好了"));
    ASSERT_TRUE(agent->submit("开五个", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    const TurnView v = conv->current();
    ASSERT_EQ(v.step_count, wisp::shell::kTurnSteps);
    EXPECT_EQ(v.steps_total, 5);
    for (size_t i = 0; i < wisp::shell::kTurnSteps; ++i) {
        EXPECT_EQ(label(v.steps[i]), "打开场景 s" + std::to_string(i + 2));
        EXPECT_EQ(v.steps[i].st, TurnStep::State::Done);
    }
}

TEST_F(ConversationTest, ACallBeingWrittenIsAStepAlready) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    start(true,
          [&](wisp::agent::ToolRegistry& tools) { add_test_tool(tools, "scene.write_file"); });
    server.llm().push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", "scene_write_file", ""),
                    FakeLlmServer::sse_tool(0, "", "", R"({"id":"clock"})"),
                    FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()},
         .hold_after = 1});
    server.llm().push(llm_text("好了"));
    ASSERT_TRUE(agent->submit("写个文件", Source::Web));
    ASSERT_TRUE(wait_until([&] { return conv->current().step_count == 1; }, 5000));
    {
        const TurnView v = conv->current();
        ASSERT_NE(v.running_step(), nullptr);
        EXPECT_EQ(label(*v.running_step()), "写入文件");
        EXPECT_EQ(v.steps_total, 1);
    }
    server.release();
    ASSERT_TRUE(wait_turns(1));
    const TurnView v = conv->current();
    // The call itself took the same step over.
    ASSERT_EQ(v.step_count, 1);
    EXPECT_EQ(v.steps_total, 1);
    EXPECT_EQ(label(v.steps[0]), "写入文件 clock");
    EXPECT_EQ(v.steps[0].st, TurnStep::State::Done);
}

TEST_F(ConversationTest, ThinkingAfterAToolIsAPassingStep) {
    start(true, [&](wisp::agent::ToolRegistry& tools) { add_test_tool(tools, "scene.list"); });
    server.llm().push(tool_call("c1", "scene_list", "{}"));
    // The answer after the tool waits, so the turn stays thinking.
    server.llm().push({.status = 200,
                       .chunks = {FakeLlmServer::sse_text("好了"),
                                  FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()},
                       .hold_after = 0});
    // No passing step before the first one.
    std::atomic<bool> early_thinking{false};
    const uint32_t id = conv->add_listener([&](const TurnView& v) {
        if (v.thinking && v.steps_total == 0) early_thinking = true;
    });
    ASSERT_TRUE(agent->submit("看看场景", Source::Web));
    ASSERT_TRUE(wait_until([&] { return conv->current().thinking; }, 5000));
    {
        const TurnView v = conv->current();
        ASSERT_EQ(v.step_count, 1);
        EXPECT_EQ(v.steps[0].st, TurnStep::State::Done);
        EXPECT_EQ(v.running_step(), nullptr);
    }
    server.release();
    ASSERT_TRUE(wait_turns(1));
    conv->remove_listener(id);
    EXPECT_FALSE(conv->current().thinking);
    EXPECT_EQ(conv->current().step_count, 1);
    EXPECT_FALSE(early_thinking.load());
}

// A call the model wrote but that never ran is no step: it neither stays
// running nor counts in the total.
TEST_F(ConversationTest, ACallSkippedAtTheStepLimitIsNoStep) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    start(true, [&](wisp::agent::ToolRegistry& tools) { add_test_tool(tools, "scene.list"); });
    for (uint32_t i = 0; i <= wisp::agent::kMaxSteps; ++i)
        server.llm().push(tool_call("c" + std::to_string(i), "scene_list", "{}"));
    ASSERT_TRUE(agent->submit("一直看", Source::Web));
    ASSERT_TRUE(wait_turns(1, 15000));
    const TurnView v = conv->current();
    EXPECT_EQ(v.steps_total, wisp::agent::kMaxSteps);
    // The skipped call's row is gone; the one it pushed out of the kept
    // rows does not come back.
    ASSERT_GE(v.step_count, 1);
    for (size_t i = 0; i < v.step_count; ++i) {
        EXPECT_EQ(label(v.steps[i]), "查看场景列表");
        EXPECT_EQ(v.steps[i].st, TurnStep::State::Done);
    }
    EXPECT_EQ(v.running_step(), nullptr);
}

TEST_F(ConversationTest, ACallSkippedByAStopIsNoStep) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    std::atomic<bool> gate{false};
    start(true, [&](wisp::agent::ToolRegistry& tools) {
        add_test_tool(tools, "scene.write_file", false, &gate);
        add_test_tool(tools, "todo.add");
    });
    server.llm().push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", "scene_write_file", R"({"path":"a.lua"})"),
                    FakeLlmServer::sse_tool(1, "c2", "todo_add", R"({"title":"x"})"),
                    FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}});
    ASSERT_TRUE(agent->submit("写完再记", Source::Web));
    ASSERT_TRUE(wait_until(
        [&] {
            const TurnView v = conv->current();
            return v.step_count == 2 && label(v.steps[0]) == "写入文件 a.lua";
        },
        5000));
    agent->stop_turn();
    gate = true;
    ASSERT_TRUE(wait_turns(1));
    const TurnView v = conv->current();
    EXPECT_EQ(v.steps_total, 1);
    ASSERT_EQ(v.step_count, 1);
    EXPECT_EQ(label(v.steps[0]), "写入文件 a.lua");
    EXPECT_NE(v.steps[0].st, TurnStep::State::Running);
    EXPECT_EQ(v.running_step(), nullptr);
}

TEST_F(ConversationTest, ACallStillBeingWrittenWhenStoppedIsNoStep) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    start(true,
          [&](wisp::agent::ToolRegistry& tools) { add_test_tool(tools, "scene.write_file"); });
    server.llm().push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", "scene_write_file", R"({"path":"a)"),
                    FakeLlmServer::sse_tool(0, "", "", R"(.lua"})"),
                    FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()},
         .hold_after = 2});
    ASSERT_TRUE(agent->submit("写个文件", Source::Web));
    ASSERT_TRUE(wait_until([&] { return conv->current().step_count == 1; }, 5000));
    agent->stop_turn();
    ASSERT_TRUE(wait_turns(1));
    const TurnView v = conv->current();
    EXPECT_EQ(v.steps_total, 0);
    EXPECT_EQ(v.step_count, 0);
    EXPECT_EQ(v.running_step(), nullptr);
}

TEST_F(ConversationTest, ACallCutByATransportErrorIsNoStep) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    start(true,
          [&](wisp::agent::ToolRegistry& tools) { add_test_tool(tools, "scene.write_file"); });
    server.llm().push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", "scene_write_file", R"({"path":"a)"),
                    FakeLlmServer::sse_tool(0, "", "", R"(.lua"})")},
         .fail = wisp::net::TransportError::Other});
    ASSERT_TRUE(agent->submit("写个文件", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    bool prepped = false;
    for (const auto& e : events()) prepped |= e.kind == Kind::ToolPrep;
    EXPECT_TRUE(prepped);
    const TurnView v = conv->current();
    EXPECT_EQ(v.steps_total, 0);
    EXPECT_EQ(v.step_count, 0);
    EXPECT_EQ(v.running_step(), nullptr);
}
