#include "wisp/core/event_bus.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

#include "wisp/os/os.hpp"

using wisp::core::Event;
using wisp::core::EventBus;

TEST(EventBus, PrefixMatching) {
    EventBus bus;
    bus.start();
    std::vector<std::string> got;
    wisp::os::Mutex m;
    bus.subscribe("button.", [&](const Event& e) {
        std::lock_guard g(m);
        got.push_back(e.topic);
    });
    bus.publish({.topic = "button.power.short"});
    bus.publish({.topic = "buttons.x"});
    bus.publish({.topic = "power.low"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(got, std::vector<std::string>{"button.power.short"});
}

TEST(EventBus, EmptyPrefixMatchesAllInOrder) {
    EventBus bus;
    bus.start();
    std::vector<int> got;
    bus.subscribe("", [&](const Event& e) { got.push_back(e.payload.at("i").as_int()); });
    for (int i = 0; i < 20; ++i) {
        bus.publish({.topic = "t", .payload = {{"i", i}}});
    }
    ASSERT_TRUE(bus.wait_idle(1000));
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(got[i], i);
    }
}

TEST(EventBus, PublishFromHandlerDoesNotDeadlock) {
    EventBus bus;
    bus.start();
    std::vector<std::string> got;
    bus.subscribe("a", [&](const Event&) { bus.publish({.topic = "b"}); });
    bus.subscribe("", [&](const Event& e) { got.push_back(e.topic); });
    bus.publish({.topic = "a"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(got, (std::vector<std::string>{"a", "b"}));
}

TEST(EventBus, Unsubscribe) {
    EventBus bus;
    bus.start();
    int n = 0;
    auto id = bus.subscribe("x", [&](const Event&) { ++n; });
    bus.publish({.topic = "x"});
    bus.wait_idle(1000);
    bus.unsubscribe(id);
    bus.publish({.topic = "x"});
    bus.wait_idle(1000);
    EXPECT_EQ(n, 1);
}

TEST(EventBus, OverflowDropsOldestAndCounts) {
    EventBus bus(4);  // not started: events just queue up
    for (int i = 0; i < 10; ++i) {
        bus.publish({.topic = "t", .payload = {{"i", i}}});
    }
    EXPECT_EQ(bus.dropped(), 6u);
    std::vector<int> got;
    bus.subscribe("", [&](const Event& e) { got.push_back(e.payload.at("i").as_int()); });
    bus.start();
    bus.wait_idle(1000);
    EXPECT_EQ(got, (std::vector<int>{6, 7, 8, 9}));
}

TEST(EventBus, FillsTimestamp) {
    EventBus bus;
    bus.start();
    uint64_t ts = 0;
    bus.subscribe("", [&](const Event& e) { ts = e.ts_ms; });
    bus.publish({.topic = "t"});
    bus.wait_idle(1000);
    EXPECT_GT(ts, 0u);
}

TEST(EventBus, UnsubscribeFromWithinHandlerIsSafe) {
    EventBus bus;
    bus.start();
    int n = 0;
    wisp::core::SubscriptionId id = 0;
    id = bus.subscribe("x", [&](const Event&) {
        ++n;
        bus.unsubscribe(id);
    });
    bus.publish({.topic = "x"});
    bus.publish({.topic = "x"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(n, 1);
}

TEST(EventBus, StopFromWithinHandlerDoesNotDeadlock) {
    EventBus bus;
    bus.start();
    bool handled = false;
    bus.subscribe("x", [&](const Event&) {
        handled = true;
        bus.stop();
    });
    bus.publish({.topic = "x"});
    // If stop() from within the handler self-joined, this would hang and
    // be killed by the ctest TIMEOUT instead of completing.
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_TRUE(handled);
    bus.stop();  // from the test thread: must actually join the dispatcher
}

TEST(EventBus, WaitIdleFalseOnTimeoutWhenNeverStarted) {
    EventBus bus;
    bus.publish({.topic = "t"});
    EXPECT_FALSE(bus.wait_idle(50));
}
