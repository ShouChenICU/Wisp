#pragma once
// Pure parsers over bytes already read from a track: ID3v2/ID3v1 tags, MP3
// stream info and WAV headers (media spec 3.3, 4.3). No I/O; every length
// read from the data is checked against the span it came in, so hostile
// files yield empty fields or an error, never an out-of-bounds read.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"

namespace wisp::media {

// Longest tag field kept, in UTF-8 bytes; longer text is cut on a code
// point boundary. Titles that long are junk, and the index keeps every
// field in memory.
inline constexpr size_t kMaxTagFieldBytes = 255;

// UTF-8 text; a field that is missing or not decodable is empty.
struct TrackTags {
    std::string title, artist, album;
};

// Size of the ID3v2 tag at the start of `head` including its header and
// footer, i.e. the bytes to skip to reach the audio; 0 when `head` holds
// no valid ID3v2 header (it needs at least 10 bytes).
size_t id3v2_size(std::span<const uint8_t> head);

// Reads TIT2/TPE1/TALB from an ID3v2.3/2.4 tag, header included. `tag`
// may be shorter than the size the header declares (a bounded head read):
// frames past its end are ignored.
TrackTags parse_id3v2(std::span<const uint8_t> tag);

// Reads an ID3v1 tag from the last 128 bytes of a file; empty fields when
// there is no "TAG" marker.
TrackTags parse_id3v1(std::span<const uint8_t> last128);

struct Mp3Info {
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint32_t duration_ms = 0;  // 0 = unknown
    // Xing table of contents: toc[i] * audio_bytes / 256 is the offset
    // from audio_offset of i % of the duration.
    bool has_toc = false;
    std::array<uint8_t, 100> toc{};
    // The first MPEG frame (the Xing/Info/VBRI frame when there is one,
    // which is where the TOC offsets start) and the stream bytes from it.
    uint64_t audio_offset = 0;
    uint64_t audio_bytes = 0;
    // Length of the Xing/Info/VBRI frame at audio_offset, 0 when there is
    // none. It carries no audio (a decoder would play it as a frame of
    // silence), so decoding starts after it.
    uint32_t tag_frame_bytes = 0;
};

// Parses the stream that starts at file offset `audio_offset` (after the
// ID3v2 tag): the first frame header, then a Xing/Info or VBRI header for
// the duration, else an estimate from the first frame's bit rate and the
// audio size. Corrupt when no MPEG Layer III frame is found.
Result<Mp3Info> parse_mp3_info(std::span<const uint8_t> after_id3, uint64_t file_size,
                               uint64_t audio_offset);

// File offset at which decoding resumes for a seek to `ms` (clamped to
// the duration): through the Xing table of contents when there is one,
// else in proportion to the audio bytes (a constant-bit-rate estimate).
// The offset is not on a frame boundary; the decoder resyncs on the next
// frame header (spec 4.3). Never inside the tag frame.
uint64_t mp3_seek_offset(const Mp3Info& info, uint32_t ms);

struct WavInfo {
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint64_t data_offset = 0;
    uint64_t data_bytes = 0;  // whole frames only, clamped to the file
    uint32_t duration_ms = 0;
};

// Parses a RIFF/WAVE header up to the start of its `data` chunk, which
// must lie within `head`. Only PCM 16-bit with 1 or 2 channels is accepted
// (spec 4.3); other formats are Unsupported, malformed headers Corrupt.
Result<WavInfo> parse_wav(std::span<const uint8_t> head, uint64_t file_size);

// The file name of `path` without its directory and extension: the title
// of a track without a title tag.
std::string title_from_file_name(std::string_view path);

}  // namespace wisp::media
