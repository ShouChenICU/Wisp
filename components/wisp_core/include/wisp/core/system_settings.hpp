#pragma once

// SystemSettings (spec 5.4): validated access to the device-wide Settings
// keys the admin page, future agent tools and the settings scene all edit:
//   brightness   display/brightness   10-255           (default 200)
//   volume       audio/volume         0-100            (default 60)
//   screen_off_s power/screen_off_s   0 (never) or 15-3600 (default 60)
// The consumers (App at boot, the audio path, PowerManager::tick()) read
// these keys fresh, so only brightness needs an explicit "apply now" hook.
//
// Thread-safe: set() calls are serialized so concurrent batches cannot
// interleave their writes or rollbacks.

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

struct SystemSettingsValues {
    int brightness;
    int volume;
    int screen_off_s;
};

class SystemSettings {
public:
    // `apply_brightness` is called after a batch containing "brightness"
    // has been stored (App passes PowerManager::set_brightness). May be
    // empty.
    SystemSettings(SettingsBackend& backend, std::function<void(uint8_t)> apply_brightness);

    SystemSettings(const SystemSettings&) = delete;
    SystemSettings& operator=(const SystemSettings&) = delete;

    SystemSettingsValues get() const;

    // Keys are "brightness", "volume", "screen_off_s". An unknown key or an
    // out-of-range value fails the whole batch with InvalidArgument before
    // anything is written. If the backend fails partway, the keys already
    // written are restored (best effort) and the backend's error returned;
    // apply_brightness only runs once the whole batch is stored.
    Result<void> set(const std::map<std::string, int64_t>& changes);

private:
    SettingsBackend& backend_;
    std::function<void(uint8_t)> apply_brightness_;
    os::Mutex mutex_;
};

}  // namespace wisp::core
