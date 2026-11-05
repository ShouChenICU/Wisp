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
// Persistence: every change to the unread list arms one schedule(kCoalesceMs)
// timer; when it fires the whole list is written with write_atomic() in a
// submit_io job (the storage worker, which may touch flash). `wake` is not
// stored, so a notice reloaded after a restart never turns the screen on.
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
    bool listed = true;  // false: a foreground scene's own post (banner only)
    DismissReason reason = DismissReason::Dismiss;
};

struct NotificationDeps {
    EventBus& bus;
    StorageManager& storage;
    // Runs a job on the storage worker; false if it could not be queued.
    // Production: StorageWorker::submit.
    std::function<bool(std::function<void()>)> submit_io;
    // Calls the callback once after `ms`.
    std::function<void(uint32_t ms, std::function<void()>)> schedule;
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

    explicit NotificationService(NotificationDeps deps);
    ~NotificationService();

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;

    // Reads <System>/notifications.json; run it on the storage worker. A
    // missing or unreadable file is an empty list (logged), never an
    // error; the only error is a storage without a System area. Stored
    // notices go before anything already posted in memory, which wins on a
    // clash.
    Result<void> load();

    // A scene's post: `n.source` is the scene id, `n.id` empty or valid.
    // InvalidArgument for a bad title, body or id; RateLimited
    // "rate_limited" past kPerMinute. A foreground scene's own post
    // (`from_foreground`) is announced (event, listener with listed =
    // false) but not kept, and it removes an unread one with the same id.
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

private:
    // Everything lives here, shared with the timer and storage jobs, which
    // hold it weakly: one that runs after the service is gone does nothing.
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace wisp::core
