#include "wisp/core/system_settings.hpp"

#include <mutex>
#include <optional>
#include <string_view>
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

const KeySpec kBrightness{"brightness", "display", "brightness", Value(200),
                          [](const Value& v) { return int_in(v, 10, 255); }};
const KeySpec kVolume{"volume", "audio", "volume", Value(60),
                      [](const Value& v) { return int_in(v, 0, 100); }};
const KeySpec kScreenOff{"screen_off_s", "power", "screen_off_s", Value(60),
                         [](const Value& v) { return int_in(v, 0, 0) || int_in(v, 15, 3600); }};
const KeySpec kDeepAfter{"deep_after_s", "power", "deep_after_s", Value(300),
                         [](const Value& v) { return int_in(v, 0, 0) || int_in(v, 60, 3600); }};
const KeySpec kLightSleep{"light_sleep", "power", "light_sleep", Value(true),
                          [](const Value& v) { return v.type() == Value::Type::Bool; }};
const KeySpec kNotifyVol{"notify_vol", "audio", "notify_vol", Value(60),
                         [](const Value& v) { return int_in(v, 0, 100); }};
const KeySpec kLanguage{"language", "sys", "lang", Value("zh"), valid_language};

const KeySpec kUtcOffset{"utc_offset_min", "sys", "tz_min", Value(480),
                         [](const Value& v) { return int_in(v, -720, 840); }};

// One or two comma-separated time servers (connectivity spec 5.4), each a
// host name or an IPv4 address: 1-63 characters of [A-Za-z0-9.-]. No
// spaces, ports or IPv6 literals.
bool valid_ntp_servers(const Value& v) {
    if (v.type() != Value::Type::String) return false;
    std::string_view list = v.as_string();
    int entries = 0;
    while (true) {
        const size_t comma = list.find(',');
        std::string_view entry = list.substr(0, comma);
        if (entry.empty() || entry.size() > 63) return false;
        for (char c : entry) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!ok) return false;
        }
        if (++entries > 2) return false;
        if (comma == std::string_view::npos) return true;
        list.remove_prefix(comma + 1);
    }
}

const KeySpec kNtpServers{"ntp_servers", "sys", "ntp", Value(kDefaultNtpServers),
                          valid_ntp_servers};

const KeySpec* const kKeys[] = {&kBrightness, &kVolume,    &kScreenOff,  &kLanguage, &kUtcOffset,
                                &kNtpServers, &kDeepAfter, &kLightSleep, &kNotifyVol};

const KeySpec* find_spec(std::string_view name) {
    for (const KeySpec* spec : kKeys) {
        if (name == spec->name) return spec;
    }
    return nullptr;
}

// The stored value, or the default when it is missing or one set() would
// refuse (a hand-edited or corrupted store): every value get() reports is
// one its consumers can take, and an int64 past int's range never reaches
// the narrowing in get().
Value read(SettingsBackend& backend, const KeySpec& spec) {
    auto stored = backend.get(spec.ns, spec.key);
    if (!stored || !spec.valid(stored.value())) return spec.def;
    return stored.value();
}

}  // namespace

Map to_map(const SystemSettingsValues& values) {
    return Map{
        {"brightness", Value(values.brightness)},
        {"volume", Value(values.volume)},
        {"screen_off_s", Value(values.screen_off_s)},
        {"language", Value(values.language)},
        {"utc_offset_min", Value(values.utc_offset_min)},
        {"ntp_servers", Value(values.ntp_servers)},
        {"deep_after_s", Value(values.deep_after_s)},
        {"light_sleep", Value(values.light_sleep)},
        {"notify_vol", Value(values.notify_vol)},
    };
}

SystemSettings::SystemSettings(SettingsBackend& backend,
                               std::function<void(uint8_t)> apply_brightness,
                               std::function<void(const SystemSettingsValues&)> on_changed)
    : backend_(backend),
      apply_brightness_(std::move(apply_brightness)),
      on_changed_(std::move(on_changed)) {}

SystemSettingsValues SystemSettings::get() const {
    SystemSettingsValues values;
    values.brightness = static_cast<int>(read(backend_, kBrightness).as_int());
    values.volume = static_cast<int>(read(backend_, kVolume).as_int());
    values.screen_off_s = static_cast<int>(read(backend_, kScreenOff).as_int());
    values.language = read(backend_, kLanguage).as_string();
    values.utc_offset_min = static_cast<int>(read(backend_, kUtcOffset).as_int());
    values.ntp_servers = read(backend_, kNtpServers).as_string();
    values.deep_after_s = static_cast<int>(read(backend_, kDeepAfter).as_int());
    values.light_sleep = read(backend_, kLightSleep).as_bool();
    values.notify_vol = static_cast<int>(read(backend_, kNotifyVol).as_int());
    return values;
}

uint32_t SystemSettings::revision() const { return revision_.load(std::memory_order_acquire); }

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

    revision_.fetch_add(1, std::memory_order_acq_rel);
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
