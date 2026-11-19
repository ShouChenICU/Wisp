// Format probing, the decoder factory that adds WAV, and MemorySource
// (media spec 4.3).

#include "wisp/media/decoder.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "wisp/media/tags.hpp"

namespace wisp::media {

Result<Format> probe(std::span<const uint8_t> head) {
    if (head.size() >= 12 && std::memcmp(head.data(), "RIFF", 4) == 0 &&
        std::memcmp(head.data() + 8, "WAVE", 4) == 0) {
        return Result<Format>::ok(Format::Wav);
    }
    if (id3v2_size(head) > 0) return Result<Format>::ok(Format::Mp3);
    // Treating `head` as the whole file lets a single frame that fills it
    // count, while a frame whose successor lies within it must be
    // confirmed by that successor. The stream has to start at byte 0: a
    // sync found further in is as likely chance as music.
    if (head.size() >= 4 && head[0] == 0xFF) {
        auto info = parse_mp3_info(head, head.size(), 0);
        if (info && info.value().audio_offset == 0) return Result<Format>::ok(Format::Mp3);
    }
    return Result<Format>::err(ErrorCode::Unsupported, "not an MP3 or WAV file");
}

DecoderFactory with_wav(DecoderFactory platform) {
    return [platform = std::move(platform)](Format f) -> std::unique_ptr<Decoder> {
        if (f == Format::Wav) return make_wav_decoder();
        return platform ? platform(f) : nullptr;
    };
}

Result<size_t> MemorySource::read(uint8_t* dst, size_t len) {
    const size_t n = std::min(len, bytes_.size() - pos_);
    if (n > 0) std::memcpy(dst, bytes_.data() + pos_, n);
    pos_ += n;
    return Result<size_t>::ok(n);
}

Result<void> MemorySource::seek(uint64_t offset) {
    if (offset > bytes_.size()) {
        return Result<void>::err(ErrorCode::InvalidArgument, "seek past the end");
    }
    pos_ = static_cast<size_t>(offset);
    return Result<void>::ok();
}

}  // namespace wisp::media
