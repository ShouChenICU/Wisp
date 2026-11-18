#pragma once

// System notifications App posts from other services' signals
// (scene-device spec 5.2).

#include <cstdint>

#include "wisp/core/notification_service.hpp"
#include "wisp/core/wakelock.hpp"

namespace wisp::app {

// The network lock NetWiring holds while external power is present and
// Wi-Fi is on (scene-device spec 4.1). It is policy, not forgotten work,
// and costs no battery, so it never gets a reminder (spec 4.3).
inline constexpr const char* kPoweredWifiHolder = "system.powered_wifi";

// Posts a notice::kWakeLockLong for every network or cpu lock due a
// reminder at `now_ms` (WakeLockRegistry::due_reminders()), with the
// holder as its argument. A scene's lock carries the release_lock action
// with the lock id; a system lock has no action, the user cannot release
// it. A screen lock never gets a reminder: it only exists while its scene
// is in the foreground, where the user can already see it; neither does
// kPoweredWifiHolder's.
void post_wakelock_reminders(core::WakeLockRegistry& locks, core::NotificationService& notes,
                             uint64_t now_ms);

}  // namespace wisp::app
