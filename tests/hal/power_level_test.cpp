#include <gtest/gtest.h>

#include "wisp/hal/device.hpp"

using wisp::hal::PowerLevel;
using wisp::hal::StandbyPolicy;

namespace {

wisp::hal::DeviceInfo info_with_standby(wisp::Value standby) {
    wisp::hal::DeviceInfo info;
    info.id = "imu.main";
    info.cls = wisp::hal::DeviceClass::Imu;
    info.caps["standby"] = std::move(standby);
    return info;
}

class InfoOnlyDevice : public wisp::hal::Device {
public:
    InfoOnlyDevice() {
        info_.id = "clock.main";
        info_.cls = wisp::hal::DeviceClass::Clock;
    }
    const wisp::hal::DeviceInfo& info() const override { return info_; }

private:
    wisp::hal::DeviceInfo info_;
};

}  // namespace

TEST(PowerLevel, Names) {
    EXPECT_STREQ(wisp::hal::to_string(PowerLevel::Active), "active");
    EXPECT_STREQ(wisp::hal::to_string(PowerLevel::Dim), "dim");
    EXPECT_STREQ(wisp::hal::to_string(PowerLevel::Standby), "standby");
    EXPECT_STREQ(wisp::hal::to_string(PowerLevel::DeepStandby), "deep_standby");
}

TEST(StandbyPolicy, ParsedFromCaps) {
    EXPECT_EQ(wisp::hal::standby_policy(info_with_standby(wisp::Value("off"))), StandbyPolicy::Off);
    EXPECT_EQ(wisp::hal::standby_policy(info_with_standby(wisp::Value("throttle"))),
              StandbyPolicy::Throttle);
    EXPECT_EQ(wisp::hal::standby_policy(info_with_standby(wisp::Value("wake_source"))),
              StandbyPolicy::WakeSource);
    EXPECT_EQ(wisp::hal::standby_policy(info_with_standby(wisp::Value("x"))), StandbyPolicy::None);
    EXPECT_EQ(wisp::hal::standby_policy(info_with_standby(wisp::Value(int64_t{1}))),
              StandbyPolicy::None);

    wisp::hal::DeviceInfo missing;
    missing.id = "clock.main";
    missing.cls = wisp::hal::DeviceClass::Clock;
    EXPECT_EQ(wisp::hal::standby_policy(missing), StandbyPolicy::None);
}

TEST(StandbyPolicy, PoweredAt) {
    EXPECT_TRUE(wisp::hal::powered_at(StandbyPolicy::Off, PowerLevel::Active));
    EXPECT_TRUE(wisp::hal::powered_at(StandbyPolicy::Off, PowerLevel::Dim));
    EXPECT_FALSE(wisp::hal::powered_at(StandbyPolicy::Off, PowerLevel::Standby));
    EXPECT_FALSE(wisp::hal::powered_at(StandbyPolicy::Off, PowerLevel::DeepStandby));

    for (auto policy : {StandbyPolicy::None, StandbyPolicy::Throttle, StandbyPolicy::WakeSource}) {
        for (auto level :
             {PowerLevel::Active, PowerLevel::Dim, PowerLevel::Standby, PowerLevel::DeepStandby}) {
            EXPECT_TRUE(wisp::hal::powered_at(policy, level))
                << static_cast<int>(policy) << " " << wisp::hal::to_string(level);
        }
    }
}

TEST(Device, SetPowerDefaultsToUnsupported) {
    InfoOnlyDevice device;
    auto r = device.set_power(PowerLevel::Standby);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::Unsupported);
}
