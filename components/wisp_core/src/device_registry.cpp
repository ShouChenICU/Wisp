#include "wisp/core/device_registry.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/device_schema.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/roles.hpp"

namespace wisp::core {

namespace {

Value area_to_value(const hal::Area& area) {
    return Value(Map{
        {"x1", static_cast<int64_t>(area.x1)},
        {"y1", static_cast<int64_t>(area.y1)},
        {"x2", static_cast<int64_t>(area.x2)},
        {"y2", static_cast<int64_t>(area.y2)},
    });
}

// Shared "manifest and self-check screen" fallback rule (spec 5.6):
// screen_shape_from_caps() failed, but if `caps`
// at least carries a usable width/height, treat the panel as circular --
// the most conservative shape -- rather than giving up on a safe area
// entirely. Returns std::nullopt (no safe area at all) if even
// width/height aren't usable. Logs the fallback itself (wisp_hal must not
// log -- see display_geometry.hpp).
std::optional<hal::Area> fallback_safe_area(const std::string& device_id, const Properties& caps,
                                            const Error& parse_error) {
    auto width_it = caps.find("width");
    auto height_it = caps.find("height");
    if (width_it == caps.end() || width_it->second.as_int() <= 0 || height_it == caps.end() ||
        height_it->second.as_int() <= 0) {
        return std::nullopt;
    }
    WLOG_W("device_registry", "%s: invalid display shape caps (%s); treating as circle",
           device_id.c_str(), parse_error.message.c_str());
    hal::ScreenShape fallback;
    fallback.kind = hal::ScreenShapeKind::Circle;
    fallback.width = static_cast<int32_t>(width_it->second.as_int());
    fallback.height = static_cast<int32_t>(height_it->second.as_int());
    return hal::safe_rect(fallback);
}

// "<kind>.<name>": [a-z][a-z0-9_]*\.[a-z0-9_]+, at most 32 bytes. A
// generic device has no typed interface to say what it is, so its id has
// to (spec 2.2).
bool valid_generic_id(const std::string& id) {
    constexpr size_t kMaxIdBytes = 32;
    if (id.empty() || id.size() > kMaxIdBytes) return false;
    auto lower_digit_or_underscore = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    if (!(id[0] >= 'a' && id[0] <= 'z')) return false;
    size_t dot = id.find('.');
    if (dot == std::string::npos || dot + 1 == id.size()) return false;
    for (size_t i = 1; i < id.size(); ++i) {
        if (i != dot && !lower_digit_or_underscore(id[i])) return false;
    }
    return true;
}

// Whether two devices share a role namespace (spec 2.3): the same typed
// interface, or, for generic devices, the same kind -- interface None
// alone would forbid haptic.main and led.main from both being "main".
bool same_role_scope(const hal::Device& a, const hal::Device& b) {
    if (a.interface() != b.interface()) return false;
    if (a.interface() != hal::Interface::None) return true;
    const std::string& id_a = a.info().id;
    const std::string& id_b = b.info().id;
    // Both ids passed valid_generic_id(), so each has a dot.
    return std::string_view(id_a).substr(0, id_a.find('.')) ==
           std::string_view(id_b).substr(0, id_b.find('.'));
}

// The schema rules of spec 3.2-3.4 for a device about to be added. The
// schema is generated from describe(), so caps may not carry one; a
// Display's calls all belong to the UI thread and a typed device's state
// changes belong to its service, so neither offers commands.
Result<void> check_device_schema(const hal::Device& device) {
    const hal::DeviceInfo& info = device.info();
    if (info.caps.count("schema") != 0) {
        return Result<void>::err(
            ErrorCode::InvalidArgument,
            info.id + ": caps must not carry \"schema\"; describe() supplies it");
    }
    const hal::DeviceSchema* schema = device.describe();
    if (schema == nullptr) return Result<void>::ok();
    if (device.interface() == hal::Interface::Display) {
        return Result<void>::err(ErrorCode::InvalidArgument, info.id + ": a display has no schema");
    }
    if (auto checked = hal::check_schema(*schema); !checked) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 info.id + ": " + checked.error().message);
    }
    if (device.interface() != hal::Interface::None && !schema->cmds.empty()) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 info.id + ": commands are only for generic devices");
    }
    return Result<void>::ok();
}

}  // namespace

Result<void> DeviceRegistry::add(std::shared_ptr<hal::Device> device) {
    if (!device) {
        return Result<void>::err(ErrorCode::InvalidArgument, "null device");
    }
    const hal::DeviceInfo& info = device->info();
    const hal::Interface iface = device->interface();
    if (info.cls != hal::class_of(iface)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 info.id + ": class " + hal::to_string(info.cls) +
                                     " does not match interface " + hal::to_string(iface));
    }
    if (iface == hal::Interface::None && !valid_generic_id(info.id)) {
        return Result<void>::err(
            ErrorCode::InvalidArgument,
            info.id + ": a generic device id is <kind>.<name>, at most 32 bytes");
    }
    if (auto checked = check_device_schema(*device); !checked) return checked;
    std::lock_guard<os::Mutex> lock(mutex_);
    const std::string& id = info.id;
    for (const auto& existing : devices_) {
        if (existing->info().id == id) {
            return Result<void>::err(ErrorCode::AlreadyExists, id);
        }
    }
    for (const std::string& role : hal::roles_of(info)) {
        for (const auto& existing : devices_) {
            if (same_role_scope(*existing, *device) && hal::has_role(existing->info(), role)) {
                return Result<void>::err(
                    ErrorCode::AlreadyExists,
                    "role " + role + " already taken by " + existing->info().id);
            }
        }
    }
    devices_.push_back(std::move(device));
    return Result<void>::ok();
}

Result<void> DeviceRegistry::remove(const std::string& id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    for (auto it = devices_.begin(); it != devices_.end(); ++it) {
        if ((*it)->info().id == id) {
            devices_.erase(it);
            return Result<void>::ok();
        }
    }
    return Result<void>::err(ErrorCode::NotFound, id);
}

std::shared_ptr<hal::Device> DeviceRegistry::find(const std::string& id) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    for (const auto& device : devices_) {
        if (device->info().id == id) {
            return device;
        }
    }
    return nullptr;
}

std::shared_ptr<hal::Device> DeviceRegistry::find_by_role(hal::Interface iface,
                                                          std::string_view role) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    for (const auto& device : devices_) {
        if (device->interface() == iface && hal::has_role(device->info(), role)) {
            return device;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<hal::Device>> DeviceRegistry::all() const {
    std::vector<std::shared_ptr<hal::Device>> result;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        result = devices_;
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a->info().id < b->info().id; });
    return result;
}

Value DeviceRegistry::manifest(const std::string& board) const {
    wisp::List devices_list;
    for (const auto& device : all()) {
        const hal::DeviceInfo& info = device->info();
        wisp::Map entry;
        entry["id"] = info.id;
        entry["class"] = std::string(hal::to_string(info.cls));
        entry["model"] = info.model;
        entry["caps"] = hal::caps_of(*device);
        if (info.cls == hal::DeviceClass::Display) {
            auto shape = hal::screen_shape_from_caps(info.caps);
            std::optional<hal::Area> area;
            if (shape) {
                area = hal::safe_rect(shape.value());
            } else {
                area = fallback_safe_area(info.id, info.caps, shape.error());
            }
            if (area) {
                entry["safe_area"] = area_to_value(*area);
            }
        }
        devices_list.push_back(Value(std::move(entry)));
    }
    wisp::Map root;
    root["board"] = board;
    root["devices"] = std::move(devices_list);
    return Value(std::move(root));
}

std::vector<RoleGap> main_role_gaps(const DeviceRegistry& registry) {
    using hal::Interface;
    const auto devices = registry.all();
    std::vector<RoleGap> gaps;
    for (Interface iface : hal::kInterfacesWithMain) {
        RoleGap gap{iface, {}};
        bool has_main = false;
        for (const auto& device : devices) {
            if (device->interface() != iface) continue;
            gap.ids.push_back(device->info().id);
            if (hal::has_role(device->info(), hal::role::kMain)) has_main = true;
        }
        if (!gap.ids.empty() && !has_main) gaps.push_back(std::move(gap));
    }
    return gaps;
}

std::optional<std::string> power_key_also_talks(const DeviceRegistry& registry) {
    const auto key = registry.by_role<hal::Button>(hal::role::kPower);
    if (!key || !hal::has_role(key->info(), hal::role::kTalk)) return std::nullopt;
    return key->info().id;
}

}  // namespace wisp::core
