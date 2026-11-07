#include "wisp/voice/capture_buffer.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <utility>

#include "wisp/base/error.hpp"

namespace wisp::voice {

namespace {

void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// Canonical 44-byte header for `data_bytes` of kSampleRate mono 16-bit PCM.
void write_wav_header(uint8_t* out, uint32_t data_bytes) {
    constexpr uint16_t kChannels = 1;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint16_t kBlockAlign = kChannels * kBitsPerSample / 8;
    std::memcpy(out, "RIFF", 4);
    put_u32le(out + 4, 36 + data_bytes);
    std::memcpy(out + 8, "WAVE", 4);
    std::memcpy(out + 12, "fmt ", 4);
    put_u32le(out + 16, 16);  // fmt chunk size
    put_u16le(out + 20, 1);   // PCM
    put_u16le(out + 22, kChannels);
    put_u32le(out + 24, kSampleRate);
    put_u32le(out + 28, kSampleRate * kBlockAlign);
    put_u16le(out + 32, kBlockAlign);
    put_u16le(out + 34, kBitsPerSample);
    std::memcpy(out + 36, "data", 4);
    put_u32le(out + 40, data_bytes);
}

void append_field(std::string& out, std::string_view boundary, std::string_view name,
                  std::string_view value) {
    out += "--";
    out.append(boundary);
    out += "\r\nContent-Disposition: form-data; name=\"";
    out.append(name);
    out += "\"\r\n\r\n";
    out.append(value);
    out += "\r\n";
}

}  // namespace

Result<CaptureBuffer> CaptureBuffer::create(uint32_t max_ms) {
    const size_t capacity_frames = static_cast<size_t>(max_ms) * kSampleRate / 1000;
    const size_t pcm_bytes = capacity_frames * sizeof(int16_t);
    os::LargeBuffer storage = os::LargeBuffer::allocate(kHeadReserve + pcm_bytes + kTailReserve);
    if (!storage.ok()) {
        return Result<CaptureBuffer>::err(ErrorCode::NoMemory, "capture buffer: out of PSRAM");
    }
    CaptureBuffer buf;
    buf.storage_ = std::move(storage);
    buf.capacity_frames_ = capacity_frames;
    return Result<CaptureBuffer>::ok(std::move(buf));
}

std::span<int16_t> CaptureBuffer::free_space() {
    auto* pcm = reinterpret_cast<int16_t*>(storage_.data() + kHeadReserve);
    return {pcm + frames_, capacity_frames_ - frames_};
}

void CaptureBuffer::commit(size_t frames) {
    frames_ = std::min(frames_ + frames, capacity_frames_);
}

uint32_t CaptureBuffer::ms() const { return static_cast<uint32_t>(frames_ * 1000 / kSampleRate); }

std::span<const uint8_t> CaptureBuffer::finish_multipart(std::string_view boundary,
                                                         std::string_view model,
                                                         std::string_view language) {
    std::string preamble;
    append_field(preamble, boundary, "model", model);
    if (!language.empty()) append_field(preamble, boundary, "language", language);
    preamble += "--";
    preamble.append(boundary);
    preamble +=
        "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";

    const size_t pcm_bytes = frames_ * sizeof(int16_t);
    const size_t head_len = preamble.size() + kWavHeaderBytes;
    // model and language are each bounded well below what it takes to
    // overrun kHeadReserve (see the header comment); this is the only
    // thing that keeps the writes below from running past the buffer.
    assert(head_len <= kHeadReserve);
    uint8_t* base = storage_.data();
    const size_t head_start = kHeadReserve - head_len;
    std::memcpy(base + head_start, preamble.data(), preamble.size());
    write_wav_header(base + kHeadReserve - kWavHeaderBytes, static_cast<uint32_t>(pcm_bytes));

    std::string closing = "\r\n--";
    closing.append(boundary);
    closing += "--\r\n";
    assert(closing.size() <= kTailReserve);
    uint8_t* tail = base + kHeadReserve + pcm_bytes;
    std::memcpy(tail, closing.data(), closing.size());

    const size_t total = kHeadReserve - head_start + pcm_bytes + closing.size();
    return {base + head_start, total};
}

}  // namespace wisp::voice
