#include "wisp/base/crc32.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace {

std::span<const uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

}  // namespace

TEST(Crc32, KnownVector) {
    EXPECT_EQ(wisp::crc32(bytes("123456789")), 0xCBF43926u);
    EXPECT_EQ(wisp::crc32(bytes("")), 0u);
    EXPECT_EQ(wisp::crc32(bytes("The quick brown fox jumps over the lazy dog")), 0x414FA339u);
}

TEST(Crc32, ChainsAcrossPieces) {
    EXPECT_EQ(wisp::crc32(bytes("56789"), wisp::crc32(bytes("1234"))), 0xCBF43926u);
}
