#include <gtest/gtest.h>

#include "support/fake_devices.hpp"
#include "support/fake_wifi.hpp"
#include "wisp/hal/device.hpp"

using wisp::hal::DeviceClass;
using wisp::hal::Interface;

TEST(DeviceIdentity, InterfaceComesFromTheType) {
    EXPECT_EQ(wisp::test::FakeDisplay("display.main", 480, 480).interface(), Interface::Display);

    wisp::test::FakeWifi wifi("net.wifi");
    const wisp::hal::Device& as_device = wifi;
    EXPECT_EQ(as_device.interface(), Interface::Wifi);

    EXPECT_EQ(wisp::test::FakeGeneric("haptic.main").interface(), Interface::None);
}

TEST(DeviceIdentity, ClassOfEveryInterface) {
    EXPECT_EQ(wisp::hal::class_of(Interface::None), DeviceClass::Other);
    EXPECT_EQ(wisp::hal::class_of(Interface::Display), DeviceClass::Display);
    EXPECT_EQ(wisp::hal::class_of(Interface::Touch), DeviceClass::Touch);
    EXPECT_EQ(wisp::hal::class_of(Interface::Imu), DeviceClass::Imu);
    EXPECT_EQ(wisp::hal::class_of(Interface::Power), DeviceClass::Power);
    EXPECT_EQ(wisp::hal::class_of(Interface::Clock), DeviceClass::Clock);
    EXPECT_EQ(wisp::hal::class_of(Interface::Button), DeviceClass::Button);
    EXPECT_EQ(wisp::hal::class_of(Interface::AudioOut), DeviceClass::AudioOut);
    EXPECT_EQ(wisp::hal::class_of(Interface::AudioIn), DeviceClass::AudioIn);
    EXPECT_EQ(wisp::hal::class_of(Interface::Storage), DeviceClass::Storage);
    EXPECT_EQ(wisp::hal::class_of(Interface::Wifi), DeviceClass::Network);
}

TEST(DeviceIdentity, NamesRoundTrip) {
    EXPECT_STREQ(wisp::hal::to_string(Interface::AudioIn), "audio_in");
    EXPECT_STREQ(wisp::hal::to_string(Interface::None), "none");
    EXPECT_STREQ(wisp::hal::to_string(Interface::Wifi), "wifi");
    EXPECT_STREQ(wisp::hal::to_string(DeviceClass::Other), "other");
    EXPECT_EQ(wisp::hal::class_from_string("other"), DeviceClass::Other);
}
