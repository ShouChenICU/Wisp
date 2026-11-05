#include "wisp/core/device_registry.hpp"

#include <gtest/gtest.h>

#include <memory>

#include "support/fake_devices.hpp"
#include "support/fake_wifi.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/imu.hpp"

using wisp::test::FakeDisplay;
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

TEST(DeviceRegistry, ManifestListsNetworkDevice) {
    wisp::core::DeviceRegistry reg;
    auto wifi = std::make_shared<wisp::test::FakeWifi>("net.wifi");
    ASSERT_TRUE(reg.add(wifi));
    EXPECT_EQ(reg.get<wisp::hal::Wifi>("net.wifi"), wifi);

    wisp::Value manifest = reg.manifest("b");
    const auto& devices = manifest.find("devices")->as_list();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].find("id")->as_string(), "net.wifi");
    EXPECT_EQ(devices[0].find("class")->as_string(), "network");
    const wisp::Value* ap = devices[0].find("caps")->find("ap");
    ASSERT_NE(ap, nullptr);
    EXPECT_TRUE(ap->as_bool());
}

TEST(BootReport, ToValue) {
    wisp::core::BootReport r;
    r.set_boot_ms(1234);
    r.record(
        {"imu.main", wisp::hal::DeviceClass::Imu, wisp::core::DeviceState::Failed, "disabled"});
    auto v = r.to_value("b");
    EXPECT_EQ(v.find("boot_ms")->as_int(), 1234);
    auto& d = v.find("devices")->as_list()[0];
    EXPECT_EQ(d.find("state")->as_string(), "failed");
    EXPECT_EQ(d.find("class")->as_string(), "imu");
    EXPECT_EQ(r.find("imu.main")->reason, "disabled");
}

// Spec 5.6: a Display-class manifest entry gets a "safe_area"
// derived from its caps, alongside (not inside) "caps"; a non-display
// device gets none at all.
TEST(DeviceRegistry, ManifestSafeArea) {
    {
        wisp::core::DeviceRegistry reg;
        reg.add(std::make_shared<FakeDisplay>("display.main", 480, 480));  // rounded_rect, r=40
        reg.add(std::make_shared<FakeImu>("imu.main"));

        wisp::Value manifest = reg.manifest("b");
        const auto& devices = manifest.find("devices")->as_list();
        ASSERT_EQ(devices.size(), 2u);

        const wisp::Value* display_entry = nullptr;
        const wisp::Value* imu_entry = nullptr;
        for (const auto& d : devices) {
            if (d.find("id")->as_string() == "display.main") {
                display_entry = &d;
            } else if (d.find("id")->as_string() == "imu.main") {
                imu_entry = &d;
            }
        }
        ASSERT_NE(display_entry, nullptr);
        ASSERT_NE(imu_entry, nullptr);

        const wisp::Value* area = display_entry->find("safe_area");
        ASSERT_NE(area, nullptr);
        EXPECT_EQ(area->find("x1")->as_int(), 15);
        EXPECT_EQ(area->find("y1")->as_int(), 15);
        EXPECT_EQ(area->find("x2")->as_int(), 464);
        EXPECT_EQ(area->find("y2")->as_int(), 464);

        EXPECT_EQ(imu_entry->find("safe_area"), nullptr);
    }

    {
        wisp::core::DeviceRegistry reg;
        reg.add(
            std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/1, "circle"));

        wisp::Value manifest = reg.manifest("b");
        const auto& devices = manifest.find("devices")->as_list();
        ASSERT_EQ(devices.size(), 1u);

        const wisp::Value* area = devices[0].find("safe_area");
        ASSERT_NE(area, nullptr);
        EXPECT_EQ(area->find("x1")->as_int(), 74);
        EXPECT_EQ(area->find("y1")->as_int(), 74);
        EXPECT_EQ(area->find("x2")->as_int(), 405);
        EXPECT_EQ(area->find("y2")->as_int(), 405);
    }
}

TEST(DeviceClass, StringRoundTrip) {
    EXPECT_EQ(wisp::hal::class_from_string("audio_out"), wisp::hal::DeviceClass::AudioOut);
    EXPECT_FALSE(wisp::hal::class_from_string("gpu").has_value());
}
