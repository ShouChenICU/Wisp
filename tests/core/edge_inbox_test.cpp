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

// The case the "no lost edge" argument rests on: the handler pushes while
// an edge is still waiting, so push() does not schedule, and only the run's
// own re-schedule at its end gets the new edge delivered.
TEST(EdgeInbox, AnEdgePushedWhileOthersWaitIsDeliveredByTheNextRun) {
    ManualDispatcher md;
    std::vector<int> seen;
    Inbox8* self = nullptr;
    Inbox8 inbox("test", md, [&](const Edge& e) {
        seen.push_back(e.v);
        if (e.v == 1) {
            self->push({e.v + 10});  // 2 is still waiting: push() does not schedule
        }
    });
    self = &inbox;
    inbox.push({1});
    inbox.push({2});
    ASSERT_TRUE(md.run_one());
    EXPECT_EQ(seen, (std::vector<int>{1, 2}));  // the two waiting at the start
    EXPECT_EQ(md.queued(), 1u);                 // queued by the run itself
    EXPECT_EQ(md.run_all(), 1u);
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 11}));
}

// The handler overflows its own ring mid-run. Pushes 1..4 fill it; the run
// starts with 4 waiting and pops 1, leaving 2, 3, 4; the handler's 101
// fills the ring and 102, 103, 104 drop 2, 3, 4. The run still delivers
// exactly 4 edges (1, 101, 102, 103), the oldest survivors in order, and
// the next run delivers 104.
TEST(EdgeInbox, OverflowDuringARunDeliversTheSnapshotCountOfSurvivorsInOrder) {
    ManualDispatcher md;
    std::vector<int> seen;
    EdgeInbox<Edge, 4>* self = nullptr;
    EdgeInbox<Edge, 4> inbox("test", md, [&](const Edge& e) {
        seen.push_back(e.v);
        if (e.v == 1) {
            for (int v = 101; v <= 104; ++v) {
                self->push({v});
            }
        }
    });
    self = &inbox;
    for (int v = 1; v <= 4; ++v) {
        inbox.push({v});
    }
    ASSERT_TRUE(md.run_one());
    EXPECT_EQ(seen, (std::vector<int>{1, 101, 102, 103}));
    EXPECT_EQ(inbox.overflows(), 3u);
    EXPECT_EQ(md.queued(), 1u);
    EXPECT_EQ(md.run_all(), 1u);
    EXPECT_EQ(seen, (std::vector<int>{1, 101, 102, 103, 104}));
}

// Legal: on the dispatcher's thread but outside the inbox's own run, no run
// of its slot is in progress, so the destructor unqueues it and returns.
TEST(EdgeInbox, DestroyedFromAnotherSlotsRunOnItsDispatcher) {
    ManualDispatcher md;
    int calls = 0;
    std::unique_ptr<Inbox8> inbox =
        std::make_unique<Inbox8>("test", md, [&](const Edge&) { ++calls; });

    struct Killer : DispatchSlot {
        std::unique_ptr<Inbox8>* target = nullptr;
    } killer;
    killer.target = &inbox;
    killer.run = [](DispatchSlot& s) { static_cast<Killer&>(s).target->reset(); };

    md.schedule(killer);
    inbox->push({1});
    inbox->push({2});
    ASSERT_TRUE(md.run_one());  // the killer, queued ahead of the inbox
    EXPECT_EQ(inbox, nullptr);
    EXPECT_EQ(md.run_all(), 0u);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(md.attached(), 0u);
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

// Forwards everything to a real dispatcher; cancel() first gives `gate`,
// so a handler parked on it can return only once the inbox's destructor
// has reached cancel().
class CancelSpy : public wisp::core::Dispatcher {
public:
    CancelSpy(Dispatcher& real, wisp::os::Semaphore& gate) : real_(real), gate_(gate) {}
    ~CancelSpy() override { check_detached("CancelSpy"); }
    void schedule(DispatchSlot& slot) override { real_.schedule(slot); }
    bool cancel(DispatchSlot& slot) override {
        gate_.give();
        return real_.cancel(slot);
    }
    bool on_dispatch_thread() const override { return real_.on_dispatch_thread(); }

private:
    Dispatcher& real_;
    wisp::os::Semaphore& gate_;
};

TEST(EdgeInbox, DestroyedOffItsDispatcherWaitsForTheHandlerInProgress) {
    EventBus bus;
    ASSERT_TRUE(bus.start().is_ok());
    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore gate(1, 0);
    CancelSpy spy(bus, gate);
    std::atomic<bool> returned{false};
    std::atomic<bool> timed_out{false};
    auto inbox = std::make_unique<Inbox8>("test", spy, [&](const Edge&) {
        entered.give();
        // Opened only by the destructor's cancel() (or the guard on a
        // failure): a destructor that never cancels leaves this to time out.
        if (!gate.take(kGateMs)) {
            timed_out = true;
        }
        returned = true;
    });
    inbox->push({1});
    ASSERT_TRUE(entered.take(kGateMs));
    std::atomic<bool> handler_had_returned{false};
    std::vector<std::thread> threads;
    {
        GateAndJoin guard(gate, threads);
        threads.emplace_back([&] {
            inbox.reset();
            handler_had_returned = returned.load();
        });
    }
    // The handler could return only after cancel() began, so seeing it
    // returned once reset() is back proves the destructor waited for it.
    EXPECT_TRUE(handler_had_returned.load());
    EXPECT_FALSE(timed_out.load());
    EXPECT_EQ(spy.attached(), 0u);
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
