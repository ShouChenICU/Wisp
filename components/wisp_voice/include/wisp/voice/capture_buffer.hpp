#pragma once

// CaptureBuffer (spec 4.2's upload path): the recording for one turn, 16
// kHz mono 16-bit PCM, in one Mem::Large allocation (PSRAM on the device
// -- AGENTS.md section 8 on why: at 30 seconds this is 960000 bytes, far
// past what belongs in internal SRAM).
//
// The allocation is [kHeadReserve][PCM capacity][kTailReserve]. commit()
// only ever appends samples into the PCM region; finish_multipart() then
// turns what was committed into a multipart/form-data body for
// AsrClient's upload without copying the PCM: it writes the field
// headers and a 44-byte WAV header right-aligned into the head reserve so
// they end exactly where the PCM starts, and the closing boundary right
// after the last committed sample (inside the unused tail of the PCM
// region while the recording is short of full, or the tail reserve once
// it is not). The returned span is one contiguous range, ready for
// HttpClientRequest::body_ref.
//
// Not thread-safe: the capture loop that commits and the call that
// finishes and sends must be the same thread, or externally serialized.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/os/large.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::voice {

class CaptureBuffer {
public:
    // Comfortably fits the longest multipart preamble (model up to
    // ModelProfileService::kMaxModelBytes, language up to kMaxLanguageBytes,
    // the "file" field's own header) plus the 44-byte WAV header, and the
    // longest closing boundary line, respectively.
    static constexpr size_t kHeadReserve = 512;
    static constexpr size_t kTailReserve = 128;

    // nullopt only on allocation failure (PSRAM exhausted).
    static Result<CaptureBuffer> create(uint32_t max_ms = kMaxRecordMs);

    CaptureBuffer(CaptureBuffer&&) = default;
    CaptureBuffer& operator=(CaptureBuffer&&) = default;
    CaptureBuffer(const CaptureBuffer&) = delete;
    CaptureBuffer& operator=(const CaptureBuffer&) = delete;

    // The unwritten tail of the PCM region, for the capture loop to fill
    // and then commit(). Empty once full().
    std::span<int16_t> free_space();
    // Advances frames() by `frames` (clamped to the capacity).
    void commit(size_t frames);

    size_t frames() const { return frames_; }
    uint32_t ms() const;
    bool full() const { return frames_ >= capacity_frames_; }
    void clear() { frames_ = 0; }

    // Builds the multipart/form-data body for POST .../audio/transcriptions:
    // "model" and "file" (a WAV of the committed samples) always, "language"
    // only when it is not "". `boundary` is the bare boundary token (no
    // leading "--"). Valid until the next commit()/clear() or this
    // CaptureBuffer is destroyed.
    std::span<const uint8_t> finish_multipart(std::string_view boundary, std::string_view model,
                                              std::string_view language);

private:
    CaptureBuffer() = default;

    os::LargeBuffer storage_;
    size_t capacity_frames_ = 0;
    size_t frames_ = 0;
};

}  // namespace wisp::voice
