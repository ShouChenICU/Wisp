#include "wisp/core/topics.hpp"

#include <gtest/gtest.h>

#include <iterator>
#include <set>
#include <string_view>

namespace topics = wisp::core::topics;

TEST(Topics, NamesAreUnique) {
    std::set<std::string_view> seen;
    for (const topics::Topic& t : topics::kAll) {
        EXPECT_FALSE(t.name.empty());
        EXPECT_TRUE(seen.insert(t.name).second) << "duplicate: " << t.name;
    }
}

TEST(Topics, EveryConstantIsRegistered) {
    constexpr std::string_view kConstants[] = {
        topics::kPowerState,         topics::kPowerWakelock,      topics::kPowerChargingChanged,
        topics::kPowerBatteryLow,    topics::kNotificationPosted, topics::kNotificationDismissed,
        topics::kStorageSdInserted,  topics::kStorageSdRemoved,   topics::kSystemBootCompleted,
        topics::kSystemFactoryReset, topics::kSystemFirmware,     topics::kSystemSettings,
        topics::kSystemTime,         topics::kNetWifiEnabled,     topics::kNetWifiState,
        topics::kNetApState,         topics::kNetPairingCode,     topics::kNetPairingFailed,
        topics::kSceneState,         topics::kModelProfiles,      topics::kAgentState,
        topics::kVoiceState,         topics::kMemoryChanged,      topics::kTodoChanged,
    };
    static_assert(std::size(kConstants) == 24);
    for (std::string_view name : kConstants) {
        const topics::Topic* t = topics::find(name);
        ASSERT_NE(t, nullptr) << name;
        EXPECT_EQ(t->name, name);
    }
    EXPECT_EQ(topics::find("power.stat"), nullptr);
    EXPECT_EQ(topics::find("power.state.x"), nullptr);
    EXPECT_EQ(topics::find(""), nullptr);
    // find() is usable at compile time, so a constant can be checked there.
    static_assert(topics::find(topics::kSceneState)->kind == topics::Kind::Notification);
}

TEST(Topics, TheRegistryHas28Rows) {
    // Four button patterns and 24 topics.
    EXPECT_EQ(std::size(topics::kAll), 28u);
    size_t edges = 0;
    for (const topics::Topic& t : topics::kAll) {
        if (t.kind == topics::Kind::InputEdge) {
            ++edges;
            EXPECT_TRUE(t.name.starts_with(topics::kButtonPrefix)) << t.name;
            EXPECT_FALSE(t.forwarded) << t.name;
        }
    }
    EXPECT_EQ(edges, 4u);
}
