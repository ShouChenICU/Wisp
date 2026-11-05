#include "wisp/app/system_notices.hpp"

#include <string>

namespace wisp::app {

void post_wakelock_reminders(core::WakeLockRegistry& locks, core::NotificationService& notes,
                             uint64_t now_ms) {
    for (const core::WakeLockInfo& lock : locks.due_reminders(now_ms)) {
        if (lock.holder.rfind("scene.", 0) == 0) {
            notes.post_system(core::notice::kWakeLockLong, lock.holder,
                              core::notice::kActionReleaseLock, std::to_string(lock.id));
        } else {
            notes.post_system(core::notice::kWakeLockLong, lock.holder);
        }
    }
}

}  // namespace wisp::app
