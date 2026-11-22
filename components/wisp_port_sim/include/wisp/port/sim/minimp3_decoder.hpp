#pragma once
// The simulator's MP3 decoder (media spec 4.3) on top of the vendored
// minimp3 (third_party/minimp3/), the platform half of the player's
// media::DecoderFactory; the firmware's counterpart decodes with
// esp_audio_codec. Output is interleaved int16 at the stream's rate and
// channel count from the first frame.

#include <memory>

#include "wisp/media/decoder.hpp"

namespace wisp::port::sim {

std::unique_ptr<media::Decoder> make_minimp3_decoder();

}  // namespace wisp::port::sim
