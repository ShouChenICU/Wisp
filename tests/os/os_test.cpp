#include "wisp/os/os.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "wisp/os/queue.hpp"
#include "wisp/port/sim/sim_os.hpp"

using namespace wisp::os;

TEST(Os, ClockIsMonotonic) {
    auto a = now_ms();
    sleep_ms(20);
    EXPECT_GE(now_ms() - a, 19u);
}
TEST(Os, MicrosecondClockSharesTheMillisecondBase) {
    uint64_t ms_before = now_ms();
    uint64_t us = now_us();
    uint64_t ms_after = now_ms();
    EXPECT_GE(us / 1000, ms_before);
    EXPECT_LE(us / 1000, ms_after);
    sleep_ms(5);
    EXPECT_GE(now_us() - us, 4000u);
}
TEST(Os, ClockCountsFromProcessStart) {
    // The simulator's boot is process start (os.hpp). ctest gives this
    // binary a 10 s timeout; a host-boot epoch would put the value at the
    // host's uptime, far above this bound on any machine up for more than
    // ten minutes.
    EXPECT_LT(now_ms(), 10u * 60u * 1000u);
}
TEST(Os, SemaphoreTimeout) {
    Semaphore s(1, 0);
    auto t0 = now_ms();
    EXPECT_FALSE(s.take(50));
    EXPECT_GE(now_ms() - t0, 45u);
    s.give();
    EXPECT_TRUE(s.take(0));
}
TEST(Os, TaskRunsAndJoins) {
    std::atomic<int> x{0};
    auto t = Task::spawn({.name = "t"}, [&] { x = 1; });
    t->join();
    EXPECT_EQ(x, 1);
}
TEST(Os, TaskIsCurrent) {
    std::atomic<bool> inside{false};
    std::unique_ptr<Task> t;
    Semaphore ready(1, 0);
    t = Task::spawn({}, [&] {
        ready.take(kForever);
        inside = t->is_current();
    });
    ready.give();
    t->join();
    EXPECT_TRUE(inside);
    EXPECT_FALSE(t->is_current());
}
TEST(Os, PeriodicTimerFiresRepeatedly) {
    std::atomic<int> n{0};
    Timer tm("tm", 10, true, [&] { ++n; });
    tm.start();
    sleep_ms(105);
    tm.stop();
    int seen = n;
    EXPECT_GE(seen, 8);
    sleep_ms(30);
    EXPECT_EQ(n, seen);
}
TEST(Os, TimerCreateCanFail) {
    std::atomic<int> n{0};
    auto tm = Timer::create("tm", 10, false, [&] { ++n; });
    ASSERT_NE(tm, nullptr);
    tm->start();
    sleep_ms(50);
    EXPECT_EQ(n.load(), 1);
    wisp::port::sim::set_timer_failure(true);
    EXPECT_EQ(Timer::create("tm", 10, false, [] {}), nullptr);
    wisp::port::sim::set_timer_failure(false);
    EXPECT_NE(Timer::create("tm", 10, false, [] {}), nullptr);
}
TEST(Queue, FifoAndTimeout) {
    Queue<std::string> q(2);
    EXPECT_TRUE(q.push("a", 0));
    EXPECT_TRUE(q.push("b", 0));
    EXPECT_FALSE(q.push("c", 10));
    std::string s;
    EXPECT_TRUE(q.pop(s, 0));
    EXPECT_EQ(s, "a");
}
TEST(Queue, DropOldestWhenFull) {
    Queue<int> q(2);
    q.push(1, 0);
    q.push(2, 0);
    EXPECT_TRUE(q.push_drop_oldest(3));
    int v;
    q.pop(v, 0);
    EXPECT_EQ(v, 2);
    q.pop(v, 0);
    EXPECT_EQ(v, 3);
}
TEST(Os, AllocKinds) {
    for (auto m : {Mem::Fast, Mem::Large, Mem::Dma, Mem::DmaLarge}) {
        void* p = alloc(64, m);
        ASSERT_NE(p, nullptr);
        dealloc(p);
    }
}

// --- Fix-round-1 coverage ---------------------------------------------
//
// Added in response to code review of the first implementation: a race in
// Queue::push_drop_oldest(), a deadlock in Timer::stop() when called from
// within its own callback, and a race between Timer::start()/stop() and
// the destructor over the timer thread.

TEST(Os, TimerStopFromCallbackDoesNotDeadlock) {
    // cb() calls stop() on its own Timer. Joining a thread with itself
    // would be undefined behavior (std::terminate under -fno-exceptions);
    // stop() must detect this and return without blocking, guaranteeing
    // cb never fires again after this particular invocation returns.
    Timer* self = nullptr;
    std::atomic<int> calls{0};
    std::atomic<bool> stopped_from_cb{false};
    Timer tm("tm", 5, /*periodic=*/true, [&] {
        ++calls;
        self->stop();
        stopped_from_cb = true;
    });
    self = &tm;

    tm.start();
    sleep_ms(100);

    EXPECT_TRUE(stopped_from_cb.load());
    int seen = calls.load();
    EXPECT_EQ(seen, 1);
    sleep_ms(30);
    EXPECT_EQ(calls.load(), seen);
    EXPECT_FALSE(tm.active());
}

TEST(Os, TimerDestructorGuaranteesCallbackStopped) {
    std::atomic<int> calls{0};
    {
        Timer tm("tm", 5, /*periodic=*/true, [&] {
            ++calls;
            sleep_ms(5);
        });
        tm.start();
        sleep_ms(30);
        // ~Timer() runs here and must block until any in-flight cb()
        // invocation has returned.
    }
    int seen = calls.load();
    EXPECT_GT(seen, 0);
    sleep_ms(30);
    EXPECT_EQ(calls.load(), seen);
}

TEST(Os, TimerStopFromCallbackConcurrentWithExternalStop) {
    // Fix-round-2 regression test: an external thread's stop() call must
    // never block while holding a lock that the timer's own
    // callback-internal stop() call also needs to make progress — that
    // was exactly the round-1 fix's new deadlock (external stop() holding
    // control_mtx while joining the timer thread, while cb()'s
    // self->stop() blocked trying to take that same control_mtx). Both
    // must recognize "this is the timer's own thread" before ever
    // touching that lock. If this regresses, the test hangs; the os_test
    // binary has a ctest TIMEOUT set so the suite fails this test instead
    // of hanging forever.
    Timer* self = nullptr;
    std::atomic<bool> in_cb{false};
    Timer tm("t", 5, /*periodic=*/true, [&] {
        in_cb = true;
        sleep_ms(50);
        self->stop();
    });
    self = &tm;
    tm.start();

    while (!in_cb.load()) {
        sleep_ms(1);
    }
    // cb() is now in the middle of its sleep_ms(50); calling stop() here,
    // from this (external) thread, races the scenario above.
    tm.stop();

    EXPECT_FALSE(tm.active());
}

TEST(Os, TimerStopFromCallbackConcurrentWithDestructor) {
    // Same scenario as above, but the external side is the destructor
    // instead of an explicit stop() call.
    std::atomic<bool> in_cb{false};
    {
        Timer* self = nullptr;
        Timer tm("t", 5, /*periodic=*/true, [&] {
            in_cb = true;
            sleep_ms(50);
            self->stop();
        });
        self = &tm;
        tm.start();

        while (!in_cb.load()) {
            sleep_ms(1);
        }
        // ~Timer() runs here while cb() is still mid-sleep.
    }
    SUCCEED();
}

TEST(Os, TimerStartStopHammeredFromTwoThreads) {
    // Regression test for a race between start()/stop()/destructor over
    // the timer's thread handle; must not crash or hang.
    std::atomic<int> calls{0};
    Timer tm("tm", 2, /*periodic=*/true, [&] { ++calls; });

    std::atomic<bool> keep_going{true};
    auto starter = Task::spawn({.name = "starter"}, [&] {
        while (keep_going.load()) {
            tm.start();
        }
    });
    auto stopper = Task::spawn({.name = "stopper"}, [&] {
        for (int i = 0; i < 300; ++i) {
            tm.stop();
        }
    });

    stopper->join();
    keep_going = false;
    starter->join();
    tm.stop();

    int seen = calls.load();
    sleep_ms(30);
    EXPECT_EQ(calls.load(), seen);
    EXPECT_FALSE(tm.active());
}

TEST(Queue, ZeroCapacityAsserts) {
    // EXPECT_DEBUG_DEATH (rather than EXPECT_DEATH) because the assert()
    // in Queue's constructor compiles away entirely when NDEBUG is
    // defined; in that configuration this macro just runs the statement
    // normally instead of expecting a crash.
    EXPECT_DEBUG_DEATH({ Queue<int> q(0); }, "");
}

// A queue lives as long as its owner, so its slots belong in Mem::Large;
// LargeAllocator aborts when that runs out.
TEST(Queue, SlotsComeFromLargeMemory) {
    wisp::port::sim::set_alloc_failure(Mem::Large, true);
    EXPECT_DEATH({ Queue<int> q(4); }, "");
    wisp::port::sim::set_alloc_failure(Mem::Large, false);
    Queue<int> q(4);
    EXPECT_TRUE(q.push(1, 0));
}

TEST(Queue, PushDropOldestRaceWithConcurrentPops) {
    // Regression test: push_drop_oldest() used to ignore the return value
    // of filled_slots_.take(0) when racing with a concurrent pop() that
    // had already claimed the same removal right, corrupting tail_/size_
    // (and eventually handing a consumer a moved-from value). Payloads are
    // std::string (not int) so a moved-from value is actually observable
    // as an empty string, rather than silently copying an unmoved int.
    // With the fix, every element ever pushed is accounted for by exactly
    // one of "popped" or "dropped", and size() never goes negative/corrupt.
    constexpr int kCapacity = 1;
    constexpr int kConsumers = 4;
    constexpr int kItems = 3000;

    Queue<std::string> q(kCapacity);
    std::atomic<int> dropped_count{0};
    std::atomic<int> popped_count{0};
    std::atomic<bool> producer_done{false};

    std::vector<std::unique_ptr<Task>> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.push_back(Task::spawn({.name = "consumer"}, [&] {
            while (true) {
                std::string v;
                if (q.pop(v, 2)) {
                    EXPECT_FALSE(v.empty());  // never a moved-from value
                    ++popped_count;
                    continue;
                }
                if (producer_done.load() && q.size() == 0) {
                    break;
                }
            }
        }));
    }

    for (int i = 0; i < kItems; ++i) {
        if (q.push_drop_oldest(std::to_string(i))) {
            ++dropped_count;
        }
    }
    producer_done = true;

    for (auto& c : consumers) {
        c->join();
    }

    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(dropped_count.load() + popped_count.load(), kItems);
}

// A task declaring no flash access runs on an ordinary stack on a port
// without external RAM.
TEST(Os, NoFlashAccessTaskRunsAndJoins) {
    std::atomic<bool> ran{false};
    auto task = wisp::os::Task::spawn({.name = "ext", .stack_bytes = 4096, .flash_access = false},
                                      [&] { ran = true; });
    ASSERT_NE(task, nullptr);
    task->join();
    EXPECT_TRUE(ran);
}
