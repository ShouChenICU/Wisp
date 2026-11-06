#include "wisp/app/system_notices.hpp"

#include <string>

namespace wisp::app {

void post_wakelock_reminders(core::WakeLockRegistry& locks, core::NotificationService& notes,
                             uint64_t now_ms) {
    for (const core::WakeLockInfo& lock : locks.due_reminders(now_ms)) {
        // A scene's screen lock only exists while that scene is in the
        // foreground, so the user can already see it, and a short press
        // of the power key still turns the screen off regardless (spec
        // 4.3). A reminder would only get in the way, e.g. for a desk
        // clock.
        if (lock.kind == core::WakeLockKind::Screen) continue;
        if (lock.holder.rfind("scene.", 0) == 0) {
            notes.post_system(core::notice::kWakeLockLong, lock.holder,
                              core::notice::kActionReleaseLock, std::to_string(lock.id));
        } else {
            notes.post_system(core::notice::kWakeLockLong, lock.holder);
        }
    }
}

}  // namespace wisp::app
