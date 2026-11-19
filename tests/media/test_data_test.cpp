#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>

#include "support/media_data.hpp"

namespace {

constexpr const char* kFiles[] = {
    "sine_44k_stereo_cbr128.mp3",
    "sine_48k_mono_vbr.mp3",
    "sine_22k_noxing.mp3",
    "sine_44k_stereo.wav",
    "sine_16k_mono.wav",
    "float32.wav",
    "garbage.mp3",
};

}  // namespace

TEST(MediaTestData, FilesExistAndAreSmall) {
    std::uintmax_t total = 0;
    for (const char* name : kFiles) {
        std::error_code ec;
        const auto path = wisp::test::media_data(name);
        ASSERT_TRUE(std::filesystem::is_regular_file(path, ec)) << path;
        const auto size = std::filesystem::file_size(path, ec);
        ASSERT_GT(size, 0u) << path;
        total += size;
    }
    EXPECT_LT(total, 200u * 1024u);
    EXPECT_EQ(std::filesystem::file_size(wisp::test::media_data("garbage.mp3")), 4096u);
}
