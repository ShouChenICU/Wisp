#include "wisp/media/tags.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "support/media_data.hpp"

using namespace wisp;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes read_file(const std::string& name) {
    std::ifstream in(test::media_data(name), std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void put_be32(Bytes& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

void put_syncsafe(Bytes& b, uint32_t v) {
    for (int s = 21; s >= 0; s -= 7) b.push_back(static_cast<uint8_t>((v >> s) & 0x7F));
}

void put_le16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void put_le32(Bytes& b, uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(static_cast<uint8_t>(v >> s));
}

void put_str(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }

// One ID3v2 frame: `size_field` is written as given (syncsafe for v2.4,
// plain for v2.3), so a test can lie about it.
void put_frame(Bytes& b, uint8_t version, std::string_view id, const Bytes& body,
               uint8_t flags2 = 0, int64_t size_field = -1) {
    put_str(b, id);
    const uint32_t size =
        size_field < 0 ? static_cast<uint32_t>(body.size()) : static_cast<uint32_t>(size_field);
    if (version == 4) {
        put_syncsafe(b, size);
    } else {
        put_be32(b, size);
    }
    b.push_back(0);
    b.push_back(flags2);
    b.insert(b.end(), body.begin(), body.end());
}

Bytes text(uint8_t encoding, const Bytes& raw) {
    Bytes out{encoding};
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

Bytes text(uint8_t encoding, std::string_view s) {
    return text(encoding, Bytes(s.begin(), s.end()));
}

// Wraps frames in a tag header whose size covers exactly `frames`.
Bytes tag(uint8_t version, uint8_t flags, const Bytes& frames) {
    Bytes b;
    put_str(b, "ID3");
    b.push_back(version);
    b.push_back(0);
    b.push_back(flags);
    put_syncsafe(b, static_cast<uint32_t>(frames.size()));
    b.insert(b.end(), frames.begin(), frames.end());
    return b;
}

// ID3v2 unsynchronisation: a 0x00 after every 0xFF that is followed by
// 0x00 or a byte >= 0xE0 (or that ends the data).
Bytes unsync(const Bytes& in) {
    Bytes out;
    for (size_t i = 0; i < in.size(); ++i) {
        out.push_back(in[i]);
        if (in[i] == 0xFF && (i + 1 == in.size() || in[i + 1] == 0x00 || in[i + 1] >= 0xE0)) {
            out.push_back(0x00);
        }
    }
    return out;
}

Bytes utf16le_bom(std::u16string_view s) {
    Bytes b{0xFF, 0xFE};
    for (char16_t c : s) {
        b.push_back(static_cast<uint8_t>(c));
        b.push_back(static_cast<uint8_t>(c >> 8));
    }
    return b;
}

media::TrackTags tags_of_file(const std::string& name) {
    const Bytes data = read_file(name);
    const size_t n = media::id3v2_size(data);
    EXPECT_GT(n, 10u);
    return media::parse_id3v2(std::span<const uint8_t>(data).first(std::min(n, data.size())));
}

Result<media::Mp3Info> mp3_info_of_file(const std::string& name) {
    const Bytes data = read_file(name);
    const size_t skip = std::min(media::id3v2_size(data), data.size());
    return media::parse_mp3_info(std::span<const uint8_t>(data).subspan(skip), data.size(), skip);
}

// A canonical 44-byte PCM WAV header with a data chunk of `data_size`.
Bytes wav_header(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
                 uint32_t data_size) {
    Bytes b;
    put_str(b, "RIFF");
    put_le32(b, 36 + data_size);
    put_str(b, "WAVE");
    put_str(b, "fmt ");
    put_le32(b, 16);
    put_le16(b, format);
    put_le16(b, channels);
    put_le32(b, rate);
    put_le32(b, rate * channels * bits / 8);
    put_le16(b, static_cast<uint16_t>(channels * bits / 8));
    put_le16(b, bits);
    put_str(b, "data");
    put_le32(b, data_size);
    return b;
}

}  // namespace

using media::TrackTags;

TEST(MediaTags, Id3v2Utf8Tags) {
    const TrackTags t = tags_of_file("sine_44k_stereo_cbr128.mp3");
    EXPECT_EQ(t.title, "Sine 440");
    EXPECT_EQ(t.artist, "Wisp");
    EXPECT_EQ(t.album, "Tests");
}

TEST(MediaTags, Id3v2Utf16Tags) {
    const TrackTags t = tags_of_file("sine_48k_mono_vbr.mp3");
    EXPECT_EQ(t.title, "Sine 48k");
    EXPECT_EQ(t.artist, "Wisp");
    EXPECT_EQ(t.album, "Tests");
}

TEST(MediaTags, Id3v1Fallback) {
    const Bytes data = read_file("sine_22k_noxing.mp3");
    ASSERT_GE(data.size(), 128u);
    EXPECT_EQ(media::id3v2_size(data), 0u);
    const TrackTags t = media::parse_id3v1(std::span<const uint8_t>(data).last(128));
    EXPECT_EQ(t.title, "Sine 22k");
    EXPECT_EQ(t.artist, "Wisp");
    EXPECT_EQ(t.album, "Tests");
}

TEST(MediaTags, Id3v1WithoutMarkerIsEmpty) {
    const Bytes data = read_file("sine_44k_stereo_cbr128.mp3");
    const TrackTags t = media::parse_id3v1(std::span<const uint8_t>(data).last(128));
    EXPECT_TRUE(t.title.empty());
    EXPECT_TRUE(media::parse_id3v1(std::span<const uint8_t>(data).first(10)).title.empty());
}

TEST(MediaTags, Id3v2SizeCoversHeaderAndFooter) {
    Bytes b = tag(4, 0, Bytes(300, 0));
    EXPECT_EQ(media::id3v2_size(b), 310u);
    b[5] = 0x10;  // footer present
    EXPECT_EQ(media::id3v2_size(b), 320u);
    b[6] = 0x80;  // not syncsafe
    EXPECT_EQ(media::id3v2_size(b), 0u);
    EXPECT_EQ(media::id3v2_size(std::span<const uint8_t>(b).first(5)), 0u);
}

TEST(MediaTags, Latin1ThatIsUtf8IsReadAsUtf8) {
    Bytes frames;
    put_frame(frames, 3, "TIT2", text(0, "晴天"));
    put_frame(frames, 3, "TPE1", text(0, "Jay"));
    const TrackTags t = media::parse_id3v2(tag(3, 0, frames));
    EXPECT_EQ(t.title, "晴天");
    EXPECT_EQ(t.artist, "Jay");
}

TEST(MediaTags, Latin1NonUtf8FieldIsDropped) {
    Bytes frames;
    put_frame(frames, 4, "TIT2", text(0, Bytes{0xC7, 0xE7, 0xCC, 0xEC}));
    put_frame(frames, 4, "TALB", text(0, "Album"));
    const TrackTags t = media::parse_id3v2(tag(4, 0, frames));
    EXPECT_TRUE(t.title.empty());
    EXPECT_EQ(t.album, "Album");

    Bytes v1(128, 0);
    v1[0] = 'T';
    v1[1] = 'A';
    v1[2] = 'G';
    v1[3] = 0xC7;
    v1[4] = 0xE7;
    v1[33] = 'W';
    const TrackTags t1 = media::parse_id3v1(v1);
    EXPECT_TRUE(t1.title.empty());
    EXPECT_EQ(t1.artist, "W");
}

TEST(MediaTags, Utf16BigEndianAndSurrogates) {
    Bytes frames;
    // Encoding 2: UTF-16BE without BOM; U+1F3B5 as a surrogate pair.
    put_frame(frames, 4, "TIT2", text(2, Bytes{0xD8, 0x3C, 0xDF, 0xB5, 0x00, 'x'}));
    // Encoding 1 with a big-endian BOM.
    put_frame(frames, 4, "TPE1", text(1, Bytes{0xFE, 0xFF, 0x66, 0x74}));
    const TrackTags t = media::parse_id3v2(tag(4, 0, frames));
    EXPECT_EQ(t.title, "\xF0\x9F\x8E\xB5x");
    EXPECT_EQ(t.artist, "晴");
}

TEST(MediaTags, FirstOfSeveralValuesAndTrailingNulAreDropped) {
    Bytes frames;
    put_frame(frames, 4, "TPE1", text(3, Bytes{'A', 0, 'B', 0}));
    const TrackTags t = media::parse_id3v2(tag(4, 0, frames));
    EXPECT_EQ(t.artist, "A");
}

TEST(MediaTags, Id3FrameSizePastTheEndIsIgnored) {
    Bytes frames;
    put_frame(frames, 4, "TIT2", text(3, "Kept"));
    put_frame(frames, 4, "TPE1", text(3, "Lost"), 0, 100000);
    const Bytes t4 = tag(4, 0, frames);
    TrackTags t = media::parse_id3v2(t4);
    EXPECT_EQ(t.title, "Kept");
    EXPECT_TRUE(t.artist.empty());

    // The same with plain v2.3 sizes, and with the buffer cut short of the
    // size the header declares (the library reads a bounded head).
    Bytes frames3;
    put_frame(frames3, 3, "TIT2", text(3, "Kept"));
    put_frame(frames3, 3, "TALB", text(3, "Lost"), 0, 0xFFFFFFFF);
    t = media::parse_id3v2(tag(3, 0, frames3));
    EXPECT_EQ(t.title, "Kept");
    EXPECT_TRUE(t.album.empty());

    Bytes big;
    put_frame(big, 4, "TIT2", text(3, "Kept"));
    put_frame(big, 4, "TALB", text(3, std::string(500, 'a')));
    const Bytes full = tag(4, 0, big);
    t = media::parse_id3v2(std::span<const uint8_t>(full).first(60));
    EXPECT_EQ(t.title, "Kept");
    EXPECT_TRUE(t.album.empty());

    // Every truncation of a valid tag parses without reading past the span.
    for (size_t n = 0; n <= full.size(); ++n) {
        (void)media::parse_id3v2(std::span<const uint8_t>(full).first(n));
    }
}

TEST(MediaTags, ZeroLengthAndUnknownFramesAreSkipped) {
    Bytes frames;
    put_frame(frames, 4, "TIT2", Bytes{});
    put_frame(frames, 4, "TXXX", text(3, "other"));
    put_frame(frames, 4, "TPE1", text(3, "Artist"));
    put_frame(frames, 4, "TALB", Bytes{3});
    const TrackTags t = media::parse_id3v2(tag(4, 0, frames));
    EXPECT_TRUE(t.title.empty());
    EXPECT_EQ(t.artist, "Artist");
    EXPECT_TRUE(t.album.empty());
}

TEST(MediaTags, PaddingEndsTheFrames) {
    Bytes frames;
    put_frame(frames, 4, "TIT2", text(3, "T"));
    frames.insert(frames.end(), 64, 0);
    const TrackTags t = media::parse_id3v2(tag(4, 0, frames));
    EXPECT_EQ(t.title, "T");
}

TEST(MediaTags, V24SyncsafeFrameSizeOver127) {
    Bytes frames;
    const std::string long_title(200, 'x');
    put_frame(frames, 4, "TIT2", text(3, long_title));
    put_frame(frames, 4, "TPE1", text(3, "After"));
    const TrackTags t = media::parse_id3v2(tag(4, 0, frames));
    EXPECT_EQ(t.title, long_title);
    EXPECT_EQ(t.artist, "After");
}

TEST(MediaTags, V23PlainFrameSizeOver127) {
    Bytes frames;
    const std::string long_title(200, 'y');
    put_frame(frames, 3, "TIT2", text(3, long_title));
    put_frame(frames, 3, "TPE1", text(3, "After"));
    const TrackTags t = media::parse_id3v2(tag(3, 0, frames));
    EXPECT_EQ(t.title, long_title);
    EXPECT_EQ(t.artist, "After");
}

TEST(MediaTags, Id3UnsynchronisedTagIsRead) {
    // v2.3: the whole tag is unsynchronised and frame sizes count the
    // bytes after resynchronisation.
    Bytes frames;
    put_frame(frames, 3, "TIT2", text(1, utf16le_bom(u"ÿA")));
    put_frame(frames, 3, "TPE1", text(1, utf16le_bom(u"Wisp")));
    const Bytes body = unsync(frames);
    ASSERT_NE(body.size(), frames.size());
    TrackTags t = media::parse_id3v2(tag(3, 0x80, body));
    EXPECT_EQ(t.title,
              "\xC3\xBF"
              "A");
    EXPECT_EQ(t.artist, "Wisp");

    // v2.4: unsynchronisation is per frame (format flag 0x02) and the frame
    // size counts the stored bytes.
    Bytes frames4;
    put_frame(frames4, 4, "TIT2", unsync(text(1, utf16le_bom(u"ÿA"))), 0x02);
    put_frame(frames4, 4, "TPE1", text(1, utf16le_bom(u"Wisp")));
    t = media::parse_id3v2(tag(4, 0, frames4));
    EXPECT_EQ(t.title,
              "\xC3\xBF"
              "A");
    EXPECT_EQ(t.artist, "Wisp");

    // v2.4 with the tag-level flag: every frame is unsynchronised.
    Bytes frames4all;
    put_frame(frames4all, 4, "TIT2", unsync(text(1, utf16le_bom(u"ÿA"))));
    t = media::parse_id3v2(tag(4, 0x80, frames4all));
    EXPECT_EQ(t.title,
              "\xC3\xBF"
              "A");
}

TEST(MediaTags, ExtendedHeaderIsSkipped) {
    Bytes frames3;
    // v2.3 extended header: size (6, excluding itself), flags, padding size.
    put_be32(frames3, 6);
    frames3.insert(frames3.end(), {0, 0, 0, 0, 0, 0});
    put_frame(frames3, 3, "TIT2", text(3, "Ext3"));
    EXPECT_EQ(media::parse_id3v2(tag(3, 0x40, frames3)).title, "Ext3");

    Bytes frames4;
    // v2.4 extended header: syncsafe size including itself, 1 flag byte,
    // flags.
    put_syncsafe(frames4, 6);
    frames4.insert(frames4.end(), {1, 0});
    put_frame(frames4, 4, "TIT2", text(3, "Ext4"));
    EXPECT_EQ(media::parse_id3v2(tag(4, 0x40, frames4)).title, "Ext4");

    Bytes huge;
    put_syncsafe(huge, 0x0FFFFFFF);
    huge.insert(huge.end(), {1, 0});
    EXPECT_TRUE(media::parse_id3v2(tag(4, 0x40, huge)).title.empty());
}

TEST(MediaTags, V24GroupingAndDataLengthAreSkipped) {
    Bytes frames;
    // Grouping id byte, then a 4-byte data length indicator.
    Bytes body{0x07};
    put_syncsafe(body, 5);
    const Bytes t = text(3, "Grp1");
    body.insert(body.end(), t.begin(), t.end());
    put_frame(frames, 4, "TIT2", body, 0x41);
    // Compressed frames are not read.
    put_frame(frames, 4, "TPE1", text(3, "zzzz"), 0x08);
    const TrackTags tags = media::parse_id3v2(tag(4, 0, frames));
    EXPECT_EQ(tags.title, "Grp1");
    EXPECT_TRUE(tags.artist.empty());
}

TEST(MediaTags, OversizedTextIsCutOnACodePoint) {
    Bytes frames;
    std::string s;
    for (int i = 0; i < 2000; ++i) s += "晴";
    put_frame(frames, 4, "TIT2", text(3, s));
    const TrackTags t = media::parse_id3v2(tag(4, 0, frames));
    ASSERT_FALSE(t.title.empty());
    EXPECT_LE(t.title.size(), media::kMaxTagFieldBytes);
    EXPECT_EQ(t.title.size() % 3, 0u);
    EXPECT_EQ(t.title.substr(0, 3), "晴");
}

TEST(MediaTags, XingDuration) {
    auto r = mp3_info_of_file("sine_44k_stereo_cbr128.mp3");
    ASSERT_TRUE(r) << r.error().message;
    const media::Mp3Info& i = r.value();
    EXPECT_EQ(i.sample_rate, 44100u);
    EXPECT_EQ(i.channels, 2u);
    EXPECT_NEAR(static_cast<double>(i.duration_ms), 3000.0, 50.0);
    EXPECT_EQ(i.audio_offset, 0x61u);
    EXPECT_GT(i.audio_bytes, 0u);
    EXPECT_LE(i.audio_offset + i.audio_bytes, read_file("sine_44k_stereo_cbr128.mp3").size());
}

TEST(MediaTags, VbrTocIsKept) {
    auto r = mp3_info_of_file("sine_48k_mono_vbr.mp3");
    ASSERT_TRUE(r) << r.error().message;
    const media::Mp3Info& i = r.value();
    EXPECT_EQ(i.sample_rate, 48000u);
    EXPECT_EQ(i.channels, 1u);
    EXPECT_TRUE(i.has_toc);
    EXPECT_EQ(i.toc[0], 0u);
    EXPECT_EQ(i.toc[1], 0x13u);
    EXPECT_EQ(i.toc[98], 0xFDu);
    EXPECT_EQ(i.toc[99], 0xFFu);
    EXPECT_NEAR(static_cast<double>(i.duration_ms), 2000.0, 100.0);
    EXPECT_EQ(i.audio_offset, 0xA2u);
    EXPECT_EQ(i.audio_bytes, 0x23E8u);  // the Xing byte count
}

TEST(MediaTags, CbrEstimateWithoutXing) {
    auto r = mp3_info_of_file("sine_22k_noxing.mp3");
    ASSERT_TRUE(r) << r.error().message;
    const media::Mp3Info& i = r.value();
    EXPECT_EQ(i.sample_rate, 22050u);
    EXPECT_EQ(i.channels, 1u);
    EXPECT_FALSE(i.has_toc);
    EXPECT_NEAR(static_cast<double>(i.duration_ms), 2000.0, 150.0);
    EXPECT_EQ(i.audio_offset, 0u);
}

TEST(MediaTags, VbriDuration) {
    // MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, stereo: 417-byte frames.
    const Bytes hdr{0xFF, 0xFB, 0x90, 0x00};
    Bytes b;
    for (int f = 0; f < 3; ++f) {
        Bytes frame(417, 0);
        std::copy(hdr.begin(), hdr.end(), frame.begin());
        b.insert(b.end(), frame.begin(), frame.end());
    }
    Bytes vbri;
    put_str(vbri, "VBRI");
    vbri.insert(vbri.end(), {0, 1, 0, 0, 0, 50});  // version, delay, quality
    put_be32(vbri, 5000);                          // bytes
    put_be32(vbri, 100);                           // frames
    std::copy(vbri.begin(), vbri.end(), b.begin() + 36);
    auto r = media::parse_mp3_info(b, 1000000, 0);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().duration_ms, 100u * 1152u * 1000u / 44100u);
    EXPECT_EQ(r.value().audio_bytes, 5000u);
    EXPECT_FALSE(r.value().has_toc);
}

TEST(MediaTags, GarbageIsNotMp3Info) {
    const Bytes g = read_file("garbage.mp3");
    auto r = media::parse_mp3_info(g, g.size(), 0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Corrupt);
    EXPECT_FALSE(media::parse_mp3_info(std::span<const uint8_t>(), 0, 0));
    const Bytes lone{0xFF, 0xFB, 0x90};
    EXPECT_FALSE(media::parse_mp3_info(lone, lone.size(), 0));
}

TEST(MediaTags, TruncatedXingFrameDoesNotReadPastTheSpan) {
    const Bytes data = read_file("sine_48k_mono_vbr.mp3");
    const size_t skip = media::id3v2_size(data);
    for (size_t n = 0; n < 200; ++n) {
        (void)media::parse_mp3_info(std::span<const uint8_t>(data).subspan(skip, n), data.size(),
                                    skip);
    }
}

TEST(MediaTags, WavHeader) {
    struct Case {
        const char* name;
        uint32_t rate;
        uint8_t channels;
        uint32_t ms;
    };
    for (const Case& c :
         {Case{"sine_44k_stereo.wav", 44100, 2, 250}, Case{"sine_16k_mono.wav", 16000, 1, 1000}}) {
        const Bytes data = read_file(c.name);
        auto r = media::parse_wav(data, data.size());
        ASSERT_TRUE(r) << c.name << ": " << r.error().message;
        EXPECT_EQ(r.value().sample_rate, c.rate) << c.name;
        EXPECT_EQ(r.value().channels, c.channels) << c.name;
        EXPECT_EQ(r.value().duration_ms, c.ms) << c.name;
        EXPECT_EQ(r.value().data_offset + r.value().data_bytes, data.size()) << c.name;
    }
}

TEST(MediaTags, WavFloatIsUnsupported) {
    const Bytes data = read_file("float32.wav");
    auto r = media::parse_wav(data, data.size());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);

    for (uint16_t bits : {8, 24, 32}) {
        const Bytes h = wav_header(1, 1, 8000, bits, 0);
        auto rb = media::parse_wav(h, h.size());
        ASSERT_FALSE(rb);
        EXPECT_EQ(rb.error().code, ErrorCode::Unsupported) << bits;
    }
    const Bytes six = wav_header(1, 6, 8000, 16, 0);
    EXPECT_EQ(media::parse_wav(six, six.size()).error().code, ErrorCode::Unsupported);
}

TEST(MediaTags, WavDataChunkLongerThanFileIsClamped) {
    Bytes b = wav_header(1, 1, 16000, 16, 1000000);
    const uint64_t file_size = b.size() + 3201;  // odd: a half frame at the end
    auto r = media::parse_wav(b, file_size);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().data_offset, 44u);
    EXPECT_EQ(r.value().data_bytes, 3200u);
    EXPECT_EQ(r.value().duration_ms, 100u);
}

TEST(MediaTags, WavHostileHeaders) {
    const Bytes good = wav_header(1, 2, 44100, 16, 400);
    for (size_t n = 0; n < good.size(); ++n) {
        auto r = media::parse_wav(std::span<const uint8_t>(good).first(n), 1000);
        EXPECT_FALSE(r) << n;
    }
    Bytes not_riff = good;
    not_riff[0] = 'X';
    EXPECT_EQ(media::parse_wav(not_riff, 1000).error().code, ErrorCode::Corrupt);

    // A chunk whose size runs past the span before any data chunk.
    Bytes runaway;
    put_str(runaway, "RIFF");
    put_le32(runaway, 100);
    put_str(runaway, "WAVE");
    put_str(runaway, "LIST");
    put_le32(runaway, 0xFFFFFFF0);
    EXPECT_FALSE(media::parse_wav(runaway, 1u << 30));

    // A fmt chunk too short to hold the PCM fields.
    Bytes short_fmt;
    put_str(short_fmt, "RIFF");
    put_le32(short_fmt, 100);
    put_str(short_fmt, "WAVEfmt ");
    put_le32(short_fmt, 4);
    put_le16(short_fmt, 1);
    put_le16(short_fmt, 1);
    put_str(short_fmt, "data");
    put_le32(short_fmt, 0);
    EXPECT_FALSE(media::parse_wav(short_fmt, short_fmt.size()));

    // Zero sample rate.
    const Bytes zero_rate = wav_header(1, 1, 0, 16, 0);
    EXPECT_FALSE(media::parse_wav(zero_rate, zero_rate.size()));
}

TEST(MediaTags, WavExtensiblePcmIsAccepted) {
    Bytes b;
    put_str(b, "RIFF");
    put_le32(b, 0);
    put_str(b, "WAVEfmt ");
    put_le32(b, 40);
    put_le16(b, 0xFFFE);
    put_le16(b, 2);
    put_le32(b, 48000);
    put_le32(b, 48000 * 4);
    put_le16(b, 4);
    put_le16(b, 16);
    put_le16(b, 22);  // cbSize
    put_le16(b, 16);  // valid bits
    put_le32(b, 3);   // channel mask
    // KSDATAFORMAT_SUBTYPE_PCM
    b.insert(b.end(), {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00,
                       0x38, 0x9B, 0x71});
    put_str(b, "data");
    put_le32(b, 192000);
    auto r = media::parse_wav(b, b.size() + 192000);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().duration_ms, 1000u);

    b[b.size() - 8 - 16] = 0x03;  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    EXPECT_EQ(media::parse_wav(b, b.size() + 192000).error().code, ErrorCode::Unsupported);
}

TEST(MediaTags, TitleFromFileName) {
    EXPECT_EQ(media::title_from_file_name("music/a/晴天.mp3"), "晴天");
    EXPECT_EQ(media::title_from_file_name("song.final.WAV"), "song.final");
    EXPECT_EQ(media::title_from_file_name("music/noext"), "noext");
    EXPECT_EQ(media::title_from_file_name("music/.hidden"), ".hidden");
}
