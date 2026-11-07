#pragma once

// Energy VAD with an adaptive noise floor (MVP spec 4.1). One push() per
// kFrameMs frame; the frame energy e is the mean square of its samples.
//
// - The first kNoiseEstimateMs estimate the noise floor (running mean).
// - A frame is speech when e > max(noise * kSpeechRatio, kAbsFloor);
//   non-speech frames keep adapting the floor (1/16 per frame), so a
//   slowly rising background never starts speech.
// - Speech starts after kStartFrames speech frames in a row and counts
//   them retroactively. Auto mode gives up with NoSpeech after
//   kStartTimeoutMs without a start.
// - After the start, Auto mode ends after kEndSilenceMs of silence: Done,
//   or TooShort with under kMinSpeechMs of speech. Hold mode never ends
//   on its own; finish() (the key's release) decides.
// A final verdict sticks: later pushes return it again.

#include <cstdint>
#include <span>

namespace wisp::voice {

enum class VadMode : uint8_t { Hold, Auto };
enum class VadVerdict : uint8_t { Continue, Done, NoSpeech, TooShort };

class Vad {
public:
    static constexpr uint32_t kSpeechRatio = 6, kAbsFloor = 10000, kStartFrames = 3;

    explicit Vad(VadMode mode);

    VadVerdict push(std::span<const int16_t> frame);
    VadVerdict finish();
    uint32_t speech_ms() const { return speech_ms_; }
    // The last frame's RMS in dBFS, -60..0 mapped onto 0..100.
    uint8_t level() const { return level_; }

private:
    VadVerdict verdict_after_speech() const;

    VadMode mode_;
    VadVerdict final_ = VadVerdict::Continue;
    uint32_t elapsed_ms_ = 0;
    uint32_t noise_frames_ = 0;
    double noise_ = 0;
    bool started_ = false;
    uint32_t run_ = 0;
    uint32_t speech_ms_ = 0;
    uint32_t silence_ms_ = 0;
    uint8_t level_ = 0;
};

}  // namespace wisp::voice
