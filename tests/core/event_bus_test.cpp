#include "wisp/core/event_bus.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"

using wisp::core::Event;
using wisp::core::EventBus;

namespace {

struct CaptureSink : wisp::LogSink {
    std::vector<std::string> lines;
    void write(wisp::LogLevel, const char*, const char* msg) override { lines.push_back(msg); }
};

}  // namespace

TEST(EventBus, PrefixMatching) {
    EventBus bus;
    bus.start();
    std::vector<std::string> got;
    wisp::os::Mutex m;
    bus.subscribe("button.", [&](const Event& e) {
        std::lock_guard g(m);
        got.push_back(e.topic);
    });
    bus.publish({.topic = "button.power.short"});
    bus.publish({.topic = "buttons.x"});
    bus.publish({.topic = "power.low"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(got, std::vector<std::string>{"button.power.short"});
}

TEST(EventBus, EmptyPrefixMatchesAllInOrder) {
    EventBus bus;
    bus.start();
    std::vector<int> got;
    bus.subscribe("", [&](const Event& e) { got.push_back(e.payload.at("i").as_int()); });
    for (int i = 0; i < 20; ++i) {
        bus.publish({.topic = "t", .payload = {{"i", i}}});
    }
    ASSERT_TRUE(bus.wait_idle(1000));
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(got[i], i);
    }
}

TEST(EventBus, PublishFromHandlerDoesNotDeadlock) {
    EventBus bus;
    bus.start();
    std::vector<std::string> got;
    bus.subscribe("a", [&](const Event&) { bus.publish({.topic = "b"}); });
    bus.subscribe("", [&](const Event& e) { got.push_back(e.topic); });
    bus.publish({.topic = "a"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(got, (std::vector<std::string>{"a", "b"}));
}

TEST(EventBus, Unsubscribe) {
    EventBus bus;
    bus.start();
    int n = 0;
    auto id = bus.subscribe("x", [&](const Event&) { ++n; });
    bus.publish({.topic = "x"});
    bus.wait_idle(1000);
    bus.unsubscribe(id);
    bus.publish({.topic = "x"});
    bus.wait_idle(1000);
    EXPECT_EQ(n, 1);
}

TEST(EventBus, OverflowDropsOldestAndCounts) {
    EventBus bus(4);  // not started: events just queue up
    for (int i = 0; i < 10; ++i) {
        bus.publish({.topic = "t", .payload = {{"i", i}}});
    }
    EXPECT_EQ(bus.dropped(), 6u);
    std::vector<int> got;
    bus.subscribe("", [&](const Event& e) { got.push_back(e.payload.at("i").as_int()); });
    bus.start();
    bus.wait_idle(1000);
    EXPECT_EQ(got, (std::vector<int>{6, 7, 8, 9}));
}

TEST(EventBus, FillsTimestamp) {
    EventBus bus;
    bus.start();
    uint64_t ts = 0;
    bus.subscribe("", [&](const Event& e) { ts = e.ts_ms; });
    bus.publish({.topic = "t"});
    bus.wait_idle(1000);
    EXPECT_GT(ts, 0u);
}

TEST(EventBus, UnsubscribeFromWithinHandlerIsSafe) {
    EventBus bus;
    bus.start();
    int n = 0;
    wisp::core::SubscriptionId id = 0;
    id = bus.subscribe("x", [&](const Event&) {
        ++n;
        bus.unsubscribe(id);
    });
    bus.publish({.topic = "x"});
    bus.publish({.topic = "x"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(n, 1);
}

TEST(EventBus, StopFromWithinHandlerDoesNotDeadlock) {
    EventBus bus;
    bus.start();
    bool handled = false;
    bus.subscribe("x", [&](const Event&) {
        handled = true;
        bus.stop();
    });
    bus.publish({.topic = "x"});
    // If stop() from within the handler self-joined, this would hang and
    // be killed by the ctest TIMEOUT instead of completing.
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_TRUE(handled);
    bus.stop();  // from the test thread: must actually join the dispatcher
}

TEST(EventBus, WaitIdleFalseOnTimeoutWhenNeverStarted) {
    EventBus bus;
    bus.publish({.topic = "t"});
    EXPECT_FALSE(bus.wait_idle(50));
}

// Covers fix round 1, issue 1: inflight_ used to be incremented *after*
// push_drop_oldest() returned, so under concurrent publishers the
// dispatcher could pop and start running a handler for one publisher's
// event before that publisher's own increment ran, letting wait_idle()
// observe "idle" while a handler was actually still executing. The race
// window is only a few instructions wide (no syscalls involved), so this
// needs a *busy-polling* watcher (no sleep between checks) and enough
// concurrent publish() throughput to keep the dispatcher continuously
// busy for a while -- a 1ms-polling watcher or a handful of publishes
// essentially never samples inside the window and would pass even
// against the pre-fix code. With the parameters below this reliably
// reproduces the bug against the pre-fix implementation (confirmed via a
// 30-trial standalone stress run during development: 30/30 violations
// on the old code, 0/30 on the fix) while finishing in well under a
// second against the fix.
TEST(EventBus, WaitIdleNeverTrueWhileHandlerRunningUnderConcurrentPublish) {
    EventBus bus(64);
    std::atomic<bool> handler_running{false};
    std::atomic<int> delivered{0};
    std::atomic<bool> violation{false};
    bus.subscribe("", [&](const Event&) {
        handler_running.store(true, std::memory_order_seq_cst);
        delivered.fetch_add(1, std::memory_order_relaxed);
        handler_running.store(false, std::memory_order_seq_cst);
    });
    bus.start();

    std::atomic<bool> publishing{true};
    auto watcher = wisp::os::Task::spawn({.name = "watcher"}, [&] {
        while (publishing.load(std::memory_order_relaxed)) {
            if (bus.wait_idle(0) && handler_running.load(std::memory_order_seq_cst)) {
                violation.store(true, std::memory_order_relaxed);
            }
        }
    });

    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::unique_ptr<wisp::os::Task>> publishers;
    for (int t = 0; t < kThreads; ++t) {
        publishers.push_back(wisp::os::Task::spawn({.name = "pub"}, [&bus] {
            for (int i = 0; i < kPerThread; ++i) {
                bus.publish({.topic = "t"});
            }
        }));
    }
    for (auto& p : publishers) {
        p->join();
    }
    publishing.store(false, std::memory_order_relaxed);
    watcher->join();

    ASSERT_TRUE(bus.wait_idle(2000));
    EXPECT_FALSE(violation.load());
    EXPECT_EQ(static_cast<uint64_t>(delivered.load()) + bus.dropped(),
              static_cast<uint64_t>(kThreads * kPerThread));
}

// Covers fix round 1, issue 2: a handler calling stop() on itself used to
// leave the (soon to exit) dispatcher Task unjoined; a subsequent start()
// from another thread would then overwrite that unique_ptr<Task> with a
// freshly spawned one -- destroying an unjoined Task -- and the old loop
// could in principle still be running when the new one started, double-
// dispatching events. start() must now join/reset any stale dispatcher
// first and guarantee single, in-order delivery resumes cleanly.
TEST(EventBus, StopFromHandlerThenStartFromAnotherThreadIsSafe) {
    EventBus bus;
    bus.start();
    std::vector<std::string> got;
    wisp::os::Mutex m;
    bus.subscribe("", [&](const Event& e) {
        std::lock_guard<wisp::os::Mutex> g(m);
        got.push_back(e.topic);
        if (e.topic == "stop-me") {
            bus.stop();
        }
    });

    bus.publish({.topic = "stop-me"});
    ASSERT_TRUE(bus.wait_idle(1000));

    // By the time wait_idle() returns, the handler above (and thus its
    // stop() call) has already returned, but the dispatcher task itself
    // may still be a few instructions away from actually exiting its
    // loop. start() must handle that itself (joining before replacing
    // it), not require the caller to add its own synchronization here.
    ASSERT_TRUE(bus.start());

    bus.publish({.topic = "a"});
    bus.publish({.topic = "b"});
    ASSERT_TRUE(bus.wait_idle(1000));

    EXPECT_EQ(got, (std::vector<std::string>{"stop-me", "a", "b"}));
}

TEST(EventBus, StartFromWithinHandlerReturnsError) {
    EventBus bus;
    bus.start();
    bool got_error = false;
    bus.subscribe("x", [&](const Event&) { got_error = !bus.start(); });
    bus.publish({.topic = "x"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_TRUE(got_error);
}

// Covers fix round 1, issue 4: unsubscribe() from a thread other than the
// dispatcher's own must block until any in-flight dispatch has finished,
// so that once it returns, the handler is guaranteed not to be running
// (and never will run again) -- making "unsubscribe(), then free
// whatever the handler captured" safe.
TEST(EventBus, UnsubscribeFromExternalThreadWaitsForInFlightDispatch) {
    EventBus bus;
    bus.start();
    wisp::os::Semaphore handler_started(1, 0);
    std::atomic<bool> handler_running{false};
    int n = 0;
    auto id = bus.subscribe("x", [&](const Event&) {
        handler_running.store(true, std::memory_order_seq_cst);
        ++n;
        handler_started.give();
        wisp::os::sleep_ms(50);
        handler_running.store(false, std::memory_order_seq_cst);
    });

    bus.publish({.topic = "x"});
    ASSERT_TRUE(handler_started.take(1000));  // handler has started and is now sleeping

    bus.unsubscribe(id);  // must block until the sleeping handler above returns
    EXPECT_FALSE(handler_running.load(std::memory_order_seq_cst));

    bus.publish({.topic = "x"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(n, 1);  // never invoked again once unsubscribe() had returned
}

// Covers fix round 1, issue 7: the overflow warning is throttled (logged
// for the first drop, then every 64th), rather than once per drop.
TEST(EventBus, OverflowWarningIsThrottled) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    CaptureSink sink;
    wisp::log::add_sink(&sink);

    EventBus bus(2);  // not started: events just queue up, overflowing immediately
    constexpr int kTotal = 200;
    for (int i = 0; i < kTotal; ++i) {
        bus.publish({.topic = "t"});
    }
    uint64_t total_dropped = bus.dropped();
    ASSERT_EQ(total_dropped, static_cast<uint64_t>(kTotal - 2));

    // Expected log lines: one for drop #1, then one every 64th
    // (64, 128, 192, ... up to total_dropped).
    uint64_t expected_lines = 1 + total_dropped / 64;
    EXPECT_EQ(sink.lines.size(), expected_lines);

    wisp::log::reset_for_test();
}
