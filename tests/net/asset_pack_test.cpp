#include "wisp/net/asset_pack.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/sha256.hpp"

namespace {

std::vector<uint8_t> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Hand-assembles a WispPack so the corrupt-input tests can aim at one
// specific field; see tools/pack_assets.py for the format.
struct PackBuilder {
    struct Rec {
        std::string path;
        std::string mime = "text/plain";
        uint8_t flags = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };
    std::vector<Rec> recs;
    uint16_t version = 1;
    size_t slack = 0;  // zero bytes appended after the laid-out data

    static void put16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(v & 0xFF);
        b.push_back(v >> 8);
    }
    static void put32(std::vector<uint8_t>& b, uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
    }
    static void set32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + i] = (v >> (8 * i)) & 0xFF;
    }

    size_t table_bytes() const {
        size_t n = 0;
        for (const auto& r : recs) n += 2 + r.path.size() + 1 + r.mime.size() + 1 + 4 + 4 + 32;
        return n;
    }

    // Lays out `payload` once per record (aligned) unless the record
    // already carries an explicit offset/length.
    std::vector<uint8_t> build(std::string_view payload = "hello") {
        std::vector<uint8_t> b{'W', 'P', 'A', 'K'};
        put16(b, version);
        put16(b, static_cast<uint16_t>(recs.size()));
        put32(b, static_cast<uint32_t>(table_bytes()));
        put32(b, 0);  // total_bytes, patched below
        size_t data_at = (16 + table_bytes() + 3) & ~size_t{3};
        std::vector<bool> laid_out;
        for (auto& r : recs) {
            laid_out.push_back(r.length == 0 && r.offset == 0);
            if (laid_out.back()) {
                r.offset = static_cast<uint32_t>(data_at);
                r.length = static_cast<uint32_t>(payload.size());
                data_at = (data_at + payload.size() + 3) & ~size_t{3};
            }
            put16(b, static_cast<uint16_t>(r.path.size()));
            b.insert(b.end(), r.path.begin(), r.path.end());
            b.push_back(static_cast<uint8_t>(r.mime.size()));
            b.insert(b.end(), r.mime.begin(), r.mime.end());
            b.push_back(r.flags);
            put32(b, r.offset);
            put32(b, r.length);
            b.insert(b.end(), 32, 0);
        }
        b.resize(data_at + slack, 0);
        for (size_t i = 0; i < recs.size(); ++i) {
            if (laid_out[i]) {
                std::memcpy(b.data() + recs[i].offset, payload.data(), payload.size());
            }
        }
        set32(b, 12, static_cast<uint32_t>(b.size()));
        return b;
    }
};

}  // namespace

TEST(AssetPack, ParsesFixtureAndMapsRoot) {
    // AssetPack only borrows the bytes, so they must outlive it.
    auto bytes = read_all(WISP_FIXTURE_WPAK);
    auto parsed = wisp::net::AssetPack::parse(bytes);
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto& pack = parsed.value();
    EXPECT_EQ(pack.size(), 4u);
    const auto* index = pack.find("/");
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->path, "/index.html");
    EXPECT_TRUE(index->gzip);
    EXPECT_EQ(index->mime, "text/html; charset=utf-8");
    ASSERT_NE(pack.find("/logo.png"), nullptr);
    EXPECT_FALSE(pack.find("/logo.png")->gzip);
    EXPECT_EQ(pack.find("/app/main.js")->mime, "text/javascript; charset=utf-8");
    EXPECT_EQ(pack.find("/missing"), nullptr);
    EXPECT_EQ(pack.find(""), nullptr);
}

TEST(AssetPack, UncompressedDataAndEtagMatchSource) {
    auto bytes = read_all(WISP_FIXTURE_WPAK);
    auto pack = wisp::net::AssetPack::parse(bytes).value();
    const auto* logo = pack.find("/logo.png");
    ASSERT_NE(logo, nullptr);
    auto source = read_all(std::string(WISP_FIXTURE_WEB_DIR) + "/logo.png");
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(std::vector<uint8_t>(logo->data.begin(), logo->data.end()), source);

    auto digest = wisp::sha256(logo->data);
    EXPECT_EQ(logo->etag, "\"" + wisp::to_hex({digest.data(), 8}) + "\"");
    EXPECT_EQ(logo->etag.size(), 18u);

    // gzip member magic: the browser inflates these itself.
    const auto* js = pack.find("/app/util.js");
    ASSERT_NE(js, nullptr);
    ASSERT_GE(js->data.size(), 2u);
    EXPECT_EQ(js->data[0], 0x1F);
    EXPECT_EQ(js->data[1], 0x8B);
}

TEST(AssetPack, RejectsTruncatedAndCorrupt) {
    auto bytes = read_all(WISP_FIXTURE_WPAK);
    ASSERT_GT(bytes.size(), 20u);
    for (size_t n : {0u, 8u, 20u, static_cast<unsigned>(bytes.size() - 1)}) {
        EXPECT_FALSE(wisp::net::AssetPack::parse({bytes.data(), n})) << n;
    }
    bytes[0] = 'X';
    EXPECT_FALSE(wisp::net::AssetPack::parse(bytes));
}

TEST(AssetPack, AcceptsTrailingBytesBeyondTotal) {
    // A memory-mapped partition is larger than the pack it holds.
    auto bytes = read_all(WISP_FIXTURE_WPAK);
    bytes.resize(bytes.size() + 4096, 0xFF);
    auto parsed = wisp::net::AssetPack::parse(bytes);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().size(), 4u);
}

TEST(AssetPack, HandBuiltPackParses) {
    PackBuilder pb;
    pb.recs.push_back({.path = "/a.txt"});
    pb.recs.push_back({.path = "/b.txt", .mime = "text/css", .flags = 1});
    auto bytes = pb.build();
    auto parsed = wisp::net::AssetPack::parse(bytes);
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto* b = parsed.value().find("/b.txt");
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(b->gzip);
    EXPECT_EQ(std::string(b->data.begin(), b->data.end()), "hello");
}

TEST(AssetPack, RejectsOutOfRangeFields) {
    auto expect_reject = [](PackBuilder pb, const char* what,
                            void (*mutate)(std::vector<uint8_t>&) = nullptr) {
        auto bytes = pb.build();
        if (mutate) mutate(bytes);
        EXPECT_FALSE(wisp::net::AssetPack::parse(bytes)) << what;
    };

    PackBuilder base;
    base.recs.push_back({.path = "/a.txt"});

    {
        PackBuilder pb = base;
        pb.version = 2;
        expect_reject(pb, "unknown version");
    }
    {
        PackBuilder pb;
        pb.recs.push_back({.path = "/a.txt", .offset = 0x7FFFFFF0, .length = 4});
        expect_reject(pb, "offset past end");
    }
    {
        PackBuilder pb;
        pb.recs.push_back({.path = "/a.txt", .offset = 76, .length = 0xFFFFFFF0});
        expect_reject(pb, "length overflows");
    }
    {
        PackBuilder pb;
        pb.recs.push_back({.path = "/a.txt", .offset = 4, .length = 4});
        expect_reject(pb, "data overlaps header/table");
    }
    {
        PackBuilder pb;
        pb.slack = 16;  // the table ends at 76, so [77, 78) is in range
        pb.recs.push_back({.path = "/a.txt", .offset = 77, .length = 1});
        expect_reject(pb, "misaligned offset");
    }
    {
        PackBuilder pb;
        pb.recs.push_back({.path = "a.txt"});
        expect_reject(pb, "path without leading slash");
    }
    {
        PackBuilder pb;
        pb.recs.push_back({.path = "/a.txt"});
        pb.recs.push_back({.path = "/a.txt"});
        expect_reject(pb, "duplicate path");
    }
    {
        // Data at 80 stays clear of a table that claims 64 bytes (16 + 64),
        // so only the unaccounted 4 bytes after the one record are wrong.
        PackBuilder pb;
        pb.slack = 16;
        pb.recs.push_back({.path = "/a.txt", .offset = 80, .length = 4});
        expect_reject(pb, "table_bytes longer than records",
                      [](std::vector<uint8_t>& b) { PackBuilder::set32(b, 8, 64); });
    }
    expect_reject(base, "count larger than table", [](std::vector<uint8_t>& b) { b[6] = 2; });
    expect_reject(base, "table_bytes past total",
                  [](std::vector<uint8_t>& b) { PackBuilder::set32(b, 8, 0xFFFFFF00); });
    expect_reject(base, "table_bytes shorter than records",
                  [](std::vector<uint8_t>& b) { PackBuilder::set32(b, 8, 10); });
    expect_reject(base, "path_len runs past table", [](std::vector<uint8_t>& b) { b[16] = 0xFF; });
    expect_reject(base, "total_bytes below header",
                  [](std::vector<uint8_t>& b) { PackBuilder::set32(b, 12, 8); });
}

TEST(AssetPack, EmptyPackIsValid) {
    PackBuilder pb;
    auto bytes = pb.build();
    auto parsed = wisp::net::AssetPack::parse(bytes);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().size(), 0u);
    EXPECT_EQ(parsed.value().find("/"), nullptr);
}
