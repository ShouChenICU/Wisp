#include "wisp/base/base64.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace {

std::span<const uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

}  // namespace

TEST(Base64, UrlSafeNoPadding) {
    EXPECT_EQ(wisp::base64url_encode(bytes("\xfb\xff")), "-_8");
    EXPECT_EQ(wisp::base64_encode(bytes("\xfb\xff")), "+/8=");
    EXPECT_EQ(wisp::base64_encode(bytes("")), "");
    EXPECT_EQ(wisp::base64url_encode(bytes("")), "");
}

TEST(Base64, Rfc4648Vectors) {
    EXPECT_EQ(wisp::base64_encode(bytes("f")), "Zg==");
    EXPECT_EQ(wisp::base64_encode(bytes("fo")), "Zm8=");
    EXPECT_EQ(wisp::base64_encode(bytes("foo")), "Zm9v");
    EXPECT_EQ(wisp::base64_encode(bytes("foob")), "Zm9vYg==");
    EXPECT_EQ(wisp::base64_encode(bytes("fooba")), "Zm9vYmE=");
    EXPECT_EQ(wisp::base64_encode(bytes("foobar")), "Zm9vYmFy");
    EXPECT_EQ(wisp::base64url_encode(bytes("f")), "Zg");
    EXPECT_EQ(wisp::base64url_encode(bytes("fo")), "Zm8");
    EXPECT_EQ(wisp::base64url_encode(bytes("foobar")), "Zm9vYmFy");
}
