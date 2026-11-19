// The simulator's MP3 decoder: minimp3's frame decoder (minimp3.h) fed
// from a 16 KiB window refilled from the Source. Not minimp3_ex.h's
// stream layer: its sample-accurate seek scans the stream for a frame
// index, while the player seeks through the Xing TOC or a byte estimate
// (spec 4.3). Stream info and seek offsets come from wisp_media's parsers,
// shared with the firmware's esp_audio_codec decoder.

#include "wisp/port/sim/minimp3_decoder.hpp"

#include <minimp3.h>

#include <algorithm>
#include <cstring>

#include "wisp/media/tags.hpp"
#include "wisp/os/large.hpp"

namespace wisp::port::sim {
namespace {

// minimp3 confirms a sync by walking several following frame headers and
// skips a frame cut off by the end of its input as garbage, so the window
// is refilled whenever less than half of it is left. Half (8 KiB) holds
// more than five of the longest frames (1441 bytes at 320 kbit/s, 32 kHz).
constexpr size_t kWindow = 16 * 1024;
// When minimp3 finds no frame, the window's tail may still hold the start
// of one; this much of it is kept for the next attempt.
constexpr size_t kKeepTail = 2304;
// A frame's audio may start up to 511 bytes back in earlier frames (the
// bit reservoir), which minimp3 only has once it has decoded them. A seek
// therefore starts decoding this far before its target and drops the
// frames before it, instead of losing the frames whose reservoir is
// missing (about 0.2 s for a low-rate VBR stream). Enough for the
// reservoir plus the headers and side info of the small frames holding it.
constexpr uint64_t kSeekLeadBytes = 2048;

class Minimp3Decoder : public media::Decoder {
public:
    Result<media::StreamInfo> open(media::Source& src) override {
        state_ = os::make_large<State>();
        window_ = os::LargeBuffer::allocate(kWindow);
        if (!state_ || !window_.ok()) {
            return Result<media::StreamInfo>::err(ErrorCode::NoMemory, "MP3 decoder buffers");
        }
        // The ID3v2 tag may be far larger than the window (cover art), so
        // only its header is read and the tag itself skipped.
        uint8_t id3[10] = {};
        WISP_TRY(src.seek(0));
        auto got = read_some(src, id3, sizeof id3);
        if (!got) return fail(got.error());
        const uint64_t skip = got.value() == sizeof id3 ? media::id3v2_size(id3) : 0;
        if (skip >= src.size()) {
            return Result<media::StreamInfo>::err(ErrorCode::Corrupt, "no audio after the ID3 tag");
        }
        // decode() and seek_ms() refuse to run until open() has succeeded.
        src_ = &src;
        auto started = restart(skip);
        auto info =
            started ? media::parse_mp3_info(window_.span().first(in_len_), src.size(), skip)
                    : Result<media::Mp3Info>::err(started.error().code, started.error().message);
        if (info) {
            info_ = info.value();
            started = restart(media::mp3_seek_offset(info_, 0));
        }
        if (!info || !started) {
            src_ = nullptr;
            return fail(info ? started.error() : info.error());
        }
        decoded_any_ = false;
        return Result<media::StreamInfo>::ok(
            {info_.sample_rate, info_.channels, info_.duration_ms});
    }

    Result<size_t> decode(int16_t* pcm, size_t max_frames) override {
        if (src_ == nullptr) return Result<size_t>::err(ErrorCode::InvalidArgument, "not open");
        size_t out = 0;
        while (out < max_frames) {
            if (pending_ == 0) {
                auto more = next_frame();
                if (!more) return Result<size_t>::err(more.error().code, more.error().message);
                if (!more.value()) break;
            }
            const size_t n = std::min(pending_, max_frames - out);
            copy_frames(pcm + out * info_.channels, n);
            out += n;
        }
        // A stream that parsed but yields no frame at all from its start
        // is damaged, not empty.
        if (out == 0 && max_frames > 0 && !decoded_any_) {
            return Result<size_t>::err(ErrorCode::Corrupt, "no decodable MP3 frame");
        }
        return Result<size_t>::ok(out);
    }

    Result<void> seek_ms(uint32_t ms) override {
        if (src_ == nullptr) return Result<void>::err(ErrorCode::InvalidArgument, "not open");
        // Landing past the last frame is an ordinary end, not damage.
        decoded_any_ = true;
        const uint64_t target = media::mp3_seek_offset(info_, ms);
        const uint64_t first = info_.audio_offset + info_.tag_frame_bytes;
        WISP_TRY(restart(std::max(first, target > kSeekLeadBytes ? target - kSeekLeadBytes : 0)));
        drop_before_ = target;
        return Result<void>::ok();
    }

private:
    struct State {
        mp3dec_t dec;
        int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    };

    static Result<media::StreamInfo> fail(const Error& e) {
        return Result<media::StreamInfo>::err(e.code, e.message);
    }

    // Source reads may come back short; only 0 is the end.
    static Result<size_t> read_some(media::Source& src, uint8_t* dst, size_t len) {
        size_t done = 0;
        while (done < len) {
            auto r = src.read(dst + done, len - done);
            if (!r) return r;
            if (r.value() == 0) break;
            done += r.value();
        }
        return Result<size_t>::ok(done);
    }

    // Decoding resumes at `offset`, which need not be a frame boundary:
    // minimp3 resyncs on the next frame header.
    Result<void> restart(uint64_t offset) {
        WISP_TRY(src_->seek(offset));
        base_ = offset;
        drop_before_ = 0;
        in_pos_ = in_len_ = 0;
        eof_ = false;
        pending_ = 0;
        mp3dec_init(&state_->dec);
        return refill();
    }

    Result<void> refill() {
        uint8_t* w = window_.data();
        std::memmove(w, w + in_pos_, in_len_ - in_pos_);
        base_ += in_pos_;
        in_len_ -= in_pos_;
        in_pos_ = 0;
        auto got = read_some(*src_, w + in_len_, kWindow - in_len_);
        if (!got) return Result<void>::err(got.error().code, got.error().message);
        in_len_ += got.value();
        if (in_len_ < kWindow) {
            eof_ = true;
            // minimp3 takes a frame followed by neither a frame nor the end
            // of its input for a false sync, so a trailing ID3v1 tag would
            // cost the last frame.
            constexpr size_t kId3v1 = 128;
            if (in_len_ >= kId3v1 && std::memcmp(w + in_len_ - kId3v1, "TAG", 3) == 0) {
                in_len_ -= kId3v1;
            }
        }
        return Result<void>::ok();
    }

    // Decodes the next frame with audio into state_->pcm; false at the end.
    Result<bool> next_frame() {
        for (;;) {
            if (!eof_ && in_len_ - in_pos_ < kWindow / 2) WISP_TRY(refill());
            const size_t avail = in_len_ - in_pos_;
            if (avail == 0) return Result<bool>::ok(false);
            mp3dec_frame_info_t fi{};
            const int samples = mp3dec_decode_frame(&state_->dec, window_.data() + in_pos_,
                                                    static_cast<int>(avail), state_->pcm, &fi);
            const uint64_t frame_at = base_ + in_pos_ + static_cast<uint64_t>(fi.frame_offset);
            size_t used = static_cast<size_t>(fi.frame_bytes);
            if (samples == 0 && (used == 0 || used == avail)) {
                // No frame in what is left: at the end that is trailing
                // junk; before it, keep the tail, which may hold the start
                // of a frame the next read completes.
                used = eof_ ? avail : avail - kKeepTail;
            }
            in_pos_ += used;
            if (samples > 0 && fi.channels > 0 && frame_at >= drop_before_) {
                pending_ = static_cast<size_t>(samples);
                pending_at_ = 0;
                pending_channels_ = static_cast<uint8_t>(fi.channels);
                decoded_any_ = true;
                return Result<bool>::ok(true);
            }
        }
    }

    // Frames are output with the stream's channel count from open(); a
    // frame that differs (a mono frame in a stereo stream) is converted.
    void copy_frames(int16_t* dst, size_t n) {
        const int16_t* src = state_->pcm + pending_at_ * pending_channels_;
        const uint8_t out_ch = info_.channels;
        if (pending_channels_ == out_ch) {
            std::memcpy(dst, src, n * out_ch * sizeof(int16_t));
        } else if (out_ch == 2) {
            for (size_t i = 0; i < n; ++i) dst[2 * i] = dst[2 * i + 1] = src[i];
        } else {
            for (size_t i = 0; i < n; ++i) {
                dst[i] = static_cast<int16_t>((int{src[2 * i]} + int{src[2 * i + 1]}) / 2);
            }
        }
        pending_at_ += n;
        pending_ -= n;
    }

    os::LargeUnique<State> state_;
    os::LargeBuffer window_;
    media::Source* src_ = nullptr;
    media::Mp3Info info_;
    uint64_t base_ = 0;         // file offset of window_[0]
    uint64_t drop_before_ = 0;  // frames starting before this offset are not output
    size_t in_pos_ = 0;
    size_t in_len_ = 0;
    bool eof_ = false;
    size_t pending_ = 0;  // frames of state_->pcm not yet handed out
    size_t pending_at_ = 0;
    uint8_t pending_channels_ = 0;
    bool decoded_any_ = false;
};

}  // namespace

std::unique_ptr<media::Decoder> make_minimp3_decoder() {
    return std::make_unique<Minimp3Decoder>();
}

}  // namespace wisp::port::sim
