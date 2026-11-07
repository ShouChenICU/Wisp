#pragma once

// The wisp's mood from the agent's and the voice's states (MVP spec 3.5),
// first match wins:
//   1. voice listening                                  -> Listening
//   2. voice speaking                                   -> Speaking
//   3. agent thinking, tool or waiting_confirm,
//      or voice transcribing                            -> Thinking
//   4. an "error" state (either) within kErrorMoodMs    -> Error
//   5. anything else                                    -> Idle
// The caller times the error: an agent or voice state left at "error"
// counts only through `error_within_2s`.

#include "wisp/agent/agent_service.hpp"
#include "wisp/ui/wisp_model.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::shell {

ui::WispMood mood_for(agent::AgentState a, voice::VoiceState v, bool error_within_2s);

}  // namespace wisp::shell
