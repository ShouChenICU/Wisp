#pragma once

// SystemSettings (spec 5.4): validated access to the device-wide Settings
// keys the admin page, the agent tools and the settings page all edit. Each
// key, its store name, type, range and default is a row of the settings
// schema (wisp/core/settings_schema.hpp); this class only walks that table.
// A stored value set() would refuse reads back as its default; seed_cell()
// logs each such value once at boot.
// utc_offset_min is the local time zone as minutes east of UTC; scenes read
// it as wisp.device.info().utc_offset (scene-runtime-core spec 5.2).
// ntp_servers lists the time servers net::TimeSyncService tries in order
// (connectivity spec 5.4).
// deep_after_s is how long standby lasts before deep standby turns Wi-Fi
// off; automatic light sleep is controlled at build time. notify_vol is
// the alert-sound volume (scene-device spec 4.1 and 5.4).
// tts_mute silences spoken replies (voice spec 4.4): the conversation still
// shows its text, TtsClient is simply never called.
// time_24h is how the device shows a wall-clock time to the user: a
// 24-hour clock, or a 12-hour one with the day half (mvp spec 6.4;
// ui::format_clock()).
// agent_max_steps and agent_turn_min are the agent's per-turn limits, tool
// calls and minutes (agent-core spec 3.6); App hands them to AgentService,
// which applies them from the next turn on.
// App reads them at boot; afterwards the consumers (the audio path,
// PowerManager's timeouts and the panel brightness, the rest) read or
// follow cell().
// The whole snapshot lives in a state cell (cell()) that follows every
// stored batch; consumers follow cell(). Its mirror publishes the
// system.settings event (spec 8.3) for the admin console.
//
// Thread-safe: set() calls are serialized so concurrent batches cannot
// interleave their writes or rollbacks.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

// The snapshot the settings cell holds: one field per schema row.
using SystemSettingsValues = SettingsSnapshot;

// Defaults and ranges of single keys, read from the schema table for the
// consumers that still name them.
inline constexpr std::string_view kDefaultNtpServers = setting_default_text(SettingId::NtpServers);
inline constexpr int64_t kDefaultBrightness = setting_default_int(SettingId::Brightness);
inline constexpr int64_t kDefaultVolume = setting_default_int(SettingId::Volume);
inline constexpr std::string_view kDefaultLanguage = setting_default_text(SettingId::Language);
inline constexpr int64_t kDefaultUtcOffsetMin = setting_default_int(SettingId::UtcOffsetMin);
inline constexpr int64_t kDefaultNotifyVol = setting_default_int(SettingId::NotifyVol);
inline constexpr bool kDefaultTtsMute = setting_default_int(SettingId::TtsMute) != 0;
inline constexpr bool kDefaultTime24h = setting_default_int(SettingId::Time24h) != 0;

// The power keys' defaults and valid ranges, shared with
// wisp::app::PowerManager.
inline constexpr int64_t kDefaultScreenOffS = setting_default_int(SettingId::ScreenOffS);
inline constexpr int64_t kDefaultDeepAfterS = setting_default_int(SettingId::DeepAfterS);
bool valid_screen_off_s(int64_t seconds);  // the ScreenOffS row's rule
bool valid_deep_after_s(int64_t seconds);  // the DeepAfterS row's rule

// The agent limits' defaults and ranges. wisp::agent's kMaxSteps and
// kTurnMs are the same defaults (App checks that they agree).
inline constexpr int64_t kDefaultAgentMaxSteps = setting_default_int(SettingId::AgentMaxSteps);
inline constexpr int64_t kMinAgentMaxSteps = setting_spec(SettingId::AgentMaxSteps).min;
inline constexpr int64_t kMaxAgentMaxSteps = setting_spec(SettingId::AgentMaxSteps).max;
inline constexpr int64_t kDefaultAgentTurnMin = setting_default_int(SettingId::AgentTurnMin);
inline constexpr int64_t kMinAgentTurnMin = setting_spec(SettingId::AgentTurnMin).min;
inline constexpr int64_t kMaxAgentTurnMin = setting_spec(SettingId::AgentTurnMin).max;

// Who asks SystemSettings::set() to write: each surface may write only the
// rows that carry its flag (settings-schema spec 3.4). Internal callers (App
// itself, boot code, tests) write any row, the store-only ones included.
enum class SettingSurface : uint8_t { Internal, Agent, Web, DevicePage };

// The row flag a surface needs to write a key; 0 for Internal, which is not
// checked.
constexpr uint8_t surface_flag(SettingSurface s) {
    switch (s) {
        case SettingSurface::Agent:
            return kSettingAgent;
        case SettingSurface::Web:
            return kSettingWeb;
        case SettingSurface::DevicePage:
            return kSettingDevicePage;
        case SettingSurface::Internal:
            break;
    }
    return 0;
}

// Event::source of system.settings: App published it before the cell did.
inline constexpr char kSystemSettingsSource[] = "app";

// Every kSettingWeb row under its wire name, as setting_value() gives it:
// the shape of GET /settings and of the system.settings event payload.
Map to_map(const SettingsSnapshot& values);

class SystemSettings {
public:
    // `bus` carries system.settings and must outlive this object.
    SystemSettings(SettingsBackend& backend, EventBus& bus);

    SystemSettings(const SystemSettings&) = delete;
    SystemSettings& operator=(const SystemSettings&) = delete;

    // Reads the store (the settings cache once App has attached it).
    SystemSettingsValues get() const;

    // The snapshot as the last stored batch left it (seed_cell() before
    // that). It moves only when a value changed; system.settings, published
    // by its mirror, also goes out for a stored batch that changed nothing.
    // A subscriber releases its subscription before this object is destroyed
    // (App::stop() order; wisp/core/state_cell.hpp).
    const StateCell<SystemSettingsValues>& cell() const { return cell_; }
    // Loads the cell from the store without publishing anything: App calls
    // it once at boot, after the settings cache is attached and before
    // anyone follows the cell. Logs one warning per stored value that is
    // present but invalid (it reads as the default), each time it runs.
    void seed_cell();

    // Keys are the schema rows' names; each value must pass the row's
    // valid_setting(). An unknown key, a key `from` may not write (its row lacks
    // surface_flag(from); reported as unknown), a value of the wrong type or an out-of-range value
    // fails the whole batch with InvalidArgument before anything is written. If the backend fails
    // partway, the keys already written are restored (best effort) and the backend's error
    // returned; the cell moves and the event goes out only once the whole batch is stored. When a
    // restore fails too, the cell is reseeded from what storage then holds (no event), so it never
    // stays behind storage. Every stored batch publishes exactly one system.settings event.
    Result<void> set(const std::map<std::string, Value>& changes,
                     SettingSurface from = SettingSurface::Internal);

private:
    // get(), logging each present but invalid stored value when `log_invalid`.
    SettingsSnapshot read_store(bool log_invalid) const;

    SettingsBackend& backend_;
    EventBus& bus_;
    os::Mutex mutex_;
    // Declared after bus_ because its constructor takes it. Its owner
    // declares this object after the bus, so the cell is destroyed before
    // the bus object is (owner order, wisp/core/state_cell.hpp).
    StateCell<SystemSettingsValues> cell_;
};

}  // namespace wisp::core
