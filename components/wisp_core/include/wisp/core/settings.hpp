#pragma once

// Portable settings facade: a small key/value store, namespaced, backed by
// a pluggable wisp::core::SettingsBackend. The simulator's backend
// (wisp::port::sim::JsonFileSettingsBackend, see
// wisp/port/sim/settings_json_file.hpp) persists to a JSON file; a later
// task adds an NVS backend for the ESP port with the same interface. Both
// backends enforce the same 15-character namespace/key limit (the NVS
// hardware limit) via validate_settings_name(), so a name that is rejected
// on one port is rejected identically on the other -- see AGENTS.md's
// "配置键" rule.

#include <cstdint>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"

namespace wisp::core {

// Validates a settings namespace or key name. Empty, or longer than 15
// characters, fails with ErrorCode::InvalidArgument; every
// SettingsBackend implementation must call this (on both ns and key) at
// the top of get()/set()/erase() so the rule behaves identically on every
// port.
Result<void> validate_settings_name(std::string_view name);

// Pluggable storage for Settings. A single backend instance may hold
// multiple namespaces; ns and key are each validated by
// validate_settings_name() before any lookup/mutation.
class SettingsBackend {
public:
    virtual ~SettingsBackend() = default;

    // Returns ErrorCode::NotFound if `ns`/`key` has never been set (or was
    // erased). Returns ErrorCode::InvalidArgument if `ns` or `key` fails
    // validate_settings_name().
    virtual Result<Value> get(const std::string& ns, const std::string& key) = 0;

    // Stores `value` under `ns`/`key`, overwriting any previous value.
    virtual Result<void> set(const std::string& ns, const std::string& key, const Value& value) = 0;

    // Removes `ns`/`key` if present; erasing a key that doesn't exist is
    // not an error.
    virtual Result<void> erase(const std::string& ns, const std::string& key) = 0;
};

// Convenience wrapper that binds a SettingsBackend to one fixed namespace.
// The typed get_*() accessors never fail: a missing key, a type mismatch,
// or an invalid (too long/empty) namespace or key all just fall back to
// the caller-supplied default.
class Settings {
public:
    Settings(SettingsBackend& backend, std::string ns);

    int64_t get_int(const std::string& key, int64_t def) const;
    bool get_bool(const std::string& key, bool def) const;
    std::string get_string(const std::string& key, const std::string& def) const;

    // Forwards to the backend; fails with ErrorCode::InvalidArgument if
    // this Settings' namespace or `key` is invalid.
    Result<void> set(const std::string& key, const Value& value);

private:
    SettingsBackend& backend_;
    std::string ns_;
};

}  // namespace wisp::core
