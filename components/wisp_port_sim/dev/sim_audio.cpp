#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "sim_devices.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::sim {

namespace {
constexpr uint16_t kBufferedSamples = 1024;
}  // namespace

SimAudioOut::SimAudioOut(std::string id) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-audio";
}

SimAudioOut::~SimAudioOut() { close(); }

wisp::Result<void> SimAudioOut::open(const wisp::hal::AudioFormat& format) {
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = static_cast<int>(format.sample_rate);
    want.format = AUDIO_S16SYS;
    want.channels = format.channels;
    want.samples = kBufferedSamples;

    SDL_AudioSpec have;
    SDL_zero(have);
    // allowed_changes=0: SDL converts internally rather than handing
    // back a different format/rate/channel count, so `have` always
    // matches what was requested (task-13-brief fix round 1, item 1).
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0, &want, &have, 0);
    if (dev == 0) {
        return wisp::Result<void>::err(wisp::ErrorCode::DeviceFailure, SDL_GetError());
    }
    device_ = dev;
    channels_ = have.channels;
    sample_rate_ = have.freq;
    SDL_PauseAudioDevice(dev, 0);
    return wisp::Result<void>::ok();
}

wisp::Result<size_t> SimAudioOut::write(const int16_t* frames, size_t count) {
    if (device_ == 0) {
        return wisp::Result<size_t>::err(wisp::ErrorCode::InvalidArgument, "audio_out not open");
    }

    size_t total_samples = count * static_cast<size_t>(channels_);
    std::vector<int16_t> scaled(total_samples);
    int vol = volume_.load();
    for (size_t i = 0; i < total_samples; ++i) {
        scaled[i] = static_cast<int16_t>((static_cast<int32_t>(frames[i]) * vol) / 100);
    }

    // Throttle so playback runs at roughly real-time pace rather than
    // letting SDL_QueueAudio buffer an unbounded amount instantly:
    // block while more than ~200ms is already queued (fix round 1,
    // item 1).
    auto device = static_cast<SDL_AudioDeviceID>(device_);
    const uint32_t bytes_per_ms = static_cast<uint32_t>(sample_rate_) *
                                  static_cast<uint32_t>(channels_) * sizeof(int16_t) / 1000;
    const uint32_t max_queued_bytes = bytes_per_ms * 200;
    while (SDL_GetQueuedAudioSize(device) > max_queued_bytes) {
        SDL_Delay(5);
    }

    if (SDL_QueueAudio(device, scaled.data(),
                       static_cast<uint32_t>(total_samples * sizeof(int16_t))) != 0) {
        return wisp::Result<size_t>::err(wisp::ErrorCode::IoError, SDL_GetError());
    }
    return wisp::Result<size_t>::ok(count);
}

void SimAudioOut::close() {
    if (device_ != 0) {
        SDL_CloseAudioDevice(static_cast<SDL_AudioDeviceID>(device_));
        device_ = 0;
    }
}

wisp::Result<void> SimAudioOut::set_volume(uint8_t percent) {
    volume_.store(std::min<uint8_t>(percent, 100));
    return wisp::Result<void>::ok();
}

SimAudioIn::SimAudioIn(std::string id) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-audio";
}

SimAudioIn::~SimAudioIn() { close(); }

wisp::Result<void> SimAudioIn::open(const wisp::hal::AudioFormat& format) {
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = static_cast<int>(format.sample_rate);
    want.format = AUDIO_S16SYS;
    want.channels = format.channels;
    want.samples = kBufferedSamples;

    SDL_AudioSpec have;
    SDL_zero(have);
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, /*iscapture=*/1, &want, &have, 0);
    if (dev == 0) {
        // Expected whenever there's no real (or, under the "dummy"
        // driver, no simulated) capture device -- SimBoard::init()
        // avoids ever reaching this by probing SDL_GetNumAudioDevices(1)
        // first (see sim_board.cpp), but open() still fails cleanly
        // here instead of crashing if called anyway.
        return wisp::Result<void>::err(wisp::ErrorCode::DeviceFailure, SDL_GetError());
    }
    device_ = dev;
    channels_ = have.channels;
    SDL_PauseAudioDevice(dev, 0);
    return wisp::Result<void>::ok();
}

wisp::Result<size_t> SimAudioIn::read(int16_t* frames, size_t count) {
    if (device_ == 0) {
        return wisp::Result<size_t>::err(wisp::ErrorCode::InvalidArgument, "audio_in not open");
    }

    auto device = static_cast<SDL_AudioDeviceID>(device_);
    const size_t need_bytes = count * static_cast<size_t>(channels_) * sizeof(int16_t);
    size_t have_bytes = 0;
    auto* dest = reinterpret_cast<uint8_t*>(frames);

    // Bounded wait (fix round 1, item 1): SDL_DequeueAudio is
    // non-blocking, so this polls it until `count` frames have arrived
    // or kTimeoutMs elapses, rather than spinning forever if the
    // capture device never produces enough data.
    constexpr uint32_t kTimeoutMs = 5000;
    uint64_t start = wisp::os::now_ms();
    while (have_bytes < need_bytes) {
        uint32_t got = SDL_DequeueAudio(device, dest + have_bytes,
                                        static_cast<uint32_t>(need_bytes - have_bytes));
        have_bytes += got;
        if (have_bytes >= need_bytes) {
            break;
        }
        if (wisp::os::now_ms() - start >= kTimeoutMs) {
            break;
        }
        SDL_Delay(5);
    }
    return wisp::Result<size_t>::ok(have_bytes /
                                    (static_cast<size_t>(channels_) * sizeof(int16_t)));
}

void SimAudioIn::close() {
    if (device_ != 0) {
        SDL_CloseAudioDevice(static_cast<SDL_AudioDeviceID>(device_));
        device_ = 0;
    }
}

wisp::Result<void> SimAudioIn::set_gain(float /*db*/) { return wisp::Result<void>::ok(); }

}  // namespace wisp::port::sim
