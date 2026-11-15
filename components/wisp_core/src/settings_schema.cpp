#include "wisp/core/settings_schema.hpp"

#include <string>
#include <type_traits>

namespace wisp::core {

namespace {

// Each store_field() takes a value valid_setting() has already accepted for
// the field's row.
void store_field(int& field, const Value& v) { field = static_cast<int>(v.as_int()); }
void store_field(bool& field, const Value& v) { field = v.as_bool(); }
void store_field(LangCode& field, const Value& v) { field = *parse_lang_code(v.as_string()); }
template <size_t N>
void store_field(FixedString<N>& field, const Value& v) {
    field.assign(v.as_string());
}

bool is_enum_value(const SettingSpec& spec, std::string_view code) {
    for (std::string_view value : spec.enum_values) {
        if (value == code) return true;
    }
    return false;
}

}  // namespace

bool valid_setting(const SettingSpec& spec, const Value& v) {
    switch (spec.type) {
        case SettingType::Int:
            return v.type() == Value::Type::Int && setting_int_in_range(spec, v.as_int());
        case SettingType::Bool:
            return v.type() == Value::Type::Bool;
        case SettingType::Enum:
            return v.type() == Value::Type::String && is_enum_value(spec, v.as_string());
        case SettingType::HostList:
            return v.type() == Value::Type::String &&
                   host_list_syntax_ok(v.as_string(), spec.min, spec.max);
    }
    return false;
}

Value setting_value(const SettingsSnapshot& s, SettingId id) {
    return visit_setting(s, id, [](auto v) -> Value {
        using T = decltype(v);
        if constexpr (std::is_same_v<T, std::string_view>) {
            return Value(std::string(v));
        } else if constexpr (std::is_same_v<T, int>) {
            return Value(static_cast<int64_t>(v));
        } else {
            return Value(v);
        }
    });
}

bool assign_setting(SettingsSnapshot& s, SettingId id, const Value& v) {
    if (static_cast<size_t>(id) >= std::size(kSettingsSchema)) return false;
    if (!valid_setting(setting_spec(id), v)) return false;
    switch (id) {
#define WISP_SETTING_ASSIGN(Id, name, ...) \
    case SettingId::Id:                    \
        store_field(s.name, v);            \
        return true;
        WISP_SETTINGS(WISP_SETTING_ASSIGN)
#undef WISP_SETTING_ASSIGN
        case SettingId::Count:
            break;
    }
    return false;
}

}  // namespace wisp::core
