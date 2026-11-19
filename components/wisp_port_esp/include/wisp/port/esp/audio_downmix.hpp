#pragma once

// Pure, platform-free helpers for wisp::port::esp::EspAudioIn
// (dev/esp_audio.cpp). A board's capture codec delivers N interleaved
// channels, each with a role the board declares (EspAudioInConfig::channels,
// spec section 11): a microphone, or a reference (the speaker signal looped
// back for echo cancellation). wisp::hal::AudioIn's contract is mono
// (AudioFormat::channels == 1), so EspAudioIn opens the codec with all N
// channels and downmixes every read() here, averaging the microphones and
// skipping the references.
//
// Kept header-only and free of any ESP-IDF/esp_codec_dev include so it can
// be unit-tested on the host (tests/port_esp/audio_downmix_test.cpp) even
// though wisp_port_esp itself is an ESP-only component (never built for
// the host -- see that component's CMakeLists.txt comment).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wisp::port::esp {

// Role of one physical capture channel. Ref: the speaker signal looped back.
enum class AudioChannel : uint8_t { Mic, Ref };

// Names published in AudioIn caps "channels".
inline const char* to_string(AudioChannel c) { return c == AudioChannel::Ref ? "ref" : "mic"; }

// At most four interleaved channels: the int32 sum of four int16 samples
// cannot overflow, and EspAudioIn's channel_mask stays within 4 bits.
inline constexpr size_t kMaxCaptureChannels = 4;

// Downmixes `frames` interleaved frames (`in` holds frames * roles.size()
// samples) into `frames` mono samples: the mean of each frame's Mic
// channels, summed in int32 and truncated toward zero (plain integer
// division, not rounded). With two Mic channels this is exactly
// (L + R) / 2. Requires 1..kMaxCaptureChannels roles with at least one Mic
// (EspAudioIn::create() refuses anything else). `mono` and `in` must not
// overlap.
inline void downmix(const int16_t* in, int16_t* mono, size_t frames,
                    std::span<const AudioChannel> roles) {
    const size_t n = roles.size();
    int32_t mics = 0;
    for (AudioChannel r : roles) {
        if (r == AudioChannel::Mic) ++mics;
    }
    for (size_t i = 0; i < frames; ++i) {
        const int16_t* frame = in + i * n;
        int32_t sum = 0;
        for (size_t c = 0; c < n; ++c) {
            if (roles[c] == AudioChannel::Mic) sum += frame[c];
        }
        mono[i] = static_cast<int16_t>(sum / mics);
    }
}

// Frames of one esp_codec_dev_read() into a scratch buffer of
// `scratch_samples` int16 samples: never more than the buffer holds at
// `channels` samples per frame, never more than the `remaining` frames.
constexpr size_t capture_chunk_frames(size_t remaining, size_t scratch_samples, size_t channels) {
    return std::min(remaining, scratch_samples / channels);
}

}  // namespace wisp::port::esp
