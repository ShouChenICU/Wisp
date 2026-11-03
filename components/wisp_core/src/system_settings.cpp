#include "wisp/core/system_settings.hpp"

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "system_settings";

struct KeySpec {
    const char* name;
    const char* ns;
    const char* key;
    Value def;
    bool (*valid)(const Value&);
};

bool int_in(const Value& v, int64_t lo, int64_t hi) {
    return v.type() == Value::Type::Int && v.as_int() >= lo && v.as_int() <= hi;
}

// The device UI languages (spec 8.3). wisp_ui's parse_lang() accepts the
// same codes; it cannot be called from here (wisp_core sits below wisp_ui).
bool valid_language(const Value& v) {
    if (v.type() != Value::Type::String) return false;
    const std::string& code = v.as_string();
    return code == "zh" || code == "ja" || code == "en";
}

const KeySpec kKeys[] = {
    {"brightness", "display", "brightness", Value(200),
     [](const Value& v) { return int_in(v, 10, 255); }},
    {"volume", "audio", "volume", Value(60), [](const Value& v) { return int_in(v, 0, 100); }},
    {"screen_off_s", "power", "screen_off_s", Value(60),
     [](const Value& v) { return int_in(v, 0, 0) || int_in(v, 15, 3600); }},
    {"language", "sys", "lang", Value("zh"), valid_language},
};

const KeySpec* find_spec(const std::string& name) {
    for (const auto& spec : kKeys) {
        if (name == spec.name) return &spec;
    }
    return nullptr;
}

// The stored value, or the default when it is missing or of the wrong
// type. An unknown language code (a hand-edited or corrupted store) also
// reads as the default, since the UI has no text for it; integers are
// returned as stored, matching what their consumers read directly.
Value read(SettingsBackend& backend, const KeySpec& spec) {
    auto stored = backend.get(spec.ns, spec.key);
    if (!stored || stored.value().type() != spec.def.type()) return spec.def;
    if (spec.def.type() == Value::Type::String && !spec.valid(stored.value())) return spec.def;
    return stored.value();
}

}  // namespace

Map to_map(const SystemSettingsValues& values) {
    return Map{
        {"brightness", Value(values.brightness)},
        {"volume", Value(values.volume)},
        {"screen_off_s", Value(values.screen_off_s)},
        {"language", Value(values.language)},
    };
}

SystemSettings::SystemSettings(SettingsBackend& backend,
                               std::function<void(uint8_t)> apply_brightness,
                               std::function<void(const SystemSettingsValues&)> on_changed)
    : backend_(backend),
      apply_brightness_(std::move(apply_brightness)),
      on_changed_(std::move(on_changed)) {}

SystemSettingsValues SystemSettings::get() const {
    return SystemSettingsValues{
        static_cast<int>(read(backend_, kKeys[0]).as_int()),
        static_cast<int>(read(backend_, kKeys[1]).as_int()),
        static_cast<int>(read(backend_, kKeys[2]).as_int()),
        read(backend_, kKeys[3]).as_string(),
    };
}

Result<void> SystemSettings::set(const std::map<std::string, Value>& changes) {
    for (const auto& [name, value] : changes) {
        const KeySpec* spec = find_spec(name);
        if (spec == nullptr) {
            return Result<void>::err(ErrorCode::InvalidArgument, "unknown setting: " + name);
        }
        if (!spec->valid(value)) {
            return Result<void>::err(ErrorCode::InvalidArgument, "invalid value: " + name);
        }
    }
    if (changes.empty()) {
        return Result<void>::ok();
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

        auto r = backend_.set(spec->ns, spec->key, value);
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
        apply_brightness_(static_cast<uint8_t>(brightness->second.as_int()));
    }
    if (on_changed_) {
        on_changed_(get());
    }
    return Result<void>::ok();
}

}  // namespace wisp::core
