#pragma once

// Portable settings facade: a small key/value store, namespaced, backed by
// a pluggable wisp::core::SettingsBackend. The simulator's backend
// (wisp::port::sim::JsonFileSettingsBackend, see
// wisp/port/sim/settings_json_file.hpp) persists to a JSON file, the ESP
// port's (wisp::port::esp::NvsSettingsBackend) to NVS. At runtime App
// wraps either in core::CachedSettingsBackend (wisp/core/
// cached_settings.hpp), which serves reads from memory and writes on the
// flash thread (spec 3.4 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md). Both
// backends enforce the same 15-character namespace/key limit (the NVS
// hardware limit) via validate_settings_name(), so a name that is rejected
// on one port is rejected identically on the other -- see AGENTS.md's
// "Settings keys" rule.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"

namespace wisp::core {

// Validates a settings namespace or key name. Empty, or longer than 15
// characters, fails with ErrorCode::InvalidArgument; every
// SettingsBackend implementation must call this (on both ns and key) at
// the top of get()/set()/erase() so the rule behaves identically on every
// port.
Result<void> validate_settings_name(std::string_view name);

// The largest value a setting may hold, as the bytes of its
// wisp::to_json_raw() text: NVS caps one string entry at 4000 bytes with
// its terminator. Every backend rejects a larger value with
// ErrorCode::InvalidArgument, so the limit is the same on every port.
inline constexpr size_t kSettingsMaxValueBytes = 3999;

// InvalidArgument when `value` serializes to more than
// kSettingsMaxValueBytes; `ns`/`key` only name it in the message.
Result<void> validate_settings_value(std::string_view ns, std::string_view key, const Value& value);

// One stored key, as SettingsBackend::load_all() returns it.
struct SettingsEntry {
    std::string ns;
    std::string key;
    Value value;
};

// One change for SettingsBackend::write_batch(): store `value`, or erase
// the key when it is nullopt.
struct SettingsChange {
    std::string ns;
    std::string key;
    std::optional<Value> value;
};

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

    // Removes every key in every namespace (factory reset). On failure the
    // stored values are left as they were wherever the backend can
    // guarantee it; see each implementation.
    virtual Result<void> erase_all() = 0;

    // Every key of every namespace. A platform backend reads its storage
    // (flash); core::CachedSettingsBackend calls this once, at boot or on
    // the flash thread, and serves get() from memory after that.
    virtual Result<std::vector<SettingsEntry>> load_all() = 0;

    // Applies `changes` in order, committed together where the backend
    // can (one file rewrite, one NVS commit per namespace). Every name is
    // validated first: an invalid one fails the whole batch with
    // InvalidArgument before anything is written. A platform backend
    // touches flash here, so it is called on the flash thread.
    virtual Result<void> write_batch(std::span<const SettingsChange> changes) = 0;
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
