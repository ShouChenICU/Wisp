// RequestWatch (wisp/net/request_watch.hpp).

#include "wisp/net/request_watch.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "support/wait.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"

namespace {

using wisp::net::RequestWatch;
using wisp::test::wait_until;
using Reason = RequestWatch::Reason;

std::unique_ptr<RequestWatch> make() {
    auto w = RequestWatch::create();
    EXPECT_NE(w, nullptr);
    return w;
}

TEST(RequestWatch, StopsAtTheDeadline) {
    auto w = make();
    const uint64_t t0 = wisp::os::now_ms();
    const uint32_t ticket = w->arm(100);
    std::atomic<int> stops{0};
    w->attach([&] { ++stops; });
    EXPECT_EQ(w->reason(), Reason::None);
    ASSERT_TRUE(wait_until([&] { return stops.load() == 1; }, 2000));
    EXPECT_GE(wisp::os::now_ms() - t0, 90u);
    EXPECT_EQ(w->reason(), Reason::Deadline);
    w->abort(ticket);
    wisp::os::sleep_ms(3 * RequestWatch::kTickMs);
    EXPECT_EQ(stops.load(), 1);
    EXPECT_EQ(w->reason(), Reason::Deadline);
}

TEST(RequestWatch, AttachAfterFiringStopsAtOnce) {
    auto w = make();
    w->arm(0);
    ASSERT_TRUE(wait_until([&] { return w->reason() == Reason::Deadline; }, 2000));
    int stops = 0;
    w->attach([&] { ++stops; });
    EXPECT_EQ(stops, 1);
}

TEST(RequestWatch, AbortStopsOnce) {
    auto w = make();
    const uint32_t ticket = w->arm(10000);
    std::atomic<int> stops{0};
    w->attach([&] { ++stops; });
    std::thread other([&] { w->abort(ticket); });
    other.join();
    EXPECT_EQ(w->reason(), Reason::Aborted);
    ASSERT_TRUE(wait_until([&] { return stops.load() == 1; }, 2000));
    w->abort(ticket);
    wisp::os::sleep_ms(3 * RequestWatch::kTickMs);
    EXPECT_EQ(stops.load(), 1);
}

// abort() only asks: a stop() that blocks (a slow socket shutdown) runs on
// the timer's task, not on the thread that aborted.
TEST(RequestWatch, AbortDoesNotWaitForStop) {
    auto w = make();
    const uint32_t ticket = w->arm(10000);
    std::atomic<bool> finished{false};
    w->attach([&] {
        wisp::os::sleep_ms(500);
        finished = true;
    });
    const uint64_t t0 = wisp::os::now_ms();
    w->abort(ticket);
    EXPECT_LT(wisp::os::now_ms() - t0, 50u);
    EXPECT_FALSE(finished.load());
    w->disarm();
}

// The ticket of an earlier request does not abort the next one.
TEST(RequestWatch, ReusedForTheNextRequest) {
    auto w = make();
    const uint32_t first = w->arm(10000);
    w->disarm();
    const uint32_t second = w->arm(10000);
    EXPECT_NE(first, second);
    std::atomic<int> stops{0};
    w->attach([&] { ++stops; });
    w->abort(first);
    wisp::os::sleep_ms(3 * RequestWatch::kTickMs);
    EXPECT_EQ(stops.load(), 0);
    EXPECT_EQ(w->reason(), Reason::None);
    w->abort(second);
    ASSERT_TRUE(wait_until([&] { return stops.load() == 1; }, 2000));
    EXPECT_EQ(w->reason(), Reason::Aborted);
    // A new request starts clean.
    w->arm(10000);
    EXPECT_EQ(w->reason(), Reason::None);
    w->disarm();
}

TEST(RequestWatch, NothingAfterDisarm) {
    auto w = make();
    std::atomic<int> stops{0};
    const uint32_t ticket = w->arm(30);
    w->attach([&] { ++stops; });
    w->disarm();
    wisp::os::sleep_ms(30 + 3 * RequestWatch::kTickMs);
    w->abort(ticket);
    w->attach([&] { ++stops; });
    wisp::os::sleep_ms(3 * RequestWatch::kTickMs);
    EXPECT_EQ(stops.load(), 0);
    EXPECT_EQ(w->reason(), Reason::None);
}

// disarm() returns only once a stop() already running has finished, so
// the client can close the socket right after it.
TEST(RequestWatch, DisarmWaitsForARunningStop) {
    auto w = make();
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    w->arm(0);
    w->attach([&] {
        running = true;
        wisp::os::sleep_ms(100);
        finished = true;
    });
    // Attached before the first tick, so the stop runs on the timer.
    ASSERT_TRUE(wait_until([&] { return running.load(); }, 2000));
    w->disarm();
    EXPECT_TRUE(finished.load());
}

TEST(RequestWatch, CreateFailsWithoutATimer) {
    wisp::port::sim::set_timer_failure(true);
    EXPECT_EQ(RequestWatch::create(), nullptr);
    wisp::port::sim::set_timer_failure(false);
}

}  // namespace
