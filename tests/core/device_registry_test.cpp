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
using wisp::test::FakeGeneric;
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
                                                     // magnetometer=false, max_rate_hz=1000,
                                                     // standby="off"; model "FAKE"
    EXPECT_EQ(
        wisp::to_json(reg.manifest("b")),
        R"({"board":"b","devices":[{"caps":{"accel":true,"gyro":true,"magnetometer":false,"max_rate_hz":1000,"standby":"off"},"class":"imu","id":"imu.main","model":"FAKE"}]})");
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

TEST(DeviceRegistry, TypedLookupChecksTheInterfaceNotTheId) {
    wisp::core::DeviceRegistry reg;
    ASSERT_TRUE(reg.add(std::make_shared<FakeGeneric>("display.x")));
    EXPECT_EQ(reg.get<wisp::hal::Display>("display.x"), nullptr);
    EXPECT_TRUE(reg.list<wisp::hal::Display>().empty());
    EXPECT_NE(reg.find("display.x"), nullptr);
}

namespace {

// A typed lookup names an interface class, never a concrete device: the
// tag check cannot tell one Display implementation from another, so
// get<FakeDisplay> would cast an EspDisplay to a FakeDisplay.
template <class T>
concept Gettable = requires(const wisp::core::DeviceRegistry& reg) { reg.get<T>(std::string()); };
template <class T>
concept Listable = requires(const wisp::core::DeviceRegistry& reg) { reg.list<T>(); };

static_assert(Gettable<wisp::hal::Display>);
static_assert(Listable<wisp::hal::Display>);
static_assert(Gettable<wisp::hal::Wifi>);
static_assert(!Gettable<FakeDisplay>);
static_assert(!Listable<FakeDisplay>);
static_assert(!Gettable<wisp::test::FakeWifi>);
static_assert(!Gettable<wisp::hal::Device>);

// Naming itself as the interface does not make a concrete device one: the
// accepted set is the closed list of interface classes.
class SelfNamedDisplay : public FakeDisplay {
public:
    using InterfaceType = SelfNamedDisplay;
    using FakeDisplay::FakeDisplay;
};
static_assert(!Gettable<SelfNamedDisplay>);
static_assert(!Listable<SelfNamedDisplay>);

}  // namespace

// A generic device can say Display in its class, but has no way to report
// the Display interface: add() refuses the mismatch and the typed lookup
// never sees it.
TEST(DeviceRegistry, AGenericDeviceCannotPassAsATypedOne) {
    auto liar = std::make_shared<FakeGeneric>("display.main", wisp::hal::DeviceClass::Display);
    EXPECT_EQ(liar->interface(), wisp::hal::Interface::None);

    wisp::core::DeviceRegistry reg;
    auto added = reg.add(liar);
    ASSERT_FALSE(added);
    EXPECT_EQ(added.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(added.error().message, "display.main: class display does not match interface none");
    EXPECT_EQ(reg.get<wisp::hal::Display>("display.main"), nullptr);
    EXPECT_TRUE(reg.list<wisp::hal::Display>().empty());
}

namespace {

// An IMU that claims to be a display: the class is a data field a device
// can get wrong, the interface is not.
class MislabelledImu : public FakeImu {
public:
    explicit MislabelledImu(std::string id) : FakeImu(std::move(id)) {
        mislabelled_ = FakeImu::info();
        mislabelled_.cls = wisp::hal::DeviceClass::Display;
    }
    const wisp::hal::DeviceInfo& info() const override { return mislabelled_; }

private:
    wisp::hal::DeviceInfo mislabelled_;
};

}  // namespace

TEST(DeviceRegistry, RejectsAClassThatDoesNotMatchTheInterface) {
    wisp::core::DeviceRegistry reg;
    auto added = reg.add(std::make_shared<MislabelledImu>("imu.main"));
    ASSERT_FALSE(added);
    EXPECT_EQ(added.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(added.error().message, "imu.main: class display does not match interface imu");
    EXPECT_EQ(reg.find("imu.main"), nullptr);
}

TEST(DeviceRegistry, RejectsAGenericDeviceWithATypedClass) {
    wisp::core::DeviceRegistry reg;
    auto added =
        reg.add(std::make_shared<FakeGeneric>("haptic.main", wisp::hal::DeviceClass::Button));
    ASSERT_FALSE(added);
    EXPECT_EQ(added.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(reg.find("haptic.main"), nullptr);
}

TEST(DeviceRegistry, RejectsANullDevice) {
    wisp::core::DeviceRegistry reg;
    auto added = reg.add(nullptr);
    ASSERT_FALSE(added);
    EXPECT_EQ(added.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_TRUE(reg.all().empty());
}

TEST(DeviceRegistry, GenericDeviceIdsNeedAKind) {
    wisp::core::DeviceRegistry reg;
    const std::string too_long = "h." + std::string(31, 'x');  // 33 bytes
    for (const std::string& id : {std::string("haptic"), std::string("Haptic.main"), too_long}) {
        auto added = reg.add(std::make_shared<FakeGeneric>(id));
        ASSERT_FALSE(added) << id;
        EXPECT_EQ(added.error().code, wisp::ErrorCode::InvalidArgument) << id;
        EXPECT_EQ(added.error().message,
                  id + ": a generic device id is <kind>.<name>, at most 32 bytes");
    }
    EXPECT_TRUE(reg.add(std::make_shared<FakeGeneric>("h." + std::string(30, 'x'))));
    EXPECT_TRUE(reg.add(std::make_shared<FakeGeneric>("led.status")));
    EXPECT_EQ(reg.all().size(), 2u);
}

TEST(DeviceClass, StringRoundTrip) {
    EXPECT_EQ(wisp::hal::class_from_string("audio_out"), wisp::hal::DeviceClass::AudioOut);
    EXPECT_EQ(wisp::hal::class_from_string("other"), wisp::hal::DeviceClass::Other);
    EXPECT_STREQ(wisp::hal::to_string(wisp::hal::DeviceClass::Other), "other");
    EXPECT_FALSE(wisp::hal::class_from_string("gpu").has_value());
}
