// The firmware's MP3 decoder: esp_audio_codec's simple decoder (its MP3
// parser finds the frames in input of any length) fed from a 16 KiB window
// refilled from the Source. The codec itself cannot seek, so stream info
// and seek offsets come from wisp_media's parsers (Xing TOC or a byte
// estimate, spec 4.3), shared with the simulator's minimp3 decoder
// (components/wisp_port_sim/media/minimp3_decoder.cpp), whose behaviour
// this one follows.

#include "wisp/port/esp/esp_mp3_decoder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "wisp/core/log.hpp"
#include "wisp/media/tags.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::esp {
namespace {

constexpr const char* kTag = "mp3";

// Refilled whenever less than half is left, so the parser always sees
// several whole frames (the longest is 1441 bytes at 320 kbit/s, 32 kHz).
constexpr size_t kWindow = 16 * 1024;
// When the parser takes nothing from a window it cannot use, the tail may
// still hold the start of a frame; this much of it is kept.
constexpr size_t kKeepTail = 2304;
// A frame's audio may start up to 511 bytes back in earlier frames (the
// bit reservoir), so a seek starts decoding this far before its target and
// drops what comes before it (as the minimp3 decoder does).
constexpr uint64_t kSeekLeadBytes = 2048;
// A stream with no decodable frame this far past its start is damaged;
// without the limit a large file of garbage would be scanned to its end.
constexpr uint64_t kCorruptScanBytes = 64 * 1024;
// One MPEG-1 Layer III frame: 1152 samples, 2 channels, 16 bits. The
// buffer grows if the codec ever asks for more.
constexpr size_t kOutBytes = 1152 * 2 * sizeof(int16_t);

class EspMp3Decoder : public media::Decoder {
public:
    ~EspMp3Decoder() override {
        if (handle_ != nullptr) esp_audio_simple_dec_close(handle_);
    }

    Result<media::StreamInfo> open(media::Source& src) override {
        window_ = os::LargeBuffer::allocate(kWindow);
        out_ = os::LargeBuffer::allocate(kOutBytes);
        if (!window_.ok() || !out_.ok()) {
            return Result<media::StreamInfo>::err(ErrorCode::NoMemory, "MP3 decoder buffers");
        }
        esp_audio_simple_dec_cfg_t cfg = {};
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        if (const esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &handle_);
            ret != ESP_AUDIO_ERR_OK) {
            handle_ = nullptr;
            const ErrorCode code = ret == ESP_AUDIO_ERR_MEM_LACK      ? ErrorCode::NoMemory
                                   : ret == ESP_AUDIO_ERR_NOT_SUPPORT ? ErrorCode::Unsupported
                                                                      : ErrorCode::DeviceFailure;
            return Result<media::StreamInfo>::err(code, "MP3 decoder");
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
        scan_from_ = base_;
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
    // the parser resyncs on the next frame header. The reset drops what
    // the parser holds of the old position.
    Result<void> restart(uint64_t offset) {
        WISP_TRY(src_->seek(offset));
        WISP_TRY(reset_codec());
        base_ = offset;
        last_frame_at_ = offset;
        drop_before_ = 0;
        in_pos_ = in_len_ = 0;
        eof_ = false;
        pending_ = 0;
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
            // A trailing ID3v1 tag is not audio; the parser need not see it.
            constexpr size_t kId3v1 = 128;
            if (in_len_ >= kId3v1 && std::memcmp(w + in_len_ - kId3v1, "TAG", 3) == 0) {
                in_len_ -= kId3v1;
            }
        }
        return Result<void>::ok();
    }

    // Starts the codec's parser and decoder afresh, dropping what they hold
    // (the bit reservoir included).
    Result<void> reset_codec() {
        if (esp_audio_simple_dec_reset(handle_) != ESP_AUDIO_ERR_OK) {
            return Result<void>::err(ErrorCode::DeviceFailure, "MP3 decoder reset");
        }
        return Result<void>::ok();
    }

    // Decodes the next frame with audio into out_; false at the end.
    //
    // The simple decoder documents only OK ("decode success or data fed
    // into the cached buffer"), INVALID_PARAMETER, BUFF_NOT_ENOUGH and
    // NOT_SUPPORT for process(); its MP3 parser resyncs on frame headers
    // itself. So, as with minimp3, a damaged frame is only stepped over:
    // a reset would drop the bit reservoir and cost the next frames too
    // (after a seek, the lead-in exists to rebuild it). The codec is reset
    // only when it has made no frame over a whole window.
    Result<bool> next_frame() {
        for (;;) {
            if (!eof_ && in_len_ - in_pos_ < kWindow / 2) WISP_TRY(refill());
            const uint64_t at = base_ + in_pos_;
            if (!decoded_any_ && at - scan_from_ > kCorruptScanBytes) {
                return Result<bool>::err(ErrorCode::Corrupt, "no decodable MP3 frame");
            }
            if (at - last_frame_at_ > kWindow) {
                WISP_TRY(reset_codec());
                last_frame_at_ = at;
            }
            const size_t avail = in_len_ - in_pos_;
            if (avail == 0) return Result<bool>::ok(false);
            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = window_.data() + in_pos_;
            raw.len = static_cast<uint32_t>(avail);
            raw.eos = eof_;
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = out_.data();
            frame.len = static_cast<uint32_t>(out_.size());
            const esp_audio_err_t ret = esp_audio_simple_dec_process(handle_, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                // Nothing was consumed; the same input is offered again.
                if (frame.needed_size <= out_.size()) {
                    return Result<bool>::err(ErrorCode::DeviceFailure, "MP3 output buffer");
                }
                out_ = os::LargeBuffer::allocate(frame.needed_size);
                if (!out_.ok()) return Result<bool>::err(ErrorCode::NoMemory, "MP3 output buffer");
                continue;
            }
            if (ret == ESP_AUDIO_ERR_MEM_LACK) {
                return Result<bool>::err(ErrorCode::NoMemory, "MP3 decoder");
            }
            if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_DATA_LACK) {
                // A damaged frame: step past what the parser took, at least
                // a byte, and let it resync on the next header.
                in_pos_ += std::min(std::max<size_t>(raw.consumed, 1), avail);
                continue;
            }
            const size_t used = std::min<size_t>(raw.consumed, avail);
            in_pos_ += used;
            if (ret != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
                if (used > 0) continue;
                // No progress: the parser wants more input than it was
                // given. At the end that is a cut-off frame or trailing
                // junk. Before it, more input is read; only a full window
                // the parser cannot use is dropped, keeping the tail, which
                // may start a frame.
                if (eof_) return Result<bool>::ok(false);
                if (in_pos_ > 0) {
                    WISP_TRY(refill());
                } else {
                    in_pos_ = avail > kKeepTail ? avail - kKeepTail : avail;
                }
                continue;
            }
            last_frame_at_ = base_ + in_pos_;
            esp_audio_simple_dec_info_t fi = {};
            if (esp_audio_simple_dec_get_info(handle_, &fi) != ESP_AUDIO_ERR_OK ||
                fi.channel == 0 || fi.channel > 2) {
                if (!warned_info_) {
                    WLOG_W(kTag, "frame without usable info (%u channels); skipped",
                           unsigned(fi.channel));
                    warned_info_ = true;
                }
                continue;
            }
            if (fi.bits_per_sample != 16) {
                return Result<bool>::err(ErrorCode::Unsupported, "MP3 output is not 16-bit");
            }
            // The codec tells where a frame ends in the input, not where it
            // starts: a frame is kept once its end passes the seek target,
            // so at most one frame before the target is heard.
            if (base_ + in_pos_ <= drop_before_) continue;
            pending_channels_ = fi.channel;
            pending_ = frame.decoded_size / (sizeof(int16_t) * pending_channels_);
            pending_at_ = 0;
            if (pending_ == 0) continue;
            decoded_any_ = true;
            return Result<bool>::ok(true);
        }
    }

    // Frames are output with the stream's channel count from open(); a
    // frame that differs (a mono frame in a stereo stream) is converted.
    void copy_frames(int16_t* dst, size_t n) {
        const int16_t* src =
            reinterpret_cast<const int16_t*>(out_.data()) + pending_at_ * pending_channels_;
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

    esp_audio_simple_dec_handle_t handle_ = nullptr;
    os::LargeBuffer window_;
    os::LargeBuffer out_;  // one decoded frame, interleaved int16
    media::Source* src_ = nullptr;
    media::Mp3Info info_;
    uint64_t base_ = 0;           // file offset of window_[0]
    uint64_t drop_before_ = 0;    // frames ending at or before this offset are not output
    uint64_t scan_from_ = 0;      // where decoding from the start began (corrupt limit)
    uint64_t last_frame_at_ = 0;  // input offset after the last frame (or restart)
    size_t in_pos_ = 0;
    size_t in_len_ = 0;
    bool eof_ = false;
    size_t pending_ = 0;  // frames of out_ not yet handed out
    size_t pending_at_ = 0;
    uint8_t pending_channels_ = 0;
    bool decoded_any_ = false;
    bool warned_info_ = false;
};

}  // namespace

std::unique_ptr<media::Decoder> make_esp_mp3_decoder() {
    // The simple decoder decodes MP3 through the codec's decoder registry;
    // only MP3 is registered, so the other codecs are not linked in.
    static const bool registered = [] {
        const esp_audio_err_t ret = esp_mp3_dec_register();
        if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
            WLOG_E(kTag, "registering the MP3 decoder failed: %d", int(ret));
            return false;
        }
        return true;
    }();
    if (!registered) return nullptr;
    return std::make_unique<EspMp3Decoder>();
}

}  // namespace wisp::port::esp

// esp_audio_codec allocates its decoder state through these hooks, whose
// weak defaults call malloc()/calloc(). Each block is under
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, so the defaults put about 30 KiB of
// MP3 decoder state in internal RAM for as long as a track is open
// (AGENTS.md section 8, "Small mallocs land in internal RAM"). These strong
// definitions send it to PSRAM instead. They live in this file because the
// linker loads it whenever the decoder is used, so they always win over the
// weak ones in the codec's archive.
extern "C" {

void* media_lib_module_malloc(const char* /*module*/, size_t size) {
    return wisp::os::alloc(size, wisp::os::Mem::Large);
}

void* media_lib_module_calloc(const char* /*module*/, size_t n, size_t size) {
    if (size != 0 && n > SIZE_MAX / size) return nullptr;
    void* p = wisp::os::alloc(n * size, wisp::os::Mem::Large);
    if (p != nullptr) std::memset(p, 0, n * size);
    return p;
}

void* media_lib_module_realloc(const char* /*module*/, void* buf, size_t size) {
    return wisp::os::realloc(buf, size, wisp::os::Mem::Large);
}

void media_lib_free(void* buf) {
    if (buf != nullptr) wisp::os::dealloc(buf);
}

}  // extern "C"
