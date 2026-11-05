#include "wisp/port/sim/sim_sntp.hpp"

#include <gtest/gtest.h>

#include <ctime>

#include "wisp/base/error.hpp"

using wisp::port::sim::SimSntp;

// The host clock is already right: every server answers with it.
TEST(SimSntp, AnswersWithTheHostClock) {
    SimSntp sntp;
    const int64_t before = static_cast<int64_t>(std::time(nullptr));
    auto r = sntp.query("pool.ntp.org", 3000);
    const int64_t after = static_cast<int64_t>(std::time(nullptr));
    ASSERT_TRUE(r);
    EXPECT_GE(r.value(), before);
    EXPECT_LE(r.value(), after);
}

// A host under the reserved .invalid domain never answers, so the
// fallback to the next server can be tried in the simulator.
TEST(SimSntp, InvalidHostsTimeOut) {
    SimSntp sntp;
    auto r = sntp.query("down.invalid", 3000);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::Timeout);
    EXPECT_TRUE(sntp.query("ntp.aliyun.com", 3000));
}
