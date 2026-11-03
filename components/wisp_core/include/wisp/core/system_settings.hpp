#pragma once

// SystemSettings (spec 5.4): validated access to the device-wide Settings
// keys the admin page, future agent tools and the settings scene all edit:
//   brightness   display/brightness   int 10-255           (default 200)
//   volume       audio/volume         int 0-100            (default 60)
//   screen_off_s power/screen_off_s   int 0 (never) or 15-3600 (default 60)
//   language     sys/lang             "zh", "ja" or "en"   (default "zh")
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

struct SystemSettingsValues {
    int brightness;
    int volume;
    int screen_off_s;
    std::string language;
};

// {"brightness", "volume", "screen_off_s", "language"} -- the shape of
// GET /settings and of the system.settings event payload.
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

    // Keys are "brightness", "volume", "screen_off_s" (integers) and
    // "language" (a string). An unknown key, a value of the wrong type or
    // an out-of-range value fails the whole batch with InvalidArgument
    // before anything is written. If the backend fails partway, the keys
    // already written are restored (best effort) and the backend's error
    // returned; the hooks only run once the whole batch is stored.
    Result<void> set(const std::map<std::string, Value>& changes);

private:
    SettingsBackend& backend_;
    std::function<void(uint8_t)> apply_brightness_;
    std::function<void(const SystemSettingsValues&)> on_changed_;
    os::Mutex mutex_;
    std::atomic<uint32_t> revision_{0};
};

}  // namespace wisp::core
