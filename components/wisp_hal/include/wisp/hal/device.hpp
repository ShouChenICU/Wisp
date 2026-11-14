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
// Instead, Device stores an Interface tag, and callers compare
// `device->interface() == T::kInterface` before a plain
// `std::static_pointer_cast<T>`, with T restricted to an interface class
// (TypedInterface below). The compiler guarantees the tag is right: only
// the interface classes can reach the constructor that sets a tag (they
// pass their own kInterface), interface() is not virtual, and Device
// cannot be copied, so a class deriving from Device directly always
// reports None and an object reports X only if it is an X. `info().cls`
// is a data field a device fills in itself and is only a display category
// (spec 2.1).

#include <concepts>
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
    // A device no typed interface covers (interface None).
    Other,
};

// Returns the snake_case name of `cls`, e.g. DeviceClass::AudioOut ->
// "audio_out". Used for manifests, boot reports and logging.
const char* to_string(DeviceClass cls);

// Inverse of to_string(); returns std::nullopt for any string that isn't
// one of the names to_string() produces.
std::optional<DeviceClass> class_from_string(std::string_view s);

// The typed interface a device object implements, one per interface class
// below. None is a device that only has the Device base (spec 2.1).
enum class Interface : uint8_t {
    None,
    Display,
    Touch,
    Imu,
    Power,
    Clock,
    Button,
    AudioOut,
    AudioIn,
    Storage,
    Wifi,
};

// "none", "display", ..., "audio_out", "audio_in", "storage", "wifi".
const char* to_string(Interface i);

// The class a device of interface `i` must report in info().cls: the same
// name, except Wifi -> Network and None -> Other.
DeviceClass class_of(Interface i);

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
class Display;
class Touch;
class Imu;
class Power;
class Clock;
class Button;
class AudioOut;
class AudioIn;
class Storage;
class Wifi;
struct DeviceSchema;

class Device {
public:
    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    virtual const DeviceInfo& info() const = 0;

    // The typed interface this object implements; None for a device that
    // derives from Device directly. See the file header.
    Interface interface() const { return interface_; }

    // Moves the device to `level`. A device that declares caps "standby"
    // overrides this; calling it again with a level that maps to the
    // state the device is already in must not resend hardware commands.
    // The default reports ErrorCode::Unsupported.
    virtual Result<void> set_power(PowerLevel level);

    // The generic path (spec 3): a device no core service knows describes
    // its properties and commands with a static table (device_schema.hpp),
    // and core::DeviceService reaches them through get()/invoke(). It must
    // return the same table for the device's whole lifetime: only
    // DeviceRegistry::add() validates it (hal::check_schema()), and every
    // later caller trusts it. It is normally a driver's constexpr table in
    // read-only data, so describing costs no RAM per device. The
    // defaults (no schema, Unsupported) cost nothing beyond the vtable
    // slots. A typed device may describe properties only; commands are for
    // devices whose interface is None, and a Display never has a schema
    // (DeviceRegistry::add() refuses both).
    virtual const DeviceSchema* describe() const { return nullptr; }

    // The current value of property `prop`; invoke() runs command `cmd`
    // and returns null or a small Map. Contract for overrides:
    //   - Only DeviceService calls them, so `prop`/`cmd` are in the schema
    //     and `args` is validated against it, with defaults filled
    //     (validate_args()). Clamp anyway: board code and tests can call a
    //     driver directly.
    //   - Called from any task except the UI thread and ISRs, possibly from
    //     several at once (the service does not serialise calls), so the
    //     device must be internally thread-safe.
    //   - Return within kDeviceCallBudgetMs (a bus transaction of a few
    //     milliseconds is fine). A longer action (a 2 s vibration, a fade)
    //     is started by the call and finished by the driver itself, e.g.
    //     with an os::Timer.
    //   - Never touch flash (AGENTS.md section 5: only wisp.fs does), so no
    //     Settings or StorageManager either.
    //   - Errors use the existing codes: DeviceFailure for a bus error,
    //     Busy while powered down for standby.
    // The defaults report ErrorCode::Unsupported.
    virtual Result<Value> get(std::string_view prop);
    virtual Result<Value> invoke(std::string_view cmd, const Map& args);

protected:
    Device() = default;

private:
    // Reachable only from the interface classes below, each passing its
    // own kInterface; friendship is not inherited, so a concrete device
    // cannot call it.
    explicit Device(Interface tag) : interface_(tag) {}
    friend class Display;
    friend class Touch;
    friend class Imu;
    friend class Power;
    friend class Clock;
    friend class Button;
    friend class AudioOut;
    friend class AudioIn;
    friend class Storage;
    friend class Wifi;

    const Interface interface_ = Interface::None;
};

// An interface class itself (Display, Wifi, ...), not a device deriving
// from one. DeviceRegistry::get<T>/list<T> require it, since the tag check
// cannot tell one Display implementation from another. A closed list rather
// than a trait a class declares, so a concrete device cannot opt in.
template <class T>
concept TypedInterface =
    std::same_as<T, Display> || std::same_as<T, Touch> || std::same_as<T, Imu> ||
    std::same_as<T, Power> || std::same_as<T, Clock> || std::same_as<T, Button> ||
    std::same_as<T, AudioOut> || std::same_as<T, AudioIn> || std::same_as<T, Storage> ||
    std::same_as<T, Wifi>;

}  // namespace wisp::hal
