#include "wisp/ui/setting_labels.hpp"

namespace wisp::ui {

namespace {

struct LabelText {
    std::string_view label{};
    StrId text = StrId::SettingsBrightness;
};

// The settings page's own ids, so a key reads the same on the page and in
// the agent's notes.
constexpr LabelText kLabels[] = {
    {"brightness", StrId::SettingsBrightness}, {"volume", StrId::SettingsVolume},
    {"notify_vol", StrId::SettingsNotifyVol},  {"tts_mute", StrId::SettingsSpeak},
    {"screen_off", StrId::SettingsScreenOff},  {"deep_after", StrId::SettingsDeepStandby},
    {"lang_label", StrId::SettingsLanguage},   {"tz_label", StrId::SettingsTimeZone},
    {"time_24h", StrId::SettingsTimeFormat},
};

}  // namespace

std::optional<StrId> setting_label(std::string_view label) {
    for (const LabelText& entry : kLabels) {
        if (entry.label == label) return entry.text;
    }
    return std::nullopt;
}

}  // namespace wisp::ui
