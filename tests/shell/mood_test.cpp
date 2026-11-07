// The wisp's mood from the agent's and the voice's states (mvp spec 3.5).

#include "wisp/shell/mood.hpp"

#include <gtest/gtest.h>

using wisp::agent::AgentState;
using wisp::shell::mood_for;
using wisp::ui::WispMood;
using wisp::voice::VoiceState;

TEST(Mood, PriorityTable) {
    // Listening beats everything, speaking everything below it.
    EXPECT_EQ(mood_for(AgentState::Thinking, VoiceState::Listening, true), WispMood::Listening);
    EXPECT_EQ(mood_for(AgentState::Tool, VoiceState::Speaking, true), WispMood::Speaking);
    // Thinking: the agent thinking, running a tool or asking, or the voice
    // transcribing; over a recent error.
    EXPECT_EQ(mood_for(AgentState::Thinking, VoiceState::Idle, true), WispMood::Thinking);
    EXPECT_EQ(mood_for(AgentState::Tool, VoiceState::Idle, false), WispMood::Thinking);
    EXPECT_EQ(mood_for(AgentState::WaitingConfirm, VoiceState::Idle, false), WispMood::Thinking);
    EXPECT_EQ(mood_for(AgentState::Idle, VoiceState::Transcribing, true), WispMood::Thinking);
    // An error shows only while it is recent.
    EXPECT_EQ(mood_for(AgentState::Error, VoiceState::Idle, true), WispMood::Error);
    EXPECT_EQ(mood_for(AgentState::Idle, VoiceState::Error, true), WispMood::Error);
    EXPECT_EQ(mood_for(AgentState::Error, VoiceState::Error, false), WispMood::Idle);
    // The rest is idle, streaming output included.
    EXPECT_EQ(mood_for(AgentState::Output, VoiceState::Idle, false), WispMood::Idle);
    EXPECT_EQ(mood_for(AgentState::Idle, VoiceState::Idle, false), WispMood::Idle);
}
