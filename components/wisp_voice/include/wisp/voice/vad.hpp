#pragma once

// Energy VAD with an adaptive noise floor (MVP spec 4.1). One push() per
// kFrameMs frame; the frame energy e is the mean square of its samples.
//
// - The first kNoiseEstimateMs estimate the noise floor: the mean energy
//   of their quietest half, so a loud transient there cannot inflate it.
// - A frame is speech when e > max(noise * kSpeechRatio, kAbsFloor);
//   non-speech frames keep adapting the floor, 1/kFallDiv per frame toward
//   a lower energy and 1/kRiseDiv toward a higher one, so a slowly rising
//   background never starts speech yet quiet speech cannot lift the floor
//   to its own level quickly.
// - Speech starts after kStartFrames speech frames in a row and counts
//   them retroactively. It gives up with NoSpeech after kStartTimeoutMs
//   without a start.
// - After the start, it ends after kEndSilenceMs of silence: Done, or
//   TooShort with under kMinSpeechMs of speech. finish() (a second tap)
//   ends it early with the same rule.
// A final verdict sticks: later pushes return it again.
// The VAD runs for tap captures only: a press-and-hold (key or screen)
// never runs it and sends any recording of kMinSpeechMs or more.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "wisp/voice/limits.hpp"

namespace wisp::voice {

// The level meter's mapping of a frame's mean square: RMS in dBFS,
// -60..0 onto 0..100.
uint8_t level_of(double mean_square);

enum class VadVerdict : uint8_t { Continue, Done, NoSpeech, TooShort };

class Vad {
public:
    static constexpr uint32_t kSpeechRatio = 6, kAbsFloor = 10000, kStartFrames = 3;
    static constexpr double kFallDiv = 16, kRiseDiv = 64;

    VadVerdict push(std::span<const int16_t> frame);
    VadVerdict finish();
    uint32_t speech_ms() const { return speech_ms_; }
    // level_of() of the last frame.
    uint8_t level() const { return level_; }

private:
    VadVerdict verdict_after_speech() const;

    VadVerdict final_ = VadVerdict::Continue;
    uint32_t elapsed_ms_ = 0;
    static constexpr size_t kHeadFrames = kNoiseEstimateMs / kFrameMs;
    std::array<double, kHeadFrames> head_{};
    size_t head_count_ = 0;
    double noise_ = 0;
    bool started_ = false;
    uint32_t run_ = 0;
    uint32_t speech_ms_ = 0;
    uint32_t silence_ms_ = 0;
    uint8_t level_ = 0;
};

}  // namespace wisp::voice
