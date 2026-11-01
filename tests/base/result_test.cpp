#include "wisp/base/result.hpp"

#include <gtest/gtest.h>

TEST(Result, OkHoldsValue) {
    auto r = wisp::Result<int>::ok(42);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 42);
}
TEST(Result, ErrCarriesCodeAndMessage) {
    auto r = wisp::Result<int>::err(wisp::ErrorCode::QuotaExceeded, "full");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::QuotaExceeded);
    EXPECT_EQ(r.error().message, "full");
    EXPECT_EQ(r.value_or(7), 7);
}
TEST(Result, VoidSpecialization) {
    EXPECT_TRUE(wisp::Result<void>::ok());
    EXPECT_FALSE(wisp::Result<void>::err(wisp::ErrorCode::Timeout));
}
TEST(Result, TryPropagates) {
    auto inner = [] { return wisp::Result<int>::err(wisp::ErrorCode::IoError, "disk"); };
    auto outer = [&]() -> wisp::Result<void> {
        WISP_TRY(inner());
        return wisp::Result<void>::ok();
    };
    auto r = outer();
    EXPECT_EQ(r.error().code, wisp::ErrorCode::IoError);
    EXPECT_EQ(r.error().message, "disk");
}
TEST(ErrorCode, SnakeCaseNames) {
    EXPECT_STREQ(wisp::to_string(wisp::ErrorCode::StorageUnavailable), "storage_unavailable");
    EXPECT_STREQ(wisp::to_string(wisp::ErrorCode::InvalidArgument), "invalid_argument");
}
