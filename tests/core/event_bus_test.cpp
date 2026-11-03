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
    void write(const wisp::LogRecord& rec) override { lines.push_back(rec.msg); }
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
    // now_ms() counts from boot (process start here), so it can still be
    // 0; step past that so a filled stamp is distinguishable from "unset".
    while (wisp::os::now_ms() < 2) wisp::os::sleep_ms(1);
    const uint64_t before = wisp::os::now_ms();
    bus.publish({.topic = "t"});
    bus.wait_idle(1000);
    EXPECT_GE(ts, before);
    EXPECT_LE(ts, wisp::os::now_ms());
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

// inflight_ used to be incremented *after*
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
//
// The watcher's "idle, yet a handler is running"
// check must not read wait_idle() and a separate "is a handler running"
// flag as two independent, non-atomic snapshots -- a brand new handler
// invocation (for a different, legitimately-just-published event) could
// start in the gap between the two reads and falsely look like a
// violation, even though the bus was genuinely idle for a moment and
// then genuinely picked up new work. Fixed by having each invocation
// record its own monotonically increasing sequence number while it
// runs, and only flagging a violation when the *same* invocation (same
// sequence number, not just "some invocation or other") is found still
// running both immediately before and immediately after a wait_idle()
// call that returned true -- which can only happen if that one
// invocation was running throughout the wait_idle() call itself.
TEST(EventBus, WaitIdleNeverTrueWhileHandlerRunningUnderConcurrentPublish) {
    constexpr uint64_t kIdle = UINT64_MAX;
    EventBus bus(64);
    std::atomic<uint64_t> seq_counter{0};
    std::atomic<uint64_t> active_seq{kIdle};
    std::atomic<int> delivered{0};
    std::atomic<bool> violation{false};
    bus.subscribe("", [&](const Event&) {
        uint64_t my_seq = seq_counter.fetch_add(1, std::memory_order_relaxed);
        active_seq.store(my_seq, std::memory_order_seq_cst);
        delivered.fetch_add(1, std::memory_order_relaxed);
        active_seq.store(kIdle, std::memory_order_seq_cst);
    });
    bus.start();

    std::atomic<bool> publishing{true};
    auto watcher = wisp::os::Task::spawn({.name = "watcher"}, [&] {
        while (publishing.load(std::memory_order_relaxed)) {
            uint64_t seq_before = active_seq.load(std::memory_order_seq_cst);
            if (bus.wait_idle(0) && seq_before != kIdle &&
                active_seq.load(std::memory_order_seq_cst) == seq_before) {
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

// A handler calling stop() on itself used to
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

// Unsubscribe() from a thread other than the
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

// The overflow warning is throttled (logged
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

// "am I on the dispatcher thread?" used to
// be derived from the dispatcher_ handle (dispatcher_->is_current()),
// but stop()/~EventBus()/start() move that handle out *before* the
// dispatcher task actually finishes exiting. So a handler that
// self-unsubscribes while an external stop() is concurrently joining
// would see on_dispatcher_thread == false (the handle is already gone)
// and try to lock dispatch_mutex_ -- which this very thread already
// holds via dispatch() -- a self-deadlock on the non-recursive
// os::Mutex, hanging stop()'s join() forever. If this regresses, the
// per-test ctest TIMEOUT on this binary (see tests/CMakeLists.txt) fails
// just this test via a killed process instead of hanging the run
// forever; gtest_discover_tests runs each TEST() as its own ctest entry/
// process, so one hang doesn't take any other test down with it.
TEST(EventBus, SelfUnsubscribeDuringExternalStopDoesNotDeadlock) {
    EventBus bus;
    bus.start();
    wisp::os::Semaphore handler_started(1, 0);
    wisp::core::SubscriptionId id = 0;
    id = bus.subscribe("x", [&](const Event&) {
        handler_started.give();
        wisp::os::sleep_ms(30);
        bus.unsubscribe(id);  // self-unsubscribe, racing the stop() below
    });

    bus.publish({.topic = "x"});
    ASSERT_TRUE(handler_started.take(1000));

    bus.stop();  // must return once the handler finishes, not deadlock
    SUCCEED();
}

// Same as above, but through ~EventBus() instead of an explicit stop().
TEST(EventBus, SelfUnsubscribeDuringDestructorDoesNotDeadlock) {
    auto bus = std::make_unique<EventBus>();
    bus->start();
    wisp::os::Semaphore handler_started(1, 0);
    wisp::core::SubscriptionId id = 0;
    // Captured as a raw pointer, not a reference to `bus` itself:
    // unique_ptr::reset() nulls its own stored pointer *before* running
    // ~EventBus() (whose stop() call is what blocks below, waiting for
    // this very handler to finish) -- so a handler that instead wrote
    // `bus->unsubscribe(id)` would dereference an already-null
    // unique_ptr and crash. `raw` stays valid throughout: the object it
    // points to isn't actually freed until ~EventBus()'s body (stop())
    // returns, which can't happen until this handler does.
    EventBus* raw = bus.get();
    id = raw->subscribe("x", [&](const Event&) {
        handler_started.give();
        wisp::os::sleep_ms(30);
        raw->unsubscribe(id);
    });

    raw->publish({.topic = "x"});
    ASSERT_TRUE(handler_started.take(1000));

    bus.reset();  // ~EventBus() calls stop(); must not deadlock either.
    SUCCEED();
}

// Same flaw, exercised through start()'s "am I on the dispatcher
// thread?" check (used to decide whether to return ErrorCode::Busy
// instead of joining a stale dispatcher): a handler calling start() on
// itself while an external stop() is concurrently retiring that same
// dispatcher must still get the Busy error (not attempt to join itself).
TEST(EventBus, StartFromWithinHandlerReturnsErrorDuringExternalStop) {
    EventBus bus;
    bus.start();
    wisp::os::Semaphore handler_started(1, 0);
    std::atomic<bool> got_busy_error{false};
    bus.subscribe("x", [&](const Event&) {
        handler_started.give();
        wisp::os::sleep_ms(30);
        got_busy_error.store(!bus.start(), std::memory_order_seq_cst);
    });

    bus.publish({.topic = "x"});
    ASSERT_TRUE(handler_started.take(1000));

    bus.stop();  // races the handler's start() call above
    EXPECT_TRUE(got_busy_error.load());
}

// start() used to treat *any* existing
// dispatcher_ as stale, even one that was still live and idle (blocked
// in ready_.take(kForever), nothing else around to wake it) -- joining
// it would then hang forever. start() called on an already-running bus
// must now be a prompt, idempotent no-op instead.
TEST(EventBus, StartCalledTwiceThenPublishDeliversPromptly) {
    EventBus bus;
    ASSERT_TRUE(bus.start());
    ASSERT_TRUE(bus.start());  // already running and idle: must return promptly

    std::vector<std::string> got;
    wisp::os::Mutex m;
    bus.subscribe("", [&](const Event& e) {
        std::lock_guard<wisp::os::Mutex> g(m);
        got.push_back(e.topic);
    });
    bus.publish({.topic = "x"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(got, std::vector<std::string>{"x"});
}

// When start() does retire a stale
// (no-longer-live) dispatcher, it must wake it (give a ready_ token)
// *before* joining, and a dispatcher exiting because it's stale must
// give back whatever token woke it -- otherwise the token count falls
// permanently short of the number of real undelivered events, stranding
// the last one or more of them until some unrelated future publish()
// happens to cover for it. This stresses the stale-dispatcher-retirement
// path (stop() from within a handler, then start() from another thread)
// repeatedly under concurrent publish() load and checks that every
// published event is eventually accounted for (delivered + dropped ==
// published), with a single bounded wait_idle() at the end rather than a
// live watcher -- so a regression shows up as `delivered` permanently
// short, not as a hang (see issue 4's fix in the test above for why a
// live watcher reading two separate pieces of state is worth avoiding
// when a simpler, race-free check will do).
TEST(EventBus, StopFromHandlerThenStartRepeatedlyNeverStrandsAnEvent) {
    EventBus bus(256);
    std::atomic<int> delivered{0};
    std::atomic<bool> stop_requested{false};
    bus.subscribe("", [&](const Event& e) {
        delivered.fetch_add(1, std::memory_order_relaxed);
        if (e.topic == "stop-me") {
            // Call stop() *before* signaling stop_requested, not after:
            // the test thread's spin-wait below only treats stop_requested
            // as a reliable "safe to start() again now" signal if it's
            // guaranteed stop() has already fully returned by the time
            // that flag becomes visible. Setting the flag first would let
            // the test thread's start() race ahead of this very stop()
            // call (lifecycle_mutex_ serializes them safely either way,
            // but start() could then observe the dispatcher as still
            // "live" and treat itself as an idempotent no-op before this
            // stop() has actually retired it) -- undermining the
            // "deterministic, not racing stop() vs start()" intent below.
            bus.stop();
            stop_requested.store(true, std::memory_order_relaxed);
        }
    });
    ASSERT_TRUE(bus.start());

    constexpr int kRounds = 20;
    constexpr int kEventsPerRound = 10;
    int total_published = 0;
    for (int round = 0; round < kRounds; ++round) {
        stop_requested.store(false, std::memory_order_relaxed);
        // One "stop-me" plus several ordinary events published right
        // behind it, before the handler (running on the dispatcher
        // task) has necessarily had a chance to call stop() yet --
        // maximizing the chance that real, not-yet-dispatched events
        // are still sitting in the queue at the moment the dispatcher
        // actually retires.
        bus.publish({.topic = "stop-me"});
        ++total_published;
        for (int i = 0; i < kEventsPerRound; ++i) {
            bus.publish({.topic = "x"});
            ++total_published;
        }
        // Wait for the handler to have actually called stop() before
        // restarting -- stop() itself is safe to call concurrently with
        // a start() racing in (see the other dispatcher-retirement tests
        // in this file), but
        // this test is specifically about the stale-retirement path, so
        // drive it deterministically rather than also racing stop() vs
        // start() against each other here.
        for (int spins = 0; !stop_requested.load(std::memory_order_relaxed) && spins < 100000;
             ++spins) {
            wisp::os::sleep_ms(0);
        }
        ASSERT_TRUE(stop_requested.load(std::memory_order_relaxed));
        ASSERT_TRUE(bus.start());
    }

    ASSERT_TRUE(bus.wait_idle(5000));
    EXPECT_EQ(static_cast<uint64_t>(delivered.load()) + bus.dropped(),
              static_cast<uint64_t>(total_published));
}
