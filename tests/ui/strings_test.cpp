#include "wisp/ui/strings.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "lvgl.h"
#include "wisp/ui/ui_runtime.hpp"

using wisp::ui::Lang;
using wisp::ui::StrId;

namespace {

StrId id_at(size_t i) { return static_cast<StrId>(i); }

// Decodes one UTF-8 sequence starting at *p (the tables are plain UTF-8
// source literals) and advances *p past it.
uint32_t next_codepoint(const unsigned char*& p) {
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra == 0 ? 0x7F : (0x3F >> extra);
    for (int k = 0; k < extra && *p != 0; ++k, ++p) {
        c = (c << 6) | (*p & 0x3F);
    }
    return c;
}

// Restores the process-wide language after each test.
class Strings : public ::testing::Test {
protected:
    void TearDown() override { wisp::ui::set_lang(Lang::Zh); }
};

}  // namespace

TEST_F(Strings, EveryIdHasTextInEveryLanguage) {
    for (size_t i = 0; i < wisp::ui::kStrCount; ++i) {
        for (Lang lang : wisp::ui::kAllLangs) {
            const char* text = wisp::ui::detail::table_text(id_at(i), lang);
            ASSERT_NE(text, nullptr) << "id " << i << " lang " << wisp::ui::lang_code(lang);
            EXPECT_GT(std::strlen(text), 0u) << "id " << i << " lang " << wisp::ui::lang_code(lang);
        }
    }
}

// Every codepoint of every string must render with the UI font (Montserrat
// for ASCII, falling back to the built-in CJK font): a missing glyph would
// show as an empty box on the device.
TEST_F(Strings, EveryGlyphIsInTheBuiltInFont) {
    const lv_font_t* font = wisp::ui::default_font();
    for (size_t i = 0; i < wisp::ui::kStrCount; ++i) {
        for (Lang lang : wisp::ui::kAllLangs) {
            const char* text = wisp::ui::tr(id_at(i), lang);
            const auto* p = reinterpret_cast<const unsigned char*>(text);
            while (*p != 0) {
                uint32_t letter = next_codepoint(p);
                lv_font_glyph_dsc_t dsc;
                EXPECT_TRUE(lv_font_get_glyph_dsc(font, &dsc, letter, 0))
                    << "U+" << std::hex << letter << " in id " << std::dec << i << " ("
                    << wisp::ui::lang_code(lang) << "): " << text;
            }
        }
    }
}

// The Japanese glyphs the new font adds: kana, the long-vowel mark and
// level-1 kanji that are not in GB2312.
TEST_F(Strings, FontCoversJapanese) {
    const lv_font_t* font = wisp::ui::default_font();
    for (uint32_t letter : {0x3042u /* あ */, 0x30F3u /* ン */, 0x30FCu /* ー */, 0x691Cu /* 検 */,
                            0x8AADu /* 読 */, 0x96FBu /* 電 */}) {
        lv_font_glyph_dsc_t dsc;
        EXPECT_TRUE(lv_font_get_glyph_dsc(font, &dsc, letter, 0)) << "U+" << std::hex << letter;
    }
    // A missing glyph really does fail the lookup (no placeholder), so the
    // checks above and in EveryGlyphIsInTheBuiltInFont mean something.
    lv_font_glyph_dsc_t dsc;
    EXPECT_FALSE(lv_font_get_glyph_dsc(font, &dsc, 0x0E01 /* Thai ko kai */, 0));
}

TEST_F(Strings, ParseLang) {
    EXPECT_EQ(wisp::ui::parse_lang("zh"), Lang::Zh);
    EXPECT_EQ(wisp::ui::parse_lang("ja"), Lang::Ja);
    EXPECT_EQ(wisp::ui::parse_lang("en"), Lang::En);
    EXPECT_FALSE(wisp::ui::parse_lang("fr"));
    EXPECT_FALSE(wisp::ui::parse_lang(""));
    EXPECT_FALSE(wisp::ui::parse_lang("ZH"));
    EXPECT_FALSE(wisp::ui::parse_lang("zh-CN"));
    for (Lang lang : wisp::ui::kAllLangs) {
        EXPECT_EQ(wisp::ui::parse_lang(wisp::ui::lang_code(lang)), lang);
    }
}

TEST_F(Strings, TrFollowsTheCurrentLanguage) {
    EXPECT_EQ(wisp::ui::lang(), Lang::Zh);
    EXPECT_STREQ(wisp::ui::tr(StrId::Yes), "是");
    wisp::ui::set_lang(Lang::Ja);
    EXPECT_EQ(wisp::ui::lang(), Lang::Ja);
    EXPECT_STREQ(wisp::ui::tr(StrId::Yes), "はい");
    wisp::ui::set_lang(Lang::En);
    EXPECT_STREQ(wisp::ui::tr(StrId::Yes), "Yes");
    EXPECT_STREQ(wisp::ui::tr(StrId::Yes, Lang::Zh), "是");
}

TEST_F(Strings, OutOfRangeIdFallsBackToEmptyText) {
    EXPECT_STREQ(wisp::ui::tr(StrId::Count), "");
    EXPECT_EQ(wisp::ui::detail::table_text(StrId::Count, Lang::Zh), nullptr);
}

namespace {

size_t count_placeholders(std::string_view text) {
    size_t n = 0;
    for (size_t pos = text.find("{}"); pos != std::string_view::npos;
         pos = text.find("{}", pos + 2)) {
        ++n;
    }
    return n;
}

}  // namespace

// A translation may move the "{}" but never drop or add one, or a number
// would vanish (or a literal "{}" show) in that language. A text with
// several is filled by tr_fill_n(), in order, so a translation keeps that
// order too (it can only be checked by reading).
TEST_F(Strings, PlaceholdersMatchAcrossLanguages) {
    for (size_t i = 0; i < wisp::ui::kStrCount; ++i) {
        const size_t zh = count_placeholders(wisp::ui::tr(id_at(i), Lang::Zh));
        EXPECT_LE(zh, 3u) << "id " << i;
        for (Lang lang : wisp::ui::kAllLangs) {
            EXPECT_EQ(count_placeholders(wisp::ui::tr(id_at(i), lang)), zh)
                << "id " << i << " lang " << wisp::ui::lang_code(lang);
        }
    }
}

TEST_F(Strings, TrFillReplacesThePlaceholder) {
    EXPECT_EQ(wisp::ui::tr_fill(StrId::PairRemaining, "42"), "剩余 42 秒");
    wisp::ui::set_lang(Lang::En);
    EXPECT_EQ(wisp::ui::tr_fill(StrId::PairRemaining, "42"), "42 s left");
    // Text without a placeholder comes back unchanged.
    EXPECT_EQ(wisp::ui::tr_fill(StrId::Yes, "42"), "Yes");
}

// A screen with a language of its own (the self-check page) fills its text
// in that language, whatever ui::lang() is.
TEST_F(Strings, TrFillTakesAnExplicitLanguage) {
    wisp::ui::set_lang(Lang::En);
    EXPECT_EQ(wisp::ui::tr_fill(StrId::PairRemaining, Lang::Ja, "42"), "残り 42 秒");
    EXPECT_EQ(wisp::ui::tr_fill(StrId::PairRemaining, Lang::Zh, "42"), "剩余 42 秒");
    EXPECT_EQ(wisp::ui::tr_fill(StrId::Yes, Lang::Zh, "42"), "是");
}

TEST_F(Strings, SelfCheckTitleAndBoardCard) {
    EXPECT_STREQ(wisp::ui::tr(StrId::SelfCheckTitle, Lang::En), "Self Check");
    EXPECT_STREQ(wisp::ui::tr(StrId::SelfCheckTitle, Lang::Zh), "自检");
    EXPECT_STREQ(wisp::ui::tr(StrId::SelfCheckTitle, Lang::Ja), "セルフチェック");
    EXPECT_STREQ(wisp::ui::tr(StrId::SectionBoard, Lang::En), "Board");
    EXPECT_STREQ(wisp::ui::tr(StrId::SectionBoard, Lang::Zh), "设备信息");
    EXPECT_STREQ(wisp::ui::tr(StrId::SectionBoard, Lang::Ja), "ボード情報");
}

// The language switcher names each language in that language, the same in
// every table.
TEST_F(Strings, LanguageNamesAreNotTranslated) {
    const std::pair<StrId, const char*> names[] = {
        {StrId::LangNameEn, "EN"}, {StrId::LangNameZh, "中文"}, {StrId::LangNameJa, "日本語"}};
    for (const auto& [id, text] : names) {
        for (Lang lang : wisp::ui::kAllLangs) {
            EXPECT_STREQ(wisp::ui::tr(id, lang), text) << wisp::ui::lang_code(lang);
        }
    }
}

// The system notification, wake lock holder and pull-down panel text
// (scene-device spec 5.2, 5.6).
TEST_F(Strings, NoticeAndPanelText) {
    EXPECT_STREQ(wisp::ui::tr(StrId::NoticeSystemTitle, Lang::Zh), "系统");
    EXPECT_EQ(wisp::ui::tr_fill(StrId::NoticeBatteryLow, Lang::Zh, "15"), "电量低：15%");
    EXPECT_EQ(wisp::ui::tr_fill(StrId::NoticeSceneError, Lang::Ja, "A"),
              "Aでエラーが発生し、停止しました");
    EXPECT_EQ(wisp::ui::tr_fill(StrId::NoticeEvictedMemory, Lang::En, "A"),
              "A was closed to free memory");
    EXPECT_STREQ(wisp::ui::tr(StrId::NoticeRelease, Lang::En), "Release");
    EXPECT_STREQ(wisp::ui::tr(StrId::HolderUpload, Lang::Zh), "上传");
    EXPECT_STREQ(wisp::ui::tr(StrId::PanelClearAll, Lang::Ja), "すべて消去");
    EXPECT_EQ(wisp::ui::tr_fill(StrId::PanelMinutes, Lang::Zh, "5"), "5 分钟");
    EXPECT_EQ(wisp::ui::tr_fill(StrId::PanelHours, Lang::En, "2"), "2 h");
}

// The agent's notes, confirmations and setting names (agent-core spec
// 3.4, 6.1, 6.2): every one has text in every language, with the same
// number of placeholders.
TEST_F(Strings, AgentStringsInEveryLanguage) {
    const auto first = static_cast<size_t>(StrId::AgentStepLimit);
    for (size_t i = first; i < wisp::ui::kStrCount; ++i) {
        const char* zh = wisp::ui::detail::table_text(id_at(i), Lang::Zh);
        ASSERT_NE(zh, nullptr) << "id " << i;
        for (Lang lang : wisp::ui::kAllLangs) {
            const char* text = wisp::ui::detail::table_text(id_at(i), lang);
            ASSERT_NE(text, nullptr) << "id " << i << " lang " << wisp::ui::lang_code(lang);
            EXPECT_GT(std::strlen(text), 0u) << "id " << i << " lang " << wisp::ui::lang_code(lang);
            EXPECT_EQ(count_placeholders(text), count_placeholders(zh))
                << "id " << i << " lang " << wisp::ui::lang_code(lang);
        }
    }
    EXPECT_STREQ(wisp::ui::tr(StrId::AgentConfirmClear, Lang::Zh), "清空全部 Agent 对话？");
    EXPECT_STREQ(wisp::ui::tr(StrId::AgentAllow, Lang::Zh), "允许");
    EXPECT_STREQ(wisp::ui::tr(StrId::AgentDeny, Lang::Zh), "拒绝");
}

// tr_fill_n() fills the placeholders in order, in the current language or
// an explicit one; extra values are ignored, missing ones leave "{}", and
// a value is never searched for placeholders itself.
TEST_F(Strings, FillNInOrder) {
    const std::string_view three[] = {"亮度", "200", "120"};
    EXPECT_EQ(wisp::ui::tr_fill_n(StrId::AgentNoteSetting, three), "亮度 200 → 120");
    wisp::ui::set_lang(Lang::En);
    const std::string_view two[] = {"Clock", "a.png"};
    EXPECT_EQ(wisp::ui::tr_fill_n(StrId::AgentNoteFileRemoved, two),
              "Deleted a file of \"Clock\": a.png");
    EXPECT_EQ(wisp::ui::tr_fill_n(StrId::AgentNoteFileRemoved, Lang::Zh, two),
              "删除了「Clock」的文件 a.png");
    const std::string_view braces[] = {"{}", "x"};
    EXPECT_EQ(wisp::ui::tr_fill_n(StrId::AgentNoteSetting, Lang::Zh, braces), "{} x → {}");
    const std::string_view four[] = {"a", "b", "c", "d"};
    EXPECT_EQ(wisp::ui::tr_fill_n(StrId::AgentNoteSetting, Lang::Zh, four), "a b → c");
    EXPECT_EQ(wisp::ui::tr_fill_n(StrId::Yes, Lang::En, four), "Yes");
}
