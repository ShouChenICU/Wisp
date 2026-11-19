// PCM 16-bit WAV decoder (media spec 4.3): the samples are the output,
// so decoding is reading the data chunk in whole frames.

#include <algorithm>
#include <bit>

#include "wisp/media/decoder.hpp"
#include "wisp/media/tags.hpp"
#include "wisp/os/large.hpp"

namespace wisp::media {
namespace {

// WAV samples are little-endian and are handed out as they are read.
static_assert(std::endian::native == std::endian::little);

// The header, including any chunks before `data` (LIST, bext, ...), must
// fit in one read of this size: a single Source read cap (spec 4.4).
constexpr size_t kHeadBytes = 64 * 1024;

class WavDecoder : public Decoder {
public:
    Result<StreamInfo> open(Source& src) override {
        auto head = os::LargeBuffer::allocate(
            static_cast<size_t>(std::min<uint64_t>(src.size(), kHeadBytes)));
        if (!head.ok()) return Result<StreamInfo>::err(ErrorCode::NoMemory, "WAV header buffer");
        WISP_TRY(src.seek(0));
        auto got = read_fully(src, head.data(), head.size());
        if (!got) return fail(got.error());
        auto info = parse_wav(head.span().first(got.value()), src.size());
        if (!info) return fail(info.error());
        info_ = info.value();
        frame_bytes_ = size_t{info_.channels} * 2;
        total_frames_ = info_.data_bytes / frame_bytes_;
        src_ = &src;
        if (auto r = seek_frame(0); !r) {
            src_ = nullptr;  // decode() and seek_ms() refuse to run until open() succeeds
            return fail(r.error());
        }
        return Result<StreamInfo>::ok({info_.sample_rate, info_.channels, info_.duration_ms});
    }

    Result<size_t> decode(int16_t* pcm, size_t max_frames) override {
        if (src_ == nullptr) return Result<size_t>::err(ErrorCode::InvalidArgument, "not open");
        const uint64_t frames = std::min<uint64_t>(max_frames, total_frames_ - pos_);
        auto got = read_fully(*src_, reinterpret_cast<uint8_t*>(pcm), frames * frame_bytes_);
        if (!got) return got;
        // A partial frame is only possible at a short end of file.
        const size_t n = got.value() / frame_bytes_;
        pos_ += n;
        if (n < frames) pos_ = total_frames_;  // the file ended early
        return Result<size_t>::ok(n);
    }

    Result<void> seek_ms(uint32_t ms) override {
        if (src_ == nullptr) return Result<void>::err(ErrorCode::InvalidArgument, "not open");
        return seek_frame(
            std::min<uint64_t>(uint64_t{ms} * info_.sample_rate / 1000, total_frames_));
    }

private:
    static Result<StreamInfo> fail(const Error& e) {
        return Result<StreamInfo>::err(e.code, e.message);
    }

    // Source reads may come back short (the prefetch ring hands out what
    // it has); only 0 is the end.
    static Result<size_t> read_fully(Source& src, uint8_t* dst, size_t len) {
        size_t done = 0;
        while (done < len) {
            auto r = src.read(dst + done, len - done);
            if (!r) return r;
            if (r.value() == 0) break;
            done += r.value();
        }
        return Result<size_t>::ok(done);
    }

    Result<void> seek_frame(uint64_t frame) {
        WISP_TRY(src_->seek(info_.data_offset + frame * frame_bytes_));
        pos_ = frame;
        return Result<void>::ok();
    }

    Source* src_ = nullptr;
    WavInfo info_;
    size_t frame_bytes_ = 0;
    uint64_t total_frames_ = 0;
    uint64_t pos_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> make_wav_decoder() { return std::make_unique<WavDecoder>(); }

}  // namespace wisp::media
