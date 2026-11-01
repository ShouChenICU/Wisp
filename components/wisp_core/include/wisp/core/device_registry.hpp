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

    // Registers `device`. Fails with ErrorCode::AlreadyExists (leaving the
    // existing registration untouched) if a device with the same id is
    // already registered.
    Result<void> add(std::shared_ptr<hal::Device> device);

    // Unregisters the device with id `id`. Fails with ErrorCode::NotFound
    // if no such device is registered.
    Result<void> remove(const std::string& id);

    // Returns the device registered under `id`, or nullptr if none.
    std::shared_ptr<hal::Device> find(const std::string& id) const;

    // Like find(), but also requires the device's class to match T::kClass
    // (every concrete hal:: interface declares this constant), returning
    // nullptr instead of the device if it doesn't. No RTTI is used: the
    // cast below is a plain static_pointer_cast, safe only because of the
    // class check immediately above it. See device.hpp's file header.
    template <class T>
    std::shared_ptr<T> get(const std::string& id) const {
        auto device = find(id);
        if (!device || device->info().cls != T::kClass) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(device);
    }

    // Returns every registered device whose class matches T::kClass, in
    // the same (sorted-by-id) order as all().
    template <class T>
    std::vector<std::shared_ptr<T>> list() const {
        std::vector<std::shared_ptr<T>> result;
        for (const auto& device : all()) {
            if (device->info().cls == T::kClass) {
                result.push_back(std::static_pointer_cast<T>(device));
            }
        }
        return result;
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
    mutable os::Mutex mutex_;
    std::vector<std::shared_ptr<hal::Device>> devices_;
};

}  // namespace wisp::core
