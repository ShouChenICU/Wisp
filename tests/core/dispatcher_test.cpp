#include "wisp/core/dispatcher.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

#include "support/manual_dispatcher.hpp"
#include "support/wait.hpp"
#include "wisp/os/os.hpp"

using wisp::core::DispatchSlot;
using wisp::core::SlotQueue;
using wisp::test::ManualDispatcher;

namespace {

// A slot that records its id into a shared log each time it runs.
struct TestSlot : DispatchSlot {
    TestSlot(int id_, std::vector<int>* log_) : id(id_), log(log_) {
        run = [](DispatchSlot& s) {
            auto& self = static_cast<TestSlot&>(s);
            self.log->push_back(self.id);
        };
        finish = [](DispatchSlot& s) { ++static_cast<TestSlot&>(s).finished; };
    }
    int id;
    std::vector<int>* log;
    int finished = 0;
};

struct RestoreDeathStyle {
    std::string old = GTEST_FLAG_GET(death_test_style);
    ~RestoreDeathStyle() { GTEST_FLAG_SET(death_test_style, old); }
};

}  // namespace

TEST(SlotQueue, PushTwiceQueuesOnce) {
    std::vector<int> log;
    TestSlot a(1, &log);
    SlotQueue q;
    EXPECT_TRUE(q.push(a));
    EXPECT_FALSE(q.push(a));
    EXPECT_EQ(q.pending(), 1u);
    EXPECT_EQ(q.take(), &a);
    q.done(a);
    EXPECT_EQ(q.take(), nullptr);
}

TEST(SlotQueue, RunsInFifoOrder) {
    std::vector<int> log;
    TestSlot a(1, &log), b(2, &log), c(3, &log);
    SlotQueue q;
    q.push(a);
    q.push(b);
    q.push(c);
    for (DispatchSlot* want : {static_cast<DispatchSlot*>(&a), static_cast<DispatchSlot*>(&b),
                               static_cast<DispatchSlot*>(&c)}) {
        DispatchSlot* got = q.take();
        EXPECT_EQ(got, want);
        q.done(*got);
    }
    EXPECT_EQ(q.take(), nullptr);
}

TEST(SlotQueue, PushWhileRunningQueuesItAgain) {
    std::vector<int> log;
    TestSlot a(1, &log);
    SlotQueue q;
    q.push(a);
    ASSERT_EQ(q.take(), &a);
    EXPECT_TRUE(q.push(a));
    EXPECT_EQ(q.pending(), 2u);
    q.done(a);
    EXPECT_EQ(q.take(), &a);
    q.done(a);
}

TEST(SlotQueue, CancelUnlinksAQueuedSlot) {
    std::vector<int> log;
    TestSlot a(1, &log), b(2, &log);
    SlotQueue q;
    q.push(a);
    q.push(b);
    EXPECT_TRUE(q.cancel(a, false));
    EXPECT_EQ(q.take(), &b);
    EXPECT_EQ(q.pending(), 1u);
    q.done(b);
    EXPECT_EQ(q.take(), nullptr);
}

TEST(SlotQueue, CancelInsideOwnRunDefersToFinish) {
    std::vector<int> log;
    TestSlot a(1, &log);
    SlotQueue q;
    q.push(a);
    ASSERT_EQ(q.take(), &a);
    q.push(a);  // queued again while running
    EXPECT_FALSE(q.cancel(a, true));
    EXPECT_EQ(a.finished, 0);
    q.done(a);
    EXPECT_EQ(a.finished, 1);
    EXPECT_EQ(q.pending(), 0u);
    EXPECT_EQ(q.take(), nullptr);
}

TEST(SlotQueue, CancelFromAnotherThreadWaitsForTheRun) {
    using wisp::os::kForever;
    using wisp::os::Semaphore;
    using wisp::os::Task;
    std::vector<int> log;
    TestSlot a(1, &log);
    SlotQueue q;
    q.push(a);

    Semaphore taken(1, 0);
    Semaphore gate(1, 0);
    auto runner = Task::spawn({.name = "runner"}, [&] {
        EXPECT_EQ(q.take(), &a);
        taken.give();
        gate.take(kForever);  // "the run"
        q.done(a);
    });
    ASSERT_TRUE(taken.take(1000));

    std::atomic<bool> returned{false};
    std::atomic<bool> result{false};
    auto canceller = Task::spawn({.name = "canceller"}, [&] {
        result.store(q.cancel(a, false));
        returned.store(true);
    });

    wisp::os::sleep_ms(20);
    EXPECT_FALSE(returned.load());
    gate.give();
    ASSERT_TRUE(wisp::test::wait_until([&] { return returned.load(); }, 1000));
    EXPECT_TRUE(result.load());
    runner->join();
    canceller->join();
    EXPECT_EQ(a.finished, 0);
}

TEST(SlotQueue, CancelFromAnotherThreadWaitsForExactlyTheCurrentRun) {
    using wisp::os::kForever;
    using wisp::os::Semaphore;
    using wisp::os::Task;
    std::vector<int> log;
    TestSlot a(1, &log);
    SlotQueue q;
    q.push(a);

    Semaphore rearmed(1, 0);
    Semaphore gate(1, 0);
    std::atomic<int> runs{0};
    std::atomic<bool> late_push_woke{true};
    auto runner = Task::spawn({.name = "runner"}, [&] {
        // Run 1 re-schedules itself before and after the cancel starts;
        // later runs (only reachable when the cancel let the slot back in)
        // just count.
        DispatchSlot* slot = q.take();
        ASSERT_EQ(slot, &a);
        runs.fetch_add(1);
        q.push(a);
        rearmed.give();
        gate.take(kForever);
        late_push_woke.store(q.push(a));
        q.done(a);
        while ((slot = q.take()) != nullptr) {
            runs.fetch_add(1);
            q.done(*slot);
        }
    });
    ASSERT_TRUE(rearmed.take(1000));
    ASSERT_EQ(q.pending(), 2u);  // the run plus its re-push

    std::atomic<bool> returned{false};
    std::atomic<bool> result{false};
    auto canceller = Task::spawn({.name = "canceller"}, [&] {
        result.store(q.cancel(a, false));
        returned.store(true);
    });
    // The cancel has looked at the slot during run 1 once it unlinked the
    // first re-push.
    bool cancel_started = wisp::test::wait_until([&] { return q.pending() == 1u; }, 1000);
    gate.give();
    ASSERT_TRUE(cancel_started);
    ASSERT_TRUE(wisp::test::wait_until([&] { return returned.load(); }, 1000));
    runner->join();
    canceller->join();
    EXPECT_TRUE(result.load());
    // The re-push after the cancel started is refused: with the list empty
    // an accepted push would report it woke the dispatcher.
    EXPECT_FALSE(late_push_woke.load());
    EXPECT_EQ(runs.load(), 1);
    EXPECT_EQ(a.finished, 0);
    EXPECT_EQ(q.pending(), 0u);

    // The slot is free again: a later schedule queues it.
    EXPECT_TRUE(q.push(a));
    EXPECT_EQ(q.take(), &a);
    q.done(a);
    EXPECT_EQ(a.finished, 0);
}

TEST(SlotQueue, CancelInsideOwnRunWithoutFinishAborts) {
    RestoreDeathStyle restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            DispatchSlot a;
            a.run = [](DispatchSlot&) {};
            SlotQueue q;
            q.push(a);
            q.take();
            (void)q.cancel(a, true);
        },
        "cancelled inside its own run without a finish hook");
}

TEST(ManualDispatcher, RunsOnTheCallingThreadAndSaysSo) {
    ManualDispatcher d;
    struct Probe : DispatchSlot {
        ManualDispatcher* d = nullptr;
        std::vector<bool> seen;
    } probe;
    probe.d = &d;
    probe.run = [](DispatchSlot& s) {
        auto& self = static_cast<Probe&>(s);
        self.seen.push_back(self.d->on_dispatch_thread());
    };
    d.schedule(probe);
    EXPECT_EQ(d.queued(), 1u);
    EXPECT_FALSE(d.on_dispatch_thread());
    EXPECT_EQ(d.run_all(), 1u);
    EXPECT_EQ(probe.seen, std::vector<bool>{true});
    EXPECT_FALSE(d.on_dispatch_thread());
    EXPECT_EQ(d.queued(), 0u);
}

TEST(Dispatcher, DestroyedWithAnAttachedSlotAborts) {
    RestoreDeathStyle restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            ManualDispatcher d;
            d.attach();
        },
        "ManualDispatcher destroyed with 1 attached slot");
}
