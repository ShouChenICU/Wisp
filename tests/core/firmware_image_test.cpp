#include "wisp/core/firmware_image.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "support/fake_firmware.hpp"

using wisp::ErrorCode;
using wisp::core::compare_firmware_versions;
using wisp::core::FirmwareTarget;
using wisp::core::kFirmwareHeadBytes;
using wisp::core::matches_target;
using wisp::core::parse_firmware_head;
using wisp::core::verify_firmware_image;
using wisp::test::ImageSpec;
using wisp::test::make_firmware_image;

namespace {

std::span<const uint8_t> head_of(const std::vector<uint8_t>& image) {
    return std::span<const uint8_t>(image).first(kFirmwareHeadBytes);
}

FirmwareTarget target() { return {"wisp", "test_board", wisp::test::kChipEsp32s3}; }

}  // namespace

TEST(FirmwareImage, ParsesVersionAndBoardFromHeader) {
    ImageSpec spec;
    spec.version = "v0.2.0-17-g1234567-dirty";
    spec.board = "waveshare_esp32_s3_touch_amoled_2_16";
    const auto image = make_firmware_image(spec);

    auto info = parse_firmware_head(head_of(image));
    ASSERT_TRUE(info) << info.error().message;
    EXPECT_EQ(info.value().chip_id, wisp::test::kChipEsp32s3);
    EXPECT_EQ(info.value().project, "wisp");
    EXPECT_EQ(info.value().version, "v0.2.0-17-g1234567-dirty");
    EXPECT_EQ(info.value().board, "waveshare_esp32_s3_touch_amoled_2_16");
}

TEST(FirmwareImage, LongestFieldsStopAtTheirSize) {
    ImageSpec spec;
    spec.version = std::string(40, 'v');  // field holds 31 + NUL
    spec.board = std::string(50, 'b');    // field holds 39 + NUL
    const auto image = make_firmware_image(spec);
    auto info = parse_firmware_head(head_of(image));
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().version, std::string(31, 'v'));
    EXPECT_EQ(info.value().board, std::string(39, 'b'));
}

TEST(FirmwareImage, RefusesFieldsThatAreNotPrintableAscii) {
    // Version, project and board reach logs, events and the screen: a
    // newline in one could forge a log line, so only printable ASCII is
    // accepted.
    struct Case {
        const char* what;
        ImageSpec spec;
    };
    std::vector<Case> cases;
    cases.push_back({"newline in version", {}});
    cases.back().spec.version = "1.0.0\nWISP_REBOOT";
    cases.push_back({"DEL in version", {}});
    cases.back().spec.version = "1.0.0\x7f";
    cases.push_back({"UTF-8 in version", {}});
    cases.back().spec.version = "1.0.0-\xc3\xa9";
    cases.push_back({"escape in board", {}});
    cases.back().spec.board = "test_board\x1b[2J";
    cases.push_back({"tab in project", {}});
    cases.back().spec.project = "wi\tsp";
    for (const Case& c : cases) {
        const auto image = make_firmware_image(c.spec);
        auto info = parse_firmware_head(head_of(image));
        ASSERT_FALSE(info) << c.what;
        EXPECT_EQ(info.error().code, ErrorCode::InvalidArgument) << c.what;
        EXPECT_EQ(info.error().message.find_first_of("\n\x1b\x7f"), std::string::npos) << c.what;
    }

    // Printable punctuation and spaces are fine.
    ImageSpec ok;
    ok.version = "1.0.0+build.7 (rc~1)";
    const auto image = make_firmware_image(ok);
    auto info = parse_firmware_head(head_of(image));
    ASSERT_TRUE(info) << info.error().message;
    EXPECT_EQ(info.value().version, "1.0.0+build.7 (rc~1)");
}

TEST(FirmwareImage, RejectsBadMagicWrongChipWrongProject) {
    // Not an image at all.
    auto image = make_firmware_image();
    image[0] = 0x7F;
    auto bad = parse_firmware_head(head_of(image));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);

    // Too short to identify.
    const auto good = make_firmware_image();
    EXPECT_FALSE(parse_firmware_head(std::span<const uint8_t>(good).first(100)));

    // No esp_app_desc_t (e.g. a bootloader or a data blob with 0xE9 first).
    image = make_firmware_image();
    image[wisp::core::kAppDescOffset] ^= 0xFF;
    EXPECT_FALSE(parse_firmware_head(head_of(image)));

    // Zero or too many segments.
    image = make_firmware_image();
    image[1] = 0;
    EXPECT_FALSE(parse_firmware_head(head_of(image)));
    image[1] = 17;
    EXPECT_FALSE(parse_firmware_head(head_of(image)));

    // Well-formed images for something else parse, but do not match.
    ASSERT_TRUE(parse_firmware_head(head_of(good)));
    EXPECT_TRUE(matches_target(parse_firmware_head(head_of(good)).value(), target()));

    ImageSpec chip;
    chip.chip_id = 0x0005;  // ESP32-C3
    auto info = parse_firmware_head(head_of(make_firmware_image(chip)));
    ASSERT_TRUE(info);
    EXPECT_FALSE(matches_target(info.value(), target()));
    // A target without a chip (the simulator) takes any.
    FirmwareTarget any_chip = target();
    any_chip.chip_id.reset();
    EXPECT_TRUE(matches_target(info.value(), any_chip));

    ImageSpec project;
    project.project = "hello_world";
    info = parse_firmware_head(head_of(make_firmware_image(project)));
    ASSERT_TRUE(info);
    EXPECT_FALSE(matches_target(info.value(), target()));

    ImageSpec board;
    board.board = "other_board";
    info = parse_firmware_head(head_of(make_firmware_image(board)));
    ASSERT_TRUE(info);
    EXPECT_FALSE(matches_target(info.value(), target()));

    // A Wisp image without the board descriptor cannot prove its board.
    ImageSpec no_desc;
    no_desc.board_desc = false;
    info = parse_firmware_head(head_of(make_firmware_image(no_desc)));
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().board, "");
    EXPECT_FALSE(matches_target(info.value(), target()));
}

TEST(FirmwareImage, VerifiesChecksumAndHash) {
    const auto image = make_firmware_image();
    EXPECT_TRUE(verify_firmware_image(image));

    // Trailing bytes (the rest of an OTA slot) are not part of the image.
    auto padded = image;
    padded.resize(padded.size() + 1000, 0xFF);
    EXPECT_TRUE(verify_firmware_image(padded));

    // A flipped data byte breaks the checksum and the hash.
    auto corrupt = image;
    corrupt[wisp::core::kFirmwareHeadBytes + 100] ^= 0x01;
    auto r = verify_firmware_image(corrupt);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);

    // A flipped hash byte breaks only the hash.
    auto bad_hash = image;
    bad_hash.back() ^= 0x01;
    EXPECT_FALSE(verify_firmware_image(bad_hash));

    // Truncated anywhere.
    for (size_t keep : {size_t{10}, kFirmwareHeadBytes, image.size() - 33, image.size() - 1}) {
        EXPECT_FALSE(verify_firmware_image(std::span<const uint8_t>(image).first(keep))) << keep;
    }

    // Without an appended hash only the checksum guards the data.
    ImageSpec no_hash;
    no_hash.hash_appended = false;
    auto plain = make_firmware_image(no_hash);
    EXPECT_TRUE(verify_firmware_image(plain));
    plain[wisp::core::kFirmwareHeadBytes + 100] ^= 0x10;
    EXPECT_FALSE(verify_firmware_image(plain));
}

TEST(FirmwareImage, RejectsUnalignedSegments) {
    auto image = make_firmware_image();
    // The first segment's length field (offset 24 + 4).
    image[28] += 1;
    EXPECT_FALSE(verify_firmware_image(image));
}

TEST(FirmwareImage, ComparesVersions) {
    EXPECT_EQ(compare_firmware_versions("0.1.0", "0.1.0"), 0);
    EXPECT_LT(*compare_firmware_versions("0.1.0", "0.2.0"), 0);
    EXPECT_GT(*compare_firmware_versions("1.0.0", "0.9.9"), 0);
    EXPECT_LT(*compare_firmware_versions("0.1.0-5-gabcdef0", "0.1.0-12-g1234567"), 0);
    EXPECT_GT(*compare_firmware_versions("0.1.0-5-gabcdef0", "0.1.0"), 0);
    EXPECT_EQ(compare_firmware_versions("v0.1.0-5-gabcdef0-dirty", "0.1.0-5-g9999999"), 0);
    EXPECT_LT(*compare_firmware_versions("0.9.0-200-gaaaaaaa", "0.10.0"), 0);
    // Not comparable: a bare hash, garbage, a missing part.
    EXPECT_FALSE(compare_firmware_versions("14b8302-dirty", "0.1.0"));
    EXPECT_FALSE(compare_firmware_versions("0.1", "0.1.0"));
    EXPECT_FALSE(compare_firmware_versions("0.1.0", ""));
    EXPECT_FALSE(compare_firmware_versions("0.1.0-x-gabc", "0.1.0"));
}
