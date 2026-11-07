// voice::Vad (wisp/voice/vad.hpp).

#include "wisp/voice/vad.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "wisp/voice/limits.hpp"

using wisp::voice::kFrameMs;
using wisp::voice::kFrameSamples;
using wisp::voice::Vad;
using wisp::voice::VadMode;
using wisp::voice::VadVerdict;

namespace {

// Deterministic uniform noise with the given RMS.
class Noise {
public:
    std::vector<int16_t> frame(double rms) {
        std::vector<int16_t> f(kFrameSamples);
        const double amp = rms * std::sqrt(3.0);
        for (auto& s : f) {
            state_ = state_ * 1664525u + 1013904223u;
            const double u = static_cast<double>(state_ >> 8) / static_cast<double>(1u << 24);
            s = static_cast<int16_t>(std::lround((u * 2.0 - 1.0) * amp));
        }
        return f;
    }

private:
    uint32_t state_ = 12345;
};

std::vector<int16_t> tone(double rms) {
    std::vector<int16_t> f(kFrameSamples);
    const double amp = rms * std::sqrt(2.0);
    for (size_t i = 0; i < f.size(); ++i) {
        f[i] = static_cast<int16_t>(std::lround(amp * std::sin(2.0 * M_PI * 440.0 * i / 16000.0)));
    }
    return f;
}

std::vector<int16_t> square(int16_t amp) {
    std::vector<int16_t> f(kFrameSamples);
    for (size_t i = 0; i < f.size(); ++i) f[i] = (i % 2) ? amp : static_cast<int16_t>(-amp);
    return f;
}

constexpr int frames(uint32_t ms) { return static_cast<int>(ms / kFrameMs); }

}  // namespace

TEST(Vad, SpeechBetweenSilencesIsDone) {
    Vad vad(VadMode::Auto);
    Noise noise;
    for (int i = 0; i < frames(300); ++i)
        ASSERT_EQ(vad.push(noise.frame(300)), VadVerdict::Continue);
    for (int i = 0; i < frames(1000); ++i) ASSERT_EQ(vad.push(tone(8000)), VadVerdict::Continue);
    VadVerdict v = VadVerdict::Continue;
    int silent = 0;
    while (v == VadVerdict::Continue && silent < frames(900)) {
        v = vad.push(noise.frame(300));
        ++silent;
    }
    EXPECT_EQ(v, VadVerdict::Done);
    EXPECT_EQ(silent, frames(800));
    EXPECT_GE(vad.speech_ms(), 960u);
    EXPECT_LE(vad.speech_ms(), 1040u);
    // The verdict sticks.
    EXPECT_EQ(vad.push(tone(8000)), VadVerdict::Done);
}

TEST(Vad, NoSpeechWithinFiveSeconds) {
    Vad vad(VadMode::Auto);
    Noise noise;
    for (int i = 1; i < frames(5000); ++i) {
        ASSERT_EQ(vad.push(noise.frame(300)), VadVerdict::Continue) << "frame " << i;
    }
    EXPECT_EQ(vad.push(noise.frame(300)), VadVerdict::NoSpeech);
    EXPECT_EQ(vad.speech_ms(), 0u);
}

TEST(Vad, ShortBurstIsTooShort) {
    Vad vad(VadMode::Auto);
    Noise noise;
    for (int i = 0; i < frames(300); ++i) vad.push(noise.frame(300));
    for (int i = 0; i < frames(200); ++i) ASSERT_EQ(vad.push(tone(8000)), VadVerdict::Continue);
    VadVerdict v = VadVerdict::Continue;
    for (int i = 0; i < frames(900) && v == VadVerdict::Continue; ++i)
        v = vad.push(noise.frame(300));
    EXPECT_EQ(v, VadVerdict::TooShort);
    EXPECT_LT(vad.speech_ms(), 300u);
    EXPECT_GT(vad.speech_ms(), 0u);
}

TEST(Vad, AdaptsToRisingNoise) {
    Vad vad(VadMode::Auto);
    Noise noise;
    for (int i = 0; i < frames(300); ++i) vad.push(noise.frame(300));
    const int ramp = frames(3000);
    for (int i = 0; i < ramp; ++i) {
        const double rms = 300.0 + (1200.0 - 300.0) * i / (ramp - 1);
        ASSERT_EQ(vad.push(noise.frame(rms)), VadVerdict::Continue) << "frame " << i;
    }
    EXPECT_EQ(vad.speech_ms(), 0u);
    EXPECT_EQ(vad.finish(), VadVerdict::TooShort);
}

TEST(Vad, HoldModeNeverEndsOnSilence) {
    Vad vad(VadMode::Hold);
    Noise noise;
    for (int i = 0; i < frames(300); ++i) vad.push(noise.frame(300));
    for (int i = 0; i < frames(1000); ++i) ASSERT_EQ(vad.push(tone(8000)), VadVerdict::Continue);
    for (int i = 0; i < frames(6000); ++i)
        ASSERT_EQ(vad.push(noise.frame(300)), VadVerdict::Continue);
    EXPECT_EQ(vad.finish(), VadVerdict::Done);
}

TEST(Vad, HoldModeReleaseWithoutSpeechIsTooShort) {
    Vad vad(VadMode::Hold);
    Noise noise;
    for (int i = 0; i < frames(6000); ++i)
        ASSERT_EQ(vad.push(noise.frame(300)), VadVerdict::Continue);
    EXPECT_EQ(vad.finish(), VadVerdict::TooShort);
}

TEST(Vad, LevelMapsDbfs) {
    Vad vad(VadMode::Hold);
    vad.push(square(32767));
    EXPECT_EQ(vad.level(), 100);
    vad.push(square(33));  // about -60 dBFS
    EXPECT_EQ(vad.level(), 0);
    vad.push(square(0));
    EXPECT_EQ(vad.level(), 0);
    vad.push(square(3277));  // about -20 dBFS
    EXPECT_NEAR(vad.level(), 67, 1);
}
