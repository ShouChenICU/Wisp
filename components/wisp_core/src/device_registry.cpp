#include "wisp/core/device_registry.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace wisp::core {

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
        devices_list.push_back(Value(std::move(entry)));
    }
    wisp::Map root;
    root["board"] = board;
    root["devices"] = std::move(devices_list);
    return Value(std::move(root));
}

}  // namespace wisp::core
