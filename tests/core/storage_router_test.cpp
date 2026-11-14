#include "wisp/core/storage_router.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wisp/os/os.hpp"

using wisp::core::IoLane;
using wisp::core::Medium;
using wisp::core::StorageRouter;
using wisp::core::StorageWorker;

namespace {

// Releases a gated job when the test body exits, including by a failed
// ASSERT, so the workers' destructors can join. Declare it after the
// workers and the semaphore before them.
struct ReleaseOnExit {
    wisp::os::Semaphore& sem;
    ~ReleaseOnExit() { sem.give(); }
};

// Joins `t` when the test body exits, including by a failed ASSERT: a
// joinable std::thread's destructor would abort the binary.
struct JoinOnExit {
    std::thread& t;
    ~JoinOnExit() {
        if (t.joinable()) t.join();
    }
};

// Parks `w`'s thread in a job until `release` is given.
void gate(StorageWorker& w, wisp::os::Semaphore& release) {
    wisp::os::Semaphore started(1, 0);
    ASSERT_TRUE(w.submit([&] {
        started.give();
        release.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(started.take(5000));
}

}  // namespace

TEST(StorageRouter, SdJobsRunOnTheSdWorker) {
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);
    EXPECT_EQ(&r.worker_for(Medium::Sd), &sd);

    std::atomic<bool> on_sd{false};
    ASSERT_TRUE(r.submit(Medium::Sd, [&] { on_sd = sd.on_worker(); }));
    ASSERT_TRUE(r.drain(5000));
    EXPECT_TRUE(on_sd);

    on_sd = false;
    ASSERT_TRUE(r.run_sync(Medium::Sd, [&] { on_sd = sd.on_worker(); }, 5000));
    EXPECT_TRUE(on_sd);
}

TEST(StorageRouter, InternalJobsRunOnTheFlashWorker) {
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);
    EXPECT_EQ(&r.worker_for(Medium::Internal), &flash);

    std::atomic<bool> on_flash{false};
    ASSERT_TRUE(r.submit(Medium::Internal, [&] { on_flash = flash.on_worker(); }));
    ASSERT_TRUE(r.drain(5000));
    EXPECT_TRUE(on_flash);

    on_flash = false;
    ASSERT_TRUE(r.run_sync(
        Medium::Internal, [&] { on_flash = flash.on_worker(); }, 5000, IoLane::Interactive));
    EXPECT_TRUE(on_flash);
}

TEST(StorageRouter, WithoutAnSdWorkerSdJobsRunOnTheFlashWorker) {
    StorageWorker flash;
    ASSERT_TRUE(flash.start());
    StorageRouter r(flash);
    EXPECT_EQ(r.sd(), nullptr);
    EXPECT_EQ(&r.worker_for(Medium::Sd), &flash);

    std::atomic<bool> on_flash{false};
    ASSERT_TRUE(r.submit(Medium::Sd, [&] { on_flash = flash.on_worker(); }));
    ASSERT_TRUE(r.drain(5000));
    EXPECT_TRUE(on_flash);
    r.stop_sd();  // a no-op without one
    EXPECT_TRUE(flash.running());
}

TEST(StorageRouter, AfterStopSdSdJobsRunOnTheFlashWorker) {
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);
    r.stop_sd();
    r.stop_sd();  // idempotent
    EXPECT_FALSE(sd.running());
    EXPECT_EQ(&r.worker_for(Medium::Sd), &flash);

    std::atomic<bool> on_flash{false};
    ASSERT_TRUE(r.submit(Medium::Sd, [&] { on_flash = flash.on_worker(); }));
    ASSERT_TRUE(r.drain(5000));
    EXPECT_TRUE(on_flash);

    // Not inline on the caller, as the stopped SD worker's own run_sync()
    // would do.
    on_flash = false;
    ASSERT_TRUE(r.run_sync(Medium::Sd, [&] { on_flash = flash.on_worker(); }, 5000));
    EXPECT_TRUE(on_flash);
}

TEST(StorageRouter, StopSdRunsQueuedSdJobsFirst) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ReleaseOnExit guard{release};
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);
    ASSERT_NO_FATAL_FAILURE(gate(sd, release));

    std::atomic<bool> s1{false};
    ASSERT_TRUE(r.submit(Medium::Sd, [&] { s1 = true; }));
    std::atomic<bool> stopped{false};
    std::atomic<bool> s1_before_return{false};
    std::thread stopper([&] {
        r.stop_sd();
        s1_before_return = s1.load();
        stopped = true;
    });
    // Declared after `stopper`: releases the gate before joining it.
    ReleaseOnExit release_first{release};
    JoinOnExit join{stopper};
    // stop_sd() has begun (the SD worker no longer accepts work) and waits
    // on the gated job; nothing of the SD lane ran yet.
    while (sd.running()) wisp::os::sleep_ms(1);
    EXPECT_FALSE(s1);
    EXPECT_FALSE(stopped);
    // Meanwhile new SD work goes to the flash worker.
    std::atomic<bool> later_on_flash{false};
    EXPECT_TRUE(r.submit(Medium::Sd, [&] { later_on_flash = flash.on_worker(); }));
    EXPECT_TRUE(flash.drain(5000));
    EXPECT_TRUE(later_on_flash);

    release.give();
    stopper.join();
    EXPECT_TRUE(stopped);
    EXPECT_TRUE(s1_before_return);
}

TEST(StorageRouter, DrainWaitsForBothWorkers) {
    wisp::os::Semaphore release_flash(1, 0);
    wisp::os::Semaphore release_sd(1, 0);
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ReleaseOnExit guard_flash{release_flash};
    ReleaseOnExit guard_sd{release_sd};
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);
    ASSERT_NO_FATAL_FAILURE(gate(flash, release_flash));
    ASSERT_NO_FATAL_FAILURE(gate(sd, release_sd));

    EXPECT_FALSE(r.drain(100));
    EXPECT_EQ(r.pending(), 2u);
    release_flash.give();
    EXPECT_FALSE(r.drain(100));
    release_sd.give();
    EXPECT_TRUE(r.drain(5000));
    EXPECT_EQ(r.pending(), 0u);
}

TEST(StorageRouter, OnWorkerCoversBothWorkers) {
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);
    EXPECT_FALSE(r.on_worker());
    std::atomic<int> seen{0};
    ASSERT_TRUE(r.run_sync(Medium::Sd, [&] { seen += r.on_worker() ? 1 : 0; }, 5000));
    ASSERT_TRUE(r.run_sync(Medium::Internal, [&] { seen += r.on_worker() ? 1 : 0; }, 5000));
    EXPECT_EQ(seen.load(), 2);
}

TEST(StorageRouter, CallOnReturnsTheResult) {
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);

    bool on_sd = false;
    auto v = wisp::core::call_on<int>(&r, Medium::Sd, [&] {
        on_sd = sd.on_worker();
        return wisp::Result<int>::ok(7);
    });
    ASSERT_TRUE(v);
    EXPECT_EQ(v.value(), 7);
    EXPECT_TRUE(on_sd);

    bool inline_run = false;
    auto w = wisp::core::call_on<int>(nullptr, Medium::Sd, [&] {
        inline_run = !flash.on_worker() && !sd.on_worker();
        return wisp::Result<int>::ok(8);
    });
    ASSERT_TRUE(w);
    EXPECT_EQ(w.value(), 8);
    EXPECT_TRUE(inline_run);
}

TEST(StorageRouter, AnSdJobRunSyncWhileStoppingRunsInline) {
    wisp::os::Semaphore release(1, 0);
    StorageWorker flash;
    StorageWorker sd(StorageWorker::sd_io_config());
    ReleaseOnExit guard{release};
    ASSERT_TRUE(flash.start());
    ASSERT_TRUE(sd.start());
    StorageRouter r(flash, &sd);

    // Queued before stop_sd(), so it runs while the SD worker drains and no
    // longer reports running().
    wisp::os::Semaphore started(1, 0);
    std::atomic<int> sync_code{-1};
    std::atomic<int> call_code{-1};
    std::atomic<int> inline_runs{0};
    ASSERT_TRUE(r.submit(Medium::Sd, [&] {
        started.give();
        release.take(wisp::os::kForever);
        auto a = r.run_sync(
            Medium::Sd, [&] { inline_runs += sd.on_worker() ? 1 : 0; }, wisp::os::kForever);
        sync_code = a ? 0 : static_cast<int>(a.error().code);
        auto b = wisp::core::call_on<int>(&r, Medium::Sd, [&] {
            inline_runs += sd.on_worker() ? 1 : 0;
            return wisp::Result<int>::ok(3);
        });
        call_code = b ? 0 : static_cast<int>(b.error().code);
    }));
    ASSERT_TRUE(started.take(5000));
    std::thread stopper([&] { r.stop_sd(); });
    ReleaseOnExit release_first{release};
    JoinOnExit join{stopper};
    while (sd.running()) wisp::os::sleep_ms(1);

    release.give();
    stopper.join();
    EXPECT_EQ(sync_code.load(), 0);
    EXPECT_EQ(call_code.load(), 0);
    EXPECT_EQ(inline_runs.load(), 2);
}
