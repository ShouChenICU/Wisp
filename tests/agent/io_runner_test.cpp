// IoRunner (spec 3.9): ToolThread::Io's executor, one wisp.tool.io task per
// call, at most two at once, finished tasks joined by the next post() and by
// join_all().

#include "wisp/agent/io_runner.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>

#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/value.hpp"
#include "wisp/os/flash_guard.hpp"
#include "wisp/os/os.hpp"

using wisp::Map;
using wisp::Value;
using wisp::agent::IoRunner;
using wisp::agent::ToolCtx;
using wisp::agent::ToolDef;
using wisp::agent::ToolExecutors;
using wisp::agent::ToolOutput;
using wisp::agent::ToolRegistry;
using wisp::agent::ToolThread;
using wisp::os::kForever;
using wisp::os::Semaphore;

namespace {

// Polls `pred` for up to two seconds.
template <typename Pred>
bool eventually(Pred pred) {
    for (int i = 0; i < 400; ++i) {
        if (pred()) return true;
        wisp::os::sleep_ms(5);
    }
    return pred();
}

std::string error_of(const ToolOutput& out) {
    const Value* e = out.result.find("error");
    return e != nullptr ? e->as_string() : std::string("(none)");
}

}  // namespace

TEST(IoRunner, RunsOnItsOwnTask) {
    IoRunner runner;
    const std::thread::id self = std::this_thread::get_id();
    for (int i = 0; i < 2; ++i) {
        std::thread::id seen;
        Semaphore ran(1, 0);
        ASSERT_TRUE(runner.post([&] {
            seen = std::this_thread::get_id();
            ran.give();
        }));
        ASSERT_TRUE(ran.take(2000));
        EXPECT_NE(seen, self);
    }
    runner.join_all();
}

TEST(IoRunner, FinishedTasksAreJoinedByTheNextPost) {
    IoRunner runner;
    std::atomic<int> ran{0};
    ASSERT_TRUE(runner.post([&] { ran.fetch_add(1); }));
    ASSERT_TRUE(eventually([&] { return ran.load() == 1; }));
    // Finished, but nothing joined it yet.
    wisp::os::sleep_ms(20);
    EXPECT_EQ(runner.running(), 1u);

    ASSERT_TRUE(runner.post([&] { ran.fetch_add(1); }));
    ASSERT_TRUE(eventually([&] { return ran.load() == 2; }));
    // The done flag is set only after the job returns; give the second task time
    // to finish, or it still runs beside the third.
    wisp::os::sleep_ms(20);
    ASSERT_TRUE(runner.post([] {}));
    EXPECT_LE(runner.running(), 1u);

    runner.join_all();
    EXPECT_EQ(runner.running(), 0u);
}

TEST(IoRunner, TwoRunningRefuseAThird) {
    IoRunner runner;
    Semaphore release(2, 0);
    std::atomic<int> finished{0};
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(runner.post([&] {
            release.take(kForever);
            finished.fetch_add(1);
        }));
    }
    EXPECT_FALSE(runner.post([] {}));
    EXPECT_EQ(runner.running(), 2u);

    release.give();
    release.give();
    ASSERT_TRUE(eventually([&] { return finished.load() == 2; }));
    // The flag is set after the job returns; a slot frees only then.
    ASSERT_TRUE(eventually([&] { return runner.post([] {}); }));
    runner.join_all();
    EXPECT_EQ(runner.running(), 0u);
}

TEST(IoRunner, RegistryAnswersBusyPastTwo) {
    IoRunner runner;
    ToolRegistry reg(ToolExecutors{
        .ui = {},
        .fs = {},
        .io = [&runner](std::function<void()> fn) { return runner.post(std::move(fn)); },
        .fs_interactive = {},
    });
    Semaphore release(4, 0);
    ToolDef d;
    d.name = "t.io";
    d.description = "blocks";
    d.thread = ToolThread::Io;
    d.timeout_ms = 50;
    d.run = [&release](const Value&, ToolCtx&) {
        release.take(kForever);
        ToolOutput out;
        out.result = Value(Map{{"ok", true}});
        return out;
    };
    ASSERT_TRUE(reg.add(std::move(d)));
    ToolCtx c;
    c.cancelled = [] { return false; };

    EXPECT_EQ(error_of(reg.call("t_io", "", c)), "timeout");
    EXPECT_EQ(error_of(reg.call("t_io", "", c)), "timeout");
    EXPECT_EQ(error_of(reg.call("t_io", "", c)), "busy");

    release.give();
    release.give();
    runner.join_all();
    EXPECT_EQ(runner.running(), 0u);
    release.give();  // call 4 finishes at once
    EXPECT_TRUE(reg.call("t_io", "", c).ok);
    runner.join_all();
}

TEST(IoRunner, CancelledCallIsAbandonedAndJoinedLater) {
    IoRunner runner;
    ToolRegistry reg(ToolExecutors{
        .ui = {},
        .fs = {},
        .io = [&runner](std::function<void()> fn) { return runner.post(std::move(fn)); },
        .fs_interactive = {},
    });
    Semaphore release(1, 0);
    std::atomic<bool> saw_cancel{false};
    ToolDef d;
    d.name = "t.io";
    d.description = "blocks";
    d.thread = ToolThread::Io;
    d.timeout_ms = 5000;
    d.run = [&](const Value&, ToolCtx& ctx) {
        release.take(kForever);
        saw_cancel = ctx.cancelled();
        ToolOutput out;
        out.result = Value(Map{{"ok", true}});
        return out;
    };
    ASSERT_TRUE(reg.add(std::move(d)));
    std::atomic<bool> stop{false};
    ToolCtx c;
    c.cancelled = [&stop] { return stop.load(); };
    std::thread canceller([&] {
        wisp::os::sleep_ms(30);
        stop = true;
    });
    EXPECT_EQ(error_of(reg.call("t_io", "", c)), "cancelled");
    canceller.join();
    EXPECT_EQ(runner.running(), 1u);

    release.give();
    runner.join_all();
    EXPECT_EQ(runner.running(), 0u);
    // The abandoned job sees itself cancelled once call() gave up.
    EXPECT_TRUE(saw_cancel.load());
}

TEST(IoRunner, AbandonedCallIsJoinedLater) {
    IoRunner runner;
    ToolRegistry reg(ToolExecutors{
        .ui = {},
        .fs = {},
        .io = [&runner](std::function<void()> fn) { return runner.post(std::move(fn)); },
        .fs_interactive = {},
    });
    Semaphore release(1, 0);
    std::atomic<bool> finished{false};
    ToolDef d;
    d.name = "t.io";
    d.description = "blocks";
    d.thread = ToolThread::Io;
    d.timeout_ms = 50;
    d.run = [&](const Value&, ToolCtx&) {
        release.take(kForever);
        finished = true;
        ToolOutput out;
        out.result = Value(Map{{"ok", true}});
        return out;
    };
    ASSERT_TRUE(reg.add(std::move(d)));
    ToolCtx c;
    c.cancelled = [] { return false; };
    EXPECT_EQ(error_of(reg.call("t_io", "", c)), "timeout");
    EXPECT_FALSE(finished.load());
    EXPECT_EQ(runner.running(), 1u);

    release.give();
    runner.join_all();
    EXPECT_TRUE(finished.load());
    EXPECT_EQ(runner.running(), 0u);
    runner.join_all();  // a second join_all joins nothing twice
    EXPECT_EQ(runner.running(), 0u);
}

TEST(IoRunner, DestructorJoinsARunningTask) {
    std::atomic<bool> finished{false};
    {
        IoRunner runner;
        ASSERT_TRUE(runner.post([&] {
            wisp::os::sleep_ms(50);
            finished = true;
        }));
    }
    EXPECT_TRUE(finished.load());
}

TEST(IoRunner, TaskConfigNeverTouchesFlash) {
    const wisp::os::TaskConfig cfg = IoRunner::task_config();
    EXPECT_STREQ(cfg.name, "wisp.tool.io");
    EXPECT_EQ(cfg.stack_bytes, 16384u);
    EXPECT_EQ(cfg.priority, 3);
    EXPECT_FALSE(cfg.flash_access);
    EXPECT_FALSE(cfg.hot_stack);
}

TEST(IoRunnerDeathTest, FlashCallFromIoAborts) {
    // Another thread stays registered as the flash thread across the death
    // test, so it runs in the "threadsafe" style (re-executes the binary);
    // the style is restored for the tests after this one.
    struct RestoreStyle {
        std::string old = GTEST_FLAG_GET(death_test_style);
        ~RestoreStyle() { GTEST_FLAG_SET(death_test_style, old); }
    } restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Semaphore registered(1, 0);
    Semaphore release(1, 0);
    std::thread owner([&] {
        wisp::os::set_flash_thread();
        registered.give();
        release.take(kForever);
        wisp::os::clear_flash_thread();
    });
    registered.take(kForever);

    EXPECT_DEATH(
        {
            IoRunner runner;
            runner.post([] { WISP_ASSERT_FLASH_THREAD("Settings::set"); });
            runner.join_all();
        },
        "flash access off the flash thread");

    release.give();
    owner.join();
}
