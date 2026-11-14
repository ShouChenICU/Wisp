#include "wisp/core/storage_worker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "wisp/os/flash_guard.hpp"
#include "wisp/os/os.hpp"

using wisp::core::StorageWorker;
using wisp::core::StorageWorkerConfig;

namespace {

// Releases a job blocked on `sem` when the test body exits, including by
// a failed ASSERT, so the worker's destructor (stop()) can join instead of
// hanging until the ctest timeout. Declare it after the worker so it runs
// first, and the semaphore before the worker so it outlives the jobs.
struct ReleaseOnExit {
    wisp::os::Semaphore& sem;
    ~ReleaseOnExit() { sem.give(); }
};

}  // namespace

TEST(StorageWorker, RunsJobsFifoOnItsOwnTask) {
    StorageWorker w(StorageWorkerConfig{.queue_capacity = 128});
    ASSERT_TRUE(w.start());
    EXPECT_FALSE(w.on_worker());

    std::mutex m;
    std::vector<int> order;
    std::atomic<bool> all_on_worker{true};
    for (int i = 1; i <= 100; ++i) {
        ASSERT_TRUE(w.submit([&, i] {
            if (!w.on_worker()) all_on_worker = false;
            std::lock_guard<std::mutex> lock(m);
            order.push_back(i);
        }));
    }
    ASSERT_TRUE(w.drain(5000));

    std::lock_guard<std::mutex> lock(m);
    ASSERT_EQ(order.size(), 100u);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(order[i], i + 1);
    EXPECT_TRUE(all_on_worker);
}

TEST(StorageWorker, SubmitFailsWhenFull) {
    wisp::os::Semaphore started(1, 0);
    wisp::os::Semaphore release(1, 0);
    StorageWorker w(StorageWorkerConfig{.queue_capacity = 2});
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_TRUE(w.submit([&] {
        started.give();
        release.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(started.take(5000));

    EXPECT_TRUE(w.submit([] {}));
    EXPECT_TRUE(w.submit([] {}));
    EXPECT_FALSE(w.submit([] {}));

    release.give();
    EXPECT_TRUE(w.drain(5000));
    EXPECT_TRUE(w.submit([] {}));
}

TEST(StorageWorker, StopRunsQueuedJobs) {
    wisp::os::Semaphore release(1, 0);
    std::atomic<int> ran{0};
    StorageWorker w(StorageWorkerConfig{.queue_capacity = 8});
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_TRUE(w.submit([&] {
        release.take(wisp::os::kForever);
        ++ran;
    }));
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(w.submit([&] { ++ran; }));

    release.give();
    w.stop();
    EXPECT_EQ(ran.load(), 6);
    EXPECT_FALSE(w.submit([] {}));
    w.stop();  // idempotent
}

TEST(StorageWorker, SubmitBeforeStartFails) {
    StorageWorker w;
    EXPECT_FALSE(w.submit([] {}));
    EXPECT_TRUE(w.drain(0));
}

TEST(StorageWorker, StackIsInternal) {
    auto c = StorageWorker::task_config({});
    EXPECT_EQ(c.stack_bytes, 8192u);
    EXPECT_TRUE(c.flash_access);
    EXPECT_STREQ(c.name, "wisp.fs");
    EXPECT_EQ(c.priority, 3);
}

using wisp::core::IoLane;

namespace {

// Starts `w` and parks its thread in a job until `release` is given, so
// later submissions queue up behind it.
void block_worker(StorageWorker& w, wisp::os::Semaphore& release) {
    wisp::os::Semaphore started(1, 0);
    ASSERT_TRUE(w.submit([&] {
        started.give();
        release.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(started.take(5000));
}

}  // namespace

TEST(StorageWorker, NormalJobsOvertakeQueuedBulk) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    std::mutex m;
    std::vector<std::string> order;
    auto record = [&](const char* name) {
        return [&, name] {
            std::lock_guard<std::mutex> lock(m);
            order.emplace_back(name);
        };
    };
    ASSERT_TRUE(w.submit(record("B1"), IoLane::Bulk));
    ASSERT_TRUE(w.submit(record("B2"), IoLane::Bulk));
    ASSERT_TRUE(w.submit(record("N1"), IoLane::Normal));

    release.give();
    ASSERT_TRUE(w.drain(5000));
    std::lock_guard<std::mutex> lock(m);
    EXPECT_EQ(order, (std::vector<std::string>{"N1", "B1", "B2"}));
}

TEST(StorageWorker, BulkQueueHoldsFour) {
    EXPECT_EQ(StorageWorkerConfig{}.bulk_capacity, 4u);
    EXPECT_EQ(StorageWorkerConfig{}.queue_capacity, 32u);
    EXPECT_EQ(wisp::core::kIoChunk, 32u * 1024u);

    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    for (int i = 0; i < 4; ++i) EXPECT_TRUE(w.submit([] {}, IoLane::Bulk));
    EXPECT_FALSE(w.submit([] {}, IoLane::Bulk));
    // A full Bulk lane does not hold up Normal work.
    EXPECT_TRUE(w.submit([] {}));

    release.give();
    EXPECT_TRUE(w.drain(5000));
}

TEST(StorageWorker, RunSyncWaitsForTheJob) {
    StorageWorker w;
    ASSERT_TRUE(w.start());
    bool ran = false;
    bool was_on_worker = false;
    auto r = w.run_sync(
        [&] {
            wisp::os::sleep_ms(20);
            was_on_worker = w.on_worker();
            ran = true;
        },
        5000);
    ASSERT_TRUE(r);
    EXPECT_TRUE(ran);
    EXPECT_TRUE(was_on_worker);

    ran = false;
    ASSERT_TRUE(w.run_sync([&] { ran = w.on_worker(); }, wisp::os::kForever, IoLane::Bulk));
    EXPECT_TRUE(ran);
}

TEST(StorageWorker, RunSyncOnWorkerRunsInline) {
    StorageWorker w;
    ASSERT_TRUE(w.start());
    std::atomic<bool> inner{false};
    std::atomic<bool> inner_ok{false};
    ASSERT_TRUE(w.run_sync(
        [&] {
            auto r = w.run_sync([&] { inner = true; }, 1000);
            inner_ok = r.is_ok();
        },
        5000));
    EXPECT_TRUE(inner);
    EXPECT_TRUE(inner_ok);
}

TEST(StorageWorker, RunSyncBeforeStartRunsInline) {
    StorageWorker w;
    bool ran = false;
    ASSERT_TRUE(w.run_sync([&] { ran = true; }, 0));
    EXPECT_TRUE(ran);
}

TEST(StorageWorker, RunSyncAfterStopRunsInline) {
    StorageWorker w;
    ASSERT_TRUE(w.start());
    w.stop();
    bool ran = false;
    ASSERT_TRUE(w.run_sync([&] { ran = true; }, 0));
    EXPECT_TRUE(ran);
}

TEST(StorageWorker, RunSyncTimeoutStillRunsJobSafely) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    auto data = std::make_shared<std::vector<int>>(1, 7);
    std::weak_ptr<std::vector<int>> watch = data;
    auto seen = std::make_shared<std::atomic<int>>(0);
    auto r = w.run_sync(
        [data, seen] {
            data->push_back(8);
            *seen = static_cast<int>(data->size());
        },
        50);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::Timeout);
    data.reset();  // the caller gives up; the job still owns the vector
    EXPECT_FALSE(watch.expired());

    release.give();
    ASSERT_TRUE(w.drain(5000));
    EXPECT_EQ(seen->load(), 2);
    EXPECT_TRUE(watch.expired());
}

TEST(StorageWorker, RunSyncBusyWhenLaneFull) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker w(StorageWorkerConfig{.queue_capacity = 1, .bulk_capacity = 1});
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));
    ASSERT_TRUE(w.submit([] {}, IoLane::Bulk));

    auto r = w.run_sync([] {}, 1000, IoLane::Bulk);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::Busy);
}

TEST(StorageWorker, RegistersItselfAsFlashThread) {
    EXPECT_TRUE(wisp::os::on_flash_thread());  // nothing registered yet
    wisp::os::Semaphore release(1, 0);
    {
        StorageWorker w;
        ReleaseOnExit guard{release};
        ASSERT_TRUE(w.start());
        std::atomic<bool> inside{false};
        ASSERT_TRUE(w.run_sync([&] { inside = wisp::os::on_flash_thread(); }, 5000));
        EXPECT_TRUE(inside);
        EXPECT_FALSE(wisp::os::on_flash_thread());
        EXPECT_FALSE(w.on_worker());
        w.stop();
        EXPECT_TRUE(wisp::os::on_flash_thread());
    }
    EXPECT_TRUE(wisp::os::on_flash_thread());
}

TEST(StorageWorker, StopRunsQueuedJobsInEveryLane) {
    wisp::os::Semaphore release(1, 0);
    std::atomic<int> ran{0};
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(w.submit([&] { ++ran; }, IoLane::Bulk));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(w.submit([&] { ++ran; }));
    ASSERT_TRUE(w.submit([&] { ++ran; }, IoLane::Interactive));

    release.give();
    w.stop();
    EXPECT_EQ(ran.load(), 8);
    EXPECT_EQ(w.pending(), 0u);
}

namespace {

// Names of the jobs in the order they ran.
struct Recorder {
    std::mutex m;
    std::vector<std::string> order;
    std::function<void()> record(const char* name) {
        return [this, name] {
            std::lock_guard<std::mutex> lock(m);
            order.emplace_back(name);
        };
    }
    std::vector<std::string> get() {
        std::lock_guard<std::mutex> lock(m);
        return order;
    }
};

}  // namespace

TEST(StorageWorker, InteractiveOvertakesQueuedNormalAndBulk) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    Recorder rec;
    ASSERT_TRUE(w.submit(rec.record("B1"), IoLane::Bulk));
    ASSERT_TRUE(w.submit(rec.record("N1"), IoLane::Normal));
    ASSERT_TRUE(w.submit(rec.record("I1"), IoLane::Interactive));
    ASSERT_TRUE(w.submit(rec.record("I2"), IoLane::Interactive));

    release.give();
    ASSERT_TRUE(w.drain(5000));
    EXPECT_EQ(rec.get(), (std::vector<std::string>{"I1", "I2", "N1", "B1"}));
}

TEST(StorageWorker, LowerLanesGetATurnAfterEightJobs) {
    EXPECT_EQ(wisp::core::kLaneTurnAfter, 8u);
    EXPECT_EQ(wisp::core::kLaneCount, 3u);
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    Recorder rec;
    static const char* const kInteractive[] = {"I1", "I2", "I3", "I4", "I5",
                                               "I6", "I7", "I8", "I9"};
    ASSERT_TRUE(w.submit(rec.record("B1"), IoLane::Bulk));
    ASSERT_TRUE(w.submit(rec.record("N1"), IoLane::Normal));
    for (const char* name : kInteractive) {
        ASSERT_TRUE(w.submit(rec.record(name), IoLane::Interactive));
    }

    release.give();
    ASSERT_TRUE(w.drain(5000));
    EXPECT_EQ(rec.get(), (std::vector<std::string>{"I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8",
                                                   "B1", "I9", "N1"}));
}

namespace {

// Submits job `prefix`+`n` on `lane`; when it runs it records its name and
// submits job n + 1 of the same lane, up to `last`, so the lane keeps
// receiving work while the worker drains rather than holding one backlog.
void submit_chain(StorageWorker& w, Recorder& rec, IoLane lane, const char* prefix, int n,
                  int last) {
    ASSERT_TRUE(w.submit(
        [&w, &rec, lane, prefix, n, last] {
            {
                std::lock_guard<std::mutex> lock(rec.m);
                rec.order.push_back(prefix + std::to_string(n));
            }
            if (n < last) submit_chain(w, rec, lane, prefix, n + 1, last);
        },
        lane));
}

std::vector<std::string> names(const char* prefix, int first, int last) {
    std::vector<std::string> out;
    for (int i = first; i <= last; ++i) out.push_back(prefix + std::to_string(i));
    return out;
}

std::vector<std::string> concat(std::initializer_list<std::vector<std::string>> parts) {
    std::vector<std::string> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

}  // namespace

TEST(StorageWorker, ArrivingInteractiveJobsDoNotStarveLowerLanes) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    Recorder rec;
    ASSERT_TRUE(w.submit(rec.record("B1"), IoLane::Bulk));
    ASSERT_TRUE(w.submit(rec.record("N1"), IoLane::Normal));
    ASSERT_NO_FATAL_FAILURE(submit_chain(w, rec, IoLane::Interactive, "I", 1, 12));

    release.give();
    ASSERT_TRUE(w.drain(5000));
    // Bulk runs as the 9th job, Normal as the 11th: a forced Bulk turn
    // does not count toward Normal's.
    EXPECT_EQ(rec.get(), concat({names("I", 1, 8), {"B1", "I9", "N1"}, names("I", 10, 12)}));
}

TEST(StorageWorker, ArrivingInteractiveAndNormalJobsDoNotStarveBulk) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    Recorder rec;
    ASSERT_TRUE(w.submit(rec.record("B1"), IoLane::Bulk));
    ASSERT_TRUE(w.submit(rec.record("B2"), IoLane::Bulk));
    ASSERT_NO_FATAL_FAILURE(submit_chain(w, rec, IoLane::Normal, "N", 1, 6));
    ASSERT_NO_FATAL_FAILURE(submit_chain(w, rec, IoLane::Interactive, "I", 1, 20));

    release.give();
    ASSERT_TRUE(w.drain(5000));
    // With both higher lanes never empty, each Bulk job waits for at most
    // kLaneTurnAfter higher jobs (B2 runs 8 jobs after B1), and Normal
    // still gets its own turns in between.
    EXPECT_EQ(rec.get(), concat({names("I", 1, 8),
                                 {"B1", "I9", "N1"},
                                 names("I", 10, 15),
                                 {"B2", "I16", "I17", "N2"},
                                 names("I", 18, 20),
                                 names("N", 3, 6)}));
}

TEST(StorageWorker, InteractiveRunsRightAfterTheRunningBulkJob) {
    wisp::os::Semaphore release(1, 0);
    wisp::os::Semaphore started(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_TRUE(w.submit(
        [&] {
            started.give();
            release.take(wisp::os::kForever);
        },
        IoLane::Bulk));
    ASSERT_TRUE(started.take(5000));

    Recorder rec;
    ASSERT_TRUE(w.submit(rec.record("N1")));
    ASSERT_TRUE(w.submit(rec.record("N2")));
    ASSERT_TRUE(w.submit(rec.record("N3")));
    ASSERT_TRUE(w.submit(rec.record("I1"), IoLane::Interactive));

    release.give();
    ASSERT_TRUE(w.drain(5000));
    auto order = rec.get();
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order.front(), "I1");
}

TEST(StorageWorker, InteractiveLaneHoldsItsCapacity) {
    EXPECT_EQ(StorageWorkerConfig{}.interactive_capacity, 16u);
    wisp::os::Semaphore release(1, 0);
    StorageWorker w(StorageWorkerConfig{.interactive_capacity = 2});
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));

    EXPECT_TRUE(w.submit([] {}, IoLane::Interactive));
    EXPECT_TRUE(w.submit([] {}, IoLane::Interactive));
    EXPECT_FALSE(w.submit([] {}, IoLane::Interactive));
    auto r = w.run_sync([] {}, 0, IoLane::Interactive);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::Busy);
    // A full Interactive lane does not hold up the other lanes.
    EXPECT_TRUE(w.submit([] {}));
    EXPECT_TRUE(w.submit([] {}, IoLane::Bulk));

    release.give();
    EXPECT_TRUE(w.drain(5000));
}

TEST(StorageWorker, StatsRecordLongestWaitAndJob) {
    std::atomic<uint64_t> clock{0};
    wisp::os::Semaphore release(1, 0);
    StorageWorker w(StorageWorkerConfig{.now_us = [&] { return clock.load(); }});
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    ASSERT_NO_FATAL_FAILURE(block_worker(w, release));  // G, on Normal
    ASSERT_TRUE(w.submit([&] { clock += 3000; }, IoLane::Interactive));
    clock = 7000;

    release.give();
    ASSERT_TRUE(w.drain(5000));
    auto i = w.stats(IoLane::Interactive);
    EXPECT_EQ(i.jobs, 1u);
    EXPECT_EQ(i.max_wait_us, 7000u);
    EXPECT_EQ(i.max_job_us, 3000u);
    auto n = w.stats(IoLane::Normal);
    EXPECT_EQ(n.jobs, 1u);
    EXPECT_EQ(n.max_wait_us, 0u);
    EXPECT_EQ(n.max_job_us, 7000u);
    EXPECT_EQ(w.stats(IoLane::Bulk).jobs, 0u);
}

TEST(StorageWorker, InlineRunsAreNotCounted) {
    StorageWorker w;
    ASSERT_TRUE(w.run_sync([] {}, 0, IoLane::Interactive));
    ASSERT_TRUE(w.run_sync([] {}, 0));
    ASSERT_TRUE(w.start());
    ASSERT_TRUE(w.drain(5000));
    for (IoLane lane : {IoLane::Interactive, IoLane::Normal, IoLane::Bulk}) {
        EXPECT_EQ(w.stats(lane).jobs, 0u);
    }
}
