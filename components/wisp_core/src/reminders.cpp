#include "wisp/core/reminders.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "reminder";
constexpr int kDefaultOffsetMin = 480;
// How long stop() waits for a pass already queued on the worker.
constexpr uint32_t kStopWaitMs = 5000;
constexpr const char* kEllipsis = "…";
constexpr const char* kMissedId = "missed";
constexpr const char* kTitleSeparator = "、";

using LStr = os::LargeString;

std::span<const uint8_t> bytes_of(const LStr& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

size_t codepoint_count(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

// `s` cut to at most `max` bytes on a UTF-8 character boundary, ending in
// "…" when cut.
std::string cut_utf8(std::string_view s, size_t max) {
    if (s.size() <= max) return std::string(s);
    const size_t ell = std::char_traits<char>::length(kEllipsis);
    size_t end = max > ell ? max - ell : 0;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return std::string(s.substr(0, end)) + kEllipsis;
}

std::string notice_id(uint32_t id) { return "todo-" + std::to_string(id); }

std::string plain(const LStr& s) { return std::string(s.data(), s.size()); }

// The time a todo rings next, or nullopt if it cannot ring: done, undated,
// or a one-shot that rang already and was not snoozed.
std::optional<int64_t> effective(const Todo& t) {
    if (t.done) return std::nullopt;
    if (t.snooze_until) return t.snooze_until;
    if (!t.due) return std::nullopt;
    if (t.repeat.kind == Repeat::None && t.notified_at) return std::nullopt;
    return t.due;
}

bool valid_rule(const RepeatRule& r) { return r.kind != Repeat::Weekly || (r.days & 0x7F) != 0; }

const Value* typed(const Value& v, const char* key, Value::Type type) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == type ? f : nullptr;
}

std::optional<int64_t> opt_int(const Value& v, const char* key) {
    const Value* f = typed(v, key, Value::Type::Int);
    return f != nullptr ? std::optional<int64_t>(f->as_int()) : std::nullopt;
}

}  // namespace

ReminderScheduler::ReminderScheduler(ReminderDeps deps, std::string path)
    : deps_(std::move(deps)),
      path_(std::move(path)),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    std::function<bool(std::function<void()>)> submit = deps_.submit_io;
    // The callback only queues the pass: firing writes the file, which
    // only the storage worker may.
    auto fire = [this, alive, submit] {
        if (!alive->load() || !submit) return;
        if (!submit([this, alive] {
                if (alive->load()) fire_due();
            })) {
            WLOG_W(kTag, "storage queue full; reminders checked on the next timer");
        }
    };
    if (deps_.make_timer) {
        timer_ = deps_.make_timer(std::move(fire));
        return;
    }
    // os::Timer has a fixed period, so each arm() makes a new one-shot.
    struct Holder {
        os::Mutex mu;
        std::unique_ptr<os::Timer> timer;
    };
    auto holder = std::make_shared<Holder>();
    timer_.arm = [holder, fire](uint32_t ms) {
        std::lock_guard<os::Mutex> lock(holder->mu);
        holder->timer.reset();
        holder->timer = os::Timer::create("wisp.remind", std::max<uint32_t>(ms, 1), false, fire);
        if (holder->timer) {
            holder->timer->start();
        } else {
            WLOG_W(kTag, "no timer for reminders; checked on the next change");
        }
    };
    timer_.cancel = [holder] {
        std::lock_guard<os::Mutex> lock(holder->mu);
        holder->timer.reset();
    };
}

ReminderScheduler::~ReminderScheduler() { stop(); }

int64_t ReminderScheduler::now_s() const { return deps_.wall_s ? deps_.wall_s() : 0; }

int ReminderScheduler::utc_offset_min() const {
    return deps_.utc_offset_min ? deps_.utc_offset_min() : kDefaultOffsetMin;
}

void ReminderScheduler::start() {
    if (started_) return;
    started_ = true;
    alive_->store(true);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    std::function<bool(std::function<void()>)> submit = deps_.submit_io;
    auto pass = [this, alive, submit] {
        if (!alive->load() || !submit) return;
        if (!submit([this, alive] {
                if (alive->load()) fire_due();
            })) {
            WLOG_W(kTag, "storage queue full; reminders checked on the next timer");
        }
    };
    // A set clock, or one that jumped: whatever came due meanwhile is
    // reported, and the timer follows the new time.
    time_sub_ = deps_.bus.subscribe("system.time", [pass](const Event& e) {
        auto it = e.payload.find("state");
        if (it != e.payload.end() && it->second.type() == Value::Type::String &&
            it->second.as_string() == "synced") {
            pass();
        }
    });
    pass();
}

void ReminderScheduler::stop() {
    if (!started_) {
        if (timer_.cancel) timer_.cancel();
        return;
    }
    started_ = false;
    deps_.bus.unsubscribe(time_sub_);
    time_sub_ = 0;
    alive_->store(false);
    if (timer_.cancel) timer_.cancel();
    // A pass queued before this may still be running on the worker; the
    // queue runs in order, so once this job has run none is.
    if (deps_.submit_io && !os::on_flash_thread()) {
        auto done = std::make_shared<os::Semaphore>(1, 0);
        if (deps_.submit_io([done] { done->give(); }) && !done->take(kStopWaitMs)) {
            WLOG_W(kTag, "a reminder pass is still running at stop");
        }
    }
}

Result<void> ReminderScheduler::quarantine_locked() {
    const std::string bad_path = path_ + ".bad";
    // StorageManager::rename() refuses an existing target on every file
    // system (FAT cannot replace one), so an older .bad goes first.
    Result<void> cleared = deps_.storage.remove_tree(bad_path);
    Result<void> renamed = cleared ? deps_.storage.rename(path_, bad_path) : cleared;
    if (!renamed) {
        WLOG_W(kTag, "todo file is malformed and could not be moved aside: %s",
               renamed.error().message.c_str());
        load_failed_ = true;
        return Result<void>::err(renamed.error().code, renamed.error().message);
    }
    WLOG_W(kTag, "todo file was malformed; moved aside, starting empty");
    load_failed_ = false;
    return Result<void>::ok();
}

Result<void> ReminderScheduler::load() {
    std::lock_guard<os::Mutex> lock(mutex_);
    todos_.clear();
    next_id_ = 1;

    Result<os::LargeBuffer> read = deps_.storage.read_file_large(path_);
    if (!read) {
        if (read.error().code == ErrorCode::NotFound) {
            load_failed_ = false;
            return Result<void>::ok();
        }
        // The contents are unknown, so nothing may replace them.
        WLOG_W(kTag, "reading todos failed: %s", read.error().message.c_str());
        load_failed_ = true;
        return Result<void>::err(read.error().code, read.error().message);
    }

    const std::string_view text(reinterpret_cast<const char*>(read.value().data()),
                                read.value().size());
    Result<Value> doc = from_json(text);
    const Value* list = doc ? typed(doc.value(), "todos", Value::Type::List) : nullptr;
    const Value* version = doc ? typed(doc.value(), "version", Value::Type::Int) : nullptr;
    if (list == nullptr || version == nullptr || version->as_int() != 1) {
        return quarantine_locked();
    }

    // Every record is checked as add() would check it: a hand-edited file
    // cannot bring in what the API refuses.
    uint32_t max_id = 0;
    size_t dropped = 0;
    for (const Value& item : list->as_list()) {
        const Value* id = typed(item, "id", Value::Type::Int);
        const Value* title = typed(item, "title", Value::Type::String);
        const Value* done = typed(item, "done", Value::Type::Bool);
        const Value* created = typed(item, "created", Value::Type::Int);
        const Value* repeat = typed(item, "repeat", Value::Type::String);
        if (id == nullptr || title == nullptr || done == nullptr || created == nullptr ||
            repeat == nullptr || id->as_int() < 0 || id->as_int() > UINT32_MAX) {
            ++dropped;
            continue;
        }
        const auto todo_id = static_cast<uint32_t>(id->as_int());
        // Even a dropped record's id was handed out once.
        max_id = std::max(max_id, todo_id);

        Todo t;
        t.id = todo_id;
        t.done = done->as_bool();
        t.created = created->as_int();
        t.due = opt_int(item, "due");
        t.snooze_until = opt_int(item, "snooze_until");
        t.notified_at = opt_int(item, "notified_at");
        std::optional<RepeatRule> rule;
        if (repeat->as_string() == "weekly") {
            const Value* days = item.find("days");
            rule = days != nullptr ? parse_repeat(Value(Map{{"weekly", *days}})) : std::nullopt;
        } else {
            rule = parse_repeat(*repeat);
        }
        const std::string& tt = title->as_string();
        const bool duplicate = std::any_of(todos_.begin(), todos_.end(),
                                           [todo_id](const Todo& o) { return o.id == todo_id; });
        if (todo_id == 0 || duplicate || tt.empty() || codepoint_count(tt) > kTitleMax || !rule ||
            (rule->kind != Repeat::None && !t.due) || todos_.size() >= kMaxTodos) {
            ++dropped;
            continue;
        }
        t.repeat = *rule;
        t.title = LStr(tt.data(), tt.size());
        todos_.push_back(std::move(t));
    }
    std::sort(todos_.begin(), todos_.end(),
              [](const Todo& a, const Todo& b) { return a.id < b.id; });
    if (dropped > 0) WLOG_W(kTag, "todos: dropped %zu invalid record(s) on load", dropped);

    const std::optional<int64_t> next = opt_int(doc.value(), "next_id");
    next_id_ = next && *next > 0 && *next <= UINT32_MAX ? static_cast<uint32_t>(*next) : 1;
    if (next_id_ <= max_id) next_id_ = max_id + 1;
    load_failed_ = false;
    return Result<void>::ok();
}

os::LargeVector<Todo> ReminderScheduler::list(TodoFilter f) const {
    const int64_t now = now_s();
    const int off = utc_offset_min();
    std::lock_guard<os::Mutex> lock(mutex_);
    os::LargeVector<Todo> out;
    for (const Todo& t : todos_) {
        bool keep = false;
        switch (f) {
            case TodoFilter::Open:
                keep = !t.done;
                break;
            case TodoFilter::Done:
                keep = t.done;
                break;
            case TodoFilter::All:
                keep = true;
                break;
            case TodoFilter::DueToday:
                keep = !t.done && t.due && clock_set(now) &&
                       local_day(*t.due, off) == local_day(now, off);
                break;
        }
        if (keep) out.push_back(t);
    }
    return out;
}

os::LargeString ReminderScheduler::serialize_locked() const {
    LStr out;
    out += "{\"version\":1,\"next_id\":";
    out += std::to_string(next_id_);
    out += ",\"todos\":[";
    bool first = true;
    for (const Todo& t : todos_) {
        if (!first) out += ',';
        first = false;
        Map m{{"id", Value(static_cast<int64_t>(t.id))},
              {"title", Value(plain(t.title))},
              {"done", Value(t.done)},
              {"created", Value(t.created)}};
        if (t.due) m["due"] = Value(*t.due);
        if (t.snooze_until) m["snooze_until"] = Value(*t.snooze_until);
        if (t.notified_at) m["notified_at"] = Value(*t.notified_at);
        switch (t.repeat.kind) {
            case Repeat::None:
                m["repeat"] = Value("none");
                break;
            case Repeat::Daily:
                m["repeat"] = Value("daily");
                break;
            case Repeat::Weekly:
                m["repeat"] = Value("weekly");
                m["days"] = *to_value(t.repeat).find("weekly");
                break;
        }
        const std::string one = to_json(Value(std::move(m)));
        out.append(one.data(), one.size());
    }
    out += "]}";
    return out;
}

Result<void> ReminderScheduler::persist_locked() {
    const LStr body = serialize_locked();
    return deps_.storage.write_atomic(path_, bytes_of(body));
}

void ReminderScheduler::publish_count(size_t count) {
    deps_.bus.publish(Event{kTodoChangedTopic, "todo", 0,
                            Properties{{"count", Value(static_cast<int64_t>(count))}}});
}

void ReminderScheduler::rearm_locked(int64_t now) {
    if (!timer_.arm || !timer_.cancel) return;
    std::optional<int64_t> soonest;
    if (clock_set(now)) {
        for (const Todo& t : todos_) {
            if (auto e = effective(t); e && (!soonest || *e < *soonest)) soonest = e;
        }
    }
    if (!soonest) {
        timer_.cancel();
        return;
    }
    const int64_t delta_s = std::max<int64_t>(*soonest - now, 0);
    const int64_t ms = std::min<int64_t>(delta_s * 1000, kMaxArmMs);
    timer_.arm(static_cast<uint32_t>(ms));
}

void ReminderScheduler::cancel_notice(uint32_t id) {
    if (deps_.notes != nullptr) deps_.notes->cancel(kReminderSource, notice_id(id));
}

Result<uint32_t> ReminderScheduler::add(std::string_view title, std::optional<int64_t> due,
                                        RepeatRule r) {
    const int64_t now = now_s();
    size_t count = 0;
    uint32_t id = 0;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (load_failed_) {
            return Result<uint32_t>::err(ErrorCode::StorageUnavailable, "todo_unavailable");
        }
        if (title.empty()) return Result<uint32_t>::err(ErrorCode::InvalidArgument, "empty");
        if (codepoint_count(title) > kTitleMax) {
            return Result<uint32_t>::err(ErrorCode::InvalidArgument, "too_long");
        }
        if (!valid_rule(r)) return Result<uint32_t>::err(ErrorCode::InvalidArgument, "bad_repeat");
        if (r.kind != Repeat::None && !due) {
            return Result<uint32_t>::err(ErrorCode::InvalidArgument, "repeat_needs_due");
        }
        if (due && !clock_set(now)) {
            return Result<uint32_t>::err(ErrorCode::Unsupported, "clock_not_set");
        }
        if (todos_.size() >= kMaxTodos) {
            return Result<uint32_t>::err(ErrorCode::QuotaExceeded, "todo_full");
        }
        Todo t;
        t.id = next_id_++;  // never reused, even if the write fails
        t.title = LStr(title.data(), title.size());
        t.created = clock_set(now) ? now : 0;
        t.due = due;
        t.repeat = r;
        todos_.push_back(std::move(t));
        if (Result<void> w = persist_locked(); !w) {
            todos_.pop_back();
            return Result<uint32_t>::err(w.error().code, w.error().message);
        }
        id = todos_.back().id;
        count = todos_.size();
        rearm_locked(now);
    }
    publish_count(count);
    return Result<uint32_t>::ok(id);
}

Result<void> ReminderScheduler::update(uint32_t id, const TodoFields& f) {
    const int64_t now = now_s();
    size_t count = 0;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (load_failed_) {
            return Result<void>::err(ErrorCode::StorageUnavailable, "todo_unavailable");
        }
        auto it =
            std::find_if(todos_.begin(), todos_.end(), [id](const Todo& t) { return t.id == id; });
        if (it == todos_.end()) return Result<void>::err(ErrorCode::NotFound, "not_found");
        Todo next = *it;
        if (f.title) {
            if (f.title->empty()) return Result<void>::err(ErrorCode::InvalidArgument, "empty");
            if (codepoint_count(*f.title) > kTitleMax) {
                return Result<void>::err(ErrorCode::InvalidArgument, "too_long");
            }
            next.title = LStr(f.title->data(), f.title->size());
        }
        if (f.due) {
            if (*f.due && !clock_set(now)) {
                return Result<void>::err(ErrorCode::Unsupported, "clock_not_set");
            }
            next.due = *f.due;
        }
        if (f.repeat) {
            if (!valid_rule(*f.repeat)) {
                return Result<void>::err(ErrorCode::InvalidArgument, "bad_repeat");
            }
            next.repeat = *f.repeat;
        }
        if (next.repeat.kind != Repeat::None && !next.due) {
            return Result<void>::err(ErrorCode::InvalidArgument, "repeat_needs_due");
        }
        if (f.due || f.repeat) {
            // A new schedule rings afresh.
            next.snooze_until.reset();
            next.notified_at.reset();
        }
        const Todo old = std::move(*it);
        *it = std::move(next);
        if (Result<void> w = persist_locked(); !w) {
            *it = old;
            return w;
        }
        count = todos_.size();
        rearm_locked(now);
    }
    publish_count(count);
    return Result<void>::ok();
}

Result<void> ReminderScheduler::done(uint32_t id) {
    const int64_t now = now_s();
    const int off = utc_offset_min();
    size_t count = 0;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (load_failed_) {
            return Result<void>::err(ErrorCode::StorageUnavailable, "todo_unavailable");
        }
        auto it =
            std::find_if(todos_.begin(), todos_.end(), [id](const Todo& t) { return t.id == id; });
        if (it == todos_.end()) return Result<void>::err(ErrorCode::NotFound, "not_found");
        const Todo old = *it;
        if (it->repeat.kind == Repeat::None || !it->due) {
            it->done = true;
        } else if (it->notified_at) {
            // The occurrence that rang: its due time has moved on already.
            it->notified_at.reset();
        } else {
            // Done ahead of time: the coming occurrence is skipped.
            it->due = next_occurrence(*it->due, it->repeat, std::max(*it->due, now), off);
        }
        it->snooze_until.reset();
        if (Result<void> w = persist_locked(); !w) {
            *it = old;
            return w;
        }
        count = todos_.size();
        rearm_locked(now);
    }
    cancel_notice(id);
    publish_count(count);
    return Result<void>::ok();
}

Result<void> ReminderScheduler::snooze(uint32_t id) {
    const int64_t now = now_s();
    size_t count = 0;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (load_failed_) {
            return Result<void>::err(ErrorCode::StorageUnavailable, "todo_unavailable");
        }
        auto it =
            std::find_if(todos_.begin(), todos_.end(), [id](const Todo& t) { return t.id == id; });
        if (it == todos_.end()) return Result<void>::err(ErrorCode::NotFound, "not_found");
        if (it->done) return Result<void>::err(ErrorCode::InvalidArgument, "already_done");
        if (!clock_set(now)) return Result<void>::err(ErrorCode::Unsupported, "clock_not_set");
        const std::optional<int64_t> old = it->snooze_until;
        it->snooze_until = now + kSnoozeS;
        if (Result<void> w = persist_locked(); !w) {
            it->snooze_until = old;
            return w;
        }
        count = todos_.size();
        rearm_locked(now);
    }
    publish_count(count);
    return Result<void>::ok();
}

Result<void> ReminderScheduler::remove(uint32_t id) {
    const int64_t now = now_s();
    size_t count = 0;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (load_failed_) {
            return Result<void>::err(ErrorCode::StorageUnavailable, "todo_unavailable");
        }
        auto it =
            std::find_if(todos_.begin(), todos_.end(), [id](const Todo& t) { return t.id == id; });
        if (it == todos_.end()) return Result<void>::err(ErrorCode::NotFound, "not_found");
        const auto index = it - todos_.begin();
        Todo removed = std::move(*it);
        todos_.erase(it);
        if (Result<void> w = persist_locked(); !w) {
            todos_.insert(todos_.begin() + index, std::move(removed));
            return w;
        }
        count = todos_.size();
        rearm_locked(now);
    }
    cancel_notice(id);
    publish_count(count);
    return Result<void>::ok();
}

Result<void> ReminderScheduler::clear() {
    const int64_t now = now_s();
    std::vector<uint32_t> ids;
    {
        // Allowed after a failed load: it is the explicit way to start over.
        std::lock_guard<os::Mutex> lock(mutex_);
        os::LargeVector<Todo> old = std::move(todos_);
        todos_.clear();
        if (Result<void> w = persist_locked(); !w) {
            todos_ = std::move(old);
            return w;
        }
        for (const Todo& t : old) ids.push_back(t.id);
        load_failed_ = false;
        rearm_locked(now);
    }
    for (uint32_t id : ids) cancel_notice(id);
    if (deps_.notes != nullptr) deps_.notes->cancel(kReminderSource, kMissedId);
    publish_count(0);
    return Result<void>::ok();
}

void ReminderScheduler::fire_due() {
    const int64_t now = now_s();
    const int off = utc_offset_min();
    // Posted once the lock is let go: the listener may run for a while.
    std::vector<Notification> posts;
    size_t count = 0;
    bool changed = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (!clock_set(now)) {
            rearm_locked(now);
            return;
        }
        std::vector<std::string> missed;
        for (Todo& t : todos_) {
            const std::optional<int64_t> e = effective(t);
            if (!e || *e > now) continue;
            const std::string title = plain(t.title);
            if (now - *e > kLateS) {
                missed.push_back(title);
            } else {
                Notification n;
                n.source = kReminderSource;
                n.id = notice_id(t.id);
                n.title = cut_utf8(title, NotificationService::kTitleMax);
                if (n.title.size() != title.size()) n.body = title;
                n.sound = NotifySound::Alarm;
                n.wake = true;
                n.action = notice::kActionTodoDone;
                n.action_arg = std::to_string(t.id);
                n.action2 = notice::kActionTodoSnooze;
                n.action2_arg = n.action_arg;
                posts.push_back(std::move(n));
                WLOG_I(kTag, "fired id=%u", static_cast<unsigned>(t.id));
            }
            t.notified_at = now;
            t.snooze_until.reset();
            if (t.repeat.kind != Repeat::None && t.due) {
                t.due = next_occurrence(*t.due, t.repeat, now, off);
            }
            changed = true;
        }
        if (!missed.empty()) {
            std::string body;
            for (const std::string& m : missed) {
                if (!body.empty()) body += kTitleSeparator;
                body += m;
                if (body.size() > NotificationService::kBodyMax) break;
            }
            Notification n;
            n.source = kReminderSource;
            n.id = kMissedId;
            n.title = cut_utf8(deps_.missed_title ? deps_.missed_title(missed.size())
                                                  : std::to_string(missed.size()),
                               NotificationService::kTitleMax);
            n.body = cut_utf8(body, NotificationService::kBodyMax);
            n.sound = NotifySound::Tone;
            posts.push_back(std::move(n));
            WLOG_I(kTag, "missed %zu", missed.size());
        }
        if (changed) {
            // What rang stays rung even if the write fails: the next change
            // writes it again, and ringing twice would be worse.
            if (Result<void> w = persist_locked(); !w) {
                WLOG_W(kTag, "saving todos after a reminder failed: %s", w.error().message.c_str());
            }
        }
        count = todos_.size();
        rearm_locked(now);
    }
    if (deps_.notes != nullptr) {
        for (Notification& n : posts) {
            if (auto r = deps_.notes->post(std::move(n), false); !r) {
                WLOG_W(kTag, "reminder not posted: %s", r.error().message.c_str());
            }
        }
    }
    if (changed) publish_count(count);
}

}  // namespace wisp::core
