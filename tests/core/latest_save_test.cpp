#include "wisp/core/latest_save.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include "support/fake_io_queue.hpp"
#include "wisp/base/error.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::core::LatestSave;
using wisp::test::FakeIoQueue;

namespace {

LatestSave::Write writes(std::vector<int>& log, int value, bool ok = true) {
    return [&log, value, ok]() -> Result<void> {
        log.push_back(value);
        return ok ? Result<void>::ok() : Result<void>::err(ErrorCode::IoError, "injected");
    };
}

}  // namespace

TEST(LatestSave, ReportsTheThirdFailureInARowOnce) {
    LatestSave saver;
    int reports = 0;
    saver.set_on_persist_failed([&] { ++reports; });
    std::vector<int> log;
    EXPECT_FALSE(saver.save(writes(log, 1, false)));
    EXPECT_FALSE(saver.save(writes(log, 2, false)));
    EXPECT_EQ(reports, 0);
    EXPECT_FALSE(saver.save(writes(log, 3, false)));
    EXPECT_EQ(reports, 1);
    EXPECT_FALSE(saver.save(writes(log, 4, false)));
    EXPECT_EQ(reports, 1);
    // A success starts the count again.
    EXPECT_TRUE(saver.save(writes(log, 5)));
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(saver.save(writes(log, 6 + i, false)));
    EXPECT_EQ(reports, 2);
}

TEST(LatestSave, CountsFailuresOfQueuedWrites) {
    FakeIoQueue io;
    LatestSave saver(io.submit_io());
    int reports = 0;
    saver.set_on_persist_failed([&] { ++reports; });
    std::vector<int> log;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(saver.save(writes(log, i, false)));
        EXPECT_EQ(io.run_all(), 1u);
    }
    EXPECT_EQ(log, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(reports, 1);
}

TEST(LatestSave, LastSnapshotWins) {
    FakeIoQueue io;
    LatestSave saver(io.submit_io());
    std::vector<int> log;
    ASSERT_TRUE(saver.save(writes(log, 1)));
    ASSERT_TRUE(saver.save(writes(log, 2)));
    ASSERT_TRUE(saver.save(writes(log, 3)));
    EXPECT_TRUE(saver.pending());
    EXPECT_EQ(io.run_all(), 1u);
    EXPECT_EQ(log, (std::vector<int>{3}));
    EXPECT_FALSE(saver.pending());
}

// A save() that comes in while another one's submit is being refused got
// Ok, counting on that job: the refused one submits again for it.
TEST(LatestSave, RefusedSubmitRetriesForANewerSave) {
    FakeIoQueue io;
    LatestSave* self = nullptr;
    std::vector<int> log;
    int calls = 0;
    auto inner = io.submit_io();
    LatestSave saver([&](std::function<void()> job) {
        if (++calls == 1) {
            // Another thread's save() lands while this submit is refused.
            EXPECT_TRUE(self->save(writes(log, 2)));
            return false;
        }
        return inner(std::move(job));
    });
    self = &saver;
    EXPECT_TRUE(saver.save(writes(log, 1)));
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(io.run_all(), 1u);
    EXPECT_EQ(log, (std::vector<int>{2}));
}

TEST(LatestSave, RefusedSubmitIsBusyAndQueuesNothing) {
    FakeIoQueue io;
    io.refuse = true;
    LatestSave saver(io.submit_io());
    std::vector<int> log;
    auto r = saver.save(writes(log, 1));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);
    EXPECT_FALSE(saver.pending());
    io.refuse = false;
    ASSERT_TRUE(saver.save(writes(log, 2)));
    EXPECT_EQ(io.run_all(), 1u);
    EXPECT_EQ(log, (std::vector<int>{2}));
}
