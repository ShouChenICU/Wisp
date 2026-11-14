#pragma once

// SystemSettings (spec 5.4): validated access to the device-wide Settings
// keys the admin page, future agent tools and the settings scene all edit:
//   brightness     display/brightness  int 10-255                (default 200)
//   volume         audio/volume        int 0-100                 (default 60)
//   screen_off_s   power/screen_off_s  int 0 (never) or 15-3600  (default 60)
//   deep_after_s   power/deep_after_s  int 0 (never) or 60-3600  (default 300)
//   notify_vol     audio/notify_vol    int 0-100                 (default 60)
//   language       sys/lang            "zh", "ja" or "en"        (default "zh")
//   utc_offset_min sys/tz_min          int -720 to 840           (default 480)
//   ntp_servers    sys/ntp             1-2 hosts, comma-separated
//                                      (default kDefaultNtpServers)
//   tts_mute       voice/tts_mute      bool                      (default false)
//   time_24h       sys/time_24h        bool                      (default true)
//   agent_max_steps agent/max_steps    int 8-200                 (default 60)
//   agent_turn_min agent/turn_min      int 2-60                  (default 15)
// A stored value set() would refuse reads back as its default.
// utc_offset_min is the local time zone as minutes east of UTC (UTC-12:00
// to UTC+14:00, half and quarter hours included); scenes read it as
// wisp.device.info().utc_offset (scene-runtime-core spec 5.2).
// ntp_servers lists the time servers net::TimeSyncService tries in order:
// each a host name or an IPv4 address of 1-63 characters [A-Za-z0-9.-],
// no spaces (connectivity spec 5.4).
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
// The consumers (App at boot, the audio path, PowerManager::tick()) read
// these keys fresh, so only brightness needs an explicit "apply now" hook.
// The whole snapshot lives in a state cell (cell()) that follows every
// stored batch; consumers follow it. Its mirror publishes the
// system.settings event (spec 8.3), from which the UI rebuilds its text,
// with revision() as the fallback when that event is lost.
//
// Thread-safe: set() calls are serialized so concurrent batches cannot
// interleave their writes or rollbacks.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

inline constexpr const char* kDefaultNtpServers = "pool.ntp.org,ntp.aliyun.com";

// The other keys' defaults, shared by the key table (system_settings.cpp)
// and SystemSettingsValues' member initializers so the two cannot drift.
inline constexpr int64_t kDefaultBrightness = 200;
inline constexpr int64_t kDefaultVolume = 60;
inline constexpr const char* kDefaultLanguage = "zh";
inline constexpr int64_t kDefaultUtcOffsetMin = 480;
inline constexpr int64_t kDefaultNotifyVol = 60;
inline constexpr bool kDefaultTtsMute = false;
inline constexpr bool kDefaultTime24h = true;

// The power keys' defaults and valid ranges, shared with
// wisp::app::PowerManager, which reads Settings "power" itself and treats a
// stored value set() would refuse as the default, as get() does.
inline constexpr int64_t kDefaultScreenOffS = 60;
inline constexpr int64_t kDefaultDeepAfterS = 300;
bool valid_screen_off_s(int64_t seconds);  // 0 (never) or 15-3600
bool valid_deep_after_s(int64_t seconds);  // 0 (never) or 60-3600

// The agent limits' defaults and ranges. wisp::agent's kMaxSteps and
// kTurnMs are the same defaults (App checks that they agree).
inline constexpr int64_t kDefaultAgentMaxSteps = 60;
inline constexpr int64_t kMinAgentMaxSteps = 8;
inline constexpr int64_t kMaxAgentMaxSteps = 200;
inline constexpr int64_t kDefaultAgentTurnMin = 15;
inline constexpr int64_t kMinAgentTurnMin = 2;
inline constexpr int64_t kMaxAgentTurnMin = 60;

// Default-constructed, it is what an empty store reads as.
struct SystemSettingsValues {
    int brightness = static_cast<int>(kDefaultBrightness);
    int volume = static_cast<int>(kDefaultVolume);
    int screen_off_s = static_cast<int>(kDefaultScreenOffS);
    std::string language = kDefaultLanguage;
    int utc_offset_min = static_cast<int>(kDefaultUtcOffsetMin);
    std::string ntp_servers = kDefaultNtpServers;
    int deep_after_s = static_cast<int>(kDefaultDeepAfterS);
    int notify_vol = static_cast<int>(kDefaultNotifyVol);
    bool tts_mute = kDefaultTtsMute;
    bool time_24h = kDefaultTime24h;
    int agent_max_steps = static_cast<int>(kDefaultAgentMaxSteps);
    int agent_turn_min = static_cast<int>(kDefaultAgentTurnMin);

    bool operator==(const SystemSettingsValues&) const = default;
};

// Event::source of system.settings: App published it before the cell did.
inline constexpr char kSystemSettingsSource[] = "app";

// {"brightness", "volume", "screen_off_s", "language", "utc_offset_min",
// "ntp_servers", "deep_after_s", "notify_vol", "tts_mute",
// "time_24h", "agent_max_steps", "agent_turn_min"}:
// the shape of GET /settings and of the system.settings event payload.
Map to_map(const SystemSettingsValues& values);

class SystemSettings {
public:
    // `apply_brightness` is called after a batch containing "brightness"
    // has been stored (App passes PowerManager::set_brightness), with
    // set()'s lock held, in the order batches were stored, so it must not
    // call set(). It may be empty. `bus` carries system.settings and must
    // outlive this object.
    SystemSettings(SettingsBackend& backend, EventBus& bus,
                   std::function<void(uint8_t)> apply_brightness);

    SystemSettings(const SystemSettings&) = delete;
    SystemSettings& operator=(const SystemSettings&) = delete;

    // Reads the store (the settings cache once App has attached it).
    SystemSettingsValues get() const;

    // The snapshot as the last stored batch left it (seed_cell() before
    // that). It moves only when a value changed; system.settings, published
    // by its mirror, also goes out for a stored batch that changed nothing.
    // Its get() copies the snapshot and so allocates (ntp_servers outgrows
    // the short-string buffer): a frequent reader uses read() instead. A
    // subscriber releases its subscription before this object is destroyed
    // (App::stop() order; wisp/core/state_cell.hpp).
    const StateCell<SystemSettingsValues>& cell() const { return cell_; }
    // Loads the cell from the store without publishing anything: App calls
    // it once at boot, after the settings cache is attached and before
    // anyone follows the cell.
    void seed_cell();

    // Moves (wrapping) every time a batch is stored, whether or not a value
    // changed, before apply_brightness runs; rejected, empty and rolled-back
    // batches leave it alone. Lets a poller that may have missed the
    // system.settings event notice a change with one atomic load instead of
    // reading storage each time.
    uint32_t revision() const;

    // Keys are "brightness", "volume", "screen_off_s", "utc_offset_min",
    // "deep_after_s", "notify_vol", "agent_max_steps", "agent_turn_min" (integers), "language" and
    // "ntp_servers" (strings) and "tts_mute", "time_24h" (booleans). An unknown key,
    // a value of the wrong type or an out-of-range value fails the whole batch with InvalidArgument
    // before anything is written. If the backend fails partway, the keys already written are
    // restored (best effort) and the backend's error returned; apply_brightness and the event
    // only happen once the whole batch is stored. Every stored batch publishes exactly one
    // system.settings event, after apply_brightness.
    Result<void> set(const std::map<std::string, Value>& changes);

private:
    SettingsBackend& backend_;
    EventBus& bus_;
    std::function<void(uint8_t)> apply_brightness_;
    os::Mutex mutex_;
    std::atomic<uint32_t> revision_{0};
    // Declared after bus_ because its constructor takes it; this object is
    // destroyed before the bus stops (App::stop() order).
    StateCell<SystemSettingsValues> cell_;
};

}  // namespace wisp::core
