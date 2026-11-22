#include "wisp/port/sim/minimp3_decoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "support/media_data.hpp"
#include "wisp/media/decoder.hpp"

using namespace wisp;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes read_file(const std::string& name) {
    std::ifstream in(test::media_data(name), std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Decoded {
    size_t frames = 0;
    int peak = 0;
};

// Decodes to the end in blocks smaller than an MP3 frame, so the
// decoder's carry-over of a frame's samples across calls is exercised.
Decoded decode_all(media::Decoder& d, uint8_t channels, size_t block = 700) {
    Decoded out;
    std::vector<int16_t> buf(block * channels);
    for (;;) {
        auto r = d.decode(buf.data(), block);
        EXPECT_TRUE(r) << r.error().message;
        if (!r || r.value() == 0) break;
        EXPECT_LE(r.value(), block);
        out.frames += r.value();
        for (size_t i = 0; i < r.value() * channels; ++i) {
            out.peak = std::max(out.peak, std::abs(int{buf[i]}));
        }
    }
    return out;
}

double ms_of(size_t frames, uint32_t rate) { return frames * 1000.0 / rate; }

// The test sines are at 1/8 of full scale (ffmpeg's lavfi default, peak
// 4096), the stereo ones 3 dB lower from the upmix; ffmpeg's own decoder
// peaks at 2754 (44.1 kHz stereo), 4354 (48 kHz mono) and 3892 (22.05 kHz).
// Above this, the output is the tone, not silence or a few clicks.
constexpr int kMinPeak = 2000;

// The test files carry the encoder's delay and padding (about 50 ms at
// 22.05 kHz, less at higher rates), which the decoders do not trim.
constexpr double kToleranceMs = 100.0;

}  // namespace

TEST(Minimp3Decoder, DecodesSineAtRate) {
    const Bytes mp3 = read_file("sine_44k_stereo_cbr128.mp3");
    media::MemorySource src(mp3);
    auto d = port::sim::make_minimp3_decoder();
    auto s = d->open(src);
    ASSERT_TRUE(s) << s.error().message;
    EXPECT_EQ(s.value().sample_rate, 44100u);
    EXPECT_EQ(s.value().channels, 2u);
    EXPECT_NEAR(s.value().duration_ms, 3000.0, kToleranceMs);
    const Decoded out = decode_all(*d, 2);
    EXPECT_NEAR(ms_of(out.frames, 44100), 3000.0, kToleranceMs);
    EXPECT_GT(out.peak, kMinPeak);
}

TEST(Minimp3Decoder, MonoVbr48k) {
    const Bytes mp3 = read_file("sine_48k_mono_vbr.mp3");
    media::MemorySource src(mp3);
    auto d = port::sim::make_minimp3_decoder();
    auto s = d->open(src);
    ASSERT_TRUE(s) << s.error().message;
    EXPECT_EQ(s.value().sample_rate, 48000u);
    EXPECT_EQ(s.value().channels, 1u);
    const Decoded out = decode_all(*d, 1, 100);
    EXPECT_NEAR(ms_of(out.frames, 48000), 2000.0, kToleranceMs);
    EXPECT_GT(out.peak, kMinPeak);
}

TEST(Minimp3Decoder, SeekByTocAndByEstimate) {
    struct Case {
        const char* name;
        uint32_t rate;
        uint8_t channels;
        double rest_ms;
    };
    for (const Case& c : {Case{"sine_44k_stereo_cbr128.mp3", 44100, 2, 2000.0},
                          Case{"sine_48k_mono_vbr.mp3", 48000, 1, 1000.0},   // Xing TOC
                          Case{"sine_22k_noxing.mp3", 22050, 1, 1000.0}}) {  // estimate
        const Bytes mp3 = read_file(c.name);
        media::MemorySource src(mp3);
        auto d = port::sim::make_minimp3_decoder();
        ASSERT_TRUE(d->open(src)) << c.name;
        std::vector<int16_t> some(500 * c.channels);
        ASSERT_TRUE(d->decode(some.data(), 500)) << c.name;  // a frame carried over
        ASSERT_TRUE(d->seek_ms(1000)) << c.name;
        const Decoded rest = decode_all(*d, c.channels);
        EXPECT_NEAR(ms_of(rest.frames, c.rate), c.rest_ms, kToleranceMs) << c.name;
        EXPECT_GT(rest.peak, kMinPeak) << c.name;

        ASSERT_TRUE(d->seek_ms(0)) << c.name;
        EXPECT_NEAR(ms_of(decode_all(*d, c.channels).frames, c.rate), c.rest_ms + 1000.0,
                    kToleranceMs)
            << c.name;
    }
}

TEST(Minimp3Decoder, CorruptMiddleSkipsToNextFrame) {
    Bytes mp3 = read_file("sine_44k_stereo_cbr128.mp3");
    std::fill(mp3.begin() + 2000, mp3.begin() + 2400, 0);
    media::MemorySource src(mp3);
    auto d = port::sim::make_minimp3_decoder();
    ASSERT_TRUE(d->open(src));
    const Decoded out = decode_all(*d, 2);
    // At most a few frames around the damage are lost.
    EXPECT_NEAR(ms_of(out.frames, 44100), 3000.0, kToleranceMs);
    EXPECT_GT(out.peak, kMinPeak);
}

TEST(Minimp3Decoder, GarbageIsCorrupt) {
    const Bytes g = read_file("garbage.mp3");
    media::MemorySource src(g);
    auto d = port::sim::make_minimp3_decoder();
    auto s = d->open(src);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, ErrorCode::Corrupt);
    int16_t pcm[4];
    EXPECT_FALSE(d->decode(pcm, 2));
}

TEST(Minimp3Decoder, ShortReadsDecodeTheSame) {
    // The prefetch ring may hand out a few bytes at a time.
    class Trickle : public media::Source {
    public:
        explicit Trickle(std::span<const uint8_t> b) : inner_(b) {}
        Result<size_t> read(uint8_t* dst, size_t len) override {
            return inner_.read(dst, std::min<size_t>(len, 97));
        }
        Result<void> seek(uint64_t off) override { return inner_.seek(off); }
        uint64_t size() const override { return inner_.size(); }

    private:
        media::MemorySource inner_;
    };
    const Bytes mp3 = read_file("sine_48k_mono_vbr.mp3");
    media::MemorySource whole(mp3);
    Trickle trickle(mp3);
    auto a = port::sim::make_minimp3_decoder();
    auto b = port::sim::make_minimp3_decoder();
    ASSERT_TRUE(a->open(whole));
    ASSERT_TRUE(b->open(trickle));
    EXPECT_EQ(decode_all(*a, 1).frames, decode_all(*b, 1).frames);
}

TEST(Minimp3Decoder, KeepsTheLastFrameBeforeAnId3v1Tag) {
    // 79 frames of 576 samples, then the 128-byte "TAG" block. No Xing
    // frame, so no encoder delay is trimmed either: every frame is output.
    const Bytes mp3 = read_file("sine_22k_noxing.mp3");
    ASSERT_EQ(std::string(mp3.end() - 128, mp3.end() - 125), "TAG");
    media::MemorySource src(mp3);
    auto d = port::sim::make_minimp3_decoder();
    ASSERT_TRUE(d->open(src));
    EXPECT_EQ(decode_all(*d, 1).frames, 79u * 576u);
}
