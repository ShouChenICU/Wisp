#pragma once

// Placeholder wisp::core::SettingsBackend for the ESP port: keeps every
// namespace/key in a plain in-RAM map, so App::start() (which dereferences
// Platform::settings -- see wisp/app/app.hpp) always has a working backend
// to bind, even before persistent storage exists.
//
// Task 15 controller ruling: the NVS-backed settings backend is Task 18's
// job (see task-18-brief.md); until then, firmware/main/main.cpp
// constructs this one instead. It is explicitly NON-PERSISTENT -- every
// value is lost on reset/power-off -- and is meant to be deleted/replaced
// wholesale once the NVS backend lands, not extended. Like every
// SettingsBackend, it enforces the same 15-character namespace/key limit
// (see wisp/core/settings.hpp's validate_settings_name()) so behavior
// stays identical to the simulator and to the future NVS backend.

#include <map>
#include <string>

#include "wisp/base/value.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::esp {

class RamSettingsBackend : public core::SettingsBackend {
public:
    RamSettingsBackend() = default;

    Result<Value> get(const std::string& ns, const std::string& key) override;
    Result<void> set(const std::string& ns, const std::string& key, const Value& value) override;
    Result<void> erase(const std::string& ns, const std::string& key) override;

private:
    os::Mutex mutex_;
    std::map<std::string, std::map<std::string, Value>> store_;
};

}  // namespace wisp::port::esp
