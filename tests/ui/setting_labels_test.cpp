#include "wisp/ui/setting_labels.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "wisp/core/settings_schema.hpp"
#include "wisp/ui/strings.hpp"

namespace core = wisp::core;
using wisp::ui::Lang;

namespace {

// strings_test's EveryIdHasTextInEveryLanguage checks the text itself in
// zh/ja/en; this checks that every key a device surface shows has an id.
TEST(SettingLabels, EveryDeviceShownKeyHasALabel) {
    for (const core::SettingSpec& row : core::kSettingsSchema) {
        if ((row.flags & (core::kSettingAgent | core::kSettingDevicePage)) == 0) continue;
        EXPECT_TRUE(wisp::ui::setting_label(row.label).has_value()) << row.name;
    }
}

TEST(SettingLabels, UnknownAndWebOnlyLabelsHaveNoDeviceText) {
    EXPECT_FALSE(wisp::ui::setting_label("").has_value());
    EXPECT_FALSE(wisp::ui::setting_label("no_such_label").has_value());
    EXPECT_FALSE(wisp::ui::setting_label("time_servers").has_value());
    EXPECT_FALSE(wisp::ui::setting_label("agent_steps").has_value());
    EXPECT_FALSE(wisp::ui::setting_label("agent_minutes").has_value());
}

TEST(SettingLabels, EveryWebKeyHasAWebLabel) {
    std::ifstream in(WISP_WEB_I18N_DIR "/en.js", std::ios::binary);
    ASSERT_TRUE(in.good());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    for (const core::SettingSpec& row : core::kSettingsSchema) {
        if ((row.flags & core::kSettingWeb) == 0) continue;
        const std::string needle = "'sys." + std::string(row.label) + "':";
        EXPECT_NE(text.find(needle), std::string::npos) << row.name << ": " << needle;
    }
}

TEST(SettingLabels, LangCodeMatchesUiLang) {
    for (size_t i = 0; i < std::size(wisp::ui::kAllLangs); ++i) {
        EXPECT_EQ(std::string_view(wisp::ui::lang_code(wisp::ui::kAllLangs[i])),
                  core::lang_code(core::LangCode(i)));
    }
    EXPECT_EQ(wisp::ui::to_lang(core::LangCode::Ja), Lang::Ja);
    EXPECT_EQ(wisp::ui::to_lang_code(Lang::En), core::LangCode::En);
}

}  // namespace
