#pragma once

// Todos and their reminders (mvp spec 5.2).
//
// Local time: every time the user or the agent names is local wall-clock
// time at a fixed UTC offset (SystemSettings' utc_offset_min, no daylight
// saving); stored times are UTC seconds. The helpers below convert.
//
// ReminderScheduler keeps the todos as one JSON document under the System
// area, written with write_atomic():
//   {"version":1,"next_id":n,"todos":[{"id","title","done","created",
//     "due"?,"repeat":"none"|"daily"|"weekly","days"?,"snooze_until"?,
//     "notified_at"?}]}
// `days` (weekly only) lists the weekdays, 1 = Monday. `next_id` is kept
// so a removed todo's id is never handed out again.
//
// Limits: at most kMaxTodos todos (QuotaExceeded "todo_full"), titles of 1
// to kTitleMax Unicode code points (InvalidArgument "empty"/"too_long").
// A due time needs a set clock (Unsupported "clock_not_set"); a repeat
// needs a due time (InvalidArgument "repeat_needs_due"); an unknown id is
// NotFound "not_found". Done todos stay until removed.
//
// Timing: a todo's effective time is its snooze_until if set, else its
// due. One timer is armed, for the earliest effective time among the
// todos that can still ring, at most kMaxArmMs ahead (so a clock change
// the scheduler missed is caught within that), and it is armed again after
// every change, every fire and every system.time event. The timer callback
// only queues fire_due() on the storage worker (submit_io): firing writes
// the file. The timer is one-shot, so a pass the worker refuses (its queue
// full) would leave nothing armed: a refused pass starts a second timer
// that asks again every kRetryMs until a pass runs.
//
// fire_due() goes through every todo whose effective time has come:
//  - one at most kLateS late rings: a notification from kReminderSource,
//    id "todo-<id>", sound Alarm, wake, with the actions "done" and
//    "snooze" (notice::kActionTodoDone / kActionTodoSnooze, argument the
//    todo's id). Its title is the todo's title cut to
//    NotificationService::kTitleMax bytes; when cut, the body holds the
//    whole title;
//  - those more late than that were missed (the device was off, or the
//    clock jumped forward): they are merged into one notification, id
//    "missed", titled missed_title(count), its body the titles joined by
//    "、" and cut to kBodyMax;
// either way a one-shot todo records notified_at and never rings again
// (not even when the clock is set back before its due time) unless it is
// snoozed or given a new due time; a repeating one moves its due time to
// the next occurrence after now, clears its snooze and records
// notified_at, which done() then reads as "this occurrence was shown"
// while that ring is still the current one (see done()).
// start() runs the same pass right away, so whatever came due while the
// device was off is reported at boot; with the clock unset nothing rings
// and the first system.time event runs it.
//
// Threads: load() and every mutating call write the file, so they run on
// the storage worker only. list() reads memory and may be called from any
// thread. One mutex guards the todos; notifications are posted and
// cancelled outside it.
//
// A damaged file is never overwritten silently (as MemoryStore): one that
// cannot be read makes every write refuse with StorageUnavailable
// "todo_unavailable" until clear() or a later successful load(); one that
// reads but does not parse, or names another version, is renamed to
// "<path>.bad" and the scheduler starts empty. Records are re-validated
// on load; dropped ones are counted in the log, never shown.
//
// Factory reset: forget() drops every todo from memory and refuses all
// later writes, so nothing still running can write the erased file back.
//
// Privacy: logs and events never carry a todo's title. Every change
// publishes todo.changed {"count"} (source "todo").

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

// Below this the wall clock is taken as not set (2024-01-01T00:00:00Z,
// scene::MinuteTicker::kValidSince).
inline constexpr int64_t kWallClockValidSince = 1704067200;

enum class Repeat : uint8_t { None, Daily, Weekly };

struct RepeatRule {
    Repeat kind = Repeat::None;
    uint8_t days = 0;  // Weekly: bit 0 = Monday ... bit 6 = Sunday
};

bool operator==(const RepeatRule& a, const RepeatRule& b);

// "none" | "daily" | {"weekly":[1..7, ...]} (at least one day, each
// 1-7). Anything else: nullopt.
std::optional<RepeatRule> parse_repeat(const Value& v);
Value to_value(const RepeatRule& r);

// "YYYY-MM-DDTHH:MM" local time -> UTC seconds; nullopt for anything
// else, including an impossible date.
std::optional<int64_t> parse_local_minute(std::string_view s, int utc_offset_min);
// UTC seconds -> "YYYY-MM-DDTHH:MM" local time.
std::string format_local_minute(int64_t utc_s, int utc_offset_min);
// The local weekday, 1 = Monday ... 7 = Sunday.
int local_weekday(int64_t utc_s, int utc_offset_min);
// Days since 1970-01-01 of the local date.
int64_t local_day(int64_t utc_s, int utc_offset_min);
// The first time after `after` at the same local time of day as `due`
// that `r` allows; `due` itself when it is already after `after`, and for
// Repeat::None.
int64_t next_occurrence(int64_t due, const RepeatRule& r, int64_t after, int utc_offset_min);

struct Todo {
    uint32_t id = 0;
    os::LargeString title;
    bool done = false;
    int64_t created = 0;
    std::optional<int64_t> due, snooze_until, notified_at;
    RepeatRule repeat;
};

// update(): each field that is set replaces the todo's; `due` set to
// nullopt removes the due time.
struct TodoFields {
    std::optional<std::string> title;
    std::optional<std::optional<int64_t>> due;
    std::optional<RepeatRule> repeat;
};

enum class TodoFilter : uint8_t { Open, Done, All, DueToday };

// A timer of the scheduler. cancel() stops it and must wait for a fire in
// progress. The reminder timer's arm() (re)starts it to call its function
// once after `ms`, and is never called from that function. The retry
// timer's arm() starts it calling its function every `ms` until cancel();
// on a running one it is a no-op; its function calls neither.
struct ReminderTimer {
    std::function<void(uint32_t ms)> arm;
    std::function<void()> cancel;
};

struct ReminderDeps {
    StorageManager& storage;
    EventBus& bus;
    // Null: nothing is posted or cancelled.
    NotificationService* notes = nullptr;
    // Runs a job on the storage worker; false if it could not be queued.
    std::function<bool(std::function<void()>)> submit_io;
    // UTC seconds; below kWallClockValidSince: not set. Empty: never set.
    std::function<int64_t()> wall_s;
    // Minutes east of UTC, a cached value (never Settings, which is
    // flash). Empty: 480.
    std::function<int()> utc_offset_min;
    // The merged notification's title for `count` missed reminders, in
    // the device language. Empty: the count alone.
    std::function<std::string(size_t count)> missed_title;
    // Makes the timers in the constructor: first the reminder timer, then
    // the retry timer (see ReminderTimer). Empty: for the first a one-shot
    // os::Timer made again for each arm(), for the second one periodic
    // os::Timer made on its first arm().
    std::function<ReminderTimer(std::function<void()> fire)> make_timer;
};

inline constexpr const char* kTodoChangedTopic = "todo.changed";

class ReminderScheduler {
public:
    static constexpr size_t kMaxTodos = 200, kTitleMax = 100;
    static constexpr int64_t kSnoozeS = 600;
    static constexpr uint32_t kMaxArmMs = 3600000;
    // A todo whose effective time passed longer ago than this was missed
    // rather than late.
    static constexpr int64_t kLateS = 60;
    // How often a pass the worker refused is asked for again.
    static constexpr uint32_t kRetryMs = 1000;
    // A repeating todo that rang this long ago, or earlier the same local
    // day, is still on its current ring for done().
    static constexpr int64_t kRingCurrentS = 3600;

    // `path`: <System>/todo/todos.json; its directory is made on the first
    // write.
    ReminderScheduler(ReminderDeps deps, std::string path);
    // stop().
    ~ReminderScheduler();

    ReminderScheduler(const ReminderScheduler&) = delete;
    ReminderScheduler& operator=(const ReminderScheduler&) = delete;

    // Reads the file (a missing one is an empty list). Worker; before
    // start().
    Result<void> load();
    // Follows system.time and queues the first fire_due() pass on the
    // worker, which also arms the timer. Idempotent.
    void start();
    // Stops the timer and the subscription, and waits for a pass already
    // queued (unless called on the worker). Idempotent.
    void stop();

    // A copy, ascending by id. Memory only. DueToday: open todos due on
    // today's local date (none while the clock is unset).
    os::LargeVector<Todo> list(TodoFilter f) const;

    Result<uint32_t> add(std::string_view title, std::optional<int64_t> due, RepeatRule r);
    // A new due time or repeat rule also clears the snooze and
    // notified_at, so the todo rings at its new time.
    Result<void> update(uint32_t id, const TodoFields& f);
    // A one-shot todo is done. A repeating one never is: done() ends the
    // occurrence that rang (notified_at) while that ring is current -- on
    // the same local day as now, or less than kRingCurrentS ago -- and
    // otherwise skips the coming occurrence (a ring from yesterday that
    // was never answered does not stand for today's). Either way its
    // unread reminder goes.
    Result<void> done(uint32_t id);
    // Rings again kSnoozeS from now. Unsupported "clock_not_set" with the
    // clock unset; InvalidArgument "already_done" for a done todo.
    Result<void> snooze(uint32_t id);
    // Its unread reminder goes too.
    Result<void> remove(uint32_t id);
    // Removes every todo, even after a failed load (it then writes a new,
    // empty file).
    Result<void> clear();
    // Factory reset, on the worker: drops every todo from memory without
    // touching the file, stops the timers and makes every later call
    // that would write refuse ("todo_unavailable"; clear() too), and
    // fire_due() do nothing. Notifications are the caller's to clear.
    void forget();

    // The timer's job, on the worker: see the file header.
    void fire_due();

    // The local time zone the scheduler uses (ReminderDeps::utc_offset_min).
    int utc_offset_min() const;

private:
    os::LargeString serialize_locked() const;
    Result<void> persist_locked();
    Result<void> quarantine_locked();
    void publish_count(size_t count);
    void rearm_locked(int64_t now);
    bool clock_set(int64_t now) const { return now >= kWallClockValidSince; }
    int64_t now_s() const;
    void cancel_notice(uint32_t id);
    // Queues fire_due() on the worker; a refusal starts the retry timer
    // unless `from_retry` (that one asks again by itself).
    void queue_pass(bool from_retry);
    bool writable_locked() const { return !load_failed_ && !forgotten_; }

    ReminderDeps deps_;
    std::string path_;
    ReminderTimer timer_;
    ReminderTimer retry_;
    // Read by the timer and bus callbacks before they touch this object:
    // false once stop() ran.
    std::shared_ptr<std::atomic<bool>> alive_;
    SubscriptionId time_sub_ = 0;
    bool started_ = false;

    mutable os::Mutex mutex_;
    os::LargeVector<Todo> todos_;  // ascending by id
    uint32_t next_id_ = 1;
    bool load_failed_ = false;
    bool forgotten_ = false;  // forget() ran; never cleared
};

}  // namespace wisp::core
