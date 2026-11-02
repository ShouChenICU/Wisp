// Unit tests for the pure stereo->mono downmix helper used by
// wisp::port::esp::EspAudioIn (see wisp/port/esp/audio_downmix.hpp's file
// comment). Compiles on the host without any ESP-IDF/esp_codec_dev
// include -- see tests/CMakeLists.txt's audio_downmix_test target, which
// adds components/wisp_port_esp/include to this translation unit's
// include path.

#include "wisp/port/esp/audio_downmix.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using wisp::port::esp::downmix_stereo_to_mono;

TEST(AudioDownmix, AveragesEqualChannels) {
    const int16_t stereo[] = {100, 100, -200, -200, 0, 0};
    int16_t mono[3] = {};
    downmix_stereo_to_mono(stereo, mono, 3);
    EXPECT_EQ(mono[0], 100);
    EXPECT_EQ(mono[1], -200);
    EXPECT_EQ(mono[2], 0);
}

TEST(AudioDownmix, AveragesDifferentChannels) {
    const int16_t stereo[] = {10, 20, -10, -30};
    int16_t mono[2] = {};
    downmix_stereo_to_mono(stereo, mono, 2);
    EXPECT_EQ(mono[0], 15);
    EXPECT_EQ(mono[1], -20);
}

TEST(AudioDownmix, NoOverflowAtExtremes) {
    // L=R=INT16_MAX and L=R=INT16_MIN: the L+R sum (65534 / -65536)
    // overflows int16_t, so the 32-bit intermediate in
    // downmix_stereo_to_mono() matters here.
    const int16_t stereo[] = {32767, 32767, -32768, -32768};
    int16_t mono[2] = {};
    downmix_stereo_to_mono(stereo, mono, 2);
    EXPECT_EQ(mono[0], 32767);
    EXPECT_EQ(mono[1], -32768);
}

TEST(AudioDownmix, TruncatesTowardZeroRatherThanRounding) {
    // Plain C++ integer division truncates toward zero, so an odd sum
    // is *not* rounded to the nearest integer.
    //   (3 + 4) / 2 = 3 (truncated), not 4 (rounded to nearest).
    //   (-3 + -4) / 2 = -7 / 2 = -3 (truncated toward zero), not -4.
    const int16_t stereo[] = {3, 4, -3, -4};
    int16_t mono[2] = {};
    downmix_stereo_to_mono(stereo, mono, 2);
    EXPECT_EQ(mono[0], 3);
    EXPECT_EQ(mono[1], -3);
}

TEST(AudioDownmix, ZeroFramesIsNoOp) {
    const int16_t stereo[] = {1, 2};
    int16_t mono[1] = {42};
    downmix_stereo_to_mono(stereo, mono, 0);
    EXPECT_EQ(mono[0], 42);
}
