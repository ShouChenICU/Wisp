#pragma once

// The short words the shell's screens use for how a capture or a turn
// went: the home screen's hint (wisp/shell/home.hpp) and the talk overlay
// (wisp/shell/talk_overlay.hpp) say the same thing. Short rather than the
// agent's own note (TurnView::end_note), which does not fit the small
// round screens.

#include <string_view>

#include "wisp/ui/strings.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::shell::detail {

// A voice error (TurnView::error).
ui::StrId voice_error_text(voice::VoiceError e);
// A turn that ended with AgentEvent::reason `r` (TurnView::end_reason, not
// empty); anything not known here reads as the model service's error.
ui::StrId turn_end_text(std::string_view r);

}  // namespace wisp::shell::detail
