#include "wisp/core/log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "wisp/os/os.hpp"

namespace {

struct CaptureSink : wisp::LogSink {
    std::vector<std::string> lines;
    void write(wisp::LogLevel, const char* tag, const char* msg) override {
        lines.push_back(std::string(tag) + "|" + msg);
    }
};

// A sink that logs again from within its own write(), to exercise the
// reentrancy guard documented on wisp::LogSink.
struct ReentrantSink : wisp::LogSink {
    std::vector<std::string> lines;
    void write(wisp::LogLevel, const char* tag, const char* msg) override {
        lines.push_back(std::string(tag) + "|" + msg);
        WLOG_I("inner", "nested");
    }
};

}  // namespace

TEST(Log, FiltersByLevelAndFormats) {
    wisp::log::reset_for_test();
    wisp::log::init(1024);
    CaptureSink s;
    wisp::log::add_sink(&s);
    wisp::log::set_level(wisp::LogLevel::Info);

    WLOG_D("t", "hidden");
    WLOG_I("t", "v=%d", 3);

    ASSERT_EQ(s.lines.size(), 1u);
    EXPECT_EQ(s.lines[0], "t|v=3");
    EXPECT_NE(wisp::log::ring_snapshot().find(" I t: v=3\n"), std::string::npos);
}

TEST(Log, RingDropsOldestWholeLines) {
    wisp::log::reset_for_test();
    wisp::log::init(200);

    for (int i = 0; i < 50; ++i) {
        WLOG_I("t", "line-%02d", i);
    }

    auto snap = wisp::log::ring_snapshot();
    EXPECT_LE(snap.size(), 200u);
    EXPECT_NE(snap.find("line-49"), std::string::npos);
    EXPECT_EQ(snap.find("line-00"), std::string::npos);
    EXPECT_EQ(snap.back(), '\n');
    EXPECT_TRUE(isdigit(static_cast<unsigned char>(snap.front())));  // first line intact
}

TEST(Log, TruncatesLongMessages) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    CaptureSink s;
    wisp::log::add_sink(&s);

    WLOG_W("t", "%s", std::string(1000, 'x').c_str());

    EXPECT_EQ(s.lines[0].size(), 2u + 255u);
}

TEST(Log, OversizedLineDroppedWithoutClearingRing) {
    wisp::log::reset_for_test();
    wisp::log::init(100);

    WLOG_I("t", "first");
    WLOG_I("t", "second");
    auto before = wisp::log::ring_snapshot();
    ASSERT_NE(before.find("first"), std::string::npos);
    ASSERT_NE(before.find("second"), std::string::npos);

    // Longer than the entire ring: must be dropped outright, leaving the
    // existing contents untouched (not clearing the whole ring).
    WLOG_I("t", "%s", std::string(200, 'y').c_str());

    auto after = wisp::log::ring_snapshot();
    EXPECT_EQ(after, before);
    EXPECT_EQ(after.find('y'), std::string::npos);
}

TEST(Log, SinkReentrancyGuardDropsNestedDeliveryButKeepsRing) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    ReentrantSink s;
    wisp::log::add_sink(&s);

    WLOG_I("outer", "hello");

    // The nested WLOG_I from inside write() must not have been delivered
    // back to the sink (else lines.size() would be >= 2); reaching this
    // point at all also proves it didn't deadlock or recurse unboundedly.
    ASSERT_EQ(s.lines.size(), 1u);
    EXPECT_EQ(s.lines[0], "outer|hello");

    // But the nested message must still have reached the ring buffer.
    EXPECT_NE(wisp::log::ring_snapshot().find("inner: nested\n"), std::string::npos);
}

TEST(Log, ConcurrentLoggingDuringReinitDoesNotCrash) {
    wisp::log::reset_for_test();
    wisp::log::init(512);

    std::atomic<bool> stop{false};
    std::vector<std::unique_ptr<wisp::os::Task>> writers;
    for (int t = 0; t < 4; ++t) {
        writers.push_back(wisp::os::Task::spawn({.name = "log_writer"}, [&stop, t] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                WLOG_I("t", "thread %d msg %d", t, i++);
            }
        }));
    }

    // Repeatedly tear down and reinitialize the ring concurrently with
    // the writers above; this must never crash, hang, or trigger a
    // use-after-free, regardless of in-flight write() calls on other
    // threads/tasks.
    for (int i = 0; i < 200; ++i) {
        wisp::log::reset_for_test();
        wisp::log::init(256 + static_cast<size_t>(i % 4) * 64);
    }
    stop.store(true, std::memory_order_relaxed);

    for (auto& w : writers) {
        w->join();
    }

    SUCCEED();  // Reaching here without crashing/hanging is the point.
}

TEST(Log, RemovedSinkReceivesNothing) {
    wisp::log::reset_for_test();
    wisp::log::init(1024);
    CaptureSink kept;
    CaptureSink removed;
    wisp::log::add_sink(&kept);
    wisp::log::add_sink(&removed);

    WLOG_I("t", "before");
    wisp::log::remove_sink(&removed);
    WLOG_I("t", "after");
    // Removing a sink that is not registered is a no-op.
    wisp::log::remove_sink(&removed);

    EXPECT_EQ(kept.lines, (std::vector<std::string>{"t|before", "t|after"}));
    EXPECT_EQ(removed.lines, (std::vector<std::string>{"t|before"}));
}

TEST(Log, RemoveSinkWaitsForInFlightDelivery) {
    wisp::log::reset_for_test();
    wisp::log::init(1024);

    // A sink whose write() is slow: remove_sink() on another thread must
    // not return while it is still running, so the owner can destroy the
    // sink right after remove_sink() returns.
    struct SlowSink : wisp::LogSink {
        std::atomic<bool> entered{false};
        std::atomic<bool> finished{false};
        void write(wisp::LogLevel, const char*, const char*) override {
            entered = true;
            wisp::os::sleep_ms(200);
            finished = true;
        }
    } sink;
    wisp::log::add_sink(&sink);

    auto writer = wisp::os::Task::spawn({.name = "log_writer"}, [] { WLOG_I("t", "slow"); });
    while (!sink.entered) wisp::os::sleep_ms(1);
    wisp::log::remove_sink(&sink);
    EXPECT_TRUE(sink.finished);
    writer->join();
}
