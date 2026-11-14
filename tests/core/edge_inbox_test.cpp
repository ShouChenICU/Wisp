#include "wisp/core/edge_inbox.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/manual_dispatcher.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"

using wisp::core::DispatchSlot;
using wisp::core::EdgeInbox;
using wisp::core::EventBus;
using wisp::test::ManualDispatcher;

namespace {

struct Edge {
    int v;
};
// One name for the common size, so it fits in a macro argument.
using Inbox8 = EdgeInbox<Edge, 8>;

// A correct run reaches each gate in milliseconds; a wrong one fails the
// test at this bound instead of hanging it.
constexpr uint32_t kGateMs = 2000;

struct CaptureSink : wisp::LogSink {
    std::vector<std::string> lines;
    void write(const wisp::LogRecord& rec) override { lines.push_back(rec.msg); }
};

struct RestoreDeathStyle {
    std::string old = GTEST_FLAG_GET(death_test_style);
    ~RestoreDeathStyle() { GTEST_FLAG_SET(death_test_style, old); }
};

// Declared after a test's threads (and before it starts them): on every
// way out of the test, a failed ASSERT included, it opens the gate and
// joins the threads, so the failure is reported by gtest instead of
// std::terminate on a joinable thread.
class GateAndJoin {
public:
    GateAndJoin(wisp::os::Semaphore& gate, std::vector<std::thread>& threads, int opens = 1)
        : gate_(gate), threads_(threads), opens_(opens) {}
    ~GateAndJoin() {
        for (int i = 0; i < opens_; ++i) {
            gate_.give();
        }
        for (std::thread& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    GateAndJoin(const GateAndJoin&) = delete;
    GateAndJoin& operator=(const GateAndJoin&) = delete;

private:
    wisp::os::Semaphore& gate_;
    std::vector<std::thread>& threads_;
    int opens_;
};

}  // namespace

TEST(EdgeInbox, DeliversEveryEdgeInOrder) {
    ManualDispatcher md;
    std::vector<int> seen;
    EdgeInbox<Edge, 8> inbox("test", md, [&](const Edge& e) { seen.push_back(e.v); });
    inbox.push({1});
    inbox.push({2});
    inbox.push({3});
    md.run_all();
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(inbox.overflows(), 0u);
    EXPECT_STREQ(inbox.name(), "test");
}

TEST(EdgeInbox, HandlerRunsOnlyOnItsDispatcher) {
    ManualDispatcher md;
    std::vector<int> seen;
    std::vector<bool> on_dispatcher;
    EdgeInbox<Edge, 8> inbox("test", md, [&](const Edge& e) {
        seen.push_back(e.v);
        on_dispatcher.push_back(md.on_dispatch_thread());
    });
    inbox.push({1});
    inbox.push({2});
    EXPECT_TRUE(seen.empty());
    EXPECT_EQ(md.run_all(), 1u);
    EXPECT_EQ(seen, (std::vector<int>{1, 2}));
    EXPECT_EQ(on_dispatcher, (std::vector<bool>{true, true}));
}

TEST(EdgeInbox, OverflowDropsTheOldestAndCountsIt) {
    ManualDispatcher md;
    std::vector<int> seen;
    EdgeInbox<Edge, 4> inbox("test", md, [&](const Edge& e) { seen.push_back(e.v); });
    for (int v = 1; v <= 6; ++v) {
        inbox.push({v});
    }
    EXPECT_EQ(inbox.overflows(), 2u);
    md.run_all();
    EXPECT_EQ(seen, (std::vector<int>{3, 4, 5, 6}));
}

TEST(EdgeInbox, OverflowWarningIsThrottled) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    CaptureSink sink;
    wisp::log::add_sink(&sink);
    {
        ManualDispatcher md;
        EdgeInbox<Edge, 4> inbox("test", md, [](const Edge&) {});
        for (int v = 0; v < 134; ++v) {
            inbox.push({v});
        }
        EXPECT_EQ(inbox.overflows(), 130u);
        md.run_all();
    }
    wisp::log::remove_sink(&sink);
    size_t warnings = 0;
    for (const std::string& line : sink.lines) {
        if (line.find("edge inbox test overflowed") != std::string::npos) {
            ++warnings;
        }
    }
    EXPECT_EQ(warnings, 3u);  // the 1st, 64th and 128th drop
    wisp::log::reset_for_test();
}

TEST(EdgeInbox, ConcurrentProducersKeepTheirOwnOrder) {
    EventBus bus;
    ASSERT_TRUE(bus.start().is_ok());
    std::mutex mu;
    std::vector<int> seen;
    {
        EdgeInbox<Edge, 256> inbox("test", bus, [&](const Edge& e) {
            std::lock_guard<std::mutex> lock(mu);
            seen.push_back(e.v);
        });
        constexpr int kThreads = 4;
        constexpr int kEach = 50;
        wisp::os::Semaphore gate(kThreads, 0);
        std::vector<std::thread> threads;
        {
            GateAndJoin guard(gate, threads, kThreads);
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t] {
                    // Every producer starts at once, so their pushes interleave.
                    if (!gate.take(kGateMs)) {
                        return;
                    }
                    for (int i = 0; i < kEach; ++i) {
                        inbox.push({t * 1000 + i});
                    }
                });
            }
        }
        ASSERT_TRUE(bus.wait_idle(2000));
        EXPECT_EQ(inbox.overflows(), 0u);
        std::lock_guard<std::mutex> lock(mu);
        ASSERT_EQ(seen.size(), static_cast<size_t>(kThreads * kEach));
        std::vector<int> next(kThreads, 0);
        for (int v : seen) {
            int t = v / 1000;
            ASSERT_GE(t, 0);
            ASSERT_LT(t, kThreads);
            EXPECT_EQ(v % 1000, next[t]) << "thread " << t << " out of order";
            next[t] = v % 1000 + 1;
        }
    }
    bus.stop();
}

TEST(EdgeInbox, EdgesPushedByTheHandlerAreDeliveredInTheNextRun) {
    ManualDispatcher md;
    std::vector<int> seen;
    EdgeInbox<Edge, 8>* self = nullptr;
    EdgeInbox<Edge, 8> inbox("test", md, [&](const Edge& e) {
        seen.push_back(e.v);
        if (e.v < 10) {
            self->push({e.v + 10});
        }
    });
    self = &inbox;
    inbox.push({1});
    EXPECT_EQ(md.run_all(), 2u);
    EXPECT_EQ(seen, (std::vector<int>{1, 11}));
}

// A handler that pushes an edge every time it runs would hold the
// dispatcher forever if a run drained until empty; a run delivers only the
// edges waiting when it started, so other slots get their turn.
TEST(EdgeInbox, AHandlerThatAlwaysPushesDoesNotHoldItsDispatcher) {
    ManualDispatcher md;
    std::vector<int> seen;
    EdgeInbox<Edge, 8>* self = nullptr;
    EdgeInbox<Edge, 8> inbox("test", md, [&](const Edge& e) {
        seen.push_back(e.v);
        self->push({e.v + 1});
    });
    self = &inbox;

    struct Other : DispatchSlot {
        int runs = 0;
    } other;
    other.run = [](DispatchSlot& s) { ++static_cast<Other&>(s).runs; };

    inbox.push({0});
    md.schedule(other);
    ASSERT_TRUE(md.run_one());  // the inbox: one edge, then it queues itself again
    EXPECT_EQ(seen, (std::vector<int>{0}));
    EXPECT_EQ(other.runs, 0);
    ASSERT_TRUE(md.run_one());  // the other slot, queued before the inbox's next run
    EXPECT_EQ(other.runs, 1);
    EXPECT_EQ(seen, (std::vector<int>{0}));
    ASSERT_TRUE(md.run_one());  // the inbox again: exactly one more edge
    EXPECT_EQ(seen, (std::vector<int>{0, 1}));
    ASSERT_TRUE(md.run_one());
    EXPECT_EQ(seen, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(md.queued(), 1u);  // still pending; the inbox's destructor unqueues it
}

TEST(EdgeInbox, DestroyedWithQueuedEdgesRunsNothingAfterwards) {
    ManualDispatcher md;
    int calls = 0;
    {
        EdgeInbox<Edge, 8> inbox("test", md, [&](const Edge&) { ++calls; });
        inbox.push({1});
        inbox.push({2});
        EXPECT_EQ(md.attached(), 1u);
    }
    EXPECT_EQ(md.run_all(), 0u);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(md.attached(), 0u);
}

TEST(EdgeInbox, DestroyedOffItsDispatcherWaitsForTheHandlerInProgress) {
    EventBus bus;
    ASSERT_TRUE(bus.start().is_ok());
    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore gate(1, 0);
    std::atomic<bool> returned{false};
    auto inbox = std::make_unique<EdgeInbox<Edge, 8>>("test", bus, [&](const Edge&) {
        entered.give();
        (void)gate.take(kGateMs);
        returned = true;
    });
    inbox->push({1});
    ASSERT_TRUE(entered.take(kGateMs));
    std::atomic<bool> destroying{false};
    std::atomic<bool> handler_had_returned{false};
    std::vector<std::thread> threads;
    {
        GateAndJoin guard(gate, threads);
        threads.emplace_back([&] {
            destroying = true;
            inbox.reset();
            handler_had_returned = returned.load();
        });
        while (!destroying.load()) {
            std::this_thread::yield();
        }
    }
    EXPECT_TRUE(handler_had_returned.load());
    EXPECT_EQ(bus.attached(), 0u);
    bus.stop();
}

TEST(EdgeInbox, DestroyedFromItsOwnHandlerAborts) {
    RestoreDeathStyle restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            ManualDispatcher md;
            std::unique_ptr<Inbox8> holder;
            holder = std::make_unique<Inbox8>("test", md, [&](const Edge&) { holder.reset(); });
            holder->push({1});
            md.run_all();
        },
        "EdgeInbox destroyed from its own handler");
}
