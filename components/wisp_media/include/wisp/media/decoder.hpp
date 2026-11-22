#pragma once
// The audio decoder interface the player drives (media spec 4.3), format
// probing, and the portable WAV decoder. MP3 decoding is the platform's:
// minimp3 in the simulator (wisp::port::sim::make_minimp3_decoder),
// esp_audio_codec on the device. Both reach the player as a
// DecoderFactory, which with_wav() completes with the WAV decoder.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "wisp/base/result.hpp"

namespace wisp::media {

struct StreamInfo {
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t duration_ms; /* 0 = unknown */
};

// Pull-based reader over the prefetch ring (spec 4.4). A decoder keeps a
// reference to the Source it was opened on, so the Source outlives it.
class Source {
public:
    virtual ~Source() = default;
    virtual Result<size_t> read(uint8_t* dst, size_t len) = 0;  // 0 = end
    virtual Result<void> seek(uint64_t offset) = 0;
    virtual uint64_t size() const = 0;
};

// One stream per decoder: open() once, then decode() and seek_ms().
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual Result<StreamInfo> open(Source& src) = 0;                    // parses headers
    virtual Result<size_t> decode(int16_t* pcm, size_t max_frames) = 0;  // interleaved; 0 = end
    virtual Result<void> seek_ms(uint32_t ms) = 0;
};

enum class Format : uint8_t { Mp3, Wav };
using DecoderFactory = std::function<std::unique_ptr<Decoder>(Format)>;  // nullptr: unsupported

// Bytes of a track's head that probe() needs at most: an ID3v2 or RIFF
// header, or two MPEG frame headers.
inline constexpr size_t kProbeBytes = 4096;

// The format from the magic bytes at the start of a file (spec 4.3):
// RIFF....WAVE, an ID3v2 header, or an MPEG Layer III frame sync at
// offset 0 that the next frame header confirms when it lies within
// `head`. Unsupported otherwise, whatever the file's extension.
Result<Format> probe(std::span<const uint8_t> head);

// PCM 16-bit WAV, 1 or 2 channels; other encodings fail open() with
// Unsupported.
std::unique_ptr<Decoder> make_wav_decoder();

// Serves Format::Wav itself and forwards the rest to `platform`; nullptr
// for MP3 when `platform` is empty.
DecoderFactory with_wav(DecoderFactory platform);

// A Source over bytes in memory, which it does not own: for tests.
class MemorySource : public Source {
public:
    explicit MemorySource(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    Result<size_t> read(uint8_t* dst, size_t len) override;
    // Past the end is InvalidArgument; the position stays where it was.
    Result<void> seek(uint64_t offset) override;
    uint64_t size() const override { return bytes_.size(); }

private:
    std::span<const uint8_t> bytes_;
    size_t pos_ = 0;
};

}  // namespace wisp::media
