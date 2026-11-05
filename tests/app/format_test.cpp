#include "wisp/app/format.hpp"

#include <gtest/gtest.h>

#include <string>

#include "wisp/core/boot_report.hpp"
#include "wisp/ui/strings.hpp"

TEST(Format, Bytes) {
    EXPECT_EQ(wisp::app::format_bytes(0), "0 B");
    EXPECT_EQ(wisp::app::format_bytes(1536), "1.5 KB");
    EXPECT_EQ(wisp::app::format_bytes(6160384), "5.9 MB");
    EXPECT_EQ(wisp::app::format_bytes(34359738368ull), "32.0 GB");
}

TEST(Format, TimeAndState) {
    EXPECT_EQ(wisp::app::format_time(0), "1970-01-01 00:00:00");
    using wisp::ui::Lang;
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Failed, Lang::Zh), "失败");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Ok, Lang::Zh), "正常");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Degraded, Lang::Zh), "降级");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Missing, Lang::Zh), "缺失");
    // In the language asked for, whatever ui::lang() is.
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Failed, Lang::En), "Failed");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Missing, Lang::Ja), "未検出");
}

// SSIDs are raw bytes; GBK names are common. The device font can only draw
// UTF-8, so every byte of an ill-formed sequence (and every control byte)
// shows as "?" instead of a garbage glyph.
TEST(Format, DisplayableSsid) {
    using wisp::app::displayable_ssid;
    EXPECT_EQ(displayable_ssid("Home"), "Home");
    EXPECT_EQ(displayable_ssid("\xe5\x92\x96\xe5\x95\xa1 5G"), "\xe5\x92\x96\xe5\x95\xa1 5G");
    EXPECT_EQ(displayable_ssid("\xc4\xe3\xba\xc3"), "????");  // "你好" in GBK
    EXPECT_EQ(displayable_ssid(std::string("a\xff") + "b"), "a?b");
    EXPECT_EQ(displayable_ssid("x\xe4\xbd"), "x??");     // cut-off sequence
    EXPECT_EQ(displayable_ssid("\xed\xa0\x80"), "???");  // a UTF-16 surrogate
    EXPECT_EQ(displayable_ssid(std::string("a\x01\x7f", 3)), "a??");
    EXPECT_EQ(displayable_ssid(""), "");
}
