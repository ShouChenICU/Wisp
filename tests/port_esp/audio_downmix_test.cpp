// Unit tests for the pure capture downmix helpers used by
// wisp::port::esp::EspAudioIn (see wisp/port/esp/audio_downmix.hpp's file
// comment). Compiles on the host without any ESP-IDF/esp_codec_dev
// include -- see tests/CMakeLists.txt's audio_downmix_test target, which
// adds components/wisp_port_esp/include to this translation unit's
// include path.

#include "wisp/port/esp/audio_downmix.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using wisp::port::esp::AudioChannel;
using wisp::port::esp::capture_chunk_frames;
using wisp::port::esp::downmix;

namespace {

constexpr AudioChannel kTwoMics[] = {AudioChannel::Mic, AudioChannel::Mic};

}  // namespace

TEST(AudioDownmix, TwoMicsKeepTheStereoRule) {
    {
        const int16_t in[] = {100, 100, -200, -200, 0, 0};
        int16_t mono[3] = {};
        downmix(in, mono, 3, kTwoMics);
        EXPECT_EQ(mono[0], 100);
        EXPECT_EQ(mono[1], -200);
        EXPECT_EQ(mono[2], 0);
    }
    {
        const int16_t in[] = {10, 20, -10, -30};
        int16_t mono[2] = {};
        downmix(in, mono, 2, kTwoMics);
        EXPECT_EQ(mono[0], 15);
        EXPECT_EQ(mono[1], -20);
    }
    {
        // The int16 sums (65534 / -65536) overflow int16_t, so the 32-bit
        // intermediate matters here.
        const int16_t in[] = {32767, 32767, -32768, -32768};
        int16_t mono[2] = {};
        downmix(in, mono, 2, kTwoMics);
        EXPECT_EQ(mono[0], 32767);
        EXPECT_EQ(mono[1], -32768);
    }
    {
        // Truncated toward zero, not rounded: (3 + 4) / 2 = 3, (-3 + -4) / 2 = -3.
        const int16_t in[] = {3, 4, -3, -4};
        int16_t mono[2] = {};
        downmix(in, mono, 2, kTwoMics);
        EXPECT_EQ(mono[0], 3);
        EXPECT_EQ(mono[1], -3);
    }
}

TEST(AudioDownmix, ZeroFramesIsNoOp) {
    const int16_t in[] = {1, 2};
    int16_t mono[1] = {42};
    downmix(in, mono, 0, kTwoMics);
    EXPECT_EQ(mono[0], 42);
}

TEST(AudioDownmix, RefChannelsAreSkipped) {
    const AudioChannel roles[] = {AudioChannel::Mic, AudioChannel::Ref};
    const int16_t in[] = {100, 9999, -50, 7};
    int16_t mono[2] = {};
    downmix(in, mono, 2, roles);
    EXPECT_EQ(mono[0], 100);
    EXPECT_EQ(mono[1], -50);
}

TEST(AudioDownmix, ThreeMicsTruncateTowardZero) {
    const AudioChannel roles[] = {AudioChannel::Mic, AudioChannel::Mic, AudioChannel::Mic};
    const int16_t in[] = {-1, -1, -2, 1, 1, 2};
    int16_t mono[2] = {};
    downmix(in, mono, 2, roles);
    EXPECT_EQ(mono[0], -1);
    EXPECT_EQ(mono[1], 1);
}

TEST(AudioDownmix, FourMicsAtTheExtremes) {
    const AudioChannel roles[] = {AudioChannel::Mic, AudioChannel::Mic, AudioChannel::Mic,
                                  AudioChannel::Mic};
    const int16_t in[] = {32767, 32767, 32767, 32767, -32768, -32768, -32768, -32768};
    int16_t mono[2] = {};
    downmix(in, mono, 2, roles);
    EXPECT_EQ(mono[0], 32767);
    EXPECT_EQ(mono[1], -32768);
}

TEST(AudioDownmix, OneMicIsCopied) {
    const AudioChannel roles[] = {AudioChannel::Mic};
    const int16_t in[] = {5, -6};
    int16_t mono[2] = {};
    downmix(in, mono, 2, roles);
    EXPECT_EQ(mono[0], 5);
    EXPECT_EQ(mono[1], -6);
}

TEST(AudioDownmix, ChannelNames) {
    EXPECT_STREQ(to_string(AudioChannel::Mic), "mic");
    EXPECT_STREQ(to_string(AudioChannel::Ref), "ref");
}

TEST(CaptureChunk, NeverExceedsTheScratch) {
    EXPECT_EQ(capture_chunk_frames(480, 960, 2), 480u);
    EXPECT_EQ(capture_chunk_frames(1000, 960, 3), 320u);
    EXPECT_EQ(capture_chunk_frames(7, 960, 4), 7u);
    static_assert(capture_chunk_frames(1000, 960, 2) == 480);
}

// Mirrors EspAudioIn::read()'s loop: every chunk fits the scratch buffer and
// the chunks add up to exactly the requested count, including a final chunk
// smaller than the rest.
TEST(CaptureChunk, LoopCoversTheCountWithinTheScratch) {
    for (size_t n = 1; n <= wisp::port::esp::kMaxCaptureChannels; ++n) {
        const size_t count = 48000 + 7;
        size_t done = 0;
        while (done < count) {
            size_t chunk = capture_chunk_frames(count - done, 960, n);
            ASSERT_GT(chunk, 0u);
            ASSERT_LE(chunk * n, 960u);
            done += chunk;
        }
        EXPECT_EQ(done, count);
    }
}
