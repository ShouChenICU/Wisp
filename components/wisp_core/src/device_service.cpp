#include "wisp/core/device_service.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/device_schema.hpp"

namespace wisp::core {

namespace {

// How much of a caller's id or name an error message echoes: the ids and
// names that can exist are at most this long, and a scene may pass a
// string of any length.
constexpr size_t kMaxEchoBytes = 32;

std::string echoed(std::string_view s) {
    if (s.size() <= kMaxEchoBytes) return "'" + std::string(s) + "'";
    size_t cut = kMaxEchoBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return "'" + std::string(s.substr(0, cut)) + "...'";
}

Result<Value> refuse(ErrorCode code, std::string message) {
    return Result<Value>::err(code, std::move(message));
}

// Milliseconds from `from` to `to` on a monotonic clock; 0 if a test's
// clock went backwards, so no subtraction ever wraps.
uint64_t elapsed(uint64_t from, uint64_t to) { return to >= from ? to - from : 0; }

template <size_t N>
void copy_name(std::array<char, N>& dst, std::string_view src) {
    const size_t n = std::min(src.size(), N - 1);
    std::memcpy(dst.data(), src.data(), n);
    dst[n] = '\0';
}

template <size_t N>
bool same_name(const std::array<char, N>& stored, std::string_view s) {
    return s.size() < N && std::strncmp(stored.data(), s.data(), s.size()) == 0 &&
           stored[s.size()] == '\0';
}

// The first check of call() and get(): the device and its schema.
struct Target {
    std::shared_ptr<hal::Device> device;
    const hal::DeviceSchema* schema = nullptr;
};

Result<Target> find_target(const DeviceRegistry& registry, const std::string& id) {
    Target target;
    if (id.size() <= DeviceService::kMaxIdBytes) target.device = registry.find(id);
    if (!target.device) {
        return Result<Target>::err(ErrorCode::NotFound, "no device " + echoed(id));
    }
    target.schema = target.device->describe();
    if (target.schema == nullptr) {
        return Result<Target>::err(ErrorCode::NotFound, echoed(id) + " describes nothing");
    }
    return Result<Target>::ok(std::move(target));
}

bool off_for_standby(const hal::Device& device, hal::PowerLevel level) {
    return hal::standby_policy(device.info()) == hal::StandbyPolicy::Off &&
           !hal::powered_at(hal::StandbyPolicy::Off, level);
}

std::string off_message(const std::string& id) {
    return echoed(id) + " is off while the screen is off";
}

}  // namespace

DeviceService::DeviceService(DeviceRegistry& registry, OccupancyRegistry* occupancy,
                             std::function<uint64_t()> now_ms)
    : registry_(registry), occupancy_(occupancy), now_ms_(std::move(now_ms)) {
    rates_.reserve(kMaxRateEntries);
}

List DeviceService::list() const {
    List out;
    for (const auto& device : registry_.all()) {
        const hal::DeviceSchema* schema = device->describe();
        if (schema == nullptr) continue;
        const hal::DeviceInfo& info = device->info();
        auto role = info.caps.find("role");
        out.push_back(Value(Map{
            {"id", info.id},
            {"class", hal::to_string(info.cls)},
            {"model", info.model},
            {"role", role == info.caps.end() ? Value() : role->second},
            {"schema", hal::render_schema(*schema)},
        }));
    }
    return out;
}

Result<Value> DeviceService::get(const std::string& id, const std::string& prop) const {
    auto target = find_target(registry_, id);
    if (!target) return refuse(target.error().code, target.error().message);
    const auto& [device, schema] = target.value();
    if (hal::find_prop(*schema, prop) == nullptr) {
        return refuse(ErrorCode::NotFound, echoed(id) + " has no property " + echoed(prop));
    }
    if (off_for_standby(*device, level_.load())) {
        return refuse(ErrorCode::Busy, off_message(id));
    }
    const uint64_t started = now_ms_();
    return finish(id, prop, Kind::Prop, started, device->get(prop));
}

Result<Value> DeviceService::call(const std::string& id, const std::string& cmd, const Value& args,
                                  const std::string& holder) {
    auto target = find_target(registry_, id);
    if (!target) return refuse(target.error().code, target.error().message);
    const auto& [device, schema] = target.value();
    const hal::CmdSpec* spec = hal::find_cmd(*schema, cmd);
    if (spec == nullptr) {
        return refuse(ErrorCode::NotFound, echoed(id) + " has no command " + echoed(cmd));
    }
    if (device->interface() != hal::Interface::None) {
        return refuse(ErrorCode::PermissionDenied, "commands are only for generic devices");
    }
    auto validated = hal::validate_args(*spec, args);
    if (!validated) return refuse(validated.error().code, validated.error().message);
    if (occupancy_ != nullptr) {
        std::string other = occupancy_->exclusive_holder(id);
        if (!other.empty() && other != holder) {
            return refuse(ErrorCode::Busy, echoed(id) + " is held by " + other);
        }
    }
    if (off_for_standby(*device, level_.load())) {
        return refuse(ErrorCode::Busy, off_message(id));
    }

    // Checked and stamped in one critical section, last before the driver:
    // two racing calls cannot both pass, and a call refused above never
    // started the interval.
    if (spec->min_interval_ms > 0) {
        const uint64_t now = now_ms_();
        uint64_t retry_after = 0;
        bool untracked = false;
        {
            std::lock_guard<os::Mutex> lock(mutex_);
            RateEntry* entry = find_entry(id, cmd, Kind::Cmd);
            if (entry != nullptr && entry->interval_ms > 0) {
                const uint64_t since = elapsed(entry->at_ms, now);
                if (since < spec->min_interval_ms) retry_after = spec->min_interval_ms - since;
            }
            if (retry_after == 0 && entry == nullptr) {
                entry = claim_entry(id, cmd, Kind::Cmd, now);
                // A full table of running intervals fails closed: evicting
                // one would let its command be repeated at once, and the
                // interval is what protects the hardware.
                if (entry == nullptr) {
                    untracked = true;
                    retry_after = soonest_expiry(now);
                }
            }
            if (retry_after == 0) {
                entry->interval_ms = spec->min_interval_ms;
                entry->at_ms = now;
            }
        }
        if (retry_after > 0) {
            const char* why = untracked ? "' cannot be rate-tracked now; retry_after_ms="
                                        : "' called again too soon; retry_after_ms=";
            return refuse(ErrorCode::RateLimited,
                          "'" + id + "." + cmd + why + std::to_string(retry_after));
        }
    }

    const uint64_t started = now_ms_();
    auto result = finish(id, cmd, Kind::Cmd, started, device->invoke(cmd, validated.value()));
    if (result && spec->changes_state) {
        WLOG_I("device", "device %s.%s by %s", id.c_str(), cmd.c_str(), holder.c_str());
    }
    return result;
}

void DeviceService::set_power_level(hal::PowerLevel level) { level_.store(level); }

DeviceService::RateEntry* DeviceService::find_entry(std::string_view id, std::string_view name,
                                                    Kind kind) const {
    for (RateEntry& e : rates_) {
        if (e.kind == kind && same_name(e.id, id) && same_name(e.name, name)) return &e;
    }
    return nullptr;
}

namespace {

bool expired(uint32_t interval_ms, uint64_t at_ms, uint64_t now) {
    return interval_ms == 0 || elapsed(at_ms, now) >= interval_ms;
}

}  // namespace

DeviceService::RateEntry* DeviceService::claim_entry(std::string_view id, std::string_view name,
                                                     Kind kind, uint64_t now) const {
    RateEntry* slot = nullptr;
    if (rates_.size() < kMaxRateEntries) {
        slot = &rates_.emplace_back();  // reserved: never reallocates
    } else {
        for (RateEntry& e : rates_) {
            if (expired(e.interval_ms, e.at_ms, now)) {
                slot = &e;
                break;
            }
        }
        if (slot == nullptr) return nullptr;
    }
    *slot = RateEntry{};
    copy_name(slot->id, id);
    copy_name(slot->name, name);
    slot->kind = kind;
    return slot;
}

uint64_t DeviceService::soonest_expiry(uint64_t now) const {
    uint64_t soonest = std::numeric_limits<uint64_t>::max();
    for (const RateEntry& e : rates_) {
        if (expired(e.interval_ms, e.at_ms, now)) continue;
        soonest = std::min<uint64_t>(soonest, e.interval_ms - elapsed(e.at_ms, now));
    }
    // Only called when nothing has expired, so some entry set it; 1 keeps
    // the answer a refusal even if that ever changes.
    return soonest == std::numeric_limits<uint64_t>::max() ? 1 : soonest;
}

Result<Value> DeviceService::finish(const std::string& id, std::string_view name, Kind kind,
                                    uint64_t started_ms, Result<Value> result) const {
    const uint64_t took = elapsed(started_ms, now_ms_());
    if (took > hal::kDeviceCallBudgetMs) {
        bool first = true;
        {
            std::lock_guard<os::Mutex> lock(mutex_);
            RateEntry* entry = find_entry(id, name, kind);
            // A warning never pushes out a running interval: with no room
            // it is simply not remembered, and may be logged again.
            if (entry == nullptr) entry = claim_entry(id, name, kind, started_ms);
            if (entry != nullptr) {
                first = !entry->warned;
                entry->warned = true;
            }
        }
        if (first) {
            const auto ms = static_cast<unsigned>(
                std::min<uint64_t>(took, std::numeric_limits<unsigned>::max()));
            WLOG_W("device", "%s.%.*s took %u ms (budget %u ms)", id.c_str(),
                   static_cast<int>(name.size()), name.data(), ms,
                   static_cast<unsigned>(hal::kDeviceCallBudgetMs));
        }
    }
    if (!result) return result;
    // Measured, not serialised: a faulty driver's huge result is refused
    // without a second copy of it in RAM. Its exact size, for the message,
    // is measured the same way.
    if (json_size(result.value(), kMaxResultBytes) > kMaxResultBytes) {
        const size_t bytes = json_size(result.value(), std::numeric_limits<size_t>::max() - 1);
        return refuse(ErrorCode::DeviceFailure,
                      "result too large (" + std::to_string(bytes) + " bytes)");
    }
    return result;
}

}  // namespace wisp::core
