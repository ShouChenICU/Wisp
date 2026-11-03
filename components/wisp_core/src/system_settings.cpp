#include "wisp/core/system_settings.hpp"

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "system_settings";

struct KeySpec {
    const char* name;
    const char* ns;
    const char* key;
    int64_t def;
    bool (*valid)(int64_t);
};

constexpr KeySpec kKeys[] = {
    {"brightness", "display", "brightness", 200, [](int64_t v) { return v >= 10 && v <= 255; }},
    {"volume", "audio", "volume", 60, [](int64_t v) { return v >= 0 && v <= 100; }},
    {"screen_off_s", "power", "screen_off_s", 60,
     [](int64_t v) { return v == 0 || (v >= 15 && v <= 3600); }},
};

const KeySpec* find_spec(const std::string& name) {
    for (const auto& spec : kKeys) {
        if (name == spec.name) return &spec;
    }
    return nullptr;
}

}  // namespace

SystemSettings::SystemSettings(SettingsBackend& backend,
                               std::function<void(uint8_t)> apply_brightness)
    : backend_(backend), apply_brightness_(std::move(apply_brightness)) {}

SystemSettingsValues SystemSettings::get() const {
    auto read = [this](const KeySpec& spec) {
        return static_cast<int>(Settings(backend_, spec.ns).get_int(spec.key, spec.def));
    };
    return SystemSettingsValues{read(kKeys[0]), read(kKeys[1]), read(kKeys[2])};
}

Result<void> SystemSettings::set(const std::map<std::string, int64_t>& changes) {
    for (const auto& [name, value] : changes) {
        const KeySpec* spec = find_spec(name);
        if (spec == nullptr) {
            return Result<void>::err(ErrorCode::InvalidArgument, "unknown setting: " + name);
        }
        if (!spec->valid(value)) {
            return Result<void>::err(ErrorCode::InvalidArgument, "value out of range: " + name);
        }
    }

    std::lock_guard<os::Mutex> lock(mutex_);
    struct Undo {
        const KeySpec* spec;
        std::optional<Value> previous;  // nullopt: the key did not exist
    };
    std::vector<Undo> written;
    for (const auto& [name, value] : changes) {
        const KeySpec* spec = find_spec(name);
        auto old = backend_.get(spec->ns, spec->key);
        std::optional<Value> previous;
        if (old) previous = std::move(old.value());

        auto r = backend_.set(spec->ns, spec->key, Value(value));
        if (!r) {
            for (auto it = written.rbegin(); it != written.rend(); ++it) {
                auto undo = it->previous ? backend_.set(it->spec->ns, it->spec->key, *it->previous)
                                         : backend_.erase(it->spec->ns, it->spec->key);
                if (!undo) {
                    WLOG_W(kTag, "restoring %s/%s failed: %s", it->spec->ns, it->spec->key,
                           undo.error().message.c_str());
                }
            }
            return r;
        }
        written.push_back(Undo{spec, std::move(previous)});
    }

    auto brightness = changes.find("brightness");
    if (brightness != changes.end() && apply_brightness_) {
        apply_brightness_(static_cast<uint8_t>(brightness->second));
    }
    return Result<void>::ok();
}

}  // namespace wisp::core
