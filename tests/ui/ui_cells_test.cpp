// State cells and edge inboxes on a real UiRuntime. The UI dispatcher
// differs from the bus and the manual test dispatcher: on_dispatch_thread()
// reads the active task (null before start() and after stop()), a slot
// re-armed during a pass runs one loop iteration later, and the wake is a
// post. No callback here calls LVGL: only what the runtime itself does on
// the UI task is under test.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/gate_and_join.hpp"
#include "wisp/core/dispatcher.hpp"
#include "wisp/core/edge_inbox.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::core::CellSubscription;
using wisp::core::DispatchSlot;
using wisp::core::EdgeInbox;
using wisp::core::EventBus;
using wisp::os::Semaphore;
using wisp::test::FakeDisplay;
using wisp::test::GateAndJoin;
using wisp::ui::UiRuntime;
template <class T>
using Cell = wisp::core::StateCell<T>;

namespace {

// A correct run reaches each gate in milliseconds; a wrong one fails the
// test at this bound instead of hanging it. Below the cell's release
// bound (3 s, state_cell.cpp), so a failed teardown reports instead of
// aborting the binary.
constexpr uint32_t kGateMs = 2000;

// With this installed, dispatch_misuse() records and returns, as on the
// device, so a misuse report fails an assertion instead of aborting.
std::mutex g_misuse_mu;
std::vector<std::string> g_misuses;
void record_misuse(const char* what) {
    std::lock_guard<std::mutex> lock(g_misuse_mu);
    g_misuses.emplace_back(what);
}
std::vector<std::string> misuses() {
    std::lock_guard<std::mutex> lock(g_misuse_mu);
    return g_misuses;
}

std::shared_ptr<FakeDisplay> make_display() {
    return std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
}

class UiCells : public ::testing::Test {
protected:
    void SetUp() override {
        {
            std::lock_guard<std::mutex> lock(g_misuse_mu);
            g_misuses.clear();
        }
        wisp::core::set_misuse_handler(&record_misuse);
        ASSERT_TRUE(bus.start().is_ok());
        ui = std::make_unique<UiRuntime>(make_display(), nullptr);
        ASSERT_TRUE(ui->start());
    }
    // After the test body's cells, subscriptions and inboxes are gone, as
    // their owners must order it: the runtime goes first, then the bus.
    void TearDown() override {
        ui.reset();
        bus.stop();
        EXPECT_TRUE(misuses().empty()) << misuses().front();
        wisp::core::set_misuse_handler(nullptr);
    }

    // Pauses and returns once the UI task has reached the paused branch:
    // the job run_blocking() posts runs there.
    void pause() {
        ui->set_paused(true);
        ui->run_blocking([] {});
    }

    EventBus bus;
    std::unique_ptr<UiRuntime> ui;
};

// Forwards everything to a real dispatcher; cancel() first gives `gate`,
// so a callback parked on it can return only once a release has reached
// cancel().
class CancelSpy : public wisp::core::Dispatcher {
public:
    CancelSpy(Dispatcher& real, Semaphore& gate) : real_(real), gate_(gate) {}
    ~CancelSpy() override { check_detached("CancelSpy"); }
    void schedule(DispatchSlot& slot) override { real_.schedule(slot); }
    bool cancel(DispatchSlot& slot) override {
        gate_.give();
        return real_.cancel(slot);
    }
    bool on_dispatch_thread() const override { return real_.on_dispatch_thread(); }

private:
    Dispatcher& real_;
    Semaphore& gate_;
};

}  // namespace

TEST_F(UiCells, SubscribeOffTheUiThreadDeliversOnItWhilePaused) {
    pause();
    Cell<int> cell(bus, 7);
    Semaphore called(1, 0);
    std::atomic<int> value{-1};
    std::atomic<bool> on_ui{false};
    CellSubscription sub;
    std::thread subscriber;
    GateAndJoin guard(called, {&subscriber});
    // Nothing is posted from here on: the first delivery's wake is the
    // runtime's own.
    subscriber = std::thread([&] {
        sub = cell.subscribe(
            [&](const int& v) {
                value = v;
                on_ui = ui->on_ui_thread();
                called.give();
            },
            ui.get());
    });
    subscriber.join();
    ASSERT_TRUE(called.take(kGateMs));
    EXPECT_EQ(value.load(), 7);
    EXPECT_TRUE(on_ui.load());
    EXPECT_TRUE(ui->paused());
}

TEST_F(UiCells, SetOffTheUiThreadWhilePausedConvergesOnTheLastValue) {
    constexpr int kLast = 1000;
    pause();
    Cell<int> cell(bus, 0);
    Semaphore reached(1, 0);
    std::atomic<int> target{kLast};
    std::atomic<int> last{-1};
    std::atomic<int> out_of_order{0};
    CellSubscription sub = cell.subscribe(
        [&](const int& v) {
            // Calls come in revision order: each sees a newer value.
            if (v <= last.load()) {
                out_of_order.fetch_add(1);
            }
            last = v;
            if (v == target.load()) {
                reached.give();
            }
        },
        ui.get());
    std::thread setter;
    GateAndJoin guard(reached, {&setter});
    setter = std::thread([&] {
        for (int i = 1; i <= kLast; ++i) {
            cell.set(i);
        }
    });
    setter.join();
    ASSERT_TRUE(reached.take(kGateMs));
    EXPECT_EQ(last.load(), kLast);

    // That the paused loop then blocks again with no slot pending (no
    // 10 ms pacing left behind) is not observable from here: UiRuntime
    // exposes no ready-list count and an idle run of a cell's slot calls
    // nothing. What is observable is that nothing was left wedged: a
    // later change from another thread is still delivered.
    target = kLast + 1;
    setter = std::thread([&] { cell.set(kLast + 1); });
    setter.join();
    ASSERT_TRUE(reached.take(kGateMs));
    EXPECT_EQ(last.load(), kLast + 1);
    EXPECT_EQ(out_of_order.load(), 0);
}

TEST_F(UiCells, ResetFromAPostedJobCancelsAQueuedCall) {
    Cell<int> cell(bus, 0);
    Semaphore first(1, 0);
    std::atomic<int> calls{0};
    CellSubscription sub = cell.subscribe(
        [&](const int&) {
            if (calls.fetch_add(1) == 0) {
                first.give();
            }
        },
        ui.get());
    ASSERT_TRUE(first.take(kGateMs));
    // In one job, so the set() queues the slot on the UI task and the
    // reset() cancels it before any slot pass can run it.
    ui->run_blocking([&] {
        EXPECT_TRUE(cell.set(1));
        sub.reset();
    });
    EXPECT_FALSE(sub);
    // The pass of the iteration that ran the job above has finished by the
    // time this next job runs.
    ui->run_blocking([] {});
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(ui->attached(), 0u);
    EXPECT_EQ(bus.cell_subscriptions(), 0u);
}

TEST_F(UiCells, ResetFromItsOwnCallbackStopsCallsAndFreesAfterTheRun) {
    Cell<int> cell(bus, 0);
    Semaphore called(1, 0);
    std::atomic<int> calls{0};
    // Held only by the callback: it expires when the subscription is freed.
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> alive = token;
    CellSubscription sub;
    // Subscribed from a job on the UI task, so `sub` holds the handle
    // before the first call (which runs on that task) resets it.
    ui->run_blocking([&] {
        sub = cell.subscribe(
            [&, token](const int&) {
                calls.fetch_add(1);
                sub.reset();
                called.give();
            },
            ui.get());
    });
    token.reset();
    ASSERT_TRUE(called.take(kGateMs));
    EXPECT_TRUE(cell.set(1));
    // Posted after the callback was entered, so it runs after that run
    // returned and finish freed the subscription.
    ui->run_blocking([] {});
    EXPECT_EQ(calls.load(), 1);
    EXPECT_TRUE(alive.expired());
    EXPECT_EQ(ui->attached(), 0u);
    EXPECT_EQ(bus.cell_subscriptions(), 0u);
}

TEST_F(UiCells, ResetOffTheUiThreadWaitsForTheCallbackInProgress) {
    // What the callback touches after it parks. Reached through a pointer
    // copied to a local first: if reset() did not wait, it would free the
    // callback's callable (and its captures) under the parked callback.
    struct Shared {
        Semaphore entered{1, 0};
        Semaphore gate{1, 0};
        Semaphore finished{1, 0};
        std::atomic<bool> returned{false};
        std::atomic<bool> timed_out{false};
    } shared;
    CancelSpy spy(*ui, shared.gate);
    Cell<int> cell(bus, 0);
    Shared* captured = &shared;
    CellSubscription sub = cell.subscribe(
        [captured](const int&) {
            Shared* sh = captured;
            sh->entered.give();
            // Only the release's cancel() opens the gate.
            if (!sh->gate.take(kGateMs)) {
                sh->timed_out = true;
            }
            sh->returned = true;
            sh->finished.give();
        },
        &spy);
    ASSERT_TRUE(shared.entered.take(kGateMs));

    // On the test thread, off the UI task. The callback can return only
    // after cancel() began, so seeing it returned once reset() is back
    // proves the release waited for it.
    sub.reset();
    EXPECT_TRUE(shared.returned.load());
    // Bounded by the callback's own timeout; keeps the runtime from
    // stopping under a callback still parked when the release did not wait.
    EXPECT_TRUE(shared.finished.take(2 * kGateMs));
    EXPECT_FALSE(shared.timed_out.load());
    EXPECT_EQ(spy.attached(), 0u);
}

// The owner's teardown order: the runtime stops with a subscription's
// slot queued, the subscription is released, the cell destroyed, the
// runtime destroyed. The slot is queued after stop(): the state stop()
// leaves any queued slot in, made deterministic (a slot queued just
// before stop() may or may not run in the task's last pass).
TEST_F(UiCells, StopWithAQueuedSlotThenTeardownRunsNothingAndReportsNothing) {
    auto rt = std::make_unique<UiRuntime>(make_display(), nullptr);
    // One LVGL instance per process: the fixture's runtime steps aside.
    ui.reset();
    ASSERT_TRUE(rt->start());
    auto cell = std::make_unique<Cell<int>>(bus, 0);
    Semaphore first(1, 0);
    std::atomic<int> calls{0};
    auto sub = std::make_unique<CellSubscription>(cell->subscribe(
        [&](const int&) {
            if (calls.fetch_add(1) == 0) {
                first.give();
            }
        },
        rt.get()));
    ASSERT_TRUE(first.take(kGateMs));
    rt->stop();
    EXPECT_TRUE(cell->set(1));  // queued; nothing runs it any more

    // reset() must not wait: no run can be in progress after stop(). Done
    // on a thread so a release that waits forever fails here; then
    // everything it may still touch is leaked instead of freed under it.
    auto reset_done = std::make_shared<Semaphore>(1, 0);
    CellSubscription* raw = sub.get();
    std::thread resetter([raw, reset_done] {
        raw->reset();
        reset_done->give();
    });
    if (!reset_done->take(kGateMs)) {
        ADD_FAILURE() << "reset() after stop() waited";
        resetter.detach();
        (void)sub.release();
        (void)cell.release();
        (void)rt.release();
        return;
    }
    resetter.join();
    EXPECT_FALSE(*sub);
    sub.reset();
    cell.reset();
    EXPECT_EQ(rt->attached(), 0u);
    rt.reset();
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(bus.cell_subscriptions(), 0u);
    EXPECT_TRUE(misuses().empty()) << misuses().front();
}

namespace {

struct Edge {
    int v;
};

// An inbox handler that pushes the next edge into its own inbox: each
// pushed edge goes to a later pass, so a job the first handler call posts
// runs before the second edge is handled.
void expect_self_pushing_inbox_shares_the_loop(UiRuntime& ui) {
    constexpr int kEdges = 10;
    Semaphore job_ran(1, 0), done(1, 0);
    std::atomic<int> handled{0};
    std::atomic<int> seen_by_job{-1};
    std::atomic<int> out_of_order{0};
    EdgeInbox<Edge, 4>* self = nullptr;  // set before the first push
    EdgeInbox<Edge, 4> inbox("ui_test", ui, [&](const Edge& e) {
        int n = handled.fetch_add(1) + 1;
        if (e.v != n) {
            out_of_order.fetch_add(1);
        }
        if (e.v == 1) {
            ui.post([&] {
                seen_by_job = handled.load();
                job_ran.give();
            });
        }
        if (e.v < kEdges) {
            self->push({e.v + 1});
        } else {
            done.give();
        }
    });
    self = &inbox;
    // From the UI task, so the push posts no wake no-op: one queued ahead
    // of the handler's job would let a second pass run before it.
    ASSERT_TRUE(ui.post([&] { inbox.push({1}); }));
    ASSERT_TRUE(job_ran.take(kGateMs));
    ASSERT_TRUE(done.take(kGateMs));
    EXPECT_EQ(seen_by_job.load(), 1);
    EXPECT_EQ(handled.load(), kEdges);
    EXPECT_EQ(out_of_order.load(), 0);
    EXPECT_EQ(inbox.overflows(), 0u);
}

}  // namespace

TEST_F(UiCells, InboxPushingIntoItselfLetsAPostedJobRun) {
    expect_self_pushing_inbox_shares_the_loop(*ui);
}

TEST_F(UiCells, InboxPushingIntoItselfLetsAPostedJobRunWhilePaused) {
    pause();
    expect_self_pushing_inbox_shares_the_loop(*ui);
}
