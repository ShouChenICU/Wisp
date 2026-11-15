#include "wisp/core/settings_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/system_settings.hpp"

// Readable gtest failure output for Value comparisons.
namespace wisp {
void PrintTo(const Value& v, std::ostream* os) { *os << to_json(v); }
}  // namespace wisp

using wisp::Value;
using wisp::core::find_setting;
using wisp::core::kSettingAgent;
using wisp::core::kSettingDevicePage;
using wisp::core::kSettingScene;
using wisp::core::kSettingsSchema;
using wisp::core::kSettingWeb;
using wisp::core::kSettingZeroNever;
using wisp::core::LangCode;
using wisp::core::setting_spec;
using wisp::core::SettingId;
using wisp::core::SettingSpec;
using wisp::core::SettingsSnapshot;

namespace {

const SettingSpec& spec_named(std::string_view name) {
    const SettingSpec* spec = find_setting(name);
    EXPECT_NE(spec, nullptr) << name;
    return spec != nullptr ? *spec : kSettingsSchema[0];
}

bool valid(std::string_view name, const Value& v) {
    return wisp::core::valid_setting(spec_named(name), v);
}

// The names of the rows carrying `flag`, in table order.
std::vector<std::string_view> names_with(uint8_t flag) {
    std::vector<std::string_view> names;
    for (const SettingSpec& spec : kSettingsSchema) {
        if ((spec.flags & flag) != 0) names.push_back(spec.name);
    }
    return names;
}

}  // namespace

TEST(SettingsSchema, RowsInTableOrder) {
    std::vector<std::string_view> names;
    for (const SettingSpec& spec : kSettingsSchema) names.push_back(spec.name);
    const std::vector<std::string_view> expected{
        "brightness",      "volume",         "screen_off_s", "language",    "utc_offset_min",
        "ntp_servers",     "deep_after_s",   "notify_vol",   "tts_mute",    "time_24h",
        "agent_max_steps", "agent_turn_min", "low_batt_pct", "perf_overlay"};
    EXPECT_EQ(names, expected);
    EXPECT_EQ(std::size(kSettingsSchema), static_cast<size_t>(SettingId::Count));
    for (size_t i = 0; i < std::size(kSettingsSchema); ++i) {
        const auto id = static_cast<SettingId>(i);
        EXPECT_EQ(&setting_spec(id), &kSettingsSchema[i]);
        EXPECT_EQ(wisp::core::setting_id_of(kSettingsSchema[i]), id);
        EXPECT_EQ(find_setting(kSettingsSchema[i].name), &kSettingsSchema[i]);
    }
}

TEST(SettingsSchema, StoreNamesAreValidAndUnique) {
    std::set<std::pair<std::string_view, std::string_view>> store_names;
    std::set<std::string_view> names;
    std::set<std::string_view> scene_names;
    for (const SettingSpec& spec : kSettingsSchema) {
        EXPECT_TRUE(wisp::core::validate_settings_name(spec.ns)) << spec.name;
        EXPECT_TRUE(wisp::core::validate_settings_name(spec.key)) << spec.name;
        EXPECT_TRUE(store_names.emplace(spec.ns, spec.key).second) << spec.name;
        EXPECT_TRUE(names.insert(spec.name).second) << spec.name;
        if (!spec.scene_name.empty()) {
            EXPECT_TRUE(scene_names.insert(spec.scene_name).second) << spec.name;
        }
    }
}

TEST(SettingsSchema, DefaultsMatchTodaysValues) {
    const SettingsSnapshot d{};
    EXPECT_EQ(d.brightness, 200);
    EXPECT_EQ(d.volume, 60);
    EXPECT_EQ(d.screen_off_s, 60);
    EXPECT_EQ(d.language, LangCode::Zh);
    EXPECT_EQ(d.utc_offset_min, 480);
    EXPECT_EQ(d.ntp_servers, std::string_view("pool.ntp.org,ntp.aliyun.com"));
    EXPECT_EQ(d.deep_after_s, 300);
    EXPECT_EQ(d.notify_vol, 60);
    EXPECT_EQ(d.tts_mute, false);
    EXPECT_EQ(d.time_24h, true);
    EXPECT_EQ(d.agent_max_steps, 60);
    EXPECT_EQ(d.agent_turn_min, 15);
    EXPECT_EQ(d.low_batt_pct, 15);
    EXPECT_EQ(d.perf_overlay, false);
    // Snapshot integers are int, not int32_t (long on the ESP-IDF toolchains).
    static_assert(std::is_same_v<decltype(d.brightness), int>);
    // Every default reads back as valid by the row's own rule.
    for (const SettingSpec& spec : kSettingsSchema) {
        EXPECT_TRUE(wisp::core::valid_setting(
            spec, wisp::core::setting_value(d, wisp::core::setting_id_of(spec))))
            << spec.name;
    }
}

TEST(SettingsSchema, ValidationPerRow) {
    EXPECT_FALSE(valid("brightness", 9));
    EXPECT_TRUE(valid("brightness", 10));
    EXPECT_TRUE(valid("brightness", 255));
    EXPECT_FALSE(valid("brightness", 256));
    EXPECT_FALSE(valid("brightness", "200"));

    EXPECT_FALSE(valid("volume", -1));
    EXPECT_TRUE(valid("volume", 0));
    EXPECT_TRUE(valid("volume", 100));
    EXPECT_FALSE(valid("volume", 101));
    EXPECT_FALSE(valid("volume", 30.5));

    EXPECT_TRUE(valid("screen_off_s", 0));
    EXPECT_FALSE(valid("screen_off_s", 14));
    EXPECT_TRUE(valid("screen_off_s", 15));
    EXPECT_TRUE(valid("screen_off_s", 3600));
    EXPECT_FALSE(valid("screen_off_s", 3601));

    EXPECT_TRUE(valid("deep_after_s", 0));
    EXPECT_FALSE(valid("deep_after_s", 59));
    EXPECT_TRUE(valid("deep_after_s", 60));
    EXPECT_FALSE(valid("deep_after_s", 3601));

    EXPECT_FALSE(valid("utc_offset_min", -721));
    EXPECT_TRUE(valid("utc_offset_min", -720));
    EXPECT_TRUE(valid("utc_offset_min", 840));
    EXPECT_FALSE(valid("utc_offset_min", 841));

    EXPECT_TRUE(valid("language", "zh"));
    EXPECT_TRUE(valid("language", "ja"));
    EXPECT_TRUE(valid("language", "en"));
    EXPECT_FALSE(valid("language", "fr"));
    EXPECT_FALSE(valid("language", 1));

    EXPECT_TRUE(valid("ntp_servers", "a.b"));
    EXPECT_TRUE(valid("ntp_servers", "a,b"));
    EXPECT_FALSE(valid("ntp_servers", "a,b,c"));
    EXPECT_FALSE(valid("ntp_servers", ""));
    EXPECT_FALSE(valid("ntp_servers", "a b"));
    EXPECT_FALSE(valid("ntp_servers", std::string(64, 'a')));
    EXPECT_TRUE(valid("ntp_servers", std::string(63, 'a')));
    EXPECT_FALSE(valid("ntp_servers", 1));

    EXPECT_TRUE(valid("tts_mute", true));
    EXPECT_FALSE(valid("tts_mute", 1));
    EXPECT_TRUE(valid("time_24h", false));
    EXPECT_FALSE(valid("time_24h", "true"));

    EXPECT_FALSE(valid("agent_max_steps", 7));
    EXPECT_TRUE(valid("agent_max_steps", 8));
    EXPECT_TRUE(valid("agent_max_steps", 200));
    EXPECT_FALSE(valid("agent_max_steps", 201));

    EXPECT_FALSE(valid("agent_turn_min", 1));
    EXPECT_TRUE(valid("agent_turn_min", 2));
    EXPECT_TRUE(valid("agent_turn_min", 60));
    EXPECT_FALSE(valid("agent_turn_min", 61));

    EXPECT_FALSE(valid("low_batt_pct", 4));
    EXPECT_TRUE(valid("low_batt_pct", 5));
    EXPECT_TRUE(valid("low_batt_pct", 50));
    EXPECT_FALSE(valid("low_batt_pct", 51));

    EXPECT_TRUE(valid("perf_overlay", true));
    EXPECT_FALSE(valid("perf_overlay", 0));

    // Every Int choice is a value set() takes.
    for (const SettingSpec& spec : kSettingsSchema) {
        for (int32_t choice : spec.int_choices) {
            EXPECT_TRUE(wisp::core::valid_setting(spec, Value(int64_t{choice})))
                << spec.name << " " << choice;
        }
    }
}

TEST(SettingsSchema, AgentFlags) {
    using V = std::vector<std::string_view>;
    EXPECT_EQ(names_with(kSettingAgent),
              (V{"brightness", "volume", "screen_off_s", "language", "utc_offset_min",
                 "deep_after_s", "notify_vol", "tts_mute", "time_24h"}));
    EXPECT_EQ(names_with(kSettingWeb),
              (V{"brightness", "volume", "screen_off_s", "language", "utc_offset_min",
                 "ntp_servers", "deep_after_s", "notify_vol", "tts_mute", "time_24h",
                 "agent_max_steps", "agent_turn_min"}));
    EXPECT_EQ(names_with(kSettingScene), (V{"language", "utc_offset_min", "time_24h"}));
    V scene_names;
    for (const SettingSpec& spec : kSettingsSchema) {
        if (!spec.scene_name.empty()) scene_names.push_back(spec.scene_name);
    }
    EXPECT_EQ(scene_names, (V{"lang", "utc_offset", "time_24h"}));
    EXPECT_EQ(names_with(kSettingDevicePage),
              (V{"brightness", "volume", "screen_off_s", "language", "utc_offset_min",
                 "deep_after_s", "notify_vol", "tts_mute", "time_24h"}));
    EXPECT_EQ(names_with(kSettingZeroNever), (V{"screen_off_s", "deep_after_s"}));
}

TEST(SettingsSchema, ValuesAndVisitAgree) {
    using wisp::core::assign_setting;
    using wisp::core::setting_value;
    SettingsSnapshot s{};
    EXPECT_EQ(setting_value(s, SettingId::Language), Value("zh"));
    EXPECT_EQ(setting_value(s, SettingId::NtpServers), Value("pool.ntp.org,ntp.aliyun.com"));
    EXPECT_EQ(setting_value(s, SettingId::Brightness), Value(int64_t{200}));
    EXPECT_EQ(setting_value(s, SettingId::TtsMute), Value(false));

    std::string seen;
    wisp::core::visit_setting(s, SettingId::Language, [&](auto v) {
        if constexpr (std::is_same_v<decltype(v), std::string_view>) {
            seen = std::string(v);
        } else {
            seen = "not a string_view";
        }
    });
    EXPECT_EQ(seen, "zh");

    EXPECT_TRUE(assign_setting(s, SettingId::Language, Value("ja")));
    EXPECT_EQ(s.language, LangCode::Ja);

    const SettingsSnapshot before = s;
    EXPECT_FALSE(assign_setting(s, SettingId::Brightness, Value(5)));
    EXPECT_EQ(s, before);
    EXPECT_FALSE(assign_setting(s, SettingId::Language, Value("fr")));
    EXPECT_EQ(s, before);

    EXPECT_TRUE(assign_setting(s, SettingId::NtpServers, Value("time.example")));
    EXPECT_EQ(s.ntp_servers, std::string_view("time.example"));
    EXPECT_TRUE(assign_setting(s, SettingId::Brightness, Value(int64_t{128})));
    EXPECT_EQ(s.brightness, 128);
    EXPECT_TRUE(assign_setting(s, SettingId::Time24h, Value(false)));
    EXPECT_FALSE(s.time_24h);
}

TEST(SettingsSchema, LangCodes) {
    EXPECT_EQ(wisp::core::lang_code(LangCode::Ja), "ja");
    EXPECT_EQ(wisp::core::parse_lang_code("en"), LangCode::En);
    EXPECT_EQ(wisp::core::parse_lang_code("fr"), std::nullopt);
}

TEST(SettingsSchema, DefaultAccessors) {
    EXPECT_EQ(wisp::core::setting_default_int(SettingId::UtcOffsetMin), 480);
    EXPECT_EQ(wisp::core::setting_default_text(SettingId::Language), "zh");
    EXPECT_EQ(wisp::core::setting_default_text(SettingId::NtpServers),
              "pool.ntp.org,ntp.aliyun.com");
    EXPECT_EQ(wisp::core::setting_default_text(SettingId::Brightness), "");
    EXPECT_EQ(find_setting("nope"), nullptr);
}

// A row whose C++ field type disagrees with its SettingType must not
// compile. Checked by hand: changing the Brightness row's field type from
// `int` to `bool` (or to `long`) in WISP_SETTINGS fails the build in
// settings_schema.hpp, at the per-row static_assert or at the deleted
// setting_init primary, on the host as on the device; raising the ntp row's
// max entry length to 64 fails the host_list_fits static_assert.
template <class T>
concept HasSettingField = requires { wisp::core::SettingField<T>::type; };
template <class T>
concept HasSettingInit = requires { wisp::core::setting_init<T>(SettingId::Brightness); };

TEST(SettingsSchema, FieldTypesMatchSettingTypes) {
    static_assert(HasSettingField<int> && HasSettingField<bool> && HasSettingField<LangCode> &&
                  HasSettingField<wisp::FixedString<127>>);
    static_assert(!HasSettingField<long> && !HasSettingField<std::string>);
    static_assert(HasSettingInit<int> && !HasSettingInit<long> && !HasSettingInit<std::string>);
    EXPECT_EQ(wisp::core::SettingField<LangCode>::type, wisp::core::SettingType::Enum);
}

TEST(SettingsSchema, HostListLargestValueFitsItsField) {
    const SettingSpec& ntp = setting_spec(SettingId::NtpServers);
    // 2 entries of 63 characters and one comma: exactly 127.
    EXPECT_TRUE(wisp::core::schema_check::host_list_fits(ntp, 127));
    EXPECT_FALSE(wisp::core::schema_check::host_list_fits(ntp, 126));
    // Not a HostList: no size rule.
    EXPECT_TRUE(wisp::core::schema_check::host_list_fits(setting_spec(SettingId::Brightness), 0));
}
