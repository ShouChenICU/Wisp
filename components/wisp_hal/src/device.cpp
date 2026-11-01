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
    return std::nullopt;
}

}  // namespace wisp::hal
