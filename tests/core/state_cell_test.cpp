#include "wisp/core/state_cell.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/manual_dispatcher.hpp"
#include "wisp/base/fixed_string.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/os/os.hpp"

using wisp::core::CellSubscription;
using wisp::core::EventBus;
template <class T>
using Cell = wisp::core::StateCell<T>;
using wisp::test::ManualDispatcher;

namespace {

// Long enough that a correct run never reaches it, short enough that a
// wrong one fails the test instead of hanging the binary.
constexpr uint32_t kGateMs = 5000;

struct Seen {
    int value;
    std::thread::id thread;
};

// Records every call of a subscription. Calls on the bus happen on
// wisp.bus while the test thread reads, hence the lock.
struct Recorder {
    std::mutex mu;
    std::vector<Seen> seen;

    Cell<int>::Callback callback() {
        return [this](const int& v) {
            std::lock_guard<std::mutex> lock(mu);
            seen.push_back({v, std::this_thread::get_id()});
        };
    }
    size_t calls() {
        std::lock_guard<std::mutex> lock(mu);
        return seen.size();
    }
    int last() {
        std::lock_guard<std::mutex> lock(mu);
        return seen.empty() ? -1 : seen.back().value;
    }
    std::thread::id first_thread() {
        std::lock_guard<std::mutex> lock(mu);
        return seen.empty() ? std::thread::id() : seen.front().thread;
    }
};

std::atomic<int> g_mirror_calls{0};
void count_mirror(EventBus&, const int&, const int&) { g_mirror_calls.fetch_add(1); }

struct RestoreDeathStyle {
    std::string old = GTEST_FLAG_GET(death_test_style);
    ~RestoreDeathStyle() { GTEST_FLAG_SET(death_test_style, old); }
};

class StateCell : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(bus.start().is_ok());
        g_mirror_calls = 0;
    }
    void TearDown() override { bus.stop(); }

    EventBus bus;
    ManualDispatcher md;
};

}  // namespace

TEST_F(StateCell, StartsAtTheInitialValueAndRevisionOne) {
    Cell<int> c(bus, 7);
    EXPECT_EQ(c.get(), 7);
    EXPECT_EQ(c.revision(), 1u);
}

TEST_F(StateCell, SetChangesTheValueAndTheRevision) {
    Cell<int> c(bus, 7);
    EXPECT_TRUE(c.set(8));
    EXPECT_EQ(c.get(), 8);
    EXPECT_EQ(c.revision(), 2u);
}

TEST_F(StateCell, EqualSetIsANoOp) {
    Cell<int> c(bus, 7, &count_mirror);
    Recorder r;
    CellSubscription sub = c.subscribe(r.callback(), &md);
    EXPECT_EQ(md.run_all(), 1u);
    EXPECT_FALSE(c.set(7));
    EXPECT_EQ(c.revision(), 1u);
    EXPECT_EQ(md.run_all(), 0u);
    EXPECT_EQ(g_mirror_calls.load(), 0);
}

TEST_F(StateCell, UpdateIsReadModifyWrite) {
    Cell<int> c(bus, 7);
    EXPECT_TRUE(c.update([](int& v) { v += 5; }));
    EXPECT_EQ(c.get(), 12);
    EXPECT_FALSE(c.update([](int&) {}));
    EXPECT_EQ(c.revision(), 2u);
}

TEST_F(StateCell, SetAndUpdateCallTheMirrorWithBeforeAndAfter) {
    static int before_seen = 0, after_seen = 0;
    Cell<int> c(bus, 1, [](EventBus&, const int& before, const int& after) {
        before_seen = before;
        after_seen = after;
    });
    EXPECT_TRUE(c.set(2));
    EXPECT_EQ(before_seen, 1);
    EXPECT_EQ(after_seen, 2);
    EXPECT_TRUE(c.update([](int& v) { v = 5; }));
    EXPECT_EQ(before_seen, 2);
    EXPECT_EQ(after_seen, 5);
}

TEST_F(StateCell, SeedMovesTheValueWithoutTheMirror) {
    Cell<int> c(bus, 0, &count_mirror);
    EXPECT_TRUE(c.seed(9));
    EXPECT_EQ(g_mirror_calls.load(), 0);
    EXPECT_EQ(c.revision(), 2u);
    Recorder r;
    CellSubscription sub = c.subscribe(r.callback(), &md);
    EXPECT_EQ(md.run_all(), 1u);
    EXPECT_EQ(r.last(), 9);
}

TEST_F(StateCell, LateSubscriberGetsTheCurrentValueOnce) {
    Cell<int> c(bus, 0);
    for (int i = 1; i <= 5; ++i) {
        c.set(i);
    }
    Recorder r;
    CellSubscription sub = c.subscribe(r.callback(), &md);
    EXPECT_EQ(md.run_all(), 1u);
    ASSERT_EQ(r.calls(), 1u);
    EXPECT_EQ(r.last(), 5);
    EXPECT_EQ(md.run_all(), 0u);
}

TEST_F(StateCell, CallbackNeverRunsInsideSet) {
    Cell<int> c(bus, 0);
    Recorder r;
    CellSubscription sub = c.subscribe(r.callback(), &md);
    md.run_all();
    ASSERT_EQ(r.calls(), 1u);
    c.set(1);
    EXPECT_EQ(r.calls(), 1u);
    EXPECT_EQ(md.run_all(), 1u);
    ASSERT_EQ(r.calls(), 2u);
    EXPECT_EQ(r.last(), 1);
}

TEST_F(StateCell, IntermediateValuesAreSkipped) {
    Cell<int> c(bus, 0);
    Recorder r;
    CellSubscription sub = c.subscribe(r.callback(), &md);
    md.run_all();
    for (int i = 1; i <= 10; ++i) {
        c.set(i);
    }
    md.run_all();
    ASSERT_EQ(r.calls(), 2u);
    EXPECT_EQ(r.last(), 10);
}

TEST_F(StateCell, ABADeliversAAgain) {
    Cell<int> c(bus, 0);
    Recorder r;
    CellSubscription sub = c.subscribe(r.callback(), &md);
    md.run_all();
    c.set(1);
    c.set(0);
    EXPECT_EQ(md.run_all(), 1u);
    ASSERT_EQ(r.calls(), 2u);
    EXPECT_EQ(r.last(), 0);
}

TEST_F(StateCell, SlowSubscriberGetsExactlyOneMoreCallWithTheLastValue) {
    Cell<int> c(bus, 0);
    wisp::os::Semaphore entered(1, 0), gate(1, 0);
    std::atomic<int> inside{0}, max_inside{0};
    std::mutex mu;
    std::vector<int> values;
    CellSubscription sub = c.subscribe([&](const int& v) {
        int now = inside.fetch_add(1) + 1;
        int prev = max_inside.load();
        while (now > prev && !max_inside.compare_exchange_weak(prev, now)) {
        }
        bool first;
        {
            std::lock_guard<std::mutex> lock(mu);
            values.push_back(v);
            first = values.size() == 1;
        }
        if (first) {
            entered.give();
            gate.take(kGateMs);
        }
        inside.fetch_sub(1);
    });
    ASSERT_TRUE(entered.take(kGateMs));
    std::thread setter([&] {
        for (int i = 1; i <= 1000; ++i) {
            c.set(i);
        }
    });
    setter.join();
    gate.give();
    ASSERT_TRUE(bus.wait_idle(2000));
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(values, (std::vector<int>{0, 1000}));
    EXPECT_EQ(max_inside.load(), 1);
}

TEST_F(StateCell, TwoDispatchersAreIndependent) {
    Cell<int> c(bus, 0);
    Recorder a, b;
    CellSubscription sa = c.subscribe(a.callback());
    CellSubscription sb = c.subscribe(b.callback(), &md);
    c.set(3);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(a.last(), 3);
    EXPECT_EQ(b.calls(), 0u);
    EXPECT_EQ(md.run_all(), 1u);
    ASSERT_EQ(b.calls(), 1u);
    EXPECT_EQ(b.last(), 3);
}

TEST_F(StateCell, CallbacksRunOnTheirDispatchersThread) {
    Cell<int> c(bus, 0);
    Recorder a, b;
    CellSubscription sa = c.subscribe(a.callback());
    CellSubscription sb = c.subscribe(b.callback(), &md);
    ASSERT_TRUE(bus.wait_idle(1000));
    md.run_all();
    ASSERT_EQ(a.calls(), 1u);
    ASSERT_EQ(b.calls(), 1u);
    EXPECT_NE(a.first_thread(), std::this_thread::get_id());
    EXPECT_EQ(b.first_thread(), std::this_thread::get_id());
}

TEST_F(StateCell, UnsubscribeInsideItsOwnCallbackStopsFurtherCalls) {
    Cell<int> c(bus, 0);
    int calls = 0;
    CellSubscription sub;
    sub = c.subscribe(
        [&](const int& v) {
            ++calls;
            c.set(v + 1);  // queues this subscription again
            sub.reset();
        },
        &md);
    EXPECT_EQ(md.run_all(), 1u);
    EXPECT_EQ(md.run_all(), 0u);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(sub);
    EXPECT_EQ(md.attached(), 0u);
    EXPECT_EQ(bus.cell_subscriptions(), 0u);
}

TEST_F(StateCell, SetSubscribeAndUnsubscribeOthersFromACallback) {
    Cell<int> c(bus, 0);
    Cell<int> c2(bus, 0);
    Recorder fresh, b;
    CellSubscription added, sb;
    CellSubscription sa = c.subscribe(
        [&](const int&) {
            c2.set(42);
            added = c2.subscribe(fresh.callback(), &md);
            sb.reset();
        },
        &md);
    sb = c.subscribe(b.callback(), &md);
    while (md.run_all() != 0) {
    }
    EXPECT_EQ(c2.get(), 42);
    ASSERT_EQ(fresh.calls(), 1u);
    EXPECT_EQ(fresh.last(), 42);
    EXPECT_EQ(b.calls(), 0u);
}

TEST_F(StateCell, UnsubscribeFromAnotherThreadWaitsOnlyForItsOwnCallback) {
    Cell<int> c(bus, 0);
    wisp::os::Semaphore entered(1, 0), gate(1, 0);
    std::atomic<int> a_calls{0};
    Recorder b;
    CellSubscription sa = c.subscribe([&](const int&) {
        a_calls.fetch_add(1);
        entered.give();
        gate.take(kGateMs);
    });
    ASSERT_TRUE(entered.take(kGateMs));
    CellSubscription sb = c.subscribe(b.callback());
    sb.reset();  // B is queued behind A's run: returns without waiting for it
    EXPECT_FALSE(sb);

    std::atomic<bool> returned{false};
    std::thread t([&] {
        sa.reset();
        returned = true;
    });
    wisp::os::sleep_ms(20);  // a bounded negative check, not synchronisation
    EXPECT_FALSE(returned.load());
    gate.give();
    t.join();
    EXPECT_TRUE(returned.load());
    EXPECT_EQ(a_calls.load(), 1);
    EXPECT_EQ(b.calls(), 0u);
    EXPECT_EQ(bus.attached(), 0u);
}

TEST_F(StateCell, OffThreadUnsubscribeRacingSetsNeverCallsAfterReturn) {
    // A subscription released while another thread keeps setting: once
    // reset() returned, the callback never runs again, and nothing
    // touches the freed subscription (ASan builds catch that).
    Cell<int> c(bus, 0);
    std::atomic<bool> stop{false};
    std::thread setter([&] {
        int v = 0;
        while (!stop.load()) {
            c.set(++v);
        }
    });
    for (int i = 0; i < 200; ++i) {
        std::atomic<bool> released{false};
        std::atomic<bool> late{false};
        CellSubscription sub = c.subscribe([&](const int&) {
            if (released.load()) {
                late = true;
            }
        });
        sub.reset();
        released = true;
        ASSERT_TRUE(bus.wait_idle(1000));
        ASSERT_FALSE(late.load()) << "iteration " << i;
    }
    stop = true;
    setter.join();
    EXPECT_EQ(bus.cell_subscriptions(), 0u);
    EXPECT_EQ(bus.attached(), 0u);
}

TEST_F(StateCell, SetWhileHoldingTheCallbacksLockDoesNotDeadlock) {
    Cell<int> c(bus, 0);
    std::mutex m;
    std::atomic<int> last{-1};
    CellSubscription sub = c.subscribe([&](const int& v) {
        std::lock_guard<std::mutex> lock(m);
        last = v;
    });
    {
        std::lock_guard<std::mutex> lock(m);
        c.set(1);
    }
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(last.load(), 1);
}

TEST_F(StateCell, CountsLiveSubscriptionsOnItsBusAndAttachesToItsDispatcher) {
    Cell<int> c(bus, 0);
    CellSubscription a = c.subscribe([](const int&) {});
    CellSubscription b = c.subscribe([](const int&) {}, &md);
    EXPECT_EQ(bus.cell_subscriptions(), 2u);
    EXPECT_EQ(md.attached(), 1u);
    EXPECT_EQ(bus.attached(), 1u);
    a.reset();
    b.reset();
    EXPECT_EQ(bus.cell_subscriptions(), 0u);
    EXPECT_EQ(md.attached(), 0u);
    EXPECT_EQ(bus.attached(), 0u);
}

TEST_F(StateCell, MovingAHandleMovesTheSubscription) {
    Cell<int> c(bus, 0);
    Recorder r1, r2;
    CellSubscription a = c.subscribe(r1.callback(), &md);
    CellSubscription b = std::move(a);
    EXPECT_FALSE(a);  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(b);
    // Assigning over a live handle releases the old subscription first.
    b = c.subscribe(r2.callback(), &md);
    EXPECT_EQ(md.attached(), 1u);
    EXPECT_EQ(bus.cell_subscriptions(), 1u);
    md.run_all();
    EXPECT_EQ(r1.calls(), 0u);
    EXPECT_EQ(r2.calls(), 1u);
}

TEST_F(StateCell, DestroyedWithALiveSubscriptionAborts) {
    RestoreDeathStyle restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            ManualDispatcher d;
            CellSubscription s;
            {
                Cell<int> c(bus, 0);
                s = c.subscribe([](const int&) {}, &d);
            }
        },
        "StateCell destroyed with 1 live subscription");
}

TEST_F(StateCell, GetInsideItsOwnReadAborts) {
    RestoreDeathStyle restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Cell<int> c(bus, 0);
    EXPECT_DEATH(c.read([&](const int&) { return c.get(); }),
                 "StateCell re-entered from inside its own read\\(\\)");
}

TEST_F(StateCell, ReadOfAnotherCellInsideReadIsAllowed) {
    Cell<int> c(bus, 1), d(bus, 2);
    EXPECT_EQ(c.read([&](const int& v) { return v + d.get(); }), 3);
}

namespace {
struct Net {
    wisp::FixedString<32> ssid;
    int rssi = 0;
    bool operator==(const Net&) const = default;
};
}  // namespace

TEST_F(StateCell, WorksWithAStructOfFixedStrings) {
    Cell<Net> c(bus, Net{});
    EXPECT_TRUE(c.set(Net{std::string_view("home"), -40}));
    Net got = c.get();
    EXPECT_EQ(got.ssid.view(), "home");
    EXPECT_EQ(got.rssi, -40);
    EXPECT_FALSE(c.set(Net{std::string_view("home"), -40}));
    EXPECT_EQ(c.revision(), 2u);
}
