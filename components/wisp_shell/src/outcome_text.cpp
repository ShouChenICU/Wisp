#include "outcome_text.hpp"

namespace wisp::shell::detail {

ui::StrId voice_error_text(voice::VoiceError e) {
    switch (e) {
        case voice::VoiceError::NoAsrProfile:
            return ui::StrId::VoiceErrNoAsr;
        case voice::VoiceError::NoTtsProfile:
            return ui::StrId::VoiceErrNoTts;
        case voice::VoiceError::Offline:
            return ui::StrId::VoiceErrOffline;
        case voice::VoiceError::AsrFailed:
            return ui::StrId::VoiceErrAsr;
        case voice::VoiceError::TtsFailed:
            return ui::StrId::VoiceErrTts;
        case voice::VoiceError::MicUnavailable:
            return ui::StrId::VoiceErrMic;
    }
    return ui::StrId::VoiceErrAsr;
}

ui::StrId turn_end_text(std::string_view r) {
    if (r == "stopped") return ui::StrId::HomeTurnStopped;
    if (r == "step_limit") return ui::StrId::HomeTurnStepLimit;
    if (r == "time_limit") return ui::StrId::HomeTurnTimeLimit;
    if (r == "no_model") return ui::StrId::HomeTurnNoModel;
    if (r == "unauthorized") return ui::StrId::HomeTurnUnauthorized;
    if (r == "network") return ui::StrId::HomeTurnNetwork;
    if (r == "network_lost") return ui::StrId::HomeTurnNetworkLost;
    if (r == "context") return ui::StrId::HomeTurnContext;
    return ui::StrId::HomeTurnServer;
}

}  // namespace wisp::shell::detail
