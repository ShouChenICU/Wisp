#include "wisp/core/log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "wisp/os/os.hpp"

namespace {

struct CaptureSink : wisp::LogSink {
    std::vector<std::string> lines;
    std::vector<wisp::LogEntry> records;
    void write(const wisp::LogRecord& rec) override {
        lines.push_back(std::string(rec.tag) + "|" + rec.msg);
        records.push_back({rec.seq, rec.ts_ms, rec.level, rec.tag, rec.msg});
    }
};

// A sink that logs again from within its own write(), to exercise the
// reentrancy guard documented on wisp::LogSink.
struct ReentrantSink : wisp::LogSink {
    std::vector<std::string> lines;
    void write(const wisp::LogRecord& rec) override {
        lines.push_back(std::string(rec.tag) + "|" + rec.msg);
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

    const auto entries = wisp::log::ring_entries();
    ASSERT_FALSE(entries.empty());
    EXPECT_LT(entries.size(), 50u);
    // Whole lines only: the newest ones, consecutive, each intact.
    EXPECT_EQ(entries.back().msg, "line-49");
    for (size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(entries[i].seq, 50u - entries.size() + 1 + i);
        char want[16];
        std::snprintf(want, sizeof(want), "line-%02u", static_cast<unsigned>(entries[i].seq - 1));
        EXPECT_EQ(entries[i].msg, want);
        EXPECT_EQ(entries[i].tag, "t");
    }
    auto snap = wisp::log::ring_snapshot();
    EXPECT_EQ(snap.find("line-00"), std::string::npos);
    EXPECT_EQ(snap.back(), '\n');
    EXPECT_TRUE(isdigit(static_cast<unsigned char>(snap.front())));  // first line intact
}

// Every accepted line takes the next seq, from 1; a filtered-out one takes
// none, so a gap in seq always means a line was lost, never filtered.
TEST(Log, SeqCountsAcceptedLinesFromOne) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    CaptureSink s;
    wisp::log::add_sink(&s);
    wisp::log::set_level(wisp::LogLevel::Info);

    WLOG_I("t", "a");
    WLOG_D("t", "filtered");
    WLOG_W("t", "b");
    WLOG_E("t", "c");

    ASSERT_EQ(s.records.size(), 3u);
    EXPECT_EQ(s.records[0].seq, 1u);
    EXPECT_EQ(s.records[1].seq, 2u);
    EXPECT_EQ(s.records[2].seq, 3u);

    // init() keeps counting: seq never repeats within a process.
    wisp::log::init(4096);
    WLOG_I("t", "d");
    ASSERT_EQ(s.records.size(), 4u);
    EXPECT_EQ(s.records[3].seq, 4u);
    wisp::log::remove_sink(&s);
}

// A sink gets exactly what the ring stores: same seq, ts, level, tag, msg.
TEST(Log, SinkSeesTheRingsSeqAndTimestamp) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    CaptureSink s;
    wisp::log::add_sink(&s);

    const uint64_t before = wisp::os::now_ms();
    WLOG_I("alpha", "one");
    wisp::os::sleep_ms(3);
    WLOG_W("beta", "two %d", 2);
    const uint64_t after = wisp::os::now_ms();

    const auto ring = wisp::log::ring_entries();
    ASSERT_EQ(ring.size(), 2u);
    ASSERT_EQ(s.records.size(), 2u);
    for (size_t i = 0; i < ring.size(); ++i) {
        EXPECT_EQ(ring[i].seq, s.records[i].seq);
        EXPECT_EQ(ring[i].ts_ms, s.records[i].ts_ms);
        EXPECT_EQ(ring[i].level, s.records[i].level);
        EXPECT_EQ(ring[i].tag, s.records[i].tag);
        EXPECT_EQ(ring[i].msg, s.records[i].msg);
        EXPECT_GE(ring[i].ts_ms, before);
        EXPECT_LE(ring[i].ts_ms, after);
    }
    EXPECT_EQ(ring[0].seq + 1, ring[1].seq);
    EXPECT_LE(ring[0].ts_ms, ring[1].ts_ms);
    EXPECT_EQ(ring[1].level, wisp::LogLevel::Warn);
    EXPECT_EQ(ring[1].tag, "beta");
    EXPECT_EQ(ring[1].msg, "two 2");
    wisp::log::remove_sink(&s);
}

// Tags are cut to 31 bytes, never inside a character; the sink and the ring
// agree on the cut.
TEST(Log, LongTagsAreTruncated) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    CaptureSink s;
    wisp::log::add_sink(&s);

    WLOG_I(std::string(40, 't').c_str(), "ascii");
    // 30 ASCII bytes then a 3-byte character: a plain cut at 31 would split it.
    const std::string wide = std::string(30, 'w') + "\xe4\xb8\xad" + "tail";
    WLOG_I(wide.c_str(), "wide");
    WLOG_I(std::string(31, 'x').c_str(), "exact");

    const auto ring = wisp::log::ring_entries();
    ASSERT_EQ(ring.size(), 3u);
    ASSERT_EQ(s.records.size(), 3u);
    EXPECT_EQ(ring[0].tag, std::string(31, 't'));
    EXPECT_EQ(ring[1].tag, std::string(30, 'w'));
    EXPECT_EQ(ring[2].tag, std::string(31, 'x'));
    for (size_t i = 0; i < ring.size(); ++i) EXPECT_EQ(s.records[i].tag, ring[i].tag) << i;
    wisp::log::remove_sink(&s);
}

// Initializing again with the same size and memory class keeps the ring:
// the simulator logs before App::start() initializes logging a second time.
TEST(Log, ReinitWithSameRingKeepsLines) {
    wisp::log::reset_for_test();
    wisp::log::init(1024);
    WLOG_W("t", "early");

    wisp::log::init(1024);
    WLOG_I("t", "later");
    auto entries = wisp::log::ring_entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].msg, "early");
    EXPECT_EQ(entries[0].seq, 1u);
    EXPECT_EQ(entries[1].seq, 2u);

    // A different size replaces the ring; seq keeps counting.
    wisp::log::init(2048);
    WLOG_I("t", "resized");
    entries = wisp::log::ring_entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].msg, "resized");
    EXPECT_EQ(entries[0].seq, 3u);
}

// A message containing '\n' is one line: one entry, one seq, and evicting
// it leaves no fragment behind.
TEST(Log, MultiLineMessageIsOneEntry) {
    wisp::log::reset_for_test();
    wisp::log::init(160);

    WLOG_I("t", "first\nsecond\nthird");
    auto entries = wisp::log::ring_entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].seq, 1u);
    EXPECT_EQ(entries[0].msg, "first\nsecond\nthird");

    // Push it out: what remains are whole later entries only.
    for (int i = 0; i < 20; ++i) WLOG_I("t", "filler-%02d", i);
    entries = wisp::log::ring_entries();
    ASSERT_FALSE(entries.empty());
    for (const auto& e : entries) {
        EXPECT_EQ(e.msg.rfind("filler-", 0), 0u) << e.msg;
        EXPECT_GE(e.seq, 2u);
    }
    EXPECT_EQ(entries.back().msg, "filler-19");
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
        void write(const wisp::LogRecord&) override {
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

// The 255-byte cut must not split a multi-byte character: the sinks (the
// admin WebSocket among them) and the ring would otherwise carry a
// truncated UTF-8 sequence.
TEST(Log, TruncationKeepsWholeCodePoints) {
    wisp::log::reset_for_test();
    wisp::log::init(2048);
    CaptureSink s;
    wisp::log::add_sink(&s);

    // 1 + 100 * 3 bytes: a plain byte cut at 255 lands inside a character.
    std::string text = "a";
    for (int i = 0; i < 100; ++i) text += "\xe4\xb8\xad";
    WLOG_I("t", "%s", text.c_str());

    ASSERT_EQ(s.lines.size(), 1u);
    const std::string msg = s.lines[0].substr(2);  // drop "t|"
    EXPECT_EQ(msg, text.substr(0, 1 + 84 * 3));
    const std::string ring = wisp::log::ring_snapshot();
    EXPECT_NE(ring.find(": " + msg + "\n"), std::string::npos) << ring;

    // An ASCII message is still cut at exactly 255 bytes.
    s.lines.clear();
    WLOG_I("t", "%s", std::string(300, 'x').c_str());
    ASSERT_EQ(s.lines.size(), 1u);
    EXPECT_EQ(s.lines[0].size(), 2u + 255u);
    wisp::log::remove_sink(&s);
}
