#include "wisp/app/format.hpp"

#include <gtest/gtest.h>

#include "wisp/core/boot_report.hpp"

TEST(Format, Bytes) {
    EXPECT_EQ(wisp::app::format_bytes(0), "0 B");
    EXPECT_EQ(wisp::app::format_bytes(1536), "1.5 KB");
    EXPECT_EQ(wisp::app::format_bytes(6160384), "5.9 MB");
    EXPECT_EQ(wisp::app::format_bytes(34359738368ull), "32.0 GB");
}

TEST(Format, TimeAndState) {
    EXPECT_EQ(wisp::app::format_time(0), "1970-01-01 00:00:00");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Failed), "失败");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Ok), "正常");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Degraded), "降级");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Missing), "缺失");
}
