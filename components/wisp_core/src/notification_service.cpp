#include "wisp/core/notification_service.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "notify";
constexpr const char* kFileName = "/notifications.json";
constexpr uint64_t kRateWindowMs = 60 * 1000;

using LStr = os::LargeString;

LStr large(std::string_view s) { return LStr(s.data(), s.size()); }
std::string plain(const LStr& s) { return std::string(s.data(), s.size()); }
bool same(const LStr& a, std::string_view b) { return std::string_view(a.data(), a.size()) == b; }

const char* sound_name(NotifySound s) {
    switch (s) {
        case NotifySound::None:
            return "none";
        case NotifySound::Tone:
            return "tone";
        case NotifySound::Alarm:
            return "alarm";
    }
    return "none";
}

}  // namespace

std::optional<NotifySound> parse_notify_sound(std::string_view s) {
    if (s == "none") return NotifySound::None;
    if (s == "tone") return NotifySound::Tone;
    if (s == "alarm") return NotifySound::Alarm;
    return std::nullopt;
}

const char* to_string(NotifySound s) { return sound_name(s); }

const char* to_string(DismissReason r) {
    switch (r) {
        case DismissReason::Tap:
            return "tap";
        case DismissReason::Dismiss:
            return "dismiss";
        case DismissReason::Cancel:
            return "cancel";
        case DismissReason::Evict:
            return "evict";
    }
    return "dismiss";
}

bool valid_notification_id(std::string_view id) {
    if (id.empty() || id.size() > NotificationService::kIdMax) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// Unread notices live in PSRAM: up to kStoredMax of them, with bodies, would
// otherwise sit in internal RAM as small std::string allocations.
struct NotificationService::State {
    struct Entry {
        LStr source, id, source_name, title, body, code, arg, action, action_arg;
        NotifySound sound = NotifySound::None;
        bool wake = false;
        uint64_t posted_ms = 0;
        int64_t posted_unix = 0;
    };
    struct Rate {
        LStr source;
        os::LargeVector<uint64_t> times;  // posts within the last window
    };

    explicit State(NotificationDeps d) : deps(std::move(d)) {}
    ~State() { delete os_timer.load(); }

    static Entry to_entry(const Notification& n) {
        Entry e;
        e.source = large(n.source);
        e.id = large(n.id);
        e.source_name = large(n.source_name);
        e.title = large(n.title);
        e.body = large(n.body);
        e.code = large(n.code);
        e.arg = large(n.arg);
        e.action = large(n.action);
        e.action_arg = large(n.action_arg);
        e.sound = n.sound;
        e.wake = n.wake;
        e.posted_ms = n.posted_ms;
        e.posted_unix = n.posted_unix;
        return e;
    }

    static Notification to_notification(const Entry& e) {
        Notification n;
        n.source = plain(e.source);
        n.id = plain(e.id);
        n.source_name = plain(e.source_name);
        n.title = plain(e.title);
        n.body = plain(e.body);
        n.code = plain(e.code);
        n.arg = plain(e.arg);
        n.action = plain(e.action);
        n.action_arg = plain(e.action_arg);
        n.sound = e.sound;
        n.wake = e.wake;
        n.posted_ms = e.posted_ms;
        n.posted_unix = e.posted_unix;
        return n;
    }

    // The rest with mutex held.

    os::LargeVector<Entry>::iterator find_entry(std::string_view source, std::string_view id) {
        return std::find_if(entries.begin(), entries.end(), [&](const Entry& e) {
            return same(e.source, source) && same(e.id, id);
        });
    }

    bool exists(std::string_view source, std::string_view id) {
        return find_entry(source, id) != entries.end();
    }

    void publish_posted(const Notification& n) {
        Event e;
        e.topic = "notification.posted";
        e.source = "notifications";
        e.payload = Properties{
            {"id", Value(n.id)},
            {"source", Value(n.source)},
            {"title", Value(n.source == kSystemSource ? n.code : n.title)},
        };
        deps.bus.publish(std::move(e));
    }

    void publish_dismissed(const Entry& entry, DismissReason reason) {
        Event e;
        e.topic = "notification.dismissed";
        e.source = "notifications";
        e.payload = Properties{
            {"id", Value(plain(entry.id))},
            {"source", Value(plain(entry.source))},
            {"reason", Value(to_string(reason))},
        };
        deps.bus.publish(std::move(e));
    }

    // Removes entries[i], announcing it.
    void remove_at(size_t i, DismissReason reason, std::vector<NotificationChange>& changes) {
        publish_dismissed(entries[i], reason);
        changes.push_back(
            {NotificationChange::Kind::Dismissed, to_notification(entries[i]), true, reason});
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
        ++change_gen;
    }

    // Drops the oldest notices past the per-scene and total limits.
    void enforce_limits(std::string_view source, std::vector<NotificationChange>& changes) {
        if (source != kSystemSource) {
            size_t count = 0;
            for (const Entry& e : entries) {
                if (same(e.source, source)) ++count;
            }
            for (size_t i = 0; count > kUnreadPerScene && i < entries.size();) {
                if (same(entries[i].source, source)) {
                    remove_at(i, DismissReason::Evict, changes);
                    --count;
                } else {
                    ++i;
                }
            }
        }
        while (entries.size() > kStoredMax) remove_at(0, DismissReason::Evict, changes);
    }

    // True if `source` may post now, counting this post if so.
    bool take_rate(std::string_view source, uint64_t now) {
        auto it = std::find_if(rates.begin(), rates.end(),
                               [&](const Rate& r) { return same(r.source, source); });
        if (it == rates.end()) {
            rates.push_back(Rate{large(source), {}});
            it = rates.end() - 1;
        }
        auto& t = it->times;
        t.erase(std::remove_if(t.begin(), t.end(),
                               [&](uint64_t at) { return now - at >= kRateWindowMs; }),
                t.end());
        if (t.size() >= kPerMinute) return false;
        t.push_back(now);
        // Forget sources with nothing left in the window.
        rates.erase(std::remove_if(rates.begin(), rates.end(),
                                   [&](const Rate& r) {
                                       return r.times.empty() ||
                                              std::all_of(r.times.begin(), r.times.end(),
                                                          [&](uint64_t at) {
                                                              return now - at >= kRateWindowMs;
                                                          });
                                   }),
                    rates.end());
        return true;
    }

    // Replaces an unread notice with the same source and id, or appends.
    void store(Notification& n, std::vector<NotificationChange>& changes) {
        auto it = find_entry(n.source, n.id);
        if (it != entries.end()) entries.erase(it);
        entries.push_back(to_entry(n));
        ++change_gen;
        publish_posted(n);
        changes.push_back({NotificationChange::Kind::Posted, n, true, DismissReason::Dismiss});
        enforce_limits(n.source, changes);
    }

    // Serializes the list, oldest first. Without `wake` and posted_ms: both
    // only mean something in the boot they were posted in.
    LStr serialize() const {
        LStr out = large("[");
        bool first = true;
        for (const Entry& e : entries) {
            Map m{
                {"source", Value(plain(e.source))},
                {"id", Value(plain(e.id))},
                {"source_name", Value(plain(e.source_name))},
                {"title", Value(plain(e.title))},
                {"body", Value(plain(e.body))},
                {"code", Value(plain(e.code))},
                {"arg", Value(plain(e.arg))},
                {"action", Value(plain(e.action))},
                {"action_arg", Value(plain(e.action_arg))},
                {"sound", Value(sound_name(e.sound))},
                {"posted_unix", Value(e.posted_unix)},
            };
            if (!first) out += ',';
            first = false;
            std::string one = to_json(Value(std::move(m)));
            out.append(one.data(), one.size());
        }
        out += ']';
        return out;
    }

    NotificationDeps deps;
    std::string path;  // "" until load() found the System area

    mutable os::Mutex mutex;
    os::LargeVector<Entry> entries;  // oldest first
    os::LargeVector<Rate> rates;
    uint64_t seq = 0;
    // Bumped by every change to `entries`; written_gen is the last one on
    // disk (or queued for it).
    uint64_t change_gen = 0;
    uint64_t written_gen = 0;
    // The timer is started and has not stopped itself yet.
    bool timer_armed = false;
    // A save job is queued on the worker.
    bool write_queued = false;
    int failures = 0;       // failed saves in a row
    uint64_t retry_at = 0;  // no save before this (backoff)
    bool gave_up = false;   // kMaxSaveAttempts failed; wait for a change
    bool closed = false;

    NotificationTimer timer;
    // The default timer (deps.make_timer empty), made on its first start();
    // owned here and freed in ~State, after close() stopped it.
    std::atomic<os::Timer*> os_timer{nullptr};
    os::Mutex os_timer_mutex;  // creating it

    os::Mutex cb_mutex;
    std::shared_ptr<const std::function<void(const NotificationChange&)>> listener;
    int in_callback = 0;

    // Serializes whole writes so an older snapshot never lands after a newer
    // one.
    os::Mutex write_mutex;

    // With mutex held: counts a failed save and sets the next retry.
    void save_failed(uint64_t now) {
        ++failures;
        if (failures >= kMaxSaveAttempts) {
            gave_up = true;
            WLOG_W(kTag, "notifications not saved after %d attempts; retrying on the next change",
                   failures);
            return;
        }
        retry_at = now + (static_cast<uint64_t>(kCoalesceMs) << failures);
    }

    static bool write_now(State& s);
    static void announce(State& s, const std::vector<NotificationChange>& changes);
    static void arm(const std::shared_ptr<State>& s);
    static void tick(const std::weak_ptr<State>& weak);
};

namespace {

std::string str_field(const Value& v, const char* key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

// The limits post() and post_system() enforce, for a notice read back from
// the file (which may be damaged or edited by hand).
bool loadable(const Notification& n) {
    if (n.source == kSystemSource) {
        return !n.code.empty() && n.id == n.code + ":" + n.arg && n.title.empty() &&
               n.body.empty() && (n.action.empty() || n.action == notice::kActionReleaseLock) &&
               n.code.size() <= 64 && n.arg.size() <= 64 && n.action_arg.size() <= 16;
    }
    return valid_notification_id(n.source) && valid_notification_id(n.id) && !n.title.empty() &&
           n.title.size() <= NotificationService::kTitleMax &&
           n.body.size() <= NotificationService::kBodyMax &&
           n.source_name.size() <= NotificationService::kTitleMax && n.code.empty() &&
           n.arg.empty() && n.action.empty() && n.action_arg.empty();
}

}  // namespace

// Writes the current list if it changed since the last write; false if the
// write failed. Runs on the storage worker.
bool NotificationService::State::write_now(State& s) {
    std::lock_guard<os::Mutex> write_lock(s.write_mutex);
    LStr body;
    uint64_t gen = 0;
    std::string path;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        if (s.change_gen == s.written_gen || s.path.empty()) return true;
        body = s.serialize();
        gen = s.change_gen;
        path = s.path;
    }
    auto r = s.deps.storage.write_atomic(
        path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    if (!r) {
        WLOG_W(kTag, "saving notifications failed: %s", r.error().message.c_str());
        return false;
    }
    std::lock_guard<os::Mutex> lock(s.mutex);
    s.written_gen = std::max(s.written_gen, gen);
    return true;
}

void NotificationService::State::announce(State& s,
                                          const std::vector<NotificationChange>& changes) {
    if (changes.empty()) return;
    std::shared_ptr<const std::function<void(const NotificationChange&)>> cb;
    {
        std::lock_guard<os::Mutex> lock(s.cb_mutex);
        cb = s.listener;
        if (!cb) return;
        ++s.in_callback;
    }
    for (const auto& c : changes) (*cb)(c);
    std::lock_guard<os::Mutex> lock(s.cb_mutex);
    --s.in_callback;
}

// Starts the coalescing timer for unsaved changes unless it runs already;
// a change after the saves gave up starts a new round of attempts. Called
// without the lock: start() may block on the timer's own bookkeeping.
void NotificationService::State::arm(const std::shared_ptr<State>& s) {
    {
        std::lock_guard<os::Mutex> lock(s->mutex);
        if (s->closed || !s->deps.submit_io || s->change_gen == s->written_gen) return;
        if (s->gave_up) {
            s->gave_up = false;
            s->failures = 0;
            s->retry_at = 0;
        }
        if (s->timer_armed) return;
        s->timer_armed = true;
    }
    if (!s->timer.start()) {
        std::lock_guard<os::Mutex> lock(s->mutex);
        s->timer_armed = false;
        WLOG_W(kTag, "no timer for saving notifications; trying again on the next change");
    }
}

void NotificationService::State::tick(const std::weak_ptr<State>& weak) {
    auto s = weak.lock();
    if (!s) return;
    const uint64_t now = s->deps.now_ms();
    {
        std::lock_guard<os::Mutex> lock(s->mutex);
        if (s->closed || s->gave_up || s->change_gen == s->written_gen) {
            // Under the lock, so an arm() that sees timer_armed false starts
            // a timer that has already stopped.
            s->timer_armed = false;
            s->timer.stop();
            return;
        }
        if (s->write_queued || now < s->retry_at) return;
        s->write_queued = true;
    }
    std::weak_ptr<State> job_weak = s;
    const bool queued = s->deps.submit_io([job_weak] {
        auto js = job_weak.lock();
        if (!js) return;
        const bool ok = write_now(*js);
        std::lock_guard<os::Mutex> lock(js->mutex);
        js->write_queued = false;
        if (ok) {
            js->failures = 0;
            js->retry_at = 0;
        } else {
            js->save_failed(js->deps.now_ms());
        }
    });
    if (!queued) {
        std::lock_guard<os::Mutex> lock(s->mutex);
        s->write_queued = false;
        if (s->failures == 0) WLOG_W(kTag, "storage queue full; saving notifications later");
        s->save_failed(now);
    }
}

NotificationService::NotificationService(NotificationDeps deps)
    : state_(std::make_shared<State>(std::move(deps))) {
    std::weak_ptr<State> weak = state_;
    auto tick = [weak] { State::tick(weak); };
    if (state_->deps.make_timer) {
        state_->timer = state_->deps.make_timer(std::move(tick));
        return;
    }
    // A periodic os::Timer, made when first needed: on ESP-IDF it takes
    // internal RAM, and a failed create is tried again on the next change.
    State* st = state_.get();
    state_->timer.start = [st, tick] {
        os::Timer* t = st->os_timer.load();
        if (t == nullptr) {
            std::lock_guard<os::Mutex> lock(st->os_timer_mutex);
            t = st->os_timer.load();
            if (t == nullptr) {
                auto made = os::Timer::create("wisp.notify", kCoalesceMs, /*periodic=*/true, tick);
                if (!made) return false;
                t = made.release();
                st->os_timer.store(t);
            }
        }
        t->start();
        return true;
    };
    state_->timer.stop = [st] {
        if (os::Timer* t = st->os_timer.load()) t->stop();
    };
}

NotificationService::~NotificationService() { close(); }

void NotificationService::close() {
    {
        std::lock_guard<os::Mutex> lock(state_->mutex);
        state_->closed = true;
        state_->timer_armed = false;
    }
    if (state_->timer.stop) state_->timer.stop();
}

Result<void> NotificationService::load() {
    auto root = state_->deps.storage.write_root(Area::System);
    if (!root) return Result<void>::err(root.error().code, root.error().message);
    const std::string path = root.value() + kFileName;
    {
        std::lock_guard<os::Mutex> lock(state_->mutex);
        state_->path = path;
    }

    auto data = state_->deps.storage.read_file_large(path);
    if (!data) {
        if (data.error().code != ErrorCode::NotFound) {
            WLOG_W(kTag, "reading notifications failed: %s", data.error().message.c_str());
        }
        return Result<void>::ok();
    }
    auto parsed = from_json(
        std::string_view(reinterpret_cast<const char*>(data.value().data()), data.value().size()));
    if (!parsed || parsed.value().type() != Value::Type::List) {
        WLOG_W(kTag, "notifications file is malformed; starting empty");
        return Result<void>::ok();
    }

    os::LargeVector<State::Entry> loaded;
    unsigned skipped = 0;
    for (const Value& v : parsed.value().as_list()) {
        if (v.type() != Value::Type::Map) continue;
        Notification n;
        n.source = str_field(v, "source");
        n.id = str_field(v, "id");
        if (n.source.empty() || n.id.empty()) continue;
        n.source_name = str_field(v, "source_name");
        n.title = str_field(v, "title");
        n.body = str_field(v, "body");
        n.code = str_field(v, "code");
        n.arg = str_field(v, "arg");
        n.action = str_field(v, "action");
        n.action_arg = str_field(v, "action_arg");
        n.sound = parse_notify_sound(str_field(v, "sound")).value_or(NotifySound::None);
        if (const Value* t = v.find("posted_unix"); t != nullptr && t->type() == Value::Type::Int) {
            n.posted_unix = t->as_int();
        }
        if (!loadable(n)) {
            ++skipped;
            continue;
        }
        loaded.push_back(State::to_entry(n));
    }
    // Only the newest kUnreadPerScene of each scene, as post() would leave.
    for (size_t i = loaded.size(); i-- > 0;) {
        if (same(loaded[i].source, kSystemSource)) continue;
        size_t newer = 0;
        for (size_t j = i + 1; j < loaded.size(); ++j) {
            if (loaded[j].source == loaded[i].source) ++newer;
        }
        if (newer >= kUnreadPerScene) {
            loaded.erase(loaded.begin() + static_cast<std::ptrdiff_t>(i));
            ++skipped;
        }
    }
    if (skipped > 0) WLOG_W(kTag, "skipped %u bad or surplus stored notifications", skipped);

    std::vector<NotificationChange> changes;
    {
        std::lock_guard<os::Mutex> lock(state_->mutex);
        auto& entries = state_->entries;
        // What was posted before load() wins over a stored notice with the
        // same source and id.
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
            if (state_->exists(plain(it->source), plain(it->id))) continue;
            entries.insert(entries.begin(), std::move(*it));
        }
        while (entries.size() > kStoredMax) state_->remove_at(0, DismissReason::Evict, changes);
    }
    State::announce(*state_, changes);
    State::arm(state_);
    return Result<void>::ok();
}

Result<std::string> NotificationService::post(Notification n, bool from_foreground) {
    if (n.title.empty() || n.title.size() > kTitleMax) {
        return Result<std::string>::err(ErrorCode::InvalidArgument, "title must be 1-32 bytes");
    }
    if (n.body.size() > kBodyMax) {
        return Result<std::string>::err(ErrorCode::InvalidArgument, "body must be 0-200 bytes");
    }
    if (!n.id.empty() && !valid_notification_id(n.id)) {
        return Result<std::string>::err(ErrorCode::InvalidArgument,
                                        "id must be 1-32 bytes of [a-z0-9_-]");
    }
    if (n.source.empty() || n.source == kSystemSource) {
        return Result<std::string>::err(ErrorCode::InvalidArgument, "bad source");
    }
    n.code.clear();
    n.arg.clear();
    n.action.clear();
    n.action_arg.clear();

    State& s = *state_;
    std::vector<NotificationChange> changes;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        const uint64_t now = s.deps.now_ms();
        if (!s.take_rate(n.source, now)) {
            return Result<std::string>::err(ErrorCode::RateLimited, "rate_limited");
        }
        if (n.id.empty()) {
            do {
                n.id = "n" + std::to_string(++s.seq);
            } while (s.exists(n.source, n.id));
        }
        n.posted_ms = now;
        n.posted_unix = s.deps.wall_s ? s.deps.wall_s() : 0;
        if (n.posted_unix < 0) n.posted_unix = 0;

        if (from_foreground) {
            // Banner only; an unread one with this id is replaced by it.
            auto it = s.find_entry(n.source, n.id);
            if (it != s.entries.end()) {
                s.entries.erase(it);
                ++s.change_gen;
            }
            s.publish_posted(n);
            changes.push_back({NotificationChange::Kind::Posted, n, false, DismissReason::Dismiss});
        } else {
            s.store(n, changes);
        }
    }
    State::announce(s, changes);
    State::arm(state_);
    return Result<std::string>::ok(std::move(n.id));
}

std::string NotificationService::post_system(std::string code, std::string arg, std::string action,
                                             std::string action_arg) {
    State& s = *state_;
    Notification n;
    n.source = kSystemSource;
    n.id = code + ":" + arg;
    n.code = std::move(code);
    n.arg = std::move(arg);
    n.action = std::move(action);
    n.action_arg = std::move(action_arg);
    std::vector<NotificationChange> changes;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        n.posted_ms = s.deps.now_ms();
        n.posted_unix = s.deps.wall_s ? std::max<int64_t>(s.deps.wall_s(), 0) : 0;
        s.store(n, changes);
    }
    State::announce(s, changes);
    State::arm(state_);
    return n.id;
}

bool NotificationService::cancel(std::string_view source, std::string_view id) {
    return dismiss(source, id, DismissReason::Cancel);
}

bool NotificationService::dismiss(std::string_view source, std::string_view id,
                                  DismissReason reason) {
    State& s = *state_;
    std::vector<NotificationChange> changes;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        auto it = s.find_entry(source, id);
        if (it == s.entries.end()) return false;
        s.remove_at(static_cast<size_t>(it - s.entries.begin()), reason, changes);
    }
    State::announce(s, changes);
    State::arm(state_);
    return true;
}

size_t NotificationService::dismiss_source(std::string_view source, DismissReason reason) {
    State& s = *state_;
    std::vector<NotificationChange> changes;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        for (size_t i = 0; i < s.entries.size();) {
            if (same(s.entries[i].source, source)) {
                s.remove_at(i, reason, changes);
            } else {
                ++i;
            }
        }
    }
    State::announce(s, changes);
    State::arm(state_);
    return changes.size();
}

size_t NotificationService::dismiss_code(std::string_view code, DismissReason reason) {
    State& s = *state_;
    std::vector<NotificationChange> changes;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        for (size_t i = 0; i < s.entries.size();) {
            if (same(s.entries[i].source, kSystemSource) && same(s.entries[i].code, code)) {
                s.remove_at(i, reason, changes);
            } else {
                ++i;
            }
        }
    }
    State::announce(s, changes);
    State::arm(state_);
    return changes.size();
}

void NotificationService::clear_all() {
    State& s = *state_;
    std::vector<NotificationChange> changes;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        while (!s.entries.empty()) s.remove_at(0, DismissReason::Dismiss, changes);
    }
    State::announce(s, changes);
    State::arm(state_);
}

std::optional<Notification> NotificationService::find(std::string_view source,
                                                      std::string_view id) const {
    std::lock_guard<os::Mutex> lock(state_->mutex);
    auto it = state_->find_entry(source, id);
    if (it == state_->entries.end()) return std::nullopt;
    return State::to_notification(*it);
}

std::vector<Notification> NotificationService::unread() const {
    std::lock_guard<os::Mutex> lock(state_->mutex);
    std::vector<Notification> out;
    out.reserve(state_->entries.size());
    for (auto it = state_->entries.rbegin(); it != state_->entries.rend(); ++it) {
        out.push_back(State::to_notification(*it));
    }
    return out;
}

size_t NotificationService::unread_count() const {
    std::lock_guard<os::Mutex> lock(state_->mutex);
    return state_->entries.size();
}

void NotificationService::set_listener(std::function<void(const NotificationChange&)> cb) {
    State& s = *state_;
    auto next =
        cb ? std::make_shared<const std::function<void(const NotificationChange&)>>(std::move(cb))
           : nullptr;
    {
        std::lock_guard<os::Mutex> lock(s.cb_mutex);
        s.listener = std::move(next);
    }
    // Wait out calls already running (they may hold the old listener).
    while (true) {
        {
            std::lock_guard<os::Mutex> lock(s.cb_mutex);
            if (s.in_callback == 0) return;
        }
        os::sleep_ms(1);
    }
}

bool NotificationService::flush(uint32_t timeout_ms) {
    {
        std::lock_guard<os::Mutex> lock(state_->mutex);
        if (state_->change_gen == state_->written_gen || state_->path.empty()) return true;
    }
    auto done = std::make_shared<os::Semaphore>(1, 0);
    std::weak_ptr<State> weak = state_;
    bool queued = state_->deps.submit_io && state_->deps.submit_io([weak, done] {
        if (auto s = weak.lock()) (void)State::write_now(*s);
        done->give();
    });
    if (!queued) return false;
    if (!done->take(timeout_ms)) return false;
    std::lock_guard<os::Mutex> lock(state_->mutex);
    return state_->change_gen == state_->written_gen;
}

}  // namespace wisp::core
