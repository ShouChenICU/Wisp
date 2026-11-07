// ReminderScheduler (mvp spec 5.2): limits, the one timer, the alarm
// notification and its actions, repeats, snoozes, missed reminders after a
// restart or a clock change, time zones, filters and the file.

#include "wisp/core/reminders.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"

using wisp::ErrorCode;
using wisp::Value;
using wisp::core::Event;
using wisp::core::kReminderSource;
using wisp::core::Notification;
using wisp::core::NotificationChange;
using wisp::core::NotificationService;
using wisp::core::NotifySound;
using wisp::core::parse_local_minute;
using wisp::core::ReminderScheduler;
using wisp::core::ReminderTimer;
using wisp::core::Repeat;
using wisp::core::RepeatRule;
using wisp::core::Todo;
using wisp::core::TodoFields;
using wisp::core::TodoFilter;

namespace notice = wisp::core::notice;
namespace fs = std::filesystem;

namespace {

// 2026-11-06T08:00 at UTC+8, a Friday.
constexpr int64_t kT = 1793923200;

std::string str(const wisp::os::LargeString& s) { return std::string(s.data(), s.size()); }

std::span<const uint8_t> span_of(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

class Reminders : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init(64 * 1024);
        root = wisp::test::temp_dir().string();
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", root, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        ASSERT_TRUE(storage->init());
        path = storage->write_root(wisp::core::Area::System).value() + "/todo/todos.json";
        bus.start();
        bus.subscribe("todo.changed", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mu);
            counts.push_back(e.payload.at("count").as_int());
            sources.push_back(e.source);
        });
        notes = std::make_unique<NotificationService>(
            wisp::core::NotificationDeps{.bus = bus, .storage = *storage});
        notes->set_listener([this](const NotificationChange& c) {
            std::lock_guard<std::mutex> lock(mu);
            if (c.kind == NotificationChange::Kind::Posted) {
                posted.push_back(c.n);
            } else {
                dismissed.push_back(c.n.id + ":" + wisp::core::to_string(c.reason));
            }
        });
    }

    void TearDown() override {
        sched.reset();
        notes->set_listener(nullptr);
        notes.reset();
        bus.stop();
        wisp::log::reset_for_test();
    }

    wisp::core::ReminderDeps deps() {
        return wisp::core::ReminderDeps{
            .storage = *storage,
            .bus = bus,
            .notes = notes.get(),
            .submit_io = submit_io,
            .wall_s = [this] { return now.load(); },
            .utc_offset_min = [this] { return offset.load(); },
            .missed_title = [](size_t n) { return "missed " + std::to_string(n); },
            .make_timer =
                [this](std::function<void()> f) {
                    fire = std::move(f);
                    return ReminderTimer{
                        .arm =
                            [this](uint32_t ms) {
                                std::lock_guard<std::mutex> lock(mu);
                                arms.push_back(ms);
                                armed = true;
                            },
                        .cancel =
                            [this] {
                                std::lock_guard<std::mutex> lock(mu);
                                ++cancels;
                                armed = false;
                            },
                    };
                },
        };
    }

    // A new scheduler over the same file, loaded and started.
    ReminderScheduler& open() {
        sched.reset();
        sched = std::make_unique<ReminderScheduler>(deps(), path);
        auto r = sched->load();
        EXPECT_TRUE(r) << r.error().message;
        sched->start();
        return *sched;
    }

    uint32_t add(const std::string& title, std::optional<int64_t> due, RepeatRule r = {}) {
        auto id = sched->add(title, due, r);
        EXPECT_TRUE(id) << id.error().message;
        return id ? id.value() : 0;
    }

    Todo get(uint32_t id) {
        for (const Todo& t : sched->list(TodoFilter::All)) {
            if (t.id == id) return t;
        }
        ADD_FAILURE() << "no todo " << id;
        return Todo{};
    }

    // The timer going off: its callback, which queues fire_due().
    void ring() {
        ASSERT_TRUE(fire);
        fire();
    }

    std::vector<Notification> posts() {
        std::lock_guard<std::mutex> lock(mu);
        return posted;
    }
    std::vector<int64_t> changed_counts() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mu);
        return counts;
    }
    uint32_t last_arm() {
        std::lock_guard<std::mutex> lock(mu);
        return arms.empty() ? 0 : arms.back();
    }
    bool log_mentions(const std::string& needle) {
        for (const auto& e : wisp::log::ring_entries()) {
            if (e.msg.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    std::string root, path;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<NotificationService> notes;
    std::unique_ptr<ReminderScheduler> sched;
    std::function<bool(std::function<void()>)> submit_io = [](std::function<void()> job) {
        job();
        return true;
    };
    std::atomic<int64_t> now{kT};
    std::atomic<int> offset{480};
    std::function<void()> fire;

    std::mutex mu;
    std::vector<uint32_t> arms;
    int cancels = 0;
    bool armed = false;
    std::vector<Notification> posted;
    std::vector<std::string> dismissed;
    std::vector<int64_t> counts;
    std::vector<std::string> sources;
};

}  // namespace

TEST_F(Reminders, AddWithLocalDueStoresUtc) {
    open();
    const auto due = parse_local_minute("2026-11-07T08:00", 480);
    ASSERT_EQ(due, std::optional<int64_t>(1794009600));
    const uint32_t id = add("带伞", due);
    EXPECT_EQ(id, 1u);
    Todo t = get(id);
    EXPECT_EQ(str(t.title), "带伞");
    EXPECT_EQ(t.due, std::optional<int64_t>(1794009600));
    EXPECT_EQ(t.created, kT);
    EXPECT_FALSE(t.done);
    EXPECT_EQ(t.repeat, RepeatRule{});
    EXPECT_EQ(wisp::core::format_local_minute(*t.due, 480), "2026-11-07T08:00");
}

TEST_F(Reminders, LimitsAndClock) {
    open();
    auto empty = sched->add("", std::nullopt, {});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(empty.error().message, "empty");
    auto too_long = sched->add(std::string(101, 'a'), std::nullopt, {});
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().message, "too_long");
    // 100 code points, not bytes.
    std::string han;
    for (int i = 0; i < 100; ++i) han += "汉";
    EXPECT_TRUE(sched->add(han, std::nullopt, {}));
    auto no_due = sched->add("x", std::nullopt, RepeatRule{Repeat::Daily, 0});
    ASSERT_FALSE(no_due);
    EXPECT_EQ(no_due.error().message, "repeat_needs_due");
    auto bad_weekly = sched->add("x", kT + 60, RepeatRule{Repeat::Weekly, 0});
    ASSERT_FALSE(bad_weekly);
    EXPECT_EQ(bad_weekly.error().code, ErrorCode::InvalidArgument);

    for (size_t i = 1; i < ReminderScheduler::kMaxTodos; ++i) {
        ASSERT_TRUE(sched->add("t" + std::to_string(i), std::nullopt, {})) << i;
    }
    auto full = sched->add("one too many", std::nullopt, {});
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(full.error().message, "todo_full");

    // Without a clock: undated todos only.
    ASSERT_TRUE(sched->clear());
    now = 0;
    auto dated = sched->add("x", kT + 60, {});
    ASSERT_FALSE(dated);
    EXPECT_EQ(dated.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(dated.error().message, "clock_not_set");
    const uint32_t id = add("undated", std::nullopt);
    TodoFields f;
    f.due = std::optional<int64_t>(kT + 60);
    auto upd = sched->update(id, f);
    ASSERT_FALSE(upd);
    EXPECT_EQ(upd.error().message, "clock_not_set");
    auto snooze = sched->snooze(id);
    ASSERT_FALSE(snooze);
    EXPECT_EQ(snooze.error().message, "clock_not_set");
    TodoFields title;
    title.title = "renamed";
    EXPECT_TRUE(sched->update(id, title));
    EXPECT_EQ(str(get(id).title), "renamed");
    EXPECT_EQ(sched->update(999, title).error().message, "not_found");
    EXPECT_EQ(sched->done(999).error().message, "not_found");
    EXPECT_EQ(sched->remove(999).error().message, "not_found");
}

TEST_F(Reminders, OnlyTheNearestTimerIsArmed) {
    open();
    EXPECT_FALSE(armed);
    add("later", kT + 120);
    EXPECT_EQ(last_arm(), 120000u);
    const uint32_t sooner = add("sooner", kT + 60);
    EXPECT_EQ(last_arm(), 60000u);
    add("undated", std::nullopt);
    EXPECT_EQ(last_arm(), 60000u);
    ASSERT_TRUE(sched->remove(sooner));
    EXPECT_EQ(last_arm(), 120000u);
    // Far ahead: at most an hour, then it looks again.
    ASSERT_TRUE(sched->clear());
    EXPECT_FALSE(armed);
    add("tomorrow", kT + 86400);
    EXPECT_EQ(last_arm(), ReminderScheduler::kMaxArmMs);
    now = kT + 3600;
    ring();
    EXPECT_TRUE(posts().empty());
    EXPECT_EQ(last_arm(), ReminderScheduler::kMaxArmMs);
}

TEST_F(Reminders, FiresAnAlarmNotification) {
    open();
    const uint32_t id = add("带伞", kT + 60);
    now = kT + 60;
    ring();
    auto n = notes->find(kReminderSource, "todo-1");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->title, "带伞");
    EXPECT_EQ(n->body, "");
    EXPECT_EQ(n->sound, NotifySound::Alarm);
    EXPECT_TRUE(n->wake);
    EXPECT_EQ(n->action, notice::kActionTodoDone);
    EXPECT_EQ(n->action_arg, "1");
    EXPECT_EQ(n->action2, notice::kActionTodoSnooze);
    EXPECT_EQ(n->action2_arg, "1");
    Todo t = get(id);
    EXPECT_FALSE(t.done);
    EXPECT_EQ(t.notified_at, std::optional<int64_t>(kT + 60));
    EXPECT_FALSE(armed);
    // Once only.
    ring();
    EXPECT_EQ(posts().size(), 1u);
    EXPECT_TRUE(log_mentions("fired id=1"));
    EXPECT_FALSE(log_mentions("带伞"));
}

TEST_F(Reminders, LongTitleCutForTheNotification) {
    open();
    std::string han;
    for (int i = 0; i < 100; ++i) han += "汉";
    add(han, kT + 60);
    now = kT + 60;
    ring();
    auto n = notes->find(kReminderSource, "todo-1");
    ASSERT_TRUE(n);
    EXPECT_LE(n->title.size(), NotificationService::kTitleMax);
    const std::string ellipsis = "…";
    ASSERT_GE(n->title.size(), ellipsis.size());
    EXPECT_EQ(n->title.substr(n->title.size() - ellipsis.size()), ellipsis);
    // Whole characters only: 29 bytes are 9 of them.
    EXPECT_EQ(n->title, han.substr(0, 27) + ellipsis);
    EXPECT_EQ(n->body, han);
}

TEST_F(Reminders, RepeatsAdvanceInsteadOfDone) {
    open();
    const uint32_t id = add("吃药", kT + 60, RepeatRule{Repeat::Daily, 0});
    now = kT + 60;
    ring();
    ASSERT_TRUE(notes->find(kReminderSource, "todo-1"));
    Todo t = get(id);
    EXPECT_FALSE(t.done);
    EXPECT_EQ(t.due, std::optional<int64_t>(kT + 60 + 86400));
    EXPECT_EQ(last_arm(), ReminderScheduler::kMaxArmMs);
    // done() ends the occurrence that rang: the next stays.
    ASSERT_TRUE(sched->done(id));
    t = get(id);
    EXPECT_FALSE(t.done);
    EXPECT_EQ(t.due, std::optional<int64_t>(kT + 60 + 86400));
    EXPECT_EQ(t.notified_at, std::nullopt);
    EXPECT_FALSE(notes->find(kReminderSource, "todo-1"));
    // Before it rings, done() skips the coming one.
    ASSERT_TRUE(sched->done(id));
    EXPECT_EQ(get(id).due, std::optional<int64_t>(kT + 60 + 2 * 86400));

    // A weekly one moves to its next weekday: Friday -> Monday.
    const uint32_t w = add("周会", kT + 120, RepeatRule{Repeat::Weekly, 0b10101});
    now = kT + 120;
    ring();
    EXPECT_EQ(get(w).due, std::optional<int64_t>(kT + 120 + 3 * 86400));
}

TEST_F(Reminders, SnoozeFiresTenMinutesLater) {
    open();
    const uint32_t id = add("出门", kT + 60);
    now = kT + 60;
    ring();
    ASSERT_EQ(posts().size(), 1u);
    ASSERT_TRUE(sched->snooze(id));
    EXPECT_EQ(get(id).snooze_until, std::optional<int64_t>(kT + 60 + 600));
    EXPECT_EQ(last_arm(), 600000u);
    now = kT + 60 + 599;
    ring();
    EXPECT_EQ(posts().size(), 1u);
    now = kT + 60 + 600;
    ring();
    ASSERT_EQ(posts().size(), 2u);
    EXPECT_EQ(posts().back().id, "todo-1");
    EXPECT_EQ(posts().back().sound, NotifySound::Alarm);
    Todo t = get(id);
    EXPECT_EQ(t.snooze_until, std::nullopt);
    EXPECT_EQ(t.notified_at, std::optional<int64_t>(kT + 660));
    ring();
    EXPECT_EQ(posts().size(), 2u);

    ASSERT_TRUE(sched->done(id));
    EXPECT_TRUE(get(id).done);
    EXPECT_EQ(sched->snooze(id).error().message, "already_done");
}

TEST_F(Reminders, DoneCancelsTheUnreadReminder) {
    open();
    const uint32_t a = add("a", kT + 60);
    const uint32_t b = add("b", kT + 60);
    now = kT + 60;
    ring();
    ASSERT_TRUE(notes->find(kReminderSource, "todo-1"));
    ASSERT_TRUE(notes->find(kReminderSource, "todo-2"));
    ASSERT_TRUE(sched->done(a));
    EXPECT_FALSE(notes->find(kReminderSource, "todo-1"));
    ASSERT_TRUE(sched->remove(b));
    EXPECT_FALSE(notes->find(kReminderSource, "todo-2"));
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(dismissed, (std::vector<std::string>{"todo-1:cancel", "todo-2:cancel"}));
}

TEST_F(Reminders, MissedAtStartMergedIntoOne) {
    open();
    add("a", kT + 10);
    add("b", kT + 20);
    add("c", kT + 30);
    add("later", kT + 7200);
    add("undated", std::nullopt);
    sched->stop();
    sched.reset();

    now = kT + 3600;
    open();
    std::vector<Notification> p = posts();
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].source, kReminderSource);
    EXPECT_EQ(p[0].id, "missed");
    EXPECT_EQ(p[0].title, "missed 3");
    EXPECT_EQ(p[0].body, "a、b、c");
    EXPECT_EQ(p[0].action, "");
    for (uint32_t id : {1u, 2u, 3u}) EXPECT_EQ(get(id).notified_at, std::optional<int64_t>(now));
    EXPECT_EQ(get(4).notified_at, std::nullopt);
    EXPECT_EQ(last_arm(), 3600000u);

    // Each is reported once.
    sched.reset();
    open();
    EXPECT_EQ(posts().size(), 1u);
}

TEST_F(Reminders, MissedBodyIsCut) {
    open();
    for (int i = 0; i < 30; ++i) add("待办事项" + std::to_string(i), kT + 10 + i);
    now = kT + 3600;
    ring();
    auto n = notes->find(kReminderSource, "missed");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->title, "missed 30");
    EXPECT_LE(n->body.size(), NotificationService::kBodyMax);
    EXPECT_EQ(n->body.substr(0, std::string("待办事项0、").size()), "待办事项0、");
}

TEST_F(Reminders, FirstClockSetReportsMissed) {
    open();
    add("a", kT + 10);
    add("b", kT + 20);
    sched.reset();

    // A restart without a clock: nothing to compare against yet.
    now = 0;
    open();
    EXPECT_TRUE(posts().empty());
    EXPECT_FALSE(armed);

    now = kT + 3600;
    bus.publish(Event{
        "system.time",
        "net",
        0,
        {{"state", Value("synced")}, {"server", Value("pool")}, {"time", Value(now.load())}}});
    bus.wait_idle(2000);
    std::vector<Notification> p = posts();
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].id, "missed");
    EXPECT_EQ(p[0].title, "missed 2");
}

TEST_F(Reminders, ClockJumpForwardReportsMissedOnce) {
    open();
    add("a", kT + 3600);
    add("b", kT + 7200);
    now = kT + 3 * 3600;
    ring();
    std::vector<Notification> p = posts();
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].id, "missed");
    EXPECT_EQ(p[0].title, "missed 2");
    EXPECT_EQ(p[0].body, "a、b");
    EXPECT_FALSE(notes->find(kReminderSource, "todo-1"));
    ring();
    bus.publish(Event{"system.time", "net", 0, {{"state", Value("synced")}}});
    bus.wait_idle(2000);
    EXPECT_EQ(posts().size(), 1u);
}

TEST_F(Reminders, ClockJumpBackDoesNotRefire) {
    open();
    add("a", kT + 60);
    now = kT + 60;
    ring();
    ASSERT_EQ(posts().size(), 1u);
    now = kT - 3600;
    bus.publish(Event{"system.time", "net", 0, {{"state", Value("synced")}}});
    bus.wait_idle(2000);
    EXPECT_FALSE(armed);
    now = kT + 60;
    ring();
    now = kT + 7200;
    ring();
    EXPECT_EQ(posts().size(), 1u);
}

TEST_F(Reminders, TimezoneFollowsTheSetting) {
    offset = -300;
    open();
    const auto due = parse_local_minute("2026-11-07T08:00", offset);
    ASSERT_EQ(due, std::optional<int64_t>(1794056400));
    const uint32_t id = add("x", due);
    EXPECT_EQ(wisp::core::format_local_minute(*get(id).due, offset), "2026-11-07T08:00");
    // due_today follows it too: kT is 2026-11-05T19:00 at UTC-5.
    EXPECT_TRUE(sched->list(TodoFilter::DueToday).empty());
    now = 1794056400 - 3600;
    EXPECT_EQ(sched->list(TodoFilter::DueToday).size(), 1u);
}

TEST_F(Reminders, ListFilters) {
    // 2026-11-07T07:00 local.
    now = 1794009600 - 3600;
    open();
    const uint32_t today = add("today", *parse_local_minute("2026-11-07T08:00", 480));
    const uint32_t late_today = add("tonight", *parse_local_minute("2026-11-07T23:59", 480));
    add("tomorrow", *parse_local_minute("2026-11-08T00:00", 480));
    add("undated", std::nullopt);
    const uint32_t finished = add("finished", *parse_local_minute("2026-11-07T09:00", 480));
    ASSERT_TRUE(sched->done(finished));

    auto ids = [&](TodoFilter f) {
        std::vector<uint32_t> out;
        for (const Todo& t : sched->list(f)) out.push_back(t.id);
        return out;
    };
    EXPECT_EQ(ids(TodoFilter::DueToday), (std::vector<uint32_t>{today, late_today}));
    EXPECT_EQ(ids(TodoFilter::Open), (std::vector<uint32_t>{1, 2, 3, 4}));
    EXPECT_EQ(ids(TodoFilter::Done), (std::vector<uint32_t>{finished}));
    EXPECT_EQ(ids(TodoFilter::All), (std::vector<uint32_t>{1, 2, 3, 4, 5}));
    now = 0;
    EXPECT_TRUE(ids(TodoFilter::DueToday).empty());
}

TEST_F(Reminders, PersistsEventsAndWorker) {
    wisp::core::StorageWorker worker;
    ASSERT_TRUE(worker.start());
    std::atomic<int> off_worker{0};
    submit_io = [&worker](std::function<void()> job) { return worker.submit(std::move(job)); };
    notes->set_listener([&](const NotificationChange& c) {
        if (!worker.on_worker()) ++off_worker;
        std::lock_guard<std::mutex> lock(mu);
        if (c.kind == NotificationChange::Kind::Posted) posted.push_back(c.n);
    });
    auto on_worker = [&](std::function<void()> fn) {
        ASSERT_TRUE(worker.run_sync(std::move(fn), wisp::os::kForever));
    };
    on_worker([&] { open(); });
    on_worker([&] {
        add("周会", kT + 60, RepeatRule{Repeat::Weekly, 0b10101});
        add("带伞", kT + 120);
        add("x", std::nullopt);
        ASSERT_TRUE(sched->remove(3));
        ASSERT_TRUE(sched->snooze(2));
    });
    now = kT + 60;
    ring();  // from this thread; the pass runs on the worker
    ASSERT_TRUE(worker.drain(2000));
    EXPECT_EQ(posts().size(), 1u);
    EXPECT_EQ(off_worker.load(), 0);

    const auto before = sched->list(TodoFilter::All);
    on_worker([&] { open(); });
    const auto after = sched->list(TodoFilter::All);
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < after.size(); ++i) {
        EXPECT_EQ(after[i].id, before[i].id);
        EXPECT_EQ(str(after[i].title), str(before[i].title));
        EXPECT_EQ(after[i].done, before[i].done);
        EXPECT_EQ(after[i].created, before[i].created);
        EXPECT_EQ(after[i].due, before[i].due);
        EXPECT_EQ(after[i].snooze_until, before[i].snooze_until);
        EXPECT_EQ(after[i].notified_at, before[i].notified_at);
        EXPECT_EQ(after[i].repeat, before[i].repeat);
    }
    // A removed id is never handed out again.
    on_worker([&] { EXPECT_EQ(add("next", std::nullopt), 4u); });

    std::string text;
    on_worker([&] {
        auto file = storage->read_file_large(path);
        ASSERT_TRUE(file);
        text.assign(reinterpret_cast<const char*>(file.value().data()), file.value().size());
    });
    auto doc = wisp::from_json(text);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc.value().find("version")->as_int(), 1);
    const Value& weekly = doc.value().find("todos")->as_list()[0];
    EXPECT_EQ(weekly.find("repeat")->as_string(), "weekly");
    EXPECT_EQ(wisp::to_json(*weekly.find("days")), "[1,3,5]");

    std::vector<int64_t> c = changed_counts();
    ASSERT_GE(c.size(), 6u);
    EXPECT_EQ((std::vector<int64_t>(c.begin(), c.begin() + 4)), (std::vector<int64_t>{1, 2, 3, 2}));
    {
        std::lock_guard<std::mutex> lock(mu);
        for (const std::string& s : sources) EXPECT_EQ(s, "todo");
    }
    on_worker([&] { sched.reset(); });
    notes->set_listener(nullptr);
    worker.stop();
}

TEST_F(Reminders, QuarantinesUnparsableFile) {
    ASSERT_TRUE(storage->write_atomic(path, span_of("{not json")));
    open();
    EXPECT_TRUE(sched->list(TodoFilter::All).empty());
    EXPECT_EQ(add("fresh", std::nullopt), 1u);
    auto bad = storage->read_file_large(path + ".bad");
    ASSERT_TRUE(bad);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bad.value().data()), bad.value().size()),
              "{not json");

    ASSERT_TRUE(storage->write_atomic(path, span_of(R"({"version":2,"todos":[]})")));
    open();
    EXPECT_TRUE(sched->list(TodoFilter::All).empty());
    EXPECT_TRUE(fs::exists(fs::path(path + ".bad")));
}

TEST_F(Reminders, ReadErrorBlocksWritesUntilClear) {
    fs::create_directories(fs::path(path));
    sched = std::make_unique<ReminderScheduler>(deps(), path);
    EXPECT_FALSE(sched->load());
    sched->start();
    auto r = sched->add("x", std::nullopt, {});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::StorageUnavailable);
    EXPECT_EQ(r.error().message, "todo_unavailable");
    EXPECT_TRUE(fs::is_directory(fs::path(path)));
    fs::remove_all(fs::path(path));
    ASSERT_TRUE(sched->clear());
    EXPECT_TRUE(sched->add("x", std::nullopt, {}));
}

TEST_F(Reminders, DropsInvalidRecordsOnLoad) {
    std::string han;
    for (int i = 0; i < 101; ++i) han += "汉";
    const std::string body =
        R"({"version":1,"next_id":3,"todos":[)"
        R"({"id":1,"title":"ok","done":false,"created":1,"due":)" +
        std::to_string(kT + 60) +
        R"(,"repeat":"daily"},)"
        R"({"id":0,"title":"zero","done":false,"created":1,"repeat":"none"},)"
        R"({"id":1,"title":"dup","done":false,"created":1,"repeat":"none"},)"
        R"({"id":5,"title":"","done":false,"created":1,"repeat":"none"},)"
        R"({"id":6,"title":")" +
        han +
        R"(","done":false,"created":1,"repeat":"none"},)"
        R"({"id":7,"title":"bad repeat","done":false,"created":1,"repeat":"hourly"},)"
        R"({"id":8,"title":"weekly no days","done":false,"created":1,"due":1,"repeat":"weekly"},)"
        R"({"id":9,"title":"repeat no due","done":false,"created":1,"repeat":"daily"},)"
        R"({"id":10,"title":"kept","done":true,"created":1,"repeat":"none"}]})";
    ASSERT_TRUE(storage->write_atomic(path, span_of(body)));
    open();
    auto all = sched->list(TodoFilter::All);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].id, 1u);
    EXPECT_EQ(all[0].repeat.kind, Repeat::Daily);
    EXPECT_EQ(all[1].id, 10u);
    EXPECT_TRUE(all[1].done);
    EXPECT_EQ(add("next", std::nullopt), 11u);
    EXPECT_TRUE(log_mentions("dropped 7"));
}
