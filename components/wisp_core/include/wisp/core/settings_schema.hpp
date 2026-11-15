#pragma once

// The settings schema (settings-schema spec 3.2): WISP_SETTINGS below is the
// one place a system setting is declared. Its rows expand into SettingId,
// the constexpr table kSettingsSchema and the trivially copyable
// SettingsSnapshot the settings cell holds, so the three cannot disagree.
// Every surface (SystemSettings, REST, the agent, scenes, the Shell page,
// the web page) takes keys, types, ranges, defaults and choices from here;
// presentation (layout, widgets, label texts) lives with each surface.
//
// tools/check_settings.py parses the list one `X(` row per line, so the
// list stays inside clang-format off/on with each row on a single line.
//
// Reading the table is a read of constants, allowed from any task
// (AGENTS.md section 5).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "wisp/base/fixed_string.hpp"
#include "wisp/base/value.hpp"

namespace wisp::core {

enum class SettingType : uint8_t { Int, Bool, Enum, HostList };
enum class SettingUnit : uint8_t {
    None,
    Level,
    Percent,
    Seconds,
    Minutes,
    UtcOffsetMinutes,
    Steps
};

// Who sees a key; each surface filters by its bit.
inline constexpr uint8_t kSettingAgent = 1 << 0;  // settings.get / settings.set
inline constexpr uint8_t kSettingScene = 1 << 1;  // wisp.device.info(), under scene_name
inline constexpr uint8_t kSettingWeb =
    1 << 2;  // GET/PUT /settings, system.settings, /settings/schema
inline constexpr uint8_t kSettingDevicePage = 1 << 3;  // the Shell settings page
inline constexpr uint8_t kSettingZeroNever = 1 << 4;   // 0 is valid ("never") besides [min, max]

// The device UI languages, stored as their codes. The order is the stored
// meaning and matches ui::Lang (wisp_ui checks it); it is not a display
// order.
enum class LangCode : uint8_t { Zh, Ja, En };
inline constexpr std::string_view kLangCodeValues[] = {"zh", "ja", "en"};  // index = LangCode

constexpr std::string_view lang_code(LangCode c) { return kLangCodeValues[static_cast<size_t>(c)]; }

constexpr std::optional<LangCode> parse_lang_code(std::string_view code) {
    for (size_t i = 0; i < std::size(kLangCodeValues); ++i) {
        if (kLangCodeValues[i] == code) return static_cast<LangCode>(i);
    }
    return std::nullopt;
}

// Suggested values for Int rows (spec decision 4; the web's time-zone list).
inline constexpr int32_t kScreenOffChoices[] = {0, 15, 30, 60, 120, 300, 600};
inline constexpr int32_t kDeepAfterChoices[] = {0, 60, 120, 300, 600, 1800, 3600};
inline constexpr int32_t kUtcOffsetChoices[] = {
    -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120,
    -60,  0,    60,   120,  180,  210,  240,  270,  300,  330,  345,  360,  390,
    420,  480,  525,  540,  570,  600,  630,  660,  720,  765,  780,  840};
inline constexpr std::span<const int32_t> kNoIntChoices{};
inline constexpr std::span<const std::string_view> kNoEnumValues{};

struct SettingSpec {
    std::string_view name{};  // wire name: REST, event, agent; also the snapshot field
    std::string_view ns{};    // store namespace, 1-15 chars
    std::string_view key{};   // store key, 1-15 chars
    SettingType type = SettingType::Int;
    int32_t min = 0;              // Int: lowest; HostList: max entries
    int32_t max = 0;              // Int: highest; HostList: max entry length
    int32_t def_int = 0;          // Int value; Bool 0/1; Enum index into enum_values
    std::string_view def_text{};  // HostList only
    std::span<const int32_t> int_choices{};
    std::span<const std::string_view> enum_values{};
    SettingUnit unit = SettingUnit::None;
    uint8_t flags = 0;
    std::string_view label{};       // label id (spec 3.1); empty: no surface shows it
    std::string_view scene_name{};  // wisp.device.info() field; set exactly with kSettingScene
};

// Columns: Id, name, C++ field type, SettingType, ns, key, min, max, def_int,
// def_text, int_choices, enum_values, SettingUnit, flags, label, scene_name.
// Snapshot integers are int, not int32_t: int32_t is long on the ESP-IDF
// toolchains, which would break mixed std::clamp/std::min and %d uses.
// clang-format off
#define WISP_SETTINGS(X) \
X(Brightness,    brightness,      int,              Int,      "display", "brightness",   10,   255,  200, "",                            kNoIntChoices,     kNoEnumValues,   Level,            kSettingAgent | kSettingWeb | kSettingDevicePage,                     "brightness",    "") \
X(Volume,        volume,          int,              Int,      "audio",   "volume",       0,    100,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          kSettingAgent | kSettingWeb | kSettingDevicePage,                     "volume",        "") \
X(ScreenOffS,    screen_off_s,    int,              Int,      "power",   "screen_off_s", 15,   3600, 60,  "",                            kScreenOffChoices, kNoEnumValues,   Seconds,          kSettingAgent | kSettingWeb | kSettingDevicePage | kSettingZeroNever, "screen_off",    "") \
X(Language,      language,        LangCode,         Enum,     "sys",     "lang",         0,    0,    0,   "",                            kNoIntChoices,     kLangCodeValues, None,             kSettingAgent | kSettingScene | kSettingWeb | kSettingDevicePage,     "lang_label",    "lang") \
X(UtcOffsetMin,  utc_offset_min,  int,              Int,      "sys",     "tz_min",       -720, 840,  480, "",                            kUtcOffsetChoices, kNoEnumValues,   UtcOffsetMinutes, kSettingAgent | kSettingScene | kSettingWeb | kSettingDevicePage,                     "tz_label",      "utc_offset") \
X(NtpServers,    ntp_servers,     FixedString<127>, HostList, "sys",     "ntp",          2,    63,   0,   "pool.ntp.org,ntp.aliyun.com", kNoIntChoices,     kNoEnumValues,   None,             kSettingWeb,                                                          "time_servers",  "") \
X(DeepAfterS,    deep_after_s,    int,              Int,      "power",   "deep_after_s", 60,   3600, 300, "",                            kDeepAfterChoices, kNoEnumValues,   Seconds,          kSettingAgent | kSettingWeb | kSettingDevicePage | kSettingZeroNever, "deep_after",    "") \
X(NotifyVol,     notify_vol,      int,              Int,      "audio",   "notify_vol",   0,    100,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          kSettingAgent | kSettingWeb | kSettingDevicePage,                     "notify_vol",    "") \
X(TtsMute,       tts_mute,        bool,             Bool,     "voice",   "tts_mute",     0,    1,    0,   "",                            kNoIntChoices,     kNoEnumValues,   None,             kSettingAgent | kSettingWeb | kSettingDevicePage,                     "tts_mute",      "") \
X(Time24h,       time_24h,        bool,             Bool,     "sys",     "time_24h",     0,    1,    1,   "",                            kNoIntChoices,     kNoEnumValues,   None,             kSettingAgent | kSettingScene | kSettingWeb | kSettingDevicePage,     "time_24h",      "time_24h") \
X(AgentMaxSteps, agent_max_steps, int,              Int,      "agent",   "max_steps",    8,    200,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Steps,            kSettingWeb,                                                          "agent_steps",   "") \
X(AgentTurnMin,  agent_turn_min,  int,              Int,      "agent",   "turn_min",     2,    60,   15,  "",                            kNoIntChoices,     kNoEnumValues,   Minutes,          kSettingWeb,                                                          "agent_minutes", "") \
X(LowBattPct,    low_batt_pct,    int,              Int,      "power",   "low_batt_pct", 5,    50,   15,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          0,                                                                    "",              "") \
X(PerfOverlay,   perf_overlay,    bool,             Bool,     "ui",      "perf_overlay", 0,    1,    0,   "",                            kNoIntChoices,     kNoEnumValues,   None,             0,                                                                    "",              "")
// clang-format on

enum class SettingId : uint8_t {
#define WISP_SETTING_ID(Id, ...) Id,
    WISP_SETTINGS(WISP_SETTING_ID)
#undef WISP_SETTING_ID
        Count
};

inline constexpr SettingSpec kSettingsSchema[] = {
#define WISP_SETTING_SPEC(Id, name, ctype, type, ns, key, min, max, def_int, def_text, choices, \
                          values, unit, flags, label, scene_name)                               \
    SettingSpec{#name,                                                                          \
                ns,                                                                             \
                key,                                                                            \
                SettingType::type,                                                              \
                min,                                                                            \
                max,                                                                            \
                def_int,                                                                        \
                def_text,                                                                       \
                choices,                                                                        \
                values,                                                                         \
                SettingUnit::unit,                                                              \
                static_cast<uint8_t>(flags),                                                    \
                label,                                                                          \
                scene_name},
    WISP_SETTINGS(WISP_SETTING_SPEC)
#undef WISP_SETTING_SPEC
};

constexpr const SettingSpec& setting_spec(SettingId id) {
    return kSettingsSchema[static_cast<size_t>(id)];
}

// `spec` must be a row of kSettingsSchema.
constexpr SettingId setting_id_of(const SettingSpec& spec) {
    return static_cast<SettingId>(&spec - kSettingsSchema);
}

constexpr int32_t setting_default_int(SettingId id) { return setting_spec(id).def_int; }

// Enum: the default code; HostList: def_text; other types: "".
constexpr std::string_view setting_default_text(SettingId id) {
    const SettingSpec& spec = setting_spec(id);
    switch (spec.type) {
        case SettingType::Enum:
            return spec.enum_values[static_cast<size_t>(spec.def_int)];
        case SettingType::HostList:
            return spec.def_text;
        case SettingType::Int:
        case SettingType::Bool:
            break;
    }
    return {};
}

// nullptr when no row has that wire name.
constexpr const SettingSpec* find_setting(std::string_view name) {
    for (const SettingSpec& spec : kSettingsSchema) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

// An Int row's range rule: [min, max], or 0 on a kSettingZeroNever row.
constexpr bool setting_int_in_range(const SettingSpec& spec, int64_t v) {
    return (v >= spec.min && v <= spec.max) || (v == 0 && (spec.flags & kSettingZeroNever) != 0);
}

// A HostList's syntax (connectivity spec 5.4): 1 to max_entries
// comma-separated entries, each a host name or an IPv4 address of 1 to
// max_entry_len characters [A-Za-z0-9.-]. No spaces, ports or IPv6
// literals. constexpr so the table's HostList defaults are checked at
// compile time by the same rule set() applies.
constexpr bool host_list_syntax_ok(std::string_view list, int32_t max_entries,
                                   int32_t max_entry_len) {
    int32_t entries = 0;
    while (true) {
        const size_t comma = list.find(',');
        const std::string_view entry = list.substr(0, comma);
        if (entry.empty() || entry.size() > static_cast<size_t>(max_entry_len)) return false;
        for (char c : entry) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!ok) return false;
        }
        if (++entries > max_entries) return false;
        if (comma == std::string_view::npos) return true;
        list.remove_prefix(comma + 1);
    }
}

// The C++ field types a row may declare, with the SettingType each one
// carries. Undefined for any other type, so a row whose field type has no
// SettingType (long, std::string) fails here, on the host, rather than at
// link time on the device.
template <class T>
struct SettingField;
template <>
struct SettingField<int> {
    static constexpr SettingType type = SettingType::Int;
};
template <>
struct SettingField<bool> {
    static constexpr SettingType type = SettingType::Bool;
};
template <>
struct SettingField<LangCode> {
    static constexpr SettingType type = SettingType::Enum;
};
template <size_t N>
struct SettingField<FixedString<N>> {
    static constexpr SettingType type = SettingType::HostList;
    static constexpr size_t capacity = N;
};

// The snapshot members' initializers: each row's default as its C++ type.
// Deleted for any type without a specialization below.
template <class T>
T setting_init(SettingId id) = delete;
template <>
constexpr int setting_init<int>(SettingId id) {
    return setting_default_int(id);
}
template <>
constexpr bool setting_init<bool>(SettingId id) {
    return setting_default_int(id) != 0;
}
template <>
constexpr LangCode setting_init<LangCode>(SettingId id) {
    return static_cast<LangCode>(setting_default_int(id));
}
template <>
inline FixedString<127> setting_init<FixedString<127>>(SettingId id) {
    return FixedString<127>(setting_spec(id).def_text);
}

// Default-constructed, it is what an empty store reads as.
struct SettingsSnapshot {
#define WISP_SETTING_FIELD(Id, name, ctype, ...) ctype name = setting_init<ctype>(SettingId::Id);
    WISP_SETTINGS(WISP_SETTING_FIELD)
#undef WISP_SETTING_FIELD

    bool operator==(const SettingsSnapshot&) const = default;
};

namespace schema_check {

consteval bool row_count_matches() {
    return std::size(kSettingsSchema) == static_cast<size_t>(SettingId::Count);
}

consteval bool names_unique() {
    for (size_t i = 0; i < std::size(kSettingsSchema); ++i) {
        for (size_t j = i + 1; j < std::size(kSettingsSchema); ++j) {
            if (kSettingsSchema[i].name == kSettingsSchema[j].name) return false;
        }
    }
    return true;
}

consteval bool store_names_unique() {
    for (size_t i = 0; i < std::size(kSettingsSchema); ++i) {
        for (size_t j = i + 1; j < std::size(kSettingsSchema); ++j) {
            if (kSettingsSchema[i].ns == kSettingsSchema[j].ns &&
                kSettingsSchema[i].key == kSettingsSchema[j].key) {
                return false;
            }
        }
    }
    return true;
}

// validate_settings_name()'s rule (settings.hpp).
consteval bool store_names_fit() {
    for (const SettingSpec& spec : kSettingsSchema) {
        if (spec.ns.empty() || spec.ns.size() > 15) return false;
        if (spec.key.empty() || spec.key.size() > 15) return false;
    }
    return true;
}

consteval bool defaults_valid() {
    for (const SettingSpec& spec : kSettingsSchema) {
        switch (spec.type) {
            case SettingType::Int:
                if (!setting_int_in_range(spec, spec.def_int)) return false;
                break;
            case SettingType::Bool:
                if (spec.def_int != 0 && spec.def_int != 1) return false;
                break;
            case SettingType::Enum:
                if (spec.def_int < 0 ||
                    static_cast<size_t>(spec.def_int) >= spec.enum_values.size()) {
                    return false;
                }
                break;
            case SettingType::HostList:
                if (!host_list_syntax_ok(spec.def_text, spec.min, spec.max)) return false;
                break;
        }
    }
    return true;
}

consteval bool int_choices_valid() {
    for (const SettingSpec& spec : kSettingsSchema) {
        for (int32_t choice : spec.int_choices) {
            if (spec.type != SettingType::Int || !setting_int_in_range(spec, choice)) return false;
        }
    }
    return true;
}

// Strictly ascending: a page that lists a stored value missing from the
// choices puts it in order among them (the Shell settings page), and a
// zero-means-never row's 0 is its first choice.
consteval bool int_choices_ascending() {
    for (const SettingSpec& spec : kSettingsSchema) {
        for (size_t i = 1; i < spec.int_choices.size(); ++i) {
            if (spec.int_choices[i - 1] >= spec.int_choices[i]) return false;
        }
    }
    return true;
}

consteval bool scene_flag_matches_scene_name() {
    for (const SettingSpec& spec : kSettingsSchema) {
        if (((spec.flags & kSettingScene) != 0) == spec.scene_name.empty()) return false;
    }
    return true;
}

// A HostList's longest valid text (max_entries entries of max_entry_len
// characters and the commas between them) fits a field of `capacity` bytes,
// so assign_setting() never truncates a value valid_setting() accepted.
constexpr bool host_list_fits(const SettingSpec& spec, size_t capacity) {
    if (spec.type != SettingType::HostList) return true;
    const int64_t longest = int64_t{spec.min} * spec.max + (spec.min - 1);
    return longest <= static_cast<int64_t>(capacity);
}

template <class T>
consteval size_t field_capacity() {
    if constexpr (requires { SettingField<T>::capacity; }) {
        return SettingField<T>::capacity;
    } else {
        return 0;
    }
}

consteval bool shown_rows_have_labels() {
    constexpr uint8_t kShown = kSettingAgent | kSettingWeb | kSettingDevicePage;
    for (const SettingSpec& spec : kSettingsSchema) {
        if ((spec.flags & kShown) != 0 && spec.label.empty()) return false;
    }
    return true;
}

}  // namespace schema_check

static_assert(schema_check::row_count_matches(), "kSettingsSchema and SettingId disagree");
static_assert(schema_check::names_unique(), "two settings share a name");
static_assert(schema_check::store_names_unique(), "two settings share a (ns, key)");
static_assert(schema_check::store_names_fit(), "a settings ns or key is not 1-15 characters");
static_assert(schema_check::defaults_valid(), "a setting's default breaks its own rule");
static_assert(schema_check::int_choices_valid(), "a setting's choice breaks its own rule");
static_assert(schema_check::int_choices_ascending(), "a setting's choices are not ascending");
static_assert(schema_check::scene_flag_matches_scene_name(),
              "kSettingScene must be set exactly when scene_name is non-empty");
static_assert(schema_check::shown_rows_have_labels(), "a setting a surface shows has no label");
#define WISP_SETTING_FIELD_CHECK(Id, name, ctype, stype, ...)                          \
    static_assert(SettingField<ctype>::type == SettingType::stype,                     \
                  "the field type of " #name " does not match its SettingType");       \
    static_assert(schema_check::host_list_fits(setting_spec(SettingId::Id),            \
                                               schema_check::field_capacity<ctype>()), \
                  "the longest valid " #name " does not fit its field");
WISP_SETTINGS(WISP_SETTING_FIELD_CHECK)
#undef WISP_SETTING_FIELD_CHECK
static_assert(std::is_trivially_copyable_v<SettingsSnapshot>, "the settings cell copies it");
static_assert(sizeof(SettingsSnapshot) <= 256, "StateCell's size limit (state_cell.hpp)");

// The row's type and range, as SystemSettings::set() checks them.
bool valid_setting(const SettingSpec& spec, const Value& v);

// Int -> Value(int64_t), Bool -> bool, Enum -> its code string, HostList -> string.
Value setting_value(const SettingsSnapshot& s, SettingId id);

// Stores `v` in the field of `id`; false, leaving `s` unchanged, unless
// valid_setting() accepts it.
bool assign_setting(SettingsSnapshot& s, SettingId id, const Value& v);

namespace detail {
inline int visit_arg(int v) { return v; }
inline bool visit_arg(bool v) { return v; }
inline std::string_view visit_arg(LangCode v) { return lang_code(v); }
template <size_t N>
std::string_view visit_arg(const FixedString<N>& v) {
    return v.view();
}
}  // namespace detail

// Calls f with the field of `id` as int, bool or std::string_view (Enum: the
// code; HostList: the text), without allocating. An id outside the table
// (SettingId::Count) visits the first row rather than reading past it.
template <class F>
decltype(auto) visit_setting(const SettingsSnapshot& s, SettingId id, F&& f) {
    switch (id) {
        default:
#define WISP_SETTING_VISIT(Id, name, ...) \
    case SettingId::Id:                   \
        return f(detail::visit_arg(s.name));
            WISP_SETTINGS(WISP_SETTING_VISIT)
#undef WISP_SETTING_VISIT
    }
}

}  // namespace wisp::core
