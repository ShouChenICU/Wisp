// The text of system notifications and wake lock holders
// (wisp/ui/system_notice.hpp).

#include "wisp/ui/system_notice.hpp"

#include <gtest/gtest.h>

#include <string>

#include "wisp/core/notification_service.hpp"
#include "wisp/ui/strings.hpp"

namespace {

using wisp::ui::Lang;
using wisp::ui::StrId;

class SystemNotice : public ::testing::Test {
protected:
    void TearDown() override { wisp::ui::set_lang(Lang::Zh); }
};

TEST_F(SystemNotice, EveryCodeInEveryLanguage) {
    namespace notice = wisp::core::notice;
    const char* codes[] = {notice::kSceneEvictedMemory, notice::kSceneEvictedCount,
                           notice::kSceneError,         notice::kRestoreFailed,
                           notice::kWakeLockLong,       notice::kBatteryLow,
                           notice::kSceneDeleteFailed,  notice::kAgentDone};
    for (Lang lang : wisp::ui::kAllLangs) {
        wisp::ui::set_lang(lang);
        for (const char* code : codes) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang)) + " " + code);
            const wisp::ui::NoticeText t = wisp::ui::system_notice_text(code, "Pomo42");
            EXPECT_FALSE(t.title.empty());
            EXPECT_EQ(t.title, wisp::ui::tr(StrId::NoticeSystemTitle, lang));
            EXPECT_NE(t.body.find("Pomo42"), std::string::npos) << t.body;
            EXPECT_EQ(t.body.find("{}"), std::string::npos) << t.body;
        }
    }
    wisp::ui::set_lang(Lang::En);
    EXPECT_EQ(wisp::ui::system_notice_text(notice::kBatteryLow, "15").body, "Battery low: 15%");
    EXPECT_EQ(wisp::ui::system_notice_text(notice::kAgentDone, "All set").body, "Done: All set");
    // No argument.
    EXPECT_EQ(wisp::ui::system_notice_text(notice::kSettingsNotSaved, "").body,
              "Settings could not be saved");
    wisp::ui::set_lang(Lang::Zh);
    EXPECT_EQ(wisp::ui::system_notice_text(notice::kSettingsNotSaved, "").body, "设置未能保存");
    wisp::ui::set_lang(Lang::En);

    // An unknown code is shown as it is.
    const wisp::ui::NoticeText unknown = wisp::ui::system_notice_text("future_code", "x");
    EXPECT_EQ(unknown.title, wisp::ui::tr(StrId::NoticeSystemTitle));
    EXPECT_EQ(unknown.body, "future_code");
}

TEST_F(SystemNotice, HolderLabels) {
    EXPECT_EQ(wisp::ui::holder_label("system.upload", ""), "上传");
    EXPECT_EQ(wisp::ui::holder_label("system.ota", ""), "固件更新");
    EXPECT_EQ(wisp::ui::holder_label("system.ntp", ""), "校时");
    EXPECT_EQ(wisp::ui::holder_label("system.netwindow", ""), "后台联网");
    EXPECT_EQ(wisp::ui::holder_label("system.hotspot", ""), "配网热点");
    EXPECT_EQ(wisp::ui::holder_label("system.tone", ""), "提示音");
    EXPECT_EQ(wisp::ui::holder_label("system.download", ""), "文件下载");
    wisp::ui::set_lang(Lang::Ja);
    EXPECT_EQ(wisp::ui::holder_label("system.download", ""), "ファイルのダウンロード");
    wisp::ui::set_lang(Lang::En);
    EXPECT_EQ(wisp::ui::holder_label("system.download", ""), "File download");

    // A scene's lock shows the scene's name, or its id without one.
    EXPECT_EQ(wisp::ui::holder_label("scene.x", "番茄钟"), "番茄钟");
    EXPECT_EQ(wisp::ui::holder_label("scene.x", ""), "x");
    // A holder this version does not know reads as a generic phrase,
    // never as its raw id.
    EXPECT_EQ(wisp::ui::holder_label("system.later", ""), "System task");
    EXPECT_EQ(wisp::ui::holder_label("other", ""), "System task");
}

// Every system holder the code takes a wake lock as has its own label in
// every language.
TEST_F(SystemNotice, EverySystemHolderHasALabel) {
    const char* holders[] = {"system.upload",    "system.ota",          "system.ntp",
                             "system.netwindow", "system.hotspot",      "system.tone",
                             "system.download",  "system.powered_wifi", "system.agent",
                             "system.voice"};
    for (Lang lang : wisp::ui::kAllLangs) {
        wisp::ui::set_lang(lang);
        const std::string generic = wisp::ui::holder_label("system.unknown", "");
        for (const char* h : holders) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang)) + " " + h);
            const std::string label = wisp::ui::holder_label(h, "");
            EXPECT_FALSE(label.empty());
            EXPECT_EQ(label.find("system."), std::string::npos) << label;
            EXPECT_NE(label, generic);
        }
        EXPECT_EQ(generic.find("system."), std::string::npos) << generic;
    }
    wisp::ui::set_lang(Lang::Zh);
    EXPECT_EQ(wisp::ui::holder_label("system.powered_wifi", ""), "外接电源时的 Wi-Fi");
    EXPECT_EQ(wisp::ui::system_notice_text(wisp::core::notice::kWakeLockLong,
                                           wisp::ui::holder_label("system.agent", ""))
                  .body,
              "AI 对话已让设备保持唤醒 30 分钟");
}

}  // namespace
