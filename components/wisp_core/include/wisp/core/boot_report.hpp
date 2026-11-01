#pragma once

// Accumulates per-device outcomes during boot/device-init, for the
// self-check screen and the WISP_BOOT_REPORT diagnostics line (see
// AGENTS.md). A missing or failed device records a degraded entry here
// instead of aborting boot -- see the device model rule in AGENTS.md.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wisp/base/value.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::core {

enum class DeviceState { Ok, Degraded, Missing, Failed };

// Returns the snake_case name of `state`, e.g. DeviceState::Degraded ->
// "degraded".
const char* to_string(DeviceState state);

struct DeviceReport {
    std::string id;
    hal::DeviceClass cls;
    DeviceState state;
    std::string reason;
};

class BootReport {
public:
    BootReport() = default;

    // Appends an entry. Recording more than one entry for the same id is
    // allowed (e.g. a status changing during boot); find() returns the
    // most recently recorded one.
    void record(DeviceReport report);

    void set_boot_ms(uint64_t ms);

    // Attaches an arbitrary diagnostics note (e.g. "free_heap": 123456),
    // surfaced under to_value()'s "notes" key. A later call with the same
    // key overwrites the previous value.
    void set_note(std::string key, Value v);

    const std::vector<DeviceReport>& entries() const;

    // Returns the most recently record()ed entry for `id`, or
    // std::nullopt if none was ever recorded.
    std::optional<DeviceReport> find(const std::string& id) const;

    // Builds a Value: {"board", "boot_ms", "devices":
    // [{"id","class","state","reason"}, ...], "notes": {...}}. `devices`
    // is in record() order (not sorted).
    Value to_value(const std::string& board) const;

private:
    std::vector<DeviceReport> entries_;
    uint64_t boot_ms_ = 0;
    Map notes_;
};

}  // namespace wisp::core
