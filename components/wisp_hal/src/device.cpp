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
    return std::nullopt;
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

}  // namespace wisp::hal
