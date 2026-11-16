#include "wisp/base/base64.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
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

TEST(Base64, EncodeIntoMatchesEncode) {
    const uint8_t data[] = {0, 1, 2, 0xfb, 0xff, 0x10, 0x80};
    for (size_t n = 0; n <= sizeof(data); ++n) {
        const std::span<const uint8_t> in(data, n);
        const size_t len = wisp::base64_encoded_size(n);
        std::string buf(len + 1, '#');
        wisp::base64_encode_into(in, buf.data());
        EXPECT_EQ(buf.back(), '#') << "n=" << n;
        EXPECT_EQ(std::string_view(buf).substr(0, len), wisp::base64_encode(in)) << "n=" << n;
    }
}
