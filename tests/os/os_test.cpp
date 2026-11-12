#include "wisp/os/os.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "wisp/os/isr_queue.hpp"
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
TEST(IsrQueue, FifoAndTimeout) {
    auto result = IsrQueue::create(2);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    EXPECT_TRUE(q->push_from_isr({7, 1, 0x123456789abcdef0ULL}));
    EXPECT_TRUE(q->push({8, 0, 200}));

    IsrQueueRecord item;
    ASSERT_TRUE(q->pop(item, 0));
    EXPECT_EQ(item.source, 7u);
    EXPECT_EQ(item.value, 1u);
    EXPECT_EQ(item.timestamp_us, 0x123456789abcdef0ULL);
    ASSERT_TRUE(q->pop(item, 0));
    EXPECT_EQ(item.source, 8u);
    EXPECT_EQ(item.value, 0u);
    EXPECT_EQ(item.timestamp_us, 200u);
    EXPECT_FALSE(q->pop(item, 0));

    auto start = now_ms();
    EXPECT_FALSE(q->pop(item, 30));
    EXPECT_GE(now_ms() - start, 25u);
    EXPECT_EQ(q->dropped(), 0u);
}

TEST(IsrQueue, OverflowIsBounded) {
    auto result = IsrQueue::create(2);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    EXPECT_TRUE(q->push_from_isr({1, 1, 100}));
    EXPECT_TRUE(q->push({2, 0, 200}));
    EXPECT_FALSE(q->push_from_isr({3, 1, 300}));
    EXPECT_FALSE(q->push({4, 0, 400}));
    EXPECT_EQ(q->dropped(), 2u);

    IsrQueueRecord item;
    ASSERT_TRUE(q->pop(item, 0));
    EXPECT_EQ(item.source, 1u);
    EXPECT_TRUE(q->push_from_isr({5, 1, 500}));  // wrap the ring
    ASSERT_TRUE(q->pop(item, 0));
    EXPECT_EQ(item.source, 2u);
    ASSERT_TRUE(q->pop(item, 0));
    EXPECT_EQ(item.source, 5u);
    EXPECT_FALSE(q->pop(item, 0));
    EXPECT_EQ(q->dropped(), 2u);
}

TEST(IsrQueue, ResyncDiscardsOldRecordsAndDetectsEveryProducerAttempt) {
    auto result = IsrQueue::create(1);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    ASSERT_TRUE(q->push({1, 1, 100}));
    ASSERT_FALSE(q->push_from_isr({1, 0, 200}));
    EXPECT_EQ(q->begin_resync(), 1u);
    EXPECT_TRUE(q->resync_unchanged());
    IsrQueueRecord record;
    EXPECT_FALSE(q->pop(record, 0));
    q->wake();
    EXPECT_TRUE(q->resync_unchanged());
    ASSERT_TRUE(q->push_from_isr({1, 1, 300}));
    EXPECT_FALSE(q->resync_unchanged());
    ASSERT_FALSE(q->push({1, 0, 400}));
    EXPECT_FALSE(q->resync_unchanged());
    EXPECT_EQ(q->begin_resync(), 2u);
    EXPECT_TRUE(q->resync_unchanged());
    std::thread producer([&] { EXPECT_TRUE(q->push({1, 0, 500})); });
    producer.join();
    EXPECT_FALSE(q->resync_unchanged());
    EXPECT_EQ(q->begin_resync(), 2u);
    EXPECT_TRUE(q->resync_unchanged());
    // An enqueue after successful validation belongs to the new input history.
    ASSERT_TRUE(q->push({1, 1, 600}));
    ASSERT_TRUE(q->pop(record, 0));
    EXPECT_EQ(record.timestamp_us, 600u);
}

TEST(IsrQueue, WakeUnblocksPop) {
    auto result = IsrQueue::create(1);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    Semaphore ready(1, 0);
    Semaphore finished(1, 0);
    auto consumer = Task::spawn({}, [&] {
        IsrQueueRecord item{99, 99, 99};
        ready.give();
        EXPECT_FALSE(q->pop(item, kForever));
        EXPECT_EQ(item.source, 99u);  // wake does not fabricate a record
        finished.give();
    });
    ASSERT_NE(consumer, nullptr);
    EXPECT_TRUE(ready.take(1000));
    q->wake();
    EXPECT_TRUE(finished.take(1000));
    consumer->join();
    EXPECT_EQ(q->dropped(), 0u);
}

TEST(IsrQueue, WakeIsLatchedAndCoalescedWithoutDroppingRecords) {
    auto result = IsrQueue::create(1);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    q->wake();
    q->wake();
    EXPECT_TRUE(q->push({3, 1, 42}));
    IsrQueueRecord item;
    ASSERT_TRUE(q->pop(item, 0));
    EXPECT_EQ(item.source, 3u);
    EXPECT_FALSE(q->pop(item, kForever));  // queued data precedes the pending wake
    auto start = now_ms();
    EXPECT_FALSE(q->pop(item, 30));
    EXPECT_GE(now_ms() - start, 25u);
}

TEST(IsrQueue, ConcurrentTaskConsumer) {
    constexpr uint32_t kCount = 1000;
    auto result = IsrQueue::create(8);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    Semaphore batch_consumed(1, 0);
    std::atomic<bool> records_correct{true};
    auto consumer = Task::spawn({}, [&] {
        for (uint32_t i = 0; i < kCount; ++i) {
            IsrQueueRecord item;
            if (!q->pop(item, 1000) || item.source != i || item.value != i % 2 ||
                item.timestamp_us != uint64_t{10000000000} + i) {
                records_correct = false;
            }
            if (i % 8 == 7) batch_consumed.give();
        }
    });
    ASSERT_NE(consumer, nullptr);
    for (uint32_t i = 0; i < kCount; ++i) {
        IsrQueueRecord item{i, i % 2, uint64_t{10000000000} + i};
        EXPECT_TRUE(i % 2 == 0 ? q->push_from_isr(item) : q->push(item));
        if (i % 8 == 7) EXPECT_TRUE(batch_consumed.take(1000));
    }
    consumer->join();
    EXPECT_TRUE(records_correct.load());
    EXPECT_EQ(q->dropped(), 0u);
}

TEST(IsrQueue, RejectsInvalidCapacity) {
    auto zero = IsrQueue::create(0);
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code, wisp::ErrorCode::InvalidArgument);
    auto overflow = IsrQueue::create(std::numeric_limits<size_t>::max());
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, wisp::ErrorCode::InvalidArgument);
}

TEST(IsrQueue, RepresentableCapacityReachesAllocation) {
    // A host queue must not inherit the signal primitive's count limit:
    // the ring, not a semaphore token per record, bounds its capacity.
    constexpr size_t capacity = std::numeric_limits<uint32_t>::max();
    if (capacity > std::numeric_limits<size_t>::max() / sizeof(IsrQueueRecord)) {
        GTEST_SKIP() << "capacity does not fit this platform's address space";
    }
    wisp::port::sim::set_alloc_failure(Mem::Fast, true);
    auto result = IsrQueue::create(capacity);
    wisp::port::sim::set_alloc_failure(Mem::Fast, false);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, wisp::ErrorCode::NoMemory);
}

TEST(IsrQueue, RequiresFastMemoryButDoesNotAllocateWhileInUse) {
    wisp::port::sim::set_alloc_failure(Mem::Fast, true);
    auto failed = IsrQueue::create(2);
    wisp::port::sim::set_alloc_failure(Mem::Fast, false);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, wisp::ErrorCode::NoMemory);

    wisp::port::sim::set_alloc_failure(Mem::Large, true);
    auto result = IsrQueue::create(2);
    wisp::port::sim::set_alloc_failure(Mem::Large, false);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    wisp::port::sim::set_alloc_failure(Mem::Fast, true);
    wisp::port::sim::set_alloc_failure(Mem::Large, true);
    bool pushed = q->push_from_isr({1, 1, 12}) && q->push({2, 0, 34});
    IsrQueueRecord item;
    bool popped = q->pop(item, 0);
    q->wake();
    wisp::port::sim::set_alloc_failure(Mem::Fast, false);
    wisp::port::sim::set_alloc_failure(Mem::Large, false);
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(popped);
    EXPECT_EQ(item.source, 1u);
}

TEST(IsrQueue, EdgeBindingPreservesSourceAndTimestampWithoutServiceAccess) {
    auto result = IsrQueue::create(2);
    ASSERT_TRUE(result);
    auto q = std::move(result.value());
    auto binding = q->bind_edge(7);
    ASSERT_NE(binding, nullptr);
    EXPECT_TRUE(IsrQueue::EdgeBinding::push_edge(binding.get(), true, 0x123456789abcdef0ULL));
    EXPECT_TRUE(IsrQueue::EdgeBinding::push_edge(binding.get(), false, 99));
    EXPECT_FALSE(IsrQueue::EdgeBinding::push_edge(binding.get(), true, 100));
    IsrQueueRecord record;
    ASSERT_TRUE(q->pop(record, 0));
    EXPECT_EQ(record.source, 7u);
    EXPECT_EQ(record.value, 1u);
    EXPECT_EQ(record.timestamp_us, 0x123456789abcdef0ULL);
    ASSERT_TRUE(q->pop(record, 0));
    EXPECT_EQ(record.value, 0u);
    EXPECT_EQ(record.timestamp_us, 99u);
    EXPECT_EQ(q->dropped(), 1u);
}

TEST(IsrQueue, EdgeBindingAllocationFailureIsRecoverable) {
    auto result = IsrQueue::create(1);
    ASSERT_TRUE(result);
    wisp::port::sim::set_alloc_failure(Mem::Fast, true);
    auto binding = result.value()->bind_edge(0);
    wisp::port::sim::set_alloc_failure(Mem::Fast, false);
    EXPECT_EQ(binding, nullptr);
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

TEST(Os, TimerStartAfterStopFromCallbackRearms) {
    // A callback that stops its own timer leaves it inactive at once (as on
    // ESP-IDF), so a start() from another thread while that callback is
    // still returning arms the timer again instead of being lost.
    Timer* self = nullptr;
    std::atomic<int> calls{0};
    Semaphore stopped(1, 0);
    Timer tm("tm", 5, /*periodic=*/true, [&] {
        if (++calls == 1) {
            self->stop();
            stopped.give();
            sleep_ms(50);
        }
    });
    self = &tm;

    tm.start();
    ASSERT_TRUE(stopped.take(1000));
    tm.start();
    for (int i = 0; i < 200 && calls.load() < 2; ++i) sleep_ms(5);
    EXPECT_GE(calls.load(), 2);
    tm.stop();
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

// A queue's slots go to Mem::Large; a board without it (or with it
// exhausted) still gets a working queue from ordinary memory.
TEST(Queue, SlotsFallBackWithoutLargeMemory) {
    wisp::port::sim::set_alloc_failure(Mem::Large, true);
    Queue<std::string> q(2);
    wisp::port::sim::set_alloc_failure(Mem::Large, false);
    EXPECT_TRUE(q.push("a", 0));
    EXPECT_TRUE(q.push("b", 0));
    EXPECT_FALSE(q.push("c", 0));
    std::string out;
    ASSERT_TRUE(q.pop(out, 0));
    EXPECT_EQ(out, "a");
    ASSERT_TRUE(q.pop(out, 0));
    EXPECT_EQ(out, "b");
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

// Only wisp.fs (core::StorageWorker) opts into flash access (AGENTS.md
// section 5); every other task relies on this default.
TEST(Os, TaskConfigDefaultsToNoFlashAccess) {
    EXPECT_FALSE(TaskConfig{}.flash_access);
    EXPECT_FALSE(TaskConfig{}.hot_stack);
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
