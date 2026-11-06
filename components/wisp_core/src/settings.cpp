#include "wisp/core/settings.hpp"

#include <utility>

#include "wisp/base/json.hpp"

namespace wisp::core {

Result<void> validate_settings_name(std::string_view name) {
    if (name.empty() || name.size() > 15) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "settings name must be 1-15 characters: " + std::string(name));
    }
    return Result<void>::ok();
}

Result<void> validate_settings_value(std::string_view ns, std::string_view key,
                                     const Value& value) {
    if (to_json_raw(value).size() > kSettingsMaxValueBytes) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "settings value too large (>" +
                                     std::to_string(kSettingsMaxValueBytes) +
                                     " bytes): " + std::string(ns) + "/" + std::string(key));
    }
    return Result<void>::ok();
}

Settings::Settings(SettingsBackend& backend, std::string ns)
    : backend_(backend), ns_(std::move(ns)) {}

int64_t Settings::get_int(const std::string& key, int64_t def) const {
    Result<Value> r = backend_.get(ns_, key);
    if (!r || r.value().type() != Value::Type::Int) {
        return def;
    }
    return r.value().as_int();
}

bool Settings::get_bool(const std::string& key, bool def) const {
    Result<Value> r = backend_.get(ns_, key);
    if (!r || r.value().type() != Value::Type::Bool) {
        return def;
    }
    return r.value().as_bool();
}

std::string Settings::get_string(const std::string& key, const std::string& def) const {
    Result<Value> r = backend_.get(ns_, key);
    if (!r || r.value().type() != Value::Type::String) {
        return def;
    }
    return r.value().as_string();
}

Result<void> Settings::set(const std::string& key, const Value& value) {
    return backend_.set(ns_, key, value);
}

}  // namespace wisp::core
