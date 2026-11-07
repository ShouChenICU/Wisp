// wisp::voice::CaptureBuffer (wisp/voice/capture_buffer.hpp): the PCM
// recording buffer and its zero-copy multipart upload body.

#include "wisp/voice/capture_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/voice/limits.hpp"

using wisp::voice::CaptureBuffer;

namespace {

struct Field {
    std::string_view name;
    std::string_view value;
};

// A small, test-only multipart/form-data reader: enough to pull out each
// field's name and raw value, in order. Not a general parser (it assumes
// the exact shape CaptureBuffer::finish_multipart() writes).
std::vector<Field> parse_multipart(std::string_view body, std::string_view boundary) {
    std::vector<Field> fields;
    const std::string delim = "--" + std::string(boundary);
    size_t pos = body.find(delim);
    while (pos != std::string_view::npos) {
        pos += delim.size();
        if (body.substr(pos, 2) == "--") break;  // the closing boundary
        if (body.substr(pos, 2) == "\r\n") pos += 2;
        const size_t header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string_view::npos) break;
        std::string_view headers = body.substr(pos, header_end - pos);
        const size_t value_start = header_end + 4;
        size_t next = body.find(delim, value_start);
        const size_t value_end = next == std::string_view::npos ? body.size() : next;
        std::string_view value = body.substr(value_start, value_end - value_start);
        // Every part's content ends with the CRLF that precedes the next
        // boundary line.
        if (value.size() >= 2 && value.substr(value.size() - 2) == "\r\n") {
            value.remove_suffix(2);
        }
        std::string_view name;
        if (size_t name_pos = headers.find("name=\""); name_pos != std::string_view::npos) {
            name_pos += 6;
            const size_t name_end = headers.find('"', name_pos);
            name = headers.substr(name_pos, name_end - name_pos);
        }
        fields.push_back({name, value});
        pos = next;
    }
    return fields;
}

uint16_t read_u16le(std::string_view s, size_t at) {
    return static_cast<uint16_t>(static_cast<uint8_t>(s[at]) |
                                 (static_cast<uint8_t>(s[at + 1]) << 8));
}

uint32_t read_u32le(std::string_view s, size_t at) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3])) << 24);
}

}  // namespace

TEST(CaptureBuffer, ThirtySecondsIs960000Bytes) {
    auto r = CaptureBuffer::create();
    ASSERT_TRUE(r);
    CaptureBuffer& buf = r.value();
    EXPECT_FALSE(buf.full());

    size_t total = 0;
    while (!buf.full()) {
        auto space = buf.free_space();
        ASSERT_FALSE(space.empty());
        const size_t take = std::min<size_t>(space.size(), 4000);
        buf.commit(take);
        total += take;
    }

    EXPECT_EQ(total, 480000u);
    EXPECT_EQ(buf.frames(), 480000u);
    EXPECT_EQ(buf.frames() * sizeof(int16_t), 960000u);
    EXPECT_EQ(buf.ms(), 30000u);
    EXPECT_TRUE(buf.full());

    buf.clear();
    EXPECT_EQ(buf.frames(), 0u);
    EXPECT_FALSE(buf.full());
}

TEST(CaptureBuffer, MultipartIsContiguousWithoutCopy) {
    auto r = CaptureBuffer::create();
    ASSERT_TRUE(r);
    CaptureBuffer& buf = r.value();

    // The PCM region's address, before anything is written into it.
    const uint8_t* pcm_base = reinterpret_cast<const uint8_t*>(buf.free_space().data());

    const int16_t samples[] = {1, -2, 3, -4, 5};
    auto space = buf.free_space();
    std::copy(std::begin(samples), std::end(samples), space.begin());
    buf.commit(std::size(samples));

    constexpr std::string_view kBoundary = "abc123";
    auto body_span = buf.finish_multipart(kBoundary, "whisper-1", "en");
    std::string body(reinterpret_cast<const char*>(body_span.data()), body_span.size());

    const size_t pcm_bytes = buf.frames() * sizeof(int16_t);
    const size_t closing_len = kBoundary.size() + 8;  // "\r\n--" + boundary + "--\r\n"
    const size_t head_len = body.size() - pcm_bytes - closing_len;
    EXPECT_EQ(body_span.data() + head_len, pcm_base);

    auto fields = parse_multipart(body, kBoundary);
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[0].name, "model");
    EXPECT_EQ(fields[0].value, "whisper-1");
    EXPECT_EQ(fields[1].name, "language");
    EXPECT_EQ(fields[1].value, "en");
    EXPECT_EQ(fields[2].name, "file");
    ASSERT_EQ(fields[2].value.size(), wisp::voice::kWavHeaderBytes + sizeof(samples));
    EXPECT_EQ(std::memcmp(fields[2].value.data() + wisp::voice::kWavHeaderBytes, samples,
                          sizeof(samples)),
              0);
}

TEST(CaptureBuffer, WavHeaderDescribesThePcm) {
    auto r = CaptureBuffer::create();
    ASSERT_TRUE(r);
    CaptureBuffer& buf = r.value();
    auto space = buf.free_space();
    std::fill(space.begin(), space.begin() + 100, int16_t{0});
    buf.commit(100);

    constexpr std::string_view kBoundary = "boundaryXYZ";
    auto body_span = buf.finish_multipart(kBoundary, "m", "");
    std::string body(reinterpret_cast<const char*>(body_span.data()), body_span.size());

    auto fields = parse_multipart(body, kBoundary);
    // "" omits the language field.
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[0].name, "model");
    EXPECT_EQ(fields[1].name, "file");

    std::string_view wav = fields[1].value;
    ASSERT_GE(wav.size(), wisp::voice::kWavHeaderBytes);
    EXPECT_EQ(wav.substr(0, 4), "RIFF");
    EXPECT_EQ(wav.substr(8, 4), "WAVE");
    EXPECT_EQ(wav.substr(12, 4), "fmt ");
    EXPECT_EQ(read_u16le(wav, 20), 1u);  // PCM
    EXPECT_EQ(read_u16le(wav, 22), 1u);  // mono
    EXPECT_EQ(read_u32le(wav, 24), wisp::voice::kSampleRate);
    EXPECT_EQ(read_u16le(wav, 34), 16u);  // bits per sample
    EXPECT_EQ(wav.substr(36, 4), "data");
    const uint32_t data_size = read_u32le(wav, 40);
    EXPECT_EQ(data_size, 100u * sizeof(int16_t));
    EXPECT_EQ(wav.size() - wisp::voice::kWavHeaderBytes, data_size);
}
