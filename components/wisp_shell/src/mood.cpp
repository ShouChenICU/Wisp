#include "wisp/shell/mood.hpp"

namespace wisp::shell {

ui::WispMood mood_for(agent::AgentState a, voice::VoiceState v, bool error_within_2s) {
    using agent::AgentState;
    if (v == voice::VoiceState::Listening) return ui::WispMood::Listening;
    if (v == voice::VoiceState::Speaking) return ui::WispMood::Speaking;
    if (a == AgentState::Thinking || a == AgentState::Tool || a == AgentState::WaitingConfirm ||
        v == voice::VoiceState::Transcribing)
        return ui::WispMood::Thinking;
    if (error_within_2s) return ui::WispMood::Error;
    return ui::WispMood::Idle;
}

}  // namespace wisp::shell
