// RequestWatch (wisp/net/request_watch.hpp).

#include "wisp/net/request_watch.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "support/wait.hpp"
#include "wisp/os/os.hpp"

namespace {

using wisp::net::RequestWatch;
using wisp::test::wait_until;
using Reason = RequestWatch::Reason;

TEST(RequestWatch, StopsAtTheDeadline) {
    RequestWatch w;
    std::atomic<int> stops{0};
    w.attach([&] { ++stops; });
    const uint64_t t0 = wisp::os::now_ms();
    w.arm(50);
    EXPECT_EQ(w.reason(), Reason::None);
    ASSERT_TRUE(wait_until([&] { return stops.load() == 1; }, 2000));
    EXPECT_GE(wisp::os::now_ms() - t0, 40u);
    EXPECT_EQ(w.reason(), Reason::Deadline);
    w.abort();
    wisp::os::sleep_ms(20);
    EXPECT_EQ(stops.load(), 1);
    EXPECT_EQ(w.reason(), Reason::Deadline);
}

TEST(RequestWatch, AttachAfterFiringStopsAtOnce) {
    RequestWatch w;
    w.arm(0);
    EXPECT_EQ(w.reason(), Reason::Deadline);
    int stops = 0;
    w.attach([&] { ++stops; });
    EXPECT_EQ(stops, 1);
}

TEST(RequestWatch, AbortStopsOnce) {
    RequestWatch w;
    w.arm(10000);
    std::atomic<int> stops{0};
    w.attach([&] { ++stops; });
    std::thread other([&] { w.abort(); });
    other.join();
    EXPECT_EQ(stops.load(), 1);
    EXPECT_EQ(w.reason(), Reason::Aborted);
    w.abort();
    EXPECT_EQ(stops.load(), 1);
}

TEST(RequestWatch, NothingAfterDisarm) {
    RequestWatch w;
    std::atomic<int> stops{0};
    w.attach([&] { ++stops; });
    w.arm(30);
    w.disarm();
    wisp::os::sleep_ms(80);
    w.abort();
    w.attach([&] { ++stops; });
    EXPECT_EQ(stops.load(), 0);
    EXPECT_EQ(w.reason(), Reason::None);
}

// disarm() returns only once a stop() already running has finished, so
// the client can close the socket right after it.
TEST(RequestWatch, DisarmWaitsForARunningStop) {
    RequestWatch w;
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    w.attach([&] {
        running = true;
        wisp::os::sleep_ms(100);
        finished = true;
    });
    std::thread other([&] { w.abort(); });
    ASSERT_TRUE(wait_until([&] { return running.load(); }, 2000));
    w.disarm();
    EXPECT_TRUE(finished.load());
    other.join();
}

}  // namespace
