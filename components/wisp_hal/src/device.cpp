#include "wisp/hal/device.hpp"

namespace wisp::hal {

const char* to_string(DeviceClass cls) {
    switch (cls) {
        case DeviceClass::Display:
            return "display";
        case DeviceClass::Touch:
            return "touch";
        case DeviceClass::Imu:
            return "imu";
        case DeviceClass::Power:
            return "power";
        case DeviceClass::Clock:
            return "clock";
        case DeviceClass::Button:
            return "button";
        case DeviceClass::AudioOut:
            return "audio_out";
        case DeviceClass::AudioIn:
            return "audio_in";
        case DeviceClass::Storage:
            return "storage";
        case DeviceClass::Network:
            return "network";
        case DeviceClass::Other:
            return "other";
    }
    return "unknown";
}

std::optional<DeviceClass> class_from_string(std::string_view s) {
    if (s == "display") return DeviceClass::Display;
    if (s == "touch") return DeviceClass::Touch;
    if (s == "imu") return DeviceClass::Imu;
    if (s == "power") return DeviceClass::Power;
    if (s == "clock") return DeviceClass::Clock;
    if (s == "button") return DeviceClass::Button;
    if (s == "audio_out") return DeviceClass::AudioOut;
    if (s == "audio_in") return DeviceClass::AudioIn;
    if (s == "storage") return DeviceClass::Storage;
    if (s == "network") return DeviceClass::Network;
    if (s == "other") return DeviceClass::Other;
    return std::nullopt;
}

const char* to_string(Interface i) {
    switch (i) {
        case Interface::None:
            return "none";
        case Interface::Display:
            return "display";
        case Interface::Touch:
            return "touch";
        case Interface::Imu:
            return "imu";
        case Interface::Power:
            return "power";
        case Interface::Clock:
            return "clock";
        case Interface::Button:
            return "button";
        case Interface::AudioOut:
            return "audio_out";
        case Interface::AudioIn:
            return "audio_in";
        case Interface::Storage:
            return "storage";
        case Interface::Wifi:
            return "wifi";
    }
    return "unknown";
}

DeviceClass class_of(Interface i) {
    switch (i) {
        case Interface::None:
            return DeviceClass::Other;
        case Interface::Display:
            return DeviceClass::Display;
        case Interface::Touch:
            return DeviceClass::Touch;
        case Interface::Imu:
            return DeviceClass::Imu;
        case Interface::Power:
            return DeviceClass::Power;
        case Interface::Clock:
            return DeviceClass::Clock;
        case Interface::Button:
            return DeviceClass::Button;
        case Interface::AudioOut:
            return DeviceClass::AudioOut;
        case Interface::AudioIn:
            return DeviceClass::AudioIn;
        case Interface::Storage:
            return DeviceClass::Storage;
        case Interface::Wifi:
            return DeviceClass::Network;
    }
    return DeviceClass::Other;
}

const char* to_string(PowerLevel level) {
    switch (level) {
        case PowerLevel::Active:
            return "active";
        case PowerLevel::Dim:
            return "dim";
        case PowerLevel::Standby:
            return "standby";
        case PowerLevel::DeepStandby:
            return "deep_standby";
    }
    return "unknown";
}

StandbyPolicy standby_policy(const DeviceInfo& info) {
    auto it = info.caps.find("standby");
    if (it == info.caps.end() || it->second.type() != Value::Type::String) {
        return StandbyPolicy::None;
    }
    const std::string& s = it->second.as_string();
    if (s == "off") return StandbyPolicy::Off;
    if (s == "throttle") return StandbyPolicy::Throttle;
    if (s == "wake_source") return StandbyPolicy::WakeSource;
    return StandbyPolicy::None;
}

bool powered_at(StandbyPolicy policy, PowerLevel level) {
    if (policy != StandbyPolicy::Off) {
        return true;
    }
    return level == PowerLevel::Active || level == PowerLevel::Dim;
}

Result<void> Device::set_power(PowerLevel /*level*/) {
    return Result<void>::err(ErrorCode::Unsupported, "power levels unsupported");
}

Result<Value> Device::get(std::string_view /*prop*/) {
    return Result<Value>::err(ErrorCode::Unsupported, "no properties");
}

Result<Value> Device::invoke(std::string_view /*cmd*/, const Map& /*args*/) {
    return Result<Value>::err(ErrorCode::Unsupported, "no commands");
}

}  // namespace wisp::hal
