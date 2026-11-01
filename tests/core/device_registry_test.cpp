#include <gtest/gtest.h>

#include <memory>

#include "support/fake_devices.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/imu.hpp"

using wisp::test::FakeImu;

TEST(DeviceRegistry, AddFindGetList) {
    wisp::core::DeviceRegistry reg;
    auto imu = std::make_shared<FakeImu>("imu.main");
    ASSERT_TRUE(reg.add(imu));
    EXPECT_EQ(reg.add(std::make_shared<FakeImu>("imu.main")).error().code,
              wisp::ErrorCode::AlreadyExists);
    EXPECT_EQ(reg.get<wisp::hal::Imu>("imu.main"), imu);
    EXPECT_EQ(reg.get<wisp::hal::Display>("imu.main"), nullptr);
    EXPECT_EQ(reg.list<wisp::hal::Imu>().size(), 1u);
    EXPECT_EQ(reg.remove("nope").error().code, wisp::ErrorCode::NotFound);
    ASSERT_TRUE(reg.remove("imu.main"));
    EXPECT_EQ(reg.find("imu.main"), nullptr);
}

TEST(DeviceRegistry, ManifestJson) {
    wisp::core::DeviceRegistry reg;
    reg.add(std::make_shared<FakeImu>("imu.main"));  // caps: accel=true, gyro=true,
                                                      // magnetometer=false, max_rate_hz=1000;
                                                      // model "FAKE"
    EXPECT_EQ(
        wisp::to_json(reg.manifest("b")),
        R"({"board":"b","devices":[{"caps":{"accel":true,"gyro":true,"magnetometer":false,"max_rate_hz":1000},"class":"imu","id":"imu.main","model":"FAKE"}]})");
}

TEST(BootReport, ToValue) {
    wisp::core::BootReport r;
    r.set_boot_ms(1234);
    r.record({"imu.main", wisp::hal::DeviceClass::Imu, wisp::core::DeviceState::Failed,
              "disabled"});
    auto v = r.to_value("b");
    EXPECT_EQ(v.find("boot_ms")->as_int(), 1234);
    auto& d = v.find("devices")->as_list()[0];
    EXPECT_EQ(d.find("state")->as_string(), "failed");
    EXPECT_EQ(d.find("class")->as_string(), "imu");
    EXPECT_EQ(r.find("imu.main")->reason, "disabled");
}

TEST(DeviceClass, StringRoundTrip) {
    EXPECT_EQ(wisp::hal::class_from_string("audio_out"), wisp::hal::DeviceClass::AudioOut);
    EXPECT_FALSE(wisp::hal::class_from_string("gpu").has_value());
}
