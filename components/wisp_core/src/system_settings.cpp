#include "wisp/core/system_settings.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/topics.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "system_settings";

// The stored value of `spec`'s row, or nullopt when it is missing or one
// set() would refuse (a hand-edited or corrupted store): every value get()
// reports is one its consumers can take, and an int64 past int's range never
// reaches the narrowing in assign_setting(). `invalid` tells the two apart.
std::optional<Value> read_valid(SettingsBackend& backend, const SettingSpec& spec, bool& invalid) {
    invalid = false;
    auto stored = backend.get(std::string(spec.ns), std::string(spec.key));
    if (!stored) return std::nullopt;
    if (!valid_setting(spec, stored.value())) {
        invalid = true;
        return std::nullopt;
    }
    return std::move(stored.value());
}

// The one system.settings event: the mirror's and set()'s own publish for a
// batch that changed nothing both build it here, so the two cannot differ.
Event settings_event(const SettingsSnapshot& values) {
    return {topics::kSystemSettings, kSystemSettingsSource, 0, to_map(values)};
}

// Reproduces the system.settings event App published after every stored
// batch.
void mirror_settings(EventBus& bus, const SettingsSnapshot&, const SettingsSnapshot& after) {
    bus.publish(settings_event(after));
}

}  // namespace

bool valid_screen_off_s(int64_t s) {
    return valid_setting(setting_spec(SettingId::ScreenOffS), Value(s));
}

bool valid_deep_after_s(int64_t s) {
    return valid_setting(setting_spec(SettingId::DeepAfterS), Value(s));
}

Map to_map(const SettingsSnapshot& values) {
    Map out;
    for (const SettingSpec& spec : kSettingsSchema) {
        if ((spec.flags & kSettingWeb) != 0) {
            out.emplace(std::string(spec.name), setting_value(values, setting_id_of(spec)));
        }
    }
    return out;
}

SystemSettings::SystemSettings(SettingsBackend& backend, EventBus& bus)
    : backend_(backend), bus_(bus), cell_(bus, SettingsSnapshot{}, &mirror_settings) {}

void SystemSettings::seed_cell() {
    std::lock_guard<os::Mutex> lock(mutex_);
    cell_.seed(read_store(true));
}

SettingsSnapshot SystemSettings::get() const { return read_store(false); }

SettingsSnapshot SystemSettings::read_store(bool log_invalid) const {
    SettingsSnapshot values;
    for (const SettingSpec& spec : kSettingsSchema) {
        bool invalid = false;
        if (auto v = read_valid(backend_, spec, invalid)) {
            assign_setting(values, setting_id_of(spec), *v);
        } else if (invalid && log_invalid) {
            WLOG_W(kTag, "stored %.*s/%.*s is not valid; using the default",
                   static_cast<int>(spec.ns.size()), spec.ns.data(),
                   static_cast<int>(spec.key.size()), spec.key.data());
        }
    }
    return values;
}

Result<void> SystemSettings::set(const std::map<std::string, Value>& changes, SettingSurface from) {
    const uint8_t needed = surface_flag(from);
    for (const auto& [name, value] : changes) {
        const SettingSpec* spec = find_setting(name);
        // A key outside the caller's surface answers as an unknown one: the
        // surface's key list is the table filtered by its flag, so to that
        // caller the key does not exist.
        if (spec == nullptr || (needed != 0 && (spec->flags & needed) == 0)) {
            return Result<void>::err(ErrorCode::InvalidArgument, "unknown setting: " + name);
        }
        if (!valid_setting(*spec, value)) {
            return Result<void>::err(ErrorCode::InvalidArgument, "invalid value: " + name);
        }
    }
    if (changes.empty()) {
        return Result<void>::ok();
    }

    std::lock_guard<os::Mutex> lock(mutex_);
    struct Undo {
        const SettingSpec* spec;
        std::optional<Value> previous;  // nullopt: the key did not exist
    };
    std::vector<Undo> written;
    for (const auto& [name, value] : changes) {
        const SettingSpec* spec = find_setting(name);
        auto old = backend_.get(std::string(spec->ns), std::string(spec->key));
        std::optional<Value> previous;
        if (old) previous = std::move(old.value());

        auto r = backend_.set(std::string(spec->ns), std::string(spec->key), value);
        if (!r) {
            for (auto it = written.rbegin(); it != written.rend(); ++it) {
                auto undo = it->previous ? backend_.set(std::string(it->spec->ns),
                                                        std::string(it->spec->key), *it->previous)
                                         : backend_.erase(std::string(it->spec->ns),
                                                          std::string(it->spec->key));
                if (!undo) {
                    WLOG_W(kTag, "restoring %.*s/%.*s failed: %s",
                           static_cast<int>(it->spec->ns.size()), it->spec->ns.data(),
                           static_cast<int>(it->spec->key.size()), it->spec->key.data(),
                           undo.error().message.c_str());
                }
            }
            // A failed undo leaves part of the batch stored: the cell takes
            // what storage holds now (a no-op when the rollback worked), so
            // no follower stays behind storage until the next batch. A
            // seed, as the batch failed: no system.settings event.
            cell_.seed(get());
            return r;
        }
        written.push_back(Undo{spec, std::move(previous)});
    }

    // Exactly one system.settings per stored batch, as before the cell: the
    // mirror publishes it when a value changed, and a batch that stored
    // what was already there (the cell skips equal values) publishes it
    // here. Under mutex_, so events go out in the order batches were stored.
    const SettingsSnapshot now = get();
    if (!cell_.set(now)) {
        bus_.publish(settings_event(now));
    }
    return Result<void>::ok();
}

}  // namespace wisp::core
