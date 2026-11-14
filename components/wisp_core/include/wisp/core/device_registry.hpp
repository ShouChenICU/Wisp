#pragma once

// Thread-safe collection of live wisp::hal::Device instances, keyed by
// device id ("<class>.<name>", e.g. "imu.main"). Every portable/platform
// layer that needs to find a device (services, scenes, the self-check UI)
// goes through this registry rather than holding driver references
// directly, so a missing or failed device degrades gracefully instead of
// being a compile-time or startup-time hazard -- see AGENTS.md's device
// model rule.

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

class DeviceRegistry {
public:
    DeviceRegistry() = default;

    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    // Registers `device`, leaving the registry unchanged on any failure:
    //   - InvalidArgument for a null device, for one whose info().cls is
    //     not hal::class_of(device->interface()) (a mislabelled device is
    //     caught here at boot rather than at a cast), and for a generic
    //     device (interface None) whose id is not "<kind>.<name>"
    //     ([a-z][a-z0-9_]*\.[a-z0-9_]+) of at most 32 bytes;
    //   - AlreadyExists if a device with the same id is registered, or
    //     ("role <role> already taken by <other id>") if one of its caps
    //     roles is already held by a registered device of the same
    //     interface -- for a generic device, of the same kind (the id's
    //     prefix before the dot), so haptic.main and led.main can both be
    //     "main". Uniqueness is what lets by_role() return the one match.
    // Boards record a failure as a failed device; it never blocks boot.
    Result<void> add(std::shared_ptr<hal::Device> device);

    // Unregisters the device with id `id`. Fails with ErrorCode::NotFound
    // if no such device is registered.
    Result<void> remove(const std::string& id);

    // Returns the device registered under `id`, or nullptr if none.
    std::shared_ptr<hal::Device> find(const std::string& id) const;

    // Like find(), but also requires the device to implement interface T
    // (device->interface() == T::kInterface), returning nullptr instead of
    // the device if it doesn't, whatever its id or class say. No RTTI is
    // used: the cast below is a plain static_pointer_cast, sound because
    // only an object of interface T can carry T's tag and T must be the
    // interface class itself, never a concrete device (a SimDisplay
    // lookup would pass the tag check for any Display). See device.hpp's
    // file header.
    template <hal::TypedInterface T>
    std::shared_ptr<T> get(const std::string& id) const {
        auto device = find(id);
        if (!device || device->interface() != T::kInterface) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(device);
    }

    // Returns every registered device that implements interface T, in the
    // same (sorted-by-id) order as all().
    template <hal::TypedInterface T>
    std::vector<std::shared_ptr<T>> list() const {
        std::vector<std::shared_ptr<T>> result;
        for (const auto& device : all()) {
            if (device->interface() == T::kInterface) {
                result.push_back(std::static_pointer_cast<T>(device));
            }
        }
        return result;
    }

    // Returns the device of interface T that has `role` in its caps (see
    // wisp/hal/roles.hpp), or nullptr. add() keeps (interface, role)
    // unique, so there is at most one.
    template <hal::TypedInterface T>
    std::shared_ptr<T> by_role(std::string_view role) const {
        auto device = find_by_role(T::kInterface, role);
        return device ? std::static_pointer_cast<T>(device) : nullptr;
    }

    // Returns every registered device, sorted by id.
    std::vector<std::shared_ptr<hal::Device>> all() const;

    // Builds a manifest Value describing every registered device:
    // {"board": <board>, "devices": [{"id","class","model","caps",
    // "safe_area"?}, ...]}, devices sorted by id. A Display-class entry's
    // "safe_area" ({"x1","y1","x2","y2"}, int) is derived from its caps
    // via wisp::hal::screen_shape_from_caps()/safe_rect() (spec 5.6); it
    // is omitted if those caps don't even carry a usable width/height (an
    // invalid "shape"/"corner_radius" instead falls back to treating the
    // panel as circular -- the most conservative shape -- with a WLOG_W).
    // Intended for the WISP_MANIFEST diagnostics line (see AGENTS.md) via
    // wisp::to_json().
    Value manifest(const std::string& board) const;

private:
    std::shared_ptr<hal::Device> find_by_role(hal::Interface iface, std::string_view role) const;

    mutable os::Mutex mutex_;
    std::vector<std::shared_ptr<hal::Device>> devices_;
};

// An interface whose role main portable code looks up (wisp/hal/roles.hpp:
// Display, Touch, Power, Clock, Imu, AudioOut, AudioIn, Wifi) with
// registered devices none of which holds that role.
struct RoleGap {
    hal::Interface iface;
    std::vector<std::string> ids;  // sorted
};

// Every such interface, in the order above. An interface with no devices
// at all is not a gap (the boot report already shows it missing).
std::vector<RoleGap> main_role_gaps(const DeviceRegistry& registry);

}  // namespace wisp::core
