#include "wisp/core/device_registry.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/hal/display_geometry.hpp"

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

// Shared "manifest and self-check screen" fallback rule (spec 5.6, see
// global-constraints.md): screen_shape_from_caps() failed, but if `caps`
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

}  // namespace

Result<void> DeviceRegistry::add(std::shared_ptr<hal::Device> device) {
    std::lock_guard<os::Mutex> lock(mutex_);
    const std::string& id = device->info().id;
    for (const auto& existing : devices_) {
        if (existing->info().id == id) {
            return Result<void>::err(ErrorCode::AlreadyExists, id);
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
        entry["caps"] = info.caps;
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

}  // namespace wisp::core
