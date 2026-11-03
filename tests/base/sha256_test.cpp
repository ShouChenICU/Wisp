#include "wisp/base/sha256.hpp"

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

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(wisp::to_hex(wisp::sha256(bytes(""))),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(wisp::to_hex(wisp::sha256(bytes("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the length field no longer fits, so padding spills into a second block.
    EXPECT_EQ(wisp::to_hex(
                  wisp::sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MultiBlockInputs) {
    // FIPS 180-4 example: 112-byte message (one full 64-byte block, then 48
    // bytes plus the padding in a second block).
    EXPECT_EQ(wisp::to_hex(wisp::sha256(
                  bytes("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjk"
                        "lmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    // One million 'a' characters.
    std::string million(1000000, 'a');
    EXPECT_EQ(wisp::to_hex(wisp::sha256(bytes(million))),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, ToHexIsLowercase) {
    const uint8_t data[] = {0x00, 0x0f, 0xab, 0xff};
    EXPECT_EQ(wisp::to_hex(data), "000fabff");
    EXPECT_EQ(wisp::to_hex({}), "");
}
