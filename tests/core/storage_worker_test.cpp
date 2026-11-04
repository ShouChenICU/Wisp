#include "wisp/core/storage_worker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "wisp/os/os.hpp"

using wisp::core::StorageWorker;
using wisp::core::StorageWorkerConfig;

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
    StorageWorker w(StorageWorkerConfig{.queue_capacity = 2});
    ASSERT_TRUE(w.start());

    wisp::os::Semaphore started(1, 0);
    wisp::os::Semaphore release(1, 0);
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
    StorageWorker w(StorageWorkerConfig{.queue_capacity = 8});
    ASSERT_TRUE(w.start());

    wisp::os::Semaphore release(1, 0);
    std::atomic<int> ran{0};
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
    EXPECT_EQ(c.stack_bytes, 4096u);
    EXPECT_TRUE(c.flash_access);
    EXPECT_STREQ(c.name, "wisp.fs");
    EXPECT_EQ(c.priority, 3);
}
