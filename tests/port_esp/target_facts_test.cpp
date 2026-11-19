#include "wisp/port/esp/target_facts.hpp"

#include <gtest/gtest.h>

#include <string_view>

using namespace wisp::port::esp;

TEST(TargetFacts, Esp32s3) {
    const TargetFacts* f = find_target_facts("esp32s3");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(std::string_view(f->model), "ESP32-S3");
    EXPECT_EQ(f->dfs_min_mhz, 80u);
}

TEST(TargetFacts, UnknownTargetIsNull) {
    EXPECT_EQ(find_target_facts("esp32xx"), nullptr);
    EXPECT_EQ(find_target_facts(""), nullptr);
}

TEST(TargetFacts, LookupIsConstexpr) {
    static_assert(find_target_facts("esp32s3")->dfs_min_mhz == 80);
    static_assert(find_target_facts("nope") == nullptr);
}
