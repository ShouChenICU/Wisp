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
// The device UI language follows `on_changed` (spec 8.3): App publishes it
// as the system.settings event and the UI rebuilds its text from that,
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
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

inline constexpr const char* kDefaultNtpServers = "pool.ntp.org,ntp.aliyun.com";

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

struct SystemSettingsValues {
    int brightness;
    int volume;
    int screen_off_s;
    std::string language;
    int utc_offset_min;
    std::string ntp_servers;
    int deep_after_s;
    int notify_vol;
    bool tts_mute;
    bool time_24h;
    int agent_max_steps;
    int agent_turn_min;
};

// {"brightness", "volume", "screen_off_s", "language", "utc_offset_min",
// "ntp_servers", "deep_after_s", "notify_vol", "tts_mute",
// "time_24h", "agent_max_steps", "agent_turn_min"}:
// the shape of GET /settings and of the system.settings event payload.
Map to_map(const SystemSettingsValues& values);

class SystemSettings {
public:
    // `apply_brightness` is called after a batch containing "brightness"
    // has been stored (App passes PowerManager::set_brightness).
    // `on_changed` is called after every non-empty batch has been stored,
    // with the values as they now are. Both run with set()'s lock held, in
    // the order batches were stored, so they must not call set(). Either
    // may be empty.
    SystemSettings(SettingsBackend& backend, std::function<void(uint8_t)> apply_brightness,
                   std::function<void(const SystemSettingsValues&)> on_changed = {});

    SystemSettings(const SystemSettings&) = delete;
    SystemSettings& operator=(const SystemSettings&) = delete;

    SystemSettingsValues get() const;

    // Moves (wrapping) every time a batch is stored, before the hooks run;
    // rejected, empty and rolled-back batches leave it alone. Lets a poller
    // that may have missed `on_changed`'s event notice a change with one
    // atomic load instead of reading storage each time.
    uint32_t revision() const;

    // Keys are "brightness", "volume", "screen_off_s", "utc_offset_min",
    // "deep_after_s", "notify_vol", "agent_max_steps", "agent_turn_min" (integers), "language" and
    // "ntp_servers" (strings) and "tts_mute", "time_24h" (booleans). An unknown key,
    // a value of the wrong type or an out-of-range value fails the whole batch with InvalidArgument
    // before anything is written. If the backend fails partway, the keys already written are
    // restored (best effort) and the backend's error returned; the hooks only run once the whole
    // batch is stored.
    Result<void> set(const std::map<std::string, Value>& changes);

private:
    SettingsBackend& backend_;
    std::function<void(uint8_t)> apply_brightness_;
    std::function<void(const SystemSettingsValues&)> on_changed_;
    os::Mutex mutex_;
    std::atomic<uint32_t> revision_{0};
};

}  // namespace wisp::core
