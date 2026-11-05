#pragma once

// Portable HAL device model shared by every device interface in this
// component (Display, Touch, Imu, Power, Clock, Button, AudioOut, AudioIn,
// Storage, Network; see the sibling headers). This header declares types and
// functions only; it must never include platform headers (esp_*,
// freertos/*, driver/*, IDF hal/, soc/*, sdkconfig.h, SDL*) or expose
// platform types -- see wisp/os/os.hpp for the same rule.
//
// No RTTI: wisp_* components are built with -fno-rtti, so code that needs
// to recover a concrete device type from a `std::shared_ptr<Device>` (e.g.
// wisp::core::DeviceRegistry::get<T>()) must not use dynamic_cast/typeid.
// Instead, every concrete interface below declares a
// `static constexpr DeviceClass kClass`, and callers compare
// `device->info().cls == T::kClass` before a plain
// `std::static_pointer_cast<T>`.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"

namespace wisp::hal {

enum class DeviceClass {
    Display,
    Touch,
    Imu,
    Power,
    Clock,
    Button,
    AudioOut,
    AudioIn,
    Storage,
    Network,
};

// Returns the snake_case name of `cls`, e.g. DeviceClass::AudioOut ->
// "audio_out". Used for manifests, boot reports and logging.
const char* to_string(DeviceClass cls);

// Inverse of to_string(); returns std::nullopt for any string that isn't
// one of the names to_string() produces.
std::optional<DeviceClass> class_from_string(std::string_view s);

// The device-wide power level (spec 4.1). Active and Dim keep the screen
// on (Dim only lowers its brightness); Standby turns the screen off;
// DeepStandby additionally lets the system enter automatic light sleep.
enum class PowerLevel : uint8_t { Active, Dim, Standby, DeepStandby };

// "active", "dim", "standby", "deep_standby".
const char* to_string(PowerLevel level);

// What a device does when the device leaves Active/Dim, declared through
// caps "standby" (spec 4.2):
//   - Off: powered down at Standby and DeepStandby.
//   - Throttle: keeps running at a reduced rate.
//   - WakeSource: keeps running so it can wake the device.
//   - None: no caps "standby" (or an unrecognized value); the device is
//     not managed by power level and keeps running.
enum class StandbyPolicy : uint8_t { None, Off, Throttle, WakeSource };

struct DeviceInfo;

// Parses caps "standby": "off" | "throttle" | "wake_source"; a missing
// key, a non-string or any other string is None.
StandbyPolicy standby_policy(const DeviceInfo& info);

// Whether a device with `policy` is powered at `level`: Off only at
// Active and Dim; every other policy at every level.
bool powered_at(StandbyPolicy policy, PowerLevel level);

// Static metadata describing one device instance, as registered with
// wisp::core::DeviceRegistry. `id` follows the "<class>.<name>" convention
// (e.g. "imu.main", "button.power"). `caps` is a bag of capability
// key/value pairs specific to the device's class (documented on each
// interface below) and is what DeviceRegistry::manifest() exposes
// externally.
struct DeviceInfo {
    std::string id;
    DeviceClass cls;
    std::string model;
    Properties caps;
};

// Base interface every HAL device implements. Concrete interfaces (see
// display.hpp, touch.hpp, imu.hpp, power.hpp, clock.hpp, button.hpp,
// audio.hpp, storage.hpp, wifi.hpp) add the operations for their device class.
class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceInfo& info() const = 0;

    // Moves the device to `level`. A device that declares caps "standby"
    // overrides this; calling it again with a level that maps to the
    // state the device is already in must not resend hardware commands.
    // The default reports ErrorCode::Unsupported.
    virtual Result<void> set_power(PowerLevel level);
};

}  // namespace wisp::hal
