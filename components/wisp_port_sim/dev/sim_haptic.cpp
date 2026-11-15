#include "wisp/port/sim/sim_haptic.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::port::sim {

namespace {

namespace hal = wisp::hal;

constexpr const char* kTag = "sim";
// How long a built-in effect plays; a real driver takes it from the
// effect's waveform.
constexpr uint64_t kEffectMs = 100;
constexpr int64_t kMinPulseMs = 10;
constexpr int64_t kMaxPulseMs = 2000;
constexpr int64_t kMaxEffectId = 123;

constexpr hal::ParamSpec kProps[] = {
    {.name = "active", .type = hal::ParamType::Bool, .desc = "Motor running"},
};
constexpr hal::ParamSpec kPulseArgs[] = {
    {.name = "ms",
     .type = hal::ParamType::Int,
     .desc = "Duration",
     .min = kMinPulseMs,
     .max = kMaxPulseMs,
     .unit = "ms"},
    {.name = "strength",
     .type = hal::ParamType::Int,
     .desc = "Vibration strength",
     .min = 0,
     .max = 100,
     .unit = "%",
     .optional = true,
     .def = 80},
};
constexpr hal::ParamSpec kEffectArgs[] = {
    {.name = "id",
     .type = hal::ParamType::Int,
     .desc = "Built-in effect number",
     .min = 1,
     .max = kMaxEffectId},
};
constexpr hal::CmdSpec kCmds[] = {
    {.name = "pulse", .desc = "Vibrate once", .args = kPulseArgs, .min_interval_ms = 100},
    {.name = "effect", .desc = "Play a built-in effect", .args = kEffectArgs},
};
constexpr hal::DeviceSchema kSchema{.props = kProps, .cmds = kCmds};

// Argument `name` as an integer clamped to [lo, hi]. The service has
// validated it already; a direct caller (board code, a test) may not have,
// so a missing required argument or one that is not a number is refused
// (InvalidArgument, naming it) rather than guessed, while a number out of
// range -- or a fractional or non-finite one -- is clamped, as
// hal::Device::invoke()'s contract asks. A missing optional argument is
// `fallback`, the schema's default.
wisp::Result<int64_t> clamped_arg(const wisp::Map& args, const char* name, int64_t lo, int64_t hi,
                                  std::optional<int64_t> fallback = std::nullopt) {
    auto it = args.find(name);
    if (it == args.end()) {
        if (fallback) return wisp::Result<int64_t>::ok(*fallback);
        return wisp::Result<int64_t>::err(wisp::ErrorCode::InvalidArgument,
                                          std::string("'") + name + "' is required");
    }
    const wisp::Value& v = it->second;
    if (v.type() != wisp::Value::Type::Int && v.type() != wisp::Value::Type::Double) {
        return wisp::Result<int64_t>::err(wisp::ErrorCode::InvalidArgument,
                                          std::string("'") + name + "' must be a number");
    }
    const double d = v.as_double();
    if (!(d >= static_cast<double>(lo))) return wisp::Result<int64_t>::ok(lo);  // also NaN
    if (d >= static_cast<double>(hi)) return wisp::Result<int64_t>::ok(hi);
    return wisp::Result<int64_t>::ok(static_cast<int64_t>(d));
}

}  // namespace

SimHaptic::SimHaptic(std::string id, std::string role, std::function<uint64_t()> now_ms)
    : now_ms_(std::move(now_ms)) {
    info_.id = std::move(id);
    info_.cls = hal::DeviceClass::Other;
    info_.model = "sim-haptic";
    info_.caps = wisp::Properties{{"standby", "off"}};
    if (!role.empty()) info_.caps.emplace("role", wisp::Value(std::move(role)));
}

const hal::DeviceSchema* SimHaptic::describe() const { return &kSchema; }

wisp::Result<wisp::Value> SimHaptic::get(std::string_view prop) {
    if (prop != "active") {
        return wisp::Result<wisp::Value>::err(wisp::ErrorCode::NotFound, "no such property");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!powered_) {
        return wisp::Result<wisp::Value>::err(wisp::ErrorCode::Busy, "haptic is powered off");
    }
    return wisp::Result<wisp::Value>::ok(wisp::Value(now_ms_() < until_ms_));
}

wisp::Result<wisp::Value> SimHaptic::run(uint64_t duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!powered_) {
        return wisp::Result<wisp::Value>::err(wisp::ErrorCode::Busy, "haptic is powered off");
    }
    // Read under the lock: a `now` taken before it could be older than
    // one another thread used since, and would move a later pulse's end
    // back.
    until_ms_ = now_ms_() + duration_ms;
    return wisp::Result<wisp::Value>::ok(wisp::Value());
}

wisp::Result<wisp::Value> SimHaptic::invoke(std::string_view cmd, const wisp::Map& args) {
    if (cmd == "pulse") {
        auto ms = clamped_arg(args, "ms", kMinPulseMs, kMaxPulseMs);
        if (!ms) return wisp::Result<wisp::Value>::err(ms.error().code, ms.error().message);
        auto strength = clamped_arg(args, "strength", 0, 100, 80);
        if (!strength)
            return wisp::Result<wisp::Value>::err(strength.error().code, strength.error().message);
        auto result = run(static_cast<uint64_t>(ms.value()));
        if (result)
            WLOG_I(kTag, "haptic pulse %d ms %d%%", static_cast<int>(ms.value()),
                   static_cast<int>(strength.value()));
        return result;
    }
    if (cmd == "effect") {
        auto id = clamped_arg(args, "id", 1, kMaxEffectId);
        if (!id) return wisp::Result<wisp::Value>::err(id.error().code, id.error().message);
        auto result = run(kEffectMs);
        if (result) WLOG_I(kTag, "haptic effect %d", static_cast<int>(id.value()));
        return result;
    }
    return wisp::Result<wisp::Value>::err(wisp::ErrorCode::NotFound, "no such command");
}

wisp::Result<void> SimHaptic::set_power(hal::PowerLevel level) {
    const bool on = hal::powered_at(hal::StandbyPolicy::Off, level);
    std::lock_guard<std::mutex> lock(mutex_);
    powered_ = on;
    if (!on) until_ms_ = 0;
    return wisp::Result<void>::ok();
}

}  // namespace wisp::port::sim
