#include "wisp/port/sim/sim_firmware.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "support/fake_firmware.hpp"
#include "support/temp_dir.hpp"

using wisp::port::sim::SimFirmwareUpdater;
using wisp::test::ImageSpec;
using wisp::test::make_firmware_image;

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

}  // namespace

TEST(SimFirmware, WritesVerifiesAndIdentifiesTheImage) {
    const std::string dir = wisp::test::temp_dir().string();
    SimFirmwareUpdater up(dir);
    EXPECT_EQ(up.target().project, "wisp");
    EXPECT_EQ(up.target().board, "simulator");
    EXPECT_FALSE(up.target().chip_id.has_value());
    EXPECT_EQ(up.running_slot(), "sim");
    EXPECT_GE(up.capacity(), 1024u * 1024u);

    ImageSpec spec;
    spec.board = "simulator";
    spec.version = "0.3.0";
    const auto image = make_firmware_image(spec);
    ASSERT_TRUE(up.begin(image.size()));
    std::span<const uint8_t> rest(image);
    while (!rest.empty()) {
        const size_t n = std::min<size_t>(rest.size(), 1000);
        ASSERT_TRUE(up.write(rest.first(n)));
        rest = rest.subspan(n);
    }
    auto info = up.finish();
    ASSERT_TRUE(info) << info.error().message;
    EXPECT_EQ(info.value().version, "0.3.0");
    EXPECT_EQ(info.value().board, "simulator");
    EXPECT_EQ(read_file(up.image_path()), image);
    EXPECT_EQ(up.image_path(), dir + "/firmware.bin");

    ASSERT_TRUE(up.activate());
    EXPECT_TRUE(exists(up.image_path()));
    up.mark_valid();
}

TEST(SimFirmware, CorruptImageFailsAndAbortDropsIt) {
    const std::string dir = wisp::test::temp_dir().string();
    SimFirmwareUpdater up(dir);
    auto image = make_firmware_image();
    image[image.size() / 2] ^= 0x01;
    ASSERT_TRUE(up.begin(image.size()));
    ASSERT_TRUE(up.write(image));
    EXPECT_FALSE(up.finish());
    up.abort();
    EXPECT_FALSE(exists(up.image_path()));
    up.abort();  // safe twice

    // Fewer bytes than begin() announced.
    const auto good = make_firmware_image();
    ASSERT_TRUE(up.begin(good.size() + 10));
    ASSERT_TRUE(up.write(good));
    EXPECT_FALSE(up.finish());
    up.abort();
}

TEST(SimFirmware, UnwritableDirectoryFailsBegin) {
    SimFirmwareUpdater up("/nonexistent/wisp-sim-firmware");
    EXPECT_FALSE(up.begin(4096));
    up.abort();
}
