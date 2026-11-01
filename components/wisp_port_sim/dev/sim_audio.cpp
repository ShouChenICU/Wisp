#include <cstring>
#include <utility>

#include "sim_devices.hpp"

namespace wisp::port::sim {

SimAudioOut::SimAudioOut(std::string id) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-audio";
}

wisp::Result<void> SimAudioOut::open(const wisp::hal::AudioFormat& /*format*/) {
    open_.store(true);
    return wisp::Result<void>::ok();
}

wisp::Result<size_t> SimAudioOut::write(const int16_t* /*frames*/, size_t count) {
    if (!open_.load()) {
        return wisp::Result<size_t>::err(wisp::ErrorCode::InvalidArgument, "audio_out not open");
    }
    // No real playback device in the simulator; the samples are simply
    // discarded (see dev/sim_devices.hpp's SimAudioOut comment).
    return wisp::Result<size_t>::ok(count);
}

void SimAudioOut::close() { open_.store(false); }

wisp::Result<void> SimAudioOut::set_volume(uint8_t percent) {
    volume_.store(percent > 100 ? static_cast<uint8_t>(100) : percent);
    return wisp::Result<void>::ok();
}

SimAudioIn::SimAudioIn(std::string id) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-audio";
}

wisp::Result<void> SimAudioIn::open(const wisp::hal::AudioFormat& /*format*/) {
    open_.store(true);
    return wisp::Result<void>::ok();
}

wisp::Result<size_t> SimAudioIn::read(int16_t* frames, size_t count) {
    if (!open_.load()) {
        return wisp::Result<size_t>::err(wisp::ErrorCode::InvalidArgument, "audio_in not open");
    }
    // No real microphone in the simulator; "records" silence (see
    // dev/sim_devices.hpp's SimAudioIn comment).
    std::memset(frames, 0, count * sizeof(int16_t));
    return wisp::Result<size_t>::ok(count);
}

void SimAudioIn::close() { open_.store(false); }

wisp::Result<void> SimAudioIn::set_gain(float /*db*/) { return wisp::Result<void>::ok(); }

}  // namespace wisp::port::sim
