#include "wisp/core/wakelock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "wisp/core/event_bus.hpp"

using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::WakeLock;
using wisp::core::WakeLockKind;
using wisp::core::WakeLockRegistry;

namespace {

// A started bus collecting every power.wakelock event, and a registry on a
// clock the test moves by hand.
struct Fixture {
    Fixture() : reg(bus, [this] { return now.load(); }) {
        bus.start();
        bus.subscribe("power.wakelock", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });
    }
    ~Fixture() { bus.stop(); }

    std::vector<Event> drained() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }

    EventBus bus;
    std::atomic<uint64_t> now{1000};
    WakeLockRegistry reg;
    std::mutex mutex;
    std::vector<Event> events;
};

std::string field(const Event& e, const char* key) {
    auto it = e.payload.find(key);
    return it == e.payload.end() ? std::string("<missing>") : it->second.as_string();
}

constexpr uint64_t kMin = 60 * 1000;

}  // namespace

TEST(WakeLockRegistry, KindNames) {
    EXPECT_STREQ(wisp::core::to_string(WakeLockKind::Network), "network");
    EXPECT_STREQ(wisp::core::to_string(WakeLockKind::Cpu), "cpu");
    EXPECT_STREQ(wisp::core::to_string(WakeLockKind::Screen), "screen");
}

TEST(WakeLockRegistry, AcquireReleaseListAndEvents) {
    Fixture f;
    int changes = 0;
    f.reg.set_on_change([&] { ++changes; });

    uint32_t id = f.reg.acquire("scene.a", WakeLockKind::Screen, "reading");
    EXPECT_EQ(id, 1u);
    auto list = f.reg.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, 1u);
    EXPECT_EQ(list[0].holder, "scene.a");
    EXPECT_EQ(list[0].kind, WakeLockKind::Screen);
    EXPECT_EQ(list[0].reason, "reading");
    EXPECT_EQ(list[0].since_ms, 1000u);
    EXPECT_EQ(changes, 1);

    EXPECT_TRUE(f.reg.release(id));
    EXPECT_TRUE(f.reg.list().empty());
    EXPECT_EQ(changes, 2);
    // An unknown or already released id changes nothing.
    EXPECT_FALSE(f.reg.release(id));
    EXPECT_FALSE(f.reg.release(0));
    EXPECT_EQ(changes, 2);

    auto events = f.drained();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].source, "power");
    EXPECT_EQ(field(events[0], "op"), "acquire");
    EXPECT_EQ(field(events[0], "holder"), "scene.a");
    EXPECT_EQ(field(events[0], "kind"), "screen");
    EXPECT_EQ(field(events[0], "reason"), "reading");
    EXPECT_EQ(field(events[1], "op"), "release");
    EXPECT_EQ(field(events[1], "holder"), "scene.a");
    EXPECT_EQ(field(events[1], "kind"), "screen");
    EXPECT_EQ(field(events[1], "reason"), "reading");
}

TEST(WakeLockRegistry, HeldByKind) {
    Fixture f;
    EXPECT_FALSE(f.reg.held(WakeLockKind::Network));
    uint32_t a = f.reg.acquire("system.ntp", WakeLockKind::Network, "time sync");
    uint32_t b = f.reg.acquire("system.admin", WakeLockKind::Network, "admin session");
    EXPECT_NE(a, b);
    EXPECT_TRUE(f.reg.held(WakeLockKind::Network));
    EXPECT_FALSE(f.reg.held(WakeLockKind::Cpu));
    EXPECT_FALSE(f.reg.held(WakeLockKind::Screen));

    f.reg.release(a);
    EXPECT_TRUE(f.reg.held(WakeLockKind::Network));
    f.reg.release(b);
    EXPECT_FALSE(f.reg.held(WakeLockKind::Network));
}

TEST(WakeLockRegistry, ReleaseHolderDropsAll) {
    Fixture f;
    f.reg.acquire("scene.a", WakeLockKind::Screen, "reading");
    f.reg.acquire("scene.a", WakeLockKind::Cpu, "timer");
    uint32_t sys = f.reg.acquire("system.x", WakeLockKind::Network, "x");
    int changes = 0;
    f.reg.set_on_change([&] { ++changes; });

    EXPECT_EQ(f.reg.release_holder("scene.a"), 2u);
    auto list = f.reg.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, sys);
    EXPECT_GE(changes, 1);

    changes = 0;
    EXPECT_EQ(f.reg.release_holder("scene.a"), 0u);
    EXPECT_EQ(changes, 0);

    auto events = f.drained();
    ASSERT_EQ(events.size(), 5u);
    EXPECT_EQ(field(events[3], "op"), "release");
    EXPECT_EQ(field(events[4], "op"), "release");
    EXPECT_EQ(field(events[3], "holder"), "scene.a");
    EXPECT_EQ(field(events[4], "holder"), "scene.a");
}

TEST(WakeLockRegistry, ListIsOrderedById) {
    Fixture f;
    uint32_t a = f.reg.acquire("system.a", WakeLockKind::Cpu, "a");
    uint32_t b = f.reg.acquire("system.b", WakeLockKind::Cpu, "b");
    uint32_t c = f.reg.acquire("system.c", WakeLockKind::Cpu, "c");
    f.reg.release(b);
    uint32_t d = f.reg.acquire("system.d", WakeLockKind::Cpu, "d");
    auto list = f.reg.list();
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].id, a);
    EXPECT_EQ(list[1].id, c);
    EXPECT_EQ(list[2].id, d);
    EXPECT_GT(d, c);
}

TEST(WakeLockRegistry, RemindersEvery30Minutes) {
    Fixture f;
    const uint64_t t0 = f.now.load();
    uint32_t id = f.reg.acquire("scene.a", WakeLockKind::Screen, "reading");

    EXPECT_TRUE(f.reg.due_reminders(t0 + 30 * kMin - 1000).empty());
    auto first = f.reg.due_reminders(t0 + 30 * kMin);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].id, id);
    EXPECT_EQ(first[0].holder, "scene.a");
    // Each multiple is reported once.
    EXPECT_TRUE(f.reg.due_reminders(t0 + 30 * kMin + 5000).empty());
    EXPECT_TRUE(f.reg.due_reminders(t0 + 60 * kMin - 1000).empty());
    ASSERT_EQ(f.reg.due_reminders(t0 + 60 * kMin).size(), 1u);
    EXPECT_TRUE(f.reg.due_reminders(t0 + 60 * kMin + 1).empty());

    f.reg.release(id);
    EXPECT_TRUE(f.reg.due_reminders(t0 + 90 * kMin).empty());
}

TEST(WakeLockRegistry, ReminderSkipsMissedMultiplesAtOnce) {
    Fixture f;
    const uint64_t t0 = f.now.load();
    f.reg.acquire("system.ota", WakeLockKind::Cpu, "firmware update");
    // Nobody asked for 100 minutes: one reminder, not three.
    EXPECT_EQ(f.reg.due_reminders(t0 + 100 * kMin).size(), 1u);
    EXPECT_TRUE(f.reg.due_reminders(t0 + 110 * kMin).empty());
    EXPECT_EQ(f.reg.due_reminders(t0 + 120 * kMin).size(), 1u);
}

TEST(WakeLock, RaiiReleasesOnDestruction) {
    Fixture f;
    {
        WakeLock lock(f.reg, "system.upload", WakeLockKind::Network, "scene upload");
        EXPECT_TRUE(lock.held());
        EXPECT_TRUE(f.reg.held(WakeLockKind::Network));
    }
    EXPECT_FALSE(f.reg.held(WakeLockKind::Network));

    WakeLock empty;
    EXPECT_FALSE(empty.held());
    empty.release();  // harmless

    WakeLock a(f.reg, "system.ota", WakeLockKind::Cpu, "firmware update");
    WakeLock b(std::move(a));
    EXPECT_FALSE(a.held());  // NOLINT(bugprone-use-after-move): moved-from is empty
    EXPECT_TRUE(b.held());
    EXPECT_EQ(f.reg.list().size(), 1u);

    WakeLock c;
    c = std::move(b);
    EXPECT_TRUE(c.held());
    EXPECT_EQ(f.reg.list().size(), 1u);
    c.release();
    EXPECT_FALSE(c.held());
    EXPECT_TRUE(f.reg.list().empty());

    auto events = f.drained();
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(field(events[1], "op"), "release");
    EXPECT_EQ(field(events[3], "op"), "release");
}
