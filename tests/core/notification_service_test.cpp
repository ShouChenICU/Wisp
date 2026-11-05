#include "wisp/core/notification_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::core::DismissReason;
using wisp::core::Event;
using wisp::core::Notification;
using wisp::core::NotificationChange;
using wisp::core::NotificationDeps;
using wisp::core::NotificationService;
using wisp::core::NotifySound;

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMinute = 60 * 1000;

// A started bus that records every notification.* event, storage on a
// temp dir, file jobs run inline and timers the test fires by hand.
struct Harness {
    Harness() : in(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        bus.start();
        bus.subscribe("notification.", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });
    }
    ~Harness() { bus.stop(); }

    NotificationDeps deps() {
        NotificationDeps d{
            .bus = bus,
            .storage = *storage,
            .submit_io =
                [this](std::function<void()> job) {
                    ++submits;
                    job();
                    return true;
                },
            .schedule = [this](
                            uint32_t ms,
                            std::function<void()> cb) { timers.emplace_back(ms, std::move(cb)); },
            .now_ms = [this] { return now.load(); },
            .wall_s = [] { return int64_t{1767225600}; },
        };
        return d;
    }

    std::unique_ptr<NotificationService> make() {
        auto s = std::make_unique<NotificationService>(deps());
        auto r = s->load();
        EXPECT_TRUE(r) << r.error().message;
        return s;
    }

    void fire_timers() {
        auto due = std::move(timers);
        timers.clear();
        for (auto& t : due) t.second();
    }

    std::vector<Event> drained() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }

    std::vector<Event> of_topic(const char* topic) {
        std::vector<Event> out;
        for (auto& e : drained()) {
            if (e.topic == topic) out.push_back(e);
        }
        return out;
    }

    std::string in;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::vector<std::pair<uint32_t, std::function<void()>>> timers;
    std::atomic<uint64_t> now{1000};
    int submits = 0;
    std::mutex mutex;
    std::vector<Event> events;
};

std::string field(const Event& e, const char* key) {
    auto it = e.payload.find(key);
    return it == e.payload.end() ? std::string("<missing>") : it->second.as_string();
}

Notification scene_note(std::string source, std::string title, std::string id = {}) {
    Notification n;
    n.source = std::move(source);
    n.source_name = "Pomodoro";
    n.title = std::move(title);
    n.body = "the body text";
    n.id = std::move(id);
    return n;
}

}  // namespace

TEST(Notifications, ParseAndNames) {
    EXPECT_EQ(wisp::core::parse_notify_sound("none"), NotifySound::None);
    EXPECT_EQ(wisp::core::parse_notify_sound("tone"), NotifySound::Tone);
    EXPECT_EQ(wisp::core::parse_notify_sound("alarm"), NotifySound::Alarm);
    EXPECT_FALSE(wisp::core::parse_notify_sound("loud"));
    EXPECT_STREQ(wisp::core::to_string(DismissReason::Tap), "tap");
    EXPECT_STREQ(wisp::core::to_string(DismissReason::Dismiss), "dismiss");
    EXPECT_STREQ(wisp::core::to_string(DismissReason::Cancel), "cancel");
    EXPECT_STREQ(wisp::core::to_string(DismissReason::Evict), "evict");
    EXPECT_TRUE(wisp::core::valid_notification_id("pomo_1-a"));
    EXPECT_FALSE(wisp::core::valid_notification_id(""));
    EXPECT_FALSE(wisp::core::valid_notification_id("A!"));
    EXPECT_FALSE(wisp::core::valid_notification_id(std::string(33, 'a')));
}

TEST(Notifications, PostReturnsIdAndPublishesWithoutBody) {
    Harness h;
    auto s = h.make();
    auto id = s->post(scene_note("pomodoro", "Time is up", "pomo"), false);
    ASSERT_TRUE(id) << id.error().message;
    EXPECT_EQ(id.value(), "pomo");

    auto posted = h.of_topic("notification.posted");
    ASSERT_EQ(posted.size(), 1u);
    EXPECT_EQ(posted[0].source, "notifications");
    EXPECT_EQ(field(posted[0], "id"), "pomo");
    EXPECT_EQ(field(posted[0], "source"), "pomodoro");
    EXPECT_EQ(field(posted[0], "title"), "Time is up");
    EXPECT_EQ(posted[0].payload.find("body"), posted[0].payload.end());
    EXPECT_EQ(posted[0].payload.size(), 3u);

    auto found = s->find("pomodoro", "pomo");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->body, "the body text");
    EXPECT_EQ(found->posted_ms, 1000u);
    EXPECT_EQ(found->posted_unix, 1767225600);
}

TEST(Notifications, GeneratedIdsAreUnique) {
    Harness h;
    auto s = h.make();
    auto a = s->post(scene_note("pomodoro", "a"), false);
    auto b = s->post(scene_note("pomodoro", "b"), false);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a.value(), "n1");
    EXPECT_EQ(b.value(), "n2");
    EXPECT_EQ(s->unread_count(), 2u);
}

TEST(Notifications, SameIdReplaces) {
    Harness h;
    auto s = h.make();
    ASSERT_TRUE(s->post(scene_note("pomodoro", "old", "pomo"), false));
    ASSERT_TRUE(s->post(scene_note("pomodoro", "new", "pomo"), false));
    EXPECT_EQ(s->unread_count(), 1u);
    EXPECT_EQ(s->find("pomodoro", "pomo")->title, "new");
    EXPECT_EQ(h.of_topic("notification.posted").size(), 2u);
    EXPECT_TRUE(h.of_topic("notification.dismissed").empty());
}

TEST(Notifications, FiveUnreadPerSceneDropsOldest) {
    Harness h;
    auto s = h.make();
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(s->post(scene_note("pomodoro", "t", "id" + std::to_string(i)), false));
    }
    EXPECT_EQ(s->unread_count(), 5u);
    EXPECT_FALSE(s->find("pomodoro", "id0"));
    EXPECT_TRUE(s->find("pomodoro", "id5"));
    auto dismissed = h.of_topic("notification.dismissed");
    ASSERT_EQ(dismissed.size(), 1u);
    EXPECT_EQ(field(dismissed[0], "id"), "id0");
    EXPECT_EQ(field(dismissed[0], "source"), "pomodoro");
    EXPECT_EQ(field(dismissed[0], "reason"), "evict");
    // Newest first.
    auto list = s->unread();
    ASSERT_EQ(list.size(), 5u);
    EXPECT_EQ(list.front().id, "id5");
    EXPECT_EQ(list.back().id, "id1");
}

TEST(Notifications, RateLimitTenPerMinute) {
    Harness h;
    auto s = h.make();
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(s->post(scene_note("pomodoro", "t", "a"), false)) << i;
    }
    auto over = s->post(scene_note("pomodoro", "t", "a"), false);
    ASSERT_FALSE(over);
    EXPECT_EQ(over.error().code, ErrorCode::RateLimited);
    EXPECT_EQ(over.error().message, "rate_limited");
    // Another scene has its own allowance.
    EXPECT_TRUE(s->post(scene_note("weather", "t"), false));
    h.now += kMinute;
    EXPECT_TRUE(s->post(scene_note("pomodoro", "t", "a"), false));
}

TEST(Notifications, ForegroundPostsAreNotListed) {
    Harness h;
    auto s = h.make();
    std::vector<NotificationChange> changes;
    s->set_listener([&](const NotificationChange& c) { changes.push_back(c); });
    auto id = s->post(scene_note("pomodoro", "front"), true);
    ASSERT_TRUE(id);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kind, NotificationChange::Kind::Posted);
    EXPECT_FALSE(changes[0].listed);
    EXPECT_EQ(changes[0].n.title, "front");
    EXPECT_TRUE(s->unread().empty());
    EXPECT_EQ(h.of_topic("notification.posted").size(), 1u);
    EXPECT_TRUE(h.timers.empty());
}

TEST(Notifications, FiftyStoredOldestDropped) {
    Harness h;
    auto s = h.make();
    for (int scene = 0; scene < 11; ++scene) {
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(s->post(
                scene_note("s" + std::to_string(scene), "t", "i" + std::to_string(i)), false));
        }
    }
    EXPECT_EQ(s->unread_count(), 50u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(s->find("s0", "i" + std::to_string(i))) << i;
    }
    EXPECT_TRUE(s->find("s10", "i4"));
    auto dismissed = h.of_topic("notification.dismissed");
    ASSERT_EQ(dismissed.size(), 5u);
    for (auto& e : dismissed) EXPECT_EQ(field(e, "reason"), "evict");
}

TEST(Notifications, ValidationErrors) {
    Harness h;
    auto s = h.make();
    auto expect_invalid = [&](Notification n) {
        auto r = s->post(std::move(n), false);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    };
    expect_invalid(scene_note("pomodoro", ""));
    expect_invalid(scene_note("pomodoro", std::string(33, 't')));
    auto long_body = scene_note("pomodoro", "t");
    long_body.body = std::string(201, 'b');
    expect_invalid(long_body);
    expect_invalid(scene_note("pomodoro", "t", "A!"));
    expect_invalid(scene_note("pomodoro", "t", std::string(33, 'a')));

    auto edge = scene_note("pomodoro", std::string(32, 't'), std::string(32, 'a'));
    edge.body = std::string(200, 'b');
    EXPECT_TRUE(s->post(edge, false));
    EXPECT_EQ(s->unread_count(), 1u);
}

TEST(Notifications, CancelDismissAndReasons) {
    Harness h;
    auto s = h.make();
    ASSERT_TRUE(s->post(scene_note("pomodoro", "a", "a"), false));
    ASSERT_TRUE(s->post(scene_note("pomodoro", "b", "b"), false));
    std::vector<NotificationChange> changes;
    s->set_listener([&](const NotificationChange& c) { changes.push_back(c); });

    EXPECT_TRUE(s->cancel("pomodoro", "a"));
    EXPECT_TRUE(s->dismiss("pomodoro", "b", DismissReason::Tap));
    EXPECT_FALSE(s->cancel("pomodoro", "a"));
    EXPECT_FALSE(s->dismiss("weather", "b", DismissReason::Dismiss));
    EXPECT_EQ(s->unread_count(), 0u);

    auto dismissed = h.of_topic("notification.dismissed");
    ASSERT_EQ(dismissed.size(), 2u);
    EXPECT_EQ(field(dismissed[0], "id"), "a");
    EXPECT_EQ(field(dismissed[0], "reason"), "cancel");
    EXPECT_EQ(field(dismissed[1], "id"), "b");
    EXPECT_EQ(field(dismissed[1], "reason"), "tap");
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].kind, NotificationChange::Kind::Dismissed);
    EXPECT_EQ(changes[0].reason, DismissReason::Cancel);
    EXPECT_EQ(changes[1].reason, DismissReason::Tap);
}

TEST(Notifications, DismissSourceRemovesAll) {
    Harness h;
    auto s = h.make();
    ASSERT_TRUE(s->post(scene_note("pomodoro", "a"), false));
    ASSERT_TRUE(s->post(scene_note("pomodoro", "b"), false));
    ASSERT_TRUE(s->post(scene_note("weather", "c"), false));
    EXPECT_EQ(s->dismiss_source("pomodoro", DismissReason::Dismiss), 2u);
    EXPECT_EQ(s->dismiss_source("pomodoro", DismissReason::Dismiss), 0u);
    ASSERT_EQ(s->unread_count(), 1u);
    EXPECT_EQ(s->unread()[0].source, "weather");
    EXPECT_EQ(h.of_topic("notification.dismissed").size(), 2u);

    s->clear_all();
    EXPECT_EQ(s->unread_count(), 0u);
    auto dismissed = h.of_topic("notification.dismissed");
    ASSERT_EQ(dismissed.size(), 3u);
    EXPECT_EQ(field(dismissed[2], "reason"), "dismiss");
}

TEST(Notifications, SystemNoticesBypassRateLimitAndReplace) {
    Harness h;
    auto s = h.make();
    for (int i = 0; i < 12; ++i) {
        s->post_system(wisp::core::notice::kSceneError, "scene" + std::to_string(i));
    }
    EXPECT_EQ(s->unread_count(), 12u);
    std::string id = s->post_system(wisp::core::notice::kWakeLockLong, "scene.a",
                                    wisp::core::notice::kActionReleaseLock, "7");
    EXPECT_EQ(id, "wakelock_long:scene.a");
    s->post_system(wisp::core::notice::kWakeLockLong, "scene.a",
                   wisp::core::notice::kActionReleaseLock, "8");
    EXPECT_EQ(s->unread_count(), 13u);
    auto n = s->find("system", "wakelock_long:scene.a");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->code, "wakelock_long");
    EXPECT_EQ(n->arg, "scene.a");
    EXPECT_EQ(n->action, "release_lock");
    EXPECT_EQ(n->action_arg, "8");
    EXPECT_EQ(n->source_name, "");
}

TEST(Notifications, PersistedCoalescedAndReloaded) {
    Harness h;
    {
        auto s = h.make();
        auto a = scene_note("pomodoro", "a", "a");
        a.sound = NotifySound::Alarm;
        a.wake = true;
        ASSERT_TRUE(s->post(a, false));
        ASSERT_TRUE(s->post(scene_note("pomodoro", "b", "b"), false));
        s->post_system(wisp::core::notice::kBatteryLow, "12");
        ASSERT_EQ(h.timers.size(), 1u);
        EXPECT_EQ(h.timers[0].first, NotificationService::kCoalesceMs);
        EXPECT_EQ(h.submits, 0);
        h.fire_timers();
        EXPECT_EQ(h.submits, 1);
        EXPECT_TRUE(fs::exists(fs::path(h.in) / "system" / "notifications.json"));
        // Nothing changed since: flush has nothing to write.
        EXPECT_TRUE(s->flush(1000));
        EXPECT_EQ(h.submits, 1);
    }
    auto again = h.make();
    auto list = again->unread();
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].code, "battery_low");
    EXPECT_EQ(list[0].arg, "12");
    EXPECT_EQ(list[1].id, "b");
    EXPECT_EQ(list[2].id, "a");
    EXPECT_EQ(list[2].title, "a");
    EXPECT_EQ(list[2].body, "the body text");
    EXPECT_EQ(list[2].source_name, "Pomodoro");
    EXPECT_EQ(list[2].sound, NotifySound::Alarm);
    EXPECT_EQ(list[2].posted_unix, 1767225600);
    EXPECT_FALSE(list[2].wake);

    // A generated id after a reload does not collide with a stored one.
    ASSERT_TRUE(again->post(scene_note("weather", "x", "n1"), false));
    auto gen = again->post(scene_note("weather", "y"), false);
    ASSERT_TRUE(gen);
    EXPECT_NE(gen.value(), "n1");
}

TEST(Notifications, FlushWritesPendingChanges) {
    Harness h;
    {
        auto s = h.make();
        ASSERT_TRUE(s->post(scene_note("pomodoro", "a", "a"), false));
        EXPECT_TRUE(s->flush(1000));
        EXPECT_EQ(h.submits, 1);
        // The timer that was armed finds nothing left to write.
        h.fire_timers();
        EXPECT_EQ(h.submits, 1);
    }
    auto again = h.make();
    EXPECT_EQ(again->unread_count(), 1u);
}

TEST(Notifications, MissingOrBadFileLoadsEmpty) {
    Harness h;
    auto s = h.make();
    EXPECT_EQ(s->unread_count(), 0u);
    std::error_code ec;
    fs::create_directories(fs::path(h.in) / "system", ec);
    {
        FILE* f = std::fopen((fs::path(h.in) / "system" / "notifications.json").c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fputs("[{\"source\":\"x\"", f);
        std::fclose(f);
    }
    NotificationService bad(h.deps());
    EXPECT_TRUE(bad.load());
    EXPECT_EQ(bad.unread_count(), 0u);
}
