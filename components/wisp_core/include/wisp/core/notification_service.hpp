#pragma once

// Notifications (scene-device spec 5): scenes post them through wisp.notify,
// the system posts its own (source "system", identified by a code and an
// argument the UI turns into localized text). Unread notifications are kept
// in memory, newest last, and persisted to <System>/notifications.json.
//
// Limits (spec 5.1, 5.7): at most kUnreadPerScene unread per scene and
// kStoredMax in all, the oldest dropped first (reason "evict"); at most
// kPerMinute scene posts per scene in any 60 s ("rate_limited"); system
// notices are not rate limited. Posting an id that is already unread for
// the same source replaces it in place of a dismissal.
//
// Events (spec 5.7), source "notifications", never the body:
//   notification.posted     {"id", "source", "title"}  (title: the code for
//                                                        a system notice)
//   notification.dismissed  {"id", "source", "reason"}
//
// Persistence: a change to the unread list starts one periodic kCoalesceMs
// timer; its tick writes the whole list with write_atomic() in a submit_io
// job (the storage worker, which may touch flash) and stops the timer once
// the list on disk is current. A failed submit or write is retried with a
// doubling delay; after kMaxSaveAttempts the timer stops and the next
// change (or flush()) tries again, so a device without a working worker
// does not wake every 300 ms. `wake` is not stored, so a notice reloaded
// after a restart never turns the screen on.
//
// Thread-safe. Only load() and flush() touch flash; every other method
// changes memory and arms the timer, so the scene task may call them.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class NotifySound : uint8_t { None, Tone, Alarm };
// "none", "tone", "alarm".
std::optional<NotifySound> parse_notify_sound(std::string_view s);
const char* to_string(NotifySound s);

enum class DismissReason : uint8_t { Tap, Dismiss, Cancel, Evict };
// "tap", "dismiss", "cancel", "evict".
const char* to_string(DismissReason r);

// System notification codes and actions. The UI renders the text from the
// code and its argument, in the device language.
namespace notice {
inline constexpr const char* kSceneEvictedMemory = "scene_evicted_memory";
inline constexpr const char* kSceneEvictedCount = "scene_evicted_count";
inline constexpr const char* kSceneError = "scene_error";
inline constexpr const char* kRestoreFailed = "restore_failed";
inline constexpr const char* kWakeLockLong = "wakelock_long";
inline constexpr const char* kBatteryLow = "battery_low";
// action_arg is the wake lock id.
inline constexpr const char* kActionReleaseLock = "release_lock";
}  // namespace notice

inline constexpr const char* kSystemSource = "system";

struct Notification {
    std::string source;              // "system" or a scene id
    std::string id;                  // unique per source
    std::string source_name;         // scene name when posted; "" for system
    std::string title, body;         // scenes
    std::string code, arg;           // system
    std::string action, action_arg;  // "" or notice::kActionReleaseLock + lock id
    NotifySound sound = NotifySound::None;
    bool wake = false;
    uint64_t posted_ms = 0;
    int64_t posted_unix = 0;  // 0: clock unset
};

struct NotificationChange {
    enum class Kind : uint8_t { Posted, Dismissed } kind;
    Notification n;
    // false: a foreground scene's own post, shown as a banner only. It
    // replaces any unread notice with the same source and id, which is
    // gone from unread() without a Dismissed change: a list showing that
    // row drops it.
    bool listed = true;
    DismissReason reason = DismissReason::Dismiss;
};

// The coalescing timer. start() makes `tick` run every kCoalesceMs until
// stop(), and returns false if the timer cannot run (the next change tries
// again). The tick calls stop() on itself with the service's lock held, so
// stop() from inside the tick must not block, and a start() from another
// thread right after it must arm the timer again (os::Timer does both).
// stop() from another thread waits for a tick in progress.
struct NotificationTimer {
    std::function<bool()> start;
    std::function<void()> stop;
};

struct NotificationDeps {
    EventBus& bus;
    StorageManager& storage;
    // Runs a job on the storage worker; false if it could not be queued.
    // Production: StorageWorker::submit. Empty: nothing is saved (no
    // storage worker); load() still reads.
    std::function<bool(std::function<void()>)> submit_io;
    // Makes the coalescing timer, once, in the constructor. Empty: a
    // periodic os::Timer. See NotificationTimer.
    std::function<NotificationTimer(std::function<void()> tick)> make_timer;
    std::function<uint64_t()> now_ms = os::now_ms;
    // Seconds since the epoch; 0 or empty: unknown.
    std::function<int64_t()> wall_s;
};

// ^[a-z0-9_-]{1,32}$
bool valid_notification_id(std::string_view id);

class NotificationService {
public:
    static constexpr size_t kTitleMax = 32, kBodyMax = 200, kIdMax = 32;
    static constexpr size_t kUnreadPerScene = 5, kPerMinute = 10, kStoredMax = 50;
    static constexpr uint32_t kCoalesceMs = 300;
    // Failed saves in a row before the timer stops until the next change;
    // the retries wait 600, 1200, 2400 and 4800 ms.
    static constexpr int kMaxSaveAttempts = 5;

    explicit NotificationService(NotificationDeps deps);
    ~NotificationService();  // close()

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;

    // Reads <System>/notifications.json; run it on the storage worker. A
    // missing or unreadable file is an empty list (logged), never an
    // error; the only error is a storage without a System area. Entries
    // that break the post-time limits are skipped, and only the newest
    // kUnreadPerScene of each scene are kept. Stored notices go before
    // anything already posted in memory, which wins on a clash.
    Result<void> load();

    // A scene's post: `n.source` is the scene id, `n.id` empty or valid.
    // InvalidArgument for a bad title, body or id; RateLimited
    // "rate_limited" past kPerMinute posts by this scene in the last 60 s
    // (a sliding window; replacements and foreground posts count, rejected
    // ones do not). A foreground scene's own post (`from_foreground`) is
    // announced (notification.posted, listener with listed = false) but not
    // kept, and it silently removes an unread notice with the same id (no
    // notification.dismissed, as for any replacement).
    // Returns the id: the one given, or "n<seq>".
    Result<std::string> post(Notification n, bool from_foreground);
    // A system notice with id "<code>:<arg>", which replaces an earlier one
    // with the same code and argument.
    std::string post_system(std::string code, std::string arg, std::string action = {},
                            std::string action_arg = {});

    // Removes one unread notice; false if there is none. cancel() is
    // dismiss() with DismissReason::Cancel.
    bool cancel(std::string_view source, std::string_view id);
    bool dismiss(std::string_view source, std::string_view id, DismissReason reason);
    // Removes every unread notice of `source`; returns how many.
    size_t dismiss_source(std::string_view source, DismissReason reason);
    // Removes every system notice with `code` (any argument); returns how
    // many.
    size_t dismiss_code(std::string_view code, DismissReason reason);
    // Removes everything, each with DismissReason::Dismiss.
    void clear_all();

    std::optional<Notification> find(std::string_view source, std::string_view id) const;
    // Newest first.
    std::vector<Notification> unread() const;
    size_t unread_count() const;

    // Runs on the caller's thread after each change, outside the lock.
    // Once set_listener() returns no call of the previous listener is
    // still running, so it must not be called from inside the listener.
    void set_listener(std::function<void(const NotificationChange&)> cb);

    // Queues a write of any change not yet on disk and waits for it up to
    // `timeout_ms` (shutdown). True when nothing is left to write. Not on
    // the storage worker (it would wait on itself).
    bool flush(uint32_t timeout_ms);

    // Stops the timer for good, waiting for a tick in progress (shutdown,
    // before the storage worker goes). Changes after it stay in memory
    // until flush(). Idempotent; not from inside a tick.
    void close();

private:
    // Everything lives here, shared with the timer and storage jobs, which
    // hold it weakly: one that runs after the service is gone does nothing.
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace wisp::core
