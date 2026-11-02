#pragma once

// Pure, platform-free helper for wisp::port::esp::EspAudioIn (dev/esp_audio.cpp):
// ES7210 is wired on this board as a 2-channel (stereo) capture path (see
// docs/hardware/waveshare_amoled_216.md and task-18-brief.md's interface
// note), but wisp::hal::AudioIn's contract -- and the one caller,
// components/wisp_app/src/loopback.cpp's "录音回放" self-check -- is mono
// (AudioFormat::channels == 1). EspAudioIn therefore always opens the
// codec with 2 physical channels and downmixes every read() here before
// handing samples back to the caller.
//
// Kept header-only and free of any ESP-IDF/esp_codec_dev include so it can
// be unit-tested on the host (tests/port_esp/audio_downmix_test.cpp) even
// though wisp_port_esp itself is an ESP-only component (never built for
// the host -- see that component's CMakeLists.txt comment).

#include <cstddef>
#include <cstdint>

namespace wisp::port::esp {

// Downmixes `frames` stereo frames (L,R interleaved, `stereo` holding
// `frames*2` samples) into `frames` mono samples in `mono`:
// mono[i] = (L[i] + R[i]) / 2, truncated toward zero (plain C++ integer
// division -- not rounded to nearest), computed with a 32-bit
// intermediate so the L+R sum never overflows an int16_t. `mono` and
// `stereo` must not overlap (the caller always downmixes into a separate
// buffer -- see EspAudioIn::read()).
inline void downmix_stereo_to_mono(const int16_t* stereo, int16_t* mono, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        int32_t l = stereo[2 * i];
        int32_t r = stereo[2 * i + 1];
        mono[i] = static_cast<int16_t>((l + r) / 2);
    }
}

}  // namespace wisp::port::esp
