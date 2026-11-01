#include "wisp/core/boot_report.hpp"

#include <utility>

namespace wisp::core {

const char* to_string(DeviceState state) {
    switch (state) {
        case DeviceState::Ok:
            return "ok";
        case DeviceState::Degraded:
            return "degraded";
        case DeviceState::Missing:
            return "missing";
        case DeviceState::Failed:
            return "failed";
    }
    return "unknown";
}

void BootReport::record(DeviceReport report) { entries_.push_back(std::move(report)); }

void BootReport::set_boot_ms(uint64_t ms) { boot_ms_ = ms; }

void BootReport::set_note(std::string key, Value v) { notes_[std::move(key)] = std::move(v); }

const std::vector<DeviceReport>& BootReport::entries() const { return entries_; }

std::optional<DeviceReport> BootReport::find(const std::string& id) const {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->id == id) {
            return *it;
        }
    }
    return std::nullopt;
}

Value BootReport::to_value(const std::string& board) const {
    wisp::List devices_list;
    for (const auto& entry : entries_) {
        wisp::Map device;
        device["id"] = entry.id;
        device["class"] = std::string(hal::to_string(entry.cls));
        device["state"] = std::string(to_string(entry.state));
        device["reason"] = entry.reason;
        devices_list.push_back(Value(std::move(device)));
    }
    wisp::Map root;
    root["board"] = board;
    root["boot_ms"] = static_cast<int64_t>(boot_ms_);
    root["devices"] = std::move(devices_list);
    root["notes"] = notes_;
    return Value(std::move(root));
}

}  // namespace wisp::core
