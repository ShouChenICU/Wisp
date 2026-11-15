#pragma once

// DeviceService (device model spec 3.6): the one service through which
// the agent tools and the Lua bindings reach devices that describe
// themselves (hal::DeviceSchema). It is the single place where every check
// lives, so an adapter cannot skip one (AGENTS.md section 3, service
// layer): what may be called (the schema, commands only on generic
// devices), by whom (occupancy), how often (min_interval_ms), and when the
// device is powered (standby).
//
// Threads: get() and call() run on the caller's task (the agent's tool
// thread, a scene's Lua thread, an httpd task), never on the UI thread or
// in an ISR, and may run concurrently. The mutex guards only the rate
// table; the driver's get()/invoke() runs with no lock of the service
// held, so a slow driver delays only its own caller, never another
// device's call or set_power_level(). Devices are internally thread-safe
// (hal::Device's contract), and the service holds the registry's
// shared_ptr for the whole call, so a device removed meanwhile lives until
// the call returns. set_power_level() comes from the event bus's thread;
// the level is an atomic. Nothing here touches flash, so any task may
// call it.
//
// Memory: the rate table is reserved once in PSRAM (os::LargeVector of
// kMaxRateEntries fixed-size entries) and never grows. A call allocates
// only what Value and Result need: the validated argument map, the
// driver's result and, on refusal, the error message. The result's size
// is measured without serialising it (json_size()).
//
// Rate entries are keyed by id and outlive the device: a device removed
// and added again under the same id within its interval is still limited.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

class DeviceService {
public:
    static constexpr size_t kMaxIdBytes = 32;
    static constexpr size_t kMaxResultBytes = 1024;  // JSON of a get/invoke result
    static constexpr size_t kMaxRateEntries = 32;    // (id, cmd) pairs with min_interval_ms

    // `occupancy` may be null (no holder is ever Busy). `now_ms` is a
    // monotonic clock; tests pass one they move by hand.
    DeviceService(DeviceRegistry& registry, OccupancyRegistry* occupancy,
                  std::function<uint64_t()> now_ms = os::now_ms);

    DeviceService(const DeviceService&) = delete;
    DeviceService& operator=(const DeviceService&) = delete;

    // Devices with a schema, sorted by id: [{"id","class","model","role",
    // "schema"}]; "role" is the caps value (string or list) or null. Built
    // on each call and owned by the caller; for_each() collected.
    List list() const;

    // The same entries, one at a time: each is built just before `visit`
    // gets it, so a caller that renders and drops them holds one device's
    // tree at a time, not every device's. `visit` returns false to stop.
    // Walks a snapshot of the registry (DeviceRegistry::all()) and holds no
    // lock of the service while `visit` runs.
    void for_each(const std::function<bool(Value entry)>& visit) const;

    // Property `prop` of device `id`: NotFound (no such device, a device
    // without a schema, or no such property), Busy while a "standby: off"
    // device is off, else what the driver returns, DeviceFailure when its
    // JSON exceeds kMaxResultBytes. No occupancy, no interval: reads have
    // no side effects.
    Result<Value> get(const std::string& id, const std::string& prop) const;

    // Command `cmd` of device `id` with `args` (a Map, or null for none).
    // `holder` is "agent" or "scene.<id>", for occupancy and the log line.
    // Checks, in order, each refusal returned before the driver is reached
    // and before the interval is stamped (so it does not cost the caller
    // its next slot):
    //   1. NotFound: id longer than kMaxIdBytes, not registered, or no
    //      schema;
    //   2. NotFound: no such command;
    //   3. PermissionDenied: the device is typed (spec 3.4);
    //   4. InvalidArgument: hal::validate_args();
    //   5. Busy: another holder has the device exclusively. Occupancy is
    //      checked, not held: a holder that takes the device exclusively
    //      after this check does not stop this call;
    //   6. Busy: a "standby: off" device while the screen is off. Advisory
    //      only: PowerManager powers devices down before it publishes
    //      power.state, which the bus delivers asynchronously, so a call can
    //      still reach a driver that is already off; the driver must refuse
    //      (Busy) while off;
    //   7. RateLimited: closer than min_interval_ms to the previous call
    //      that reached the driver, across all holders ("retry_after_ms=N"
    //      in the message); otherwise the call is stamped, whatever the
    //      driver then returns. If the command is not in the table yet and
    //      the table is full of running intervals, the call is refused too
    //      ("cannot be rate-tracked now", N = the soonest any of them ends):
    //      a running interval is never evicted. While 32 pairs keep being
    //      re-stamped by their callers a 33rd may be refused repeatedly:
    //      retry_after_ms says when the earliest slot frees, not that the
    //      retry will succeed.
    // Then the driver's invoke(); its error passes through unchanged. A
    // result whose JSON exceeds kMaxResultBytes is DeviceFailure. A call
    // over hal::kDeviceCallBudgetMs logs one warning per (id, cmd); a
    // command that changes state logs "device <id>.<cmd> by <holder>"
    // (never the arguments, which may carry user content).
    Result<Value> call(const std::string& id, const std::string& cmd, const Value& args,
                       const std::string& holder);

    // Whether command `cmd` of device `id` changes state (CmdSpec::
    // changes_state): the agent's device.call shows a note only for one
    // that does. False for an unknown device or command. Reads the schema
    // only: no driver call, no rate stamp, no log.
    bool changes_state(const std::string& id, const std::string& cmd) const;

    // From power.state: at Standby/DeepStandby, "standby: off" devices are
    // unreachable (Busy). The service never wakes a device. Starts at
    // Active, the level the device boots in. The event arrives after
    // PowerManager has already switched the devices, so this check is
    // advisory; drivers stay authoritative (hal::Device::invoke()).
    void set_power_level(hal::PowerLevel level);

private:
    enum class Kind : uint8_t { Cmd, Prop };

    // One (id, name) pair: the last call stamped for its interval, and
    // whether its budget warning was logged. Fixed size, so the table
    // never allocates after construction.
    struct RateEntry {
        std::array<char, kMaxIdBytes + 1> id{};
        std::array<char, 33> name{};
        Kind kind = Kind::Cmd;
        bool warned = false;
        uint32_t interval_ms = 0;  // 0: no interval stamped (a warning-only entry)
        uint64_t at_ms = 0;
    };

    // Under mutex_: the entry for (id, name, kind), or null.
    RateEntry* find_entry(std::string_view id, std::string_view name, Kind kind) const;
    // Under mutex_: a fresh entry for (id, name, kind) in a free slot, else
    // in one whose interval has run out (or that never had one); null when
    // every entry's interval still runs. A running interval is never
    // evicted.
    RateEntry* claim_entry(std::string_view id, std::string_view name, Kind kind,
                           uint64_t now) const;
    // Under mutex_: the least time until a running interval in the table
    // ends, for the retry_after_ms of a call refused because the table is
    // full.
    uint64_t soonest_expiry(uint64_t now) const;

    // After the driver returned, for call() and get(): the budget warning
    // (once per entry) and the result size check.
    Result<Value> finish(const std::string& id, std::string_view name, Kind kind,
                         uint64_t started_ms, Result<Value> result) const;

    DeviceRegistry& registry_;
    OccupancyRegistry* occupancy_;
    std::function<uint64_t()> now_ms_;
    std::atomic<hal::PowerLevel> level_{hal::PowerLevel::Active};

    mutable os::Mutex mutex_;
    mutable os::LargeVector<RateEntry> rates_;
};

}  // namespace wisp::core
