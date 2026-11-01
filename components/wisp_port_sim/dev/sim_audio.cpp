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
// Fix round 3, item R2/R4: how long write()/read() may go *without
// making any progress* (a chunk queued / data dequeued) before giving
// up, rather than one fixed deadline for the entire call -- a 3s
// "录音回放" clip legitimately takes 3s of real wall-clock time to
// write/read via the ~200ms throttle, so a single whole-call deadline
// either has to be very generous (letting a truly stalled device hang
// the caller for a long time) or risks firing on a clip that's still
// making perfectly normal progress. Resetting this on every bit of
// progress means a device that's merely slow (but still working) never
// times out, while one that's genuinely stopped producing/consuming
// data is caught within ~1s of going quiet.
constexpr uint32_t kStallTimeoutMs = 1000;
}  // namespace

SimAudioOut::SimAudioOut(std::string id) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-audio";
}

SimAudioOut::~SimAudioOut() { close(); }

wisp::Result<void> SimAudioOut::open(const wisp::hal::AudioFormat& format) {
    if (device_ != 0) {
        // Fix round 2, item N5: refuse a second open() instead of
        // silently overwriting `device_` and leaking the first device.
        return wisp::Result<void>::err(wisp::ErrorCode::AlreadyExists, "audio_out already open");
    }

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
    buffer_frames_ = have.samples;
    SDL_PauseAudioDevice(dev, 0);
    return wisp::Result<void>::ok();
}

wisp::Result<size_t> SimAudioOut::write(const int16_t* frames, size_t count) {
    if (device_ == 0) {
        return wisp::Result<size_t>::err(wisp::ErrorCode::InvalidArgument, "audio_out not open");
    }

    auto device = static_cast<SDL_AudioDeviceID>(device_);
    const uint32_t bytes_per_ms = static_cast<uint32_t>(sample_rate_) *
                                  static_cast<uint32_t>(channels_) * sizeof(int16_t) / 1000;
    const uint32_t max_queued_bytes = bytes_per_ms * 200;
    int vol = volume_.load();

    // Fix round 2, item N1: split into ~20ms chunks and throttle *before
    // each one* instead of checking the queued-bytes cap once and then
    // queuing the entire buffer in a single SDL_QueueAudio() call. The
    // original one-shot version let a whole 3s "录音回放" clip get
    // queued (and therefore "written") near-instantly; close() then
    // dropped almost all of it by closing the device before playback
    // had caught up, so write()/close() together made playback
    // effectively silent.
    constexpr uint32_t kChunkMs = 20;
    size_t chunk_frames = std::max<size_t>(1, static_cast<size_t>(sample_rate_) * kChunkMs / 1000);

    // Fix round 3, item R2: a progress-based stall deadline, reset every
    // time a chunk is actually queued -- see kStallTimeoutMs's comment.
    uint64_t stall_deadline = wisp::os::now_ms() + kStallTimeoutMs;

    size_t done = 0;
    while (done < count) {
        size_t this_chunk = std::min(chunk_frames, count - done);
        size_t sample_count = this_chunk * static_cast<size_t>(channels_);
        std::vector<int16_t> scaled(sample_count);
        for (size_t i = 0; i < sample_count; ++i) {
            size_t src = done * static_cast<size_t>(channels_) + i;
            scaled[i] = static_cast<int16_t>((static_cast<int32_t>(frames[src]) * vol) / 100);
        }

        // Fix round 2/3, item N1/N3/R2: bail out -- instead of waiting
        // forever -- if the device stops playing or this call stalls
        // (no progress within kStallTimeoutMs) while waiting for room
        // in the queue. Once at least one chunk has been queued, a
        // stall is reported by returning what was written so far
        // (`done` frames) rather than an error, since the caller
        // already has a partial, usable write; only a stall before any
        // progress at all is reported as ErrorCode::Timeout.
        while (SDL_GetQueuedAudioSize(device) > max_queued_bytes) {
            bool stopped = SDL_GetAudioDeviceStatus(device) != SDL_AUDIO_PLAYING;
            bool stalled = wisp::os::now_ms() >= stall_deadline;
            if (stopped || stalled) {
                if (done > 0) {
                    return wisp::Result<size_t>::ok(done);
                }
                return wisp::Result<size_t>::err(
                    wisp::ErrorCode::Timeout,
                    stopped ? "audio_out device stopped while waiting to queue"
                            : "audio_out write stalled waiting for playback");
            }
            SDL_Delay(5);
        }

        if (SDL_QueueAudio(device, scaled.data(),
                           static_cast<uint32_t>(sample_count * sizeof(int16_t))) != 0) {
            return wisp::Result<size_t>::err(wisp::ErrorCode::IoError, SDL_GetError());
        }
        done += this_chunk;
        stall_deadline = wisp::os::now_ms() + kStallTimeoutMs;
    }
    return wisp::Result<size_t>::ok(count);
}

void SimAudioOut::close() {
    if (device_ == 0) {
        return;
    }
    auto device = static_cast<SDL_AudioDeviceID>(device_);

    // Fix round 2, item N1 (extended in fix round 3, item R3): drain
    // whatever is still queued before actually closing the device --
    // SDL_CloseAudioDevice() otherwise discards any unplayed audio
    // immediately, which (combined with the old one-shot write()) made
    // "录音回放" playback silent. Bounded by how long the remaining
    // queued audio plus the device's own internal buffer should take to
    // finish playing, plus a small margin, so a stalled device can't
    // hang this call (and therefore join_loopback()/App::stop())
    // forever.
    const uint32_t bytes_per_ms = static_cast<uint32_t>(sample_rate_) *
                                  static_cast<uint32_t>(channels_) * sizeof(int16_t) / 1000;
    const uint32_t queued = SDL_GetQueuedAudioSize(device);
    const uint32_t queued_ms = bytes_per_ms > 0 ? queued / bytes_per_ms : 0;
    const uint32_t buffer_ms = sample_rate_ > 0 ? static_cast<uint32_t>(buffer_frames_) * 1000 /
                                                      static_cast<uint32_t>(sample_rate_)
                                                : 0;
    const uint64_t deadline = wisp::os::now_ms() + queued_ms + buffer_ms + 100;

    while (SDL_GetQueuedAudioSize(device) > 0 && wisp::os::now_ms() < deadline) {
        if (SDL_GetAudioDeviceStatus(device) != SDL_AUDIO_PLAYING) {
            break;
        }
        SDL_Delay(5);
    }

    // Fix round 3, item R3: SDL_GetQueuedAudioSize() reaching 0 only
    // means the *queue* is empty -- the device's own internal
    // hardware/callback buffer (`buffer_frames_` frames, from `have` in
    // open()) can still hold audio that hasn't actually reached the
    // speaker yet. There's no SDL API to ask "has that buffer drained
    // too", so just wait out its duration, still bounded by `deadline`
    // above (which already included buffer_ms).
    const uint64_t buffer_wait_until = std::min(deadline, wisp::os::now_ms() + buffer_ms);
    while (wisp::os::now_ms() < buffer_wait_until) {
        SDL_Delay(5);
    }

    SDL_CloseAudioDevice(device);
    device_ = 0;
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
    if (device_ != 0) {
        // Fix round 2, item N5: refuse a second open() instead of
        // silently overwriting `device_` and leaking the first device.
        return wisp::Result<void>::err(wisp::ErrorCode::AlreadyExists, "audio_in already open");
    }

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

    // Bounded wait (fix round 1, item 1; tightened in fix round 2, item
    // N1/N3, then made progress-based in fix round 3, item R2/R4):
    // SDL_DequeueAudio is non-blocking, so this polls it until `count`
    // frames have arrived, the device stops playing, or the call stalls
    // (no new bytes within kStallTimeoutMs, reset on every bit of
    // progress). Once at least one byte has been captured, a stall is
    // reported by returning what was captured so far instead of an
    // error; only a stall before any progress at all is reported as
    // ErrorCode::Timeout.
    uint64_t stall_deadline = wisp::os::now_ms() + kStallTimeoutMs;
    while (have_bytes < need_bytes) {
        bool stopped = SDL_GetAudioDeviceStatus(device) != SDL_AUDIO_PLAYING;
        if (stopped) {
            if (have_bytes > 0) {
                break;
            }
            return wisp::Result<size_t>::err(wisp::ErrorCode::Timeout,
                                             "audio_in device stopped while capturing");
        }
        uint32_t got = SDL_DequeueAudio(device, dest + have_bytes,
                                        static_cast<uint32_t>(need_bytes - have_bytes));
        if (got > 0) {
            have_bytes += got;
            stall_deadline = wisp::os::now_ms() + kStallTimeoutMs;
        }
        if (have_bytes >= need_bytes) {
            break;
        }
        if (wisp::os::now_ms() >= stall_deadline) {
            if (have_bytes > 0) {
                break;
            }
            return wisp::Result<size_t>::err(wisp::ErrorCode::Timeout, "audio_in read timed out");
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
