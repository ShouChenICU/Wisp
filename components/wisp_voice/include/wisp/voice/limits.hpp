#pragma once

// Voice-chain constants (MVP spec 4). Capture is 16 kHz mono 16-bit PCM
// cut into kFrameMs frames; durations are in milliseconds.

#include <cstddef>
#include <cstdint>

namespace wisp::voice {

// Capture and VAD (spec 4.1).
inline constexpr uint32_t kSampleRate = 16000;
inline constexpr uint32_t kFrameMs = 20;
inline constexpr uint32_t kMaxRecordMs = 30000;
inline constexpr uint32_t kHoldStartMs = 300;
inline constexpr uint32_t kNoiseEstimateMs = 300;
inline constexpr uint32_t kStartTimeoutMs = 5000;
inline constexpr uint32_t kEndSilenceMs = 800;
inline constexpr uint32_t kMinSpeechMs = 300;

inline constexpr size_t kFrameSamples = kSampleRate / 1000 * kFrameMs;

// Recognition and speech (spec 4.2, 4.4).
inline constexpr uint32_t kAsrTimeoutMs = 15000;
inline constexpr uint32_t kAsrRetries = 1;   // on a 429 or 5xx
inline constexpr size_t kTtsMaxChars = 200;  // code points spoken per turn
inline constexpr uint32_t kTtsDefaultRate = 24000;
inline constexpr uint32_t kTtsTimeoutMs = 15000;
inline constexpr uint32_t kRingMs = 2000;
inline constexpr uint32_t kLevelIntervalMs = 50;  // 20 Hz
inline constexpr size_t kWavHeaderBytes = 44;

}  // namespace wisp::voice
