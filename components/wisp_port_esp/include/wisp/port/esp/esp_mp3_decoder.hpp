#pragma once
// The firmware's MP3 decoder (media spec 4.3) on Espressif's
// esp_audio_codec, the platform half of the player's media::DecoderFactory;
// the simulator's counterpart is wisp::port::sim::make_minimp3_decoder().
// Output is interleaved int16 at the stream's rate and channel count.
//
// The codec allocates its own state (about 28 KB per stream, upstream
// README) with the platform allocator while a track is open.

#include <memory>

#include "wisp/media/decoder.hpp"

namespace wisp::port::esp {

// Registers the codec's MP3 decoder the first time it is called.
std::unique_ptr<media::Decoder> make_esp_mp3_decoder();

}  // namespace wisp::port::esp
