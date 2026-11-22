#include "wisp/media/decoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "support/media_data.hpp"
#include "wisp/media/tags.hpp"

using namespace wisp;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes read_file(const std::string& name) {
    std::ifstream in(test::media_data(name), std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::span<const uint8_t> head_of(const Bytes& b) {
    return std::span<const uint8_t>(b).first(std::min(b.size(), media::kProbeBytes));
}

// Hands out at most `chunk` bytes per read, as the prefetch ring does when
// it is nearly empty, so a decoder cannot assume whole reads.
class ChunkedSource : public media::Source {
public:
    ChunkedSource(std::span<const uint8_t> bytes, size_t chunk) : inner_(bytes), chunk_(chunk) {}
    Result<size_t> read(uint8_t* dst, size_t len) override {
        return inner_.read(dst, std::min(len, chunk_));
    }
    Result<void> seek(uint64_t offset) override { return inner_.seek(offset); }
    uint64_t size() const override { return inner_.size(); }

private:
    media::MemorySource inner_;
    size_t chunk_;
};

// Decodes to the end in blocks of `block` frames; every sample kept.
std::vector<int16_t> decode_all(media::Decoder& d, uint8_t channels, size_t block = 500) {
    std::vector<int16_t> out;
    std::vector<int16_t> buf(block * channels);
    for (;;) {
        auto r = d.decode(buf.data(), block);
        EXPECT_TRUE(r) << r.error().message;
        if (!r || r.value() == 0) break;
        EXPECT_LE(r.value(), block);
        out.insert(out.end(), buf.begin(), buf.begin() + r.value() * channels);
    }
    return out;
}

class FakeDecoder : public media::Decoder {
public:
    Result<media::StreamInfo> open(media::Source&) override {
        return Result<media::StreamInfo>::ok({8000, 1, 0});
    }
    Result<size_t> decode(int16_t*, size_t) override { return Result<size_t>::ok(0); }
    Result<void> seek_ms(uint32_t) override { return Result<void>::ok(); }
};

}  // namespace

TEST(Probe, DetectsEachFormat) {
    const Bytes wav = read_file("sine_44k_stereo.wav");
    auto w = media::probe(head_of(wav));
    ASSERT_TRUE(w);
    EXPECT_EQ(w.value(), media::Format::Wav);

    const Bytes id3 = read_file("sine_44k_stereo_cbr128.mp3");  // starts with "ID3"
    ASSERT_GT(media::id3v2_size(id3), 0u);
    auto m = media::probe(head_of(id3));
    ASSERT_TRUE(m);
    EXPECT_EQ(m.value(), media::Format::Mp3);

    const Bytes bare = read_file("sine_22k_noxing.mp3");  // starts with a frame sync
    ASSERT_EQ(bare[0], 0xFF);
    auto b = media::probe(head_of(bare));
    ASSERT_TRUE(b);
    EXPECT_EQ(b.value(), media::Format::Mp3);

    // A float WAV is still a WAV; the decoder refuses it, not probe().
    const Bytes f = read_file("float32.wav");
    auto fw = media::probe(head_of(f));
    ASSERT_TRUE(fw);
    EXPECT_EQ(fw.value(), media::Format::Wav);
}

TEST(Probe, GarbageIsNotAnMp3) {
    const Bytes g = read_file("garbage.mp3");
    auto r = media::probe(head_of(g));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_FALSE(media::probe(std::span<const uint8_t>()));
    const Bytes riff_only{'R', 'I', 'F', 'F', 0, 0, 0, 0};
    EXPECT_FALSE(media::probe(riff_only));
    // A frame sync whose successor is not a frame header.
    Bytes fake(1000, 0x55);
    const Bytes hdr{0xFF, 0xFB, 0x90, 0x00};  // 417-byte frames
    std::copy(hdr.begin(), hdr.end(), fake.begin());
    EXPECT_FALSE(media::probe(fake));
    std::copy(hdr.begin(), hdr.end(), fake.begin() + 417);
    EXPECT_TRUE(media::probe(fake));
}

TEST(WavDecoder, DecodesAllFrames) {
    // 0.25 s at 44.1 kHz stereo (tools/gen_media_test_data.sh).
    const Bytes wav = read_file("sine_44k_stereo.wav");
    auto info = media::parse_wav(wav, wav.size());
    ASSERT_TRUE(info);
    ChunkedSource src(wav, 1000);  // not a multiple of the 4-byte frame
    auto d = media::make_wav_decoder();
    auto s = d->open(src);
    ASSERT_TRUE(s) << s.error().message;
    EXPECT_EQ(s.value().sample_rate, 44100u);
    EXPECT_EQ(s.value().channels, 2u);
    EXPECT_EQ(s.value().duration_ms, 250u);
    const auto pcm = decode_all(*d, 2, 333);
    ASSERT_EQ(pcm.size(), 11025u * 2);
    EXPECT_EQ(std::memcmp(pcm.data(), wav.data() + info.value().data_offset, pcm.size() * 2), 0);
    int16_t out[8];
    auto again = d->decode(out, 4);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), 0u);  // end stays the end
}

TEST(WavDecoder, SeekLandsOnTheFrame) {
    const Bytes wav = read_file("sine_16k_mono.wav");
    auto info = media::parse_wav(wav, wav.size());
    ASSERT_TRUE(info);
    media::MemorySource src(wav);
    auto d = media::make_wav_decoder();
    ASSERT_TRUE(d->open(src));
    int16_t first[10];
    ASSERT_TRUE(d->decode(first, 10));

    ASSERT_TRUE(d->seek_ms(250));  // frame 4000 of 16000
    const auto rest = decode_all(*d, 1);
    ASSERT_EQ(rest.size(), 12000u);
    EXPECT_EQ(std::memcmp(rest.data(), wav.data() + info.value().data_offset + 4000 * 2, 2 * 12000),
              0);

    ASSERT_TRUE(d->seek_ms(0));
    EXPECT_EQ(decode_all(*d, 1).size(), 16000u);
    ASSERT_TRUE(d->seek_ms(5000));  // past the end: nothing left
    EXPECT_EQ(decode_all(*d, 1).size(), 0u);
}

TEST(WavDecoder, RefusesOtherEncodingsAndJunk) {
    const Bytes f = read_file("float32.wav");
    media::MemorySource fs(f);
    auto r = media::make_wav_decoder()->open(fs);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);

    const Bytes g = read_file("garbage.mp3");
    media::MemorySource gs(g);
    EXPECT_FALSE(media::make_wav_decoder()->open(gs));

    int16_t pcm[4];
    EXPECT_FALSE(media::make_wav_decoder()->decode(pcm, 2));  // not opened
}

TEST(WithWav, ForwardsMp3OnlyWhenPlatformHasOne) {
    auto bare = media::with_wav({});
    EXPECT_NE(bare(media::Format::Wav), nullptr);
    EXPECT_EQ(bare(media::Format::Mp3), nullptr);

    int asked = 0;
    auto full = media::with_wav([&](media::Format f) -> std::unique_ptr<media::Decoder> {
        ++asked;
        EXPECT_EQ(f, media::Format::Mp3);
        return std::make_unique<FakeDecoder>();
    });
    EXPECT_NE(full(media::Format::Mp3), nullptr);
    EXPECT_NE(full(media::Format::Wav), nullptr);
    EXPECT_EQ(asked, 1);
}

TEST(MemorySource, ReadsSeeksAndEnds) {
    const Bytes b{1, 2, 3, 4, 5};
    media::MemorySource s(b);
    EXPECT_EQ(s.size(), 5u);
    uint8_t buf[8];
    ASSERT_EQ(s.read(buf, 3).value(), 3u);
    ASSERT_EQ(s.read(buf, 8).value(), 2u);
    EXPECT_EQ(buf[1], 5);
    ASSERT_EQ(s.read(buf, 8).value(), 0u);
    ASSERT_TRUE(s.seek(5));
    EXPECT_FALSE(s.seek(6));
    ASSERT_TRUE(s.seek(1));
    ASSERT_EQ(s.read(buf, 1).value(), 1u);
    EXPECT_EQ(buf[0], 2);
}
