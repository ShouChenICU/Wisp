#include "wisp/core/zip.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/crc32.hpp"

// The archives come from tests/core/fixtures/make_zips.py, run at build
// time into WISP_FIXTURE_ZIP_DIR.

namespace {

std::vector<uint8_t> load(const std::string& name) {
    std::ifstream f(std::string(WISP_FIXTURE_ZIP_DIR) + "/" + name, std::ios::binary);
    EXPECT_TRUE(f.good()) << name;
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::span<const uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string text(const wisp::os::LargeBuffer& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

const wisp::core::ZipEntry* find(const wisp::core::ZipReader& z, std::string_view path) {
    for (const auto& e : z.entries()) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

}  // namespace

TEST(Zip, ReadsStoredAndDeflatedEntries) {
    const auto blob = load("ok.zip");
    auto opened = wisp::core::ZipReader::open(blob);
    ASSERT_TRUE(opened.is_ok()) << opened.error().message;
    const auto& z = opened.value();
    ASSERT_EQ(z.entries().size(), 5u);

    const auto* dir = find(z, "assets/");
    ASSERT_NE(dir, nullptr);
    EXPECT_TRUE(dir->is_dir);

    const auto* manifest = find(z, "manifest.json");
    ASSERT_NE(manifest, nullptr);
    EXPECT_EQ(manifest->method, 0);
    auto m = z.read(*manifest);
    ASSERT_TRUE(m.is_ok()) << m.error().message;
    EXPECT_EQ(text(m.value()), R"({"id":"demo"})");

    std::string expected;
    for (int i = 0; i < 400; ++i) {
        expected += "line " + std::to_string(i) + " of the scene source\n";
    }
    const auto* main = find(z, "main.lua");
    ASSERT_NE(main, nullptr);
    EXPECT_EQ(main->method, 8);
    EXPECT_FALSE(main->is_dir);
    EXPECT_EQ(main->size, expected.size());
    EXPECT_LT(main->comp_size, main->size);
    EXPECT_EQ(main->crc32, wisp::crc32(bytes(expected)));
    auto body = z.read(*main);
    ASSERT_TRUE(body.is_ok()) << body.error().message;
    EXPECT_EQ(text(body.value()), expected);

    const auto* empty = find(z, "assets/empty.txt");
    ASSERT_NE(empty, nullptr);
    auto e = z.read(*empty);
    ASSERT_TRUE(e.is_ok()) << e.error().message;
    EXPECT_EQ(e.value().size(), 0u);

    const auto* cjk = find(z, "场景/说明.txt");
    ASSERT_NE(cjk, nullptr);
    auto c = z.read(*cjk);
    ASSERT_TRUE(c.is_ok()) << c.error().message;
    EXPECT_EQ(text(c.value()), "你好");
}

TEST(Zip, RejectsUnsafeEntries) {
    struct Case {
        const char* file;
        std::string path;  // as the error message renders it
    };
    const Case cases[] = {
        {"dotdot.zip", "../x"},
        {"abs.zip", "/abs"},
        {"backslash.zip", "a\\b"},
        {"dot.zip", "a/./b"},
        {"empty_seg.zip", "a//b"},
        {"drive.zip", "C:x"},
        {"symlink.zip", "link"},
        {"encrypted.zip", "secret.txt"},
        {"method12.zip", "bz.txt"},
        {"duplicate.zip", "dup.txt"},
        {"long.zip", std::string(129, 'p')},
        // Ill-formed bytes are escaped so the message itself stays UTF-8.
        {"bad_utf8.zip", "bad\\xff\\xfe.txt"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.file);
        const auto blob = load(c.file);
        auto r = wisp::core::ZipReader::open(blob);
        ASSERT_FALSE(r.is_ok());
        EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
        EXPECT_NE(r.error().message.find(c.path), std::string::npos) << r.error().message;
    }
}

TEST(Zip, RejectsMalformedArchives) {
    std::vector<uint8_t> blob = load("ok.zip");
    for (size_t n : {size_t{0}, size_t{4}, size_t{21}}) {
        auto r = wisp::core::ZipReader::open(std::span(blob.data(), n));
        ASSERT_FALSE(r.is_ok());
        EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
    }
    // Every truncation of a valid archive is either rejected cleanly by
    // open() or, if the central directory survives, by read().
    for (size_t n = 0; n < blob.size(); ++n) {
        auto r = wisp::core::ZipReader::open(std::span(blob.data(), n));
        if (r.is_ok()) {
            for (const auto& e : r.value().entries()) {
                (void)r.value().read(e);
            }
        }
    }
    // Flipping any single byte never crashes either.
    for (size_t i = 0; i < blob.size(); ++i) {
        std::vector<uint8_t> copy = blob;
        copy[i] ^= 0xA5;
        auto r = wisp::core::ZipReader::open(copy);
        if (r.is_ok()) {
            for (const auto& e : r.value().entries()) {
                (void)r.value().read(e);
            }
        }
    }
}

TEST(Zip, RejectsOversizeBeforeInflating) {
    const auto big = load("oversize.zip");
    auto r = wisp::core::ZipReader::open(big);
    ASSERT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().code, wisp::ErrorCode::QuotaExceeded);
    // A larger limit accepts the same archive.
    wisp::core::ZipLimits roomy;
    roomy.max_total = 1024 * 1024;
    EXPECT_TRUE(wisp::core::ZipReader::open(big, roomy).is_ok());

    const auto many = load("many.zip");
    auto m = wisp::core::ZipReader::open(many);
    ASSERT_FALSE(m.is_ok());
    EXPECT_EQ(m.error().code, wisp::ErrorCode::InvalidArgument);
    wisp::core::ZipLimits more;
    more.max_entries = 257;
    EXPECT_TRUE(wisp::core::ZipReader::open(many, more).is_ok());
}

TEST(Zip, CrcMismatchRejected) {
    const auto blob = load("bad_crc.zip");
    auto opened = wisp::core::ZipReader::open(blob);
    ASSERT_TRUE(opened.is_ok()) << opened.error().message;
    ASSERT_EQ(opened.value().entries().size(), 2u);
    for (const auto& e : opened.value().entries()) {
        SCOPED_TRACE(e.path);
        auto r = opened.value().read(e);
        ASSERT_FALSE(r.is_ok());
        EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
        EXPECT_NE(r.error().message.find(e.path), std::string::npos) << r.error().message;
    }
}

TEST(ZipWriter, RoundTripsThroughReader) {
    std::string large(100000, '\0');
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<char>(i * 31 + 7);
    }
    wisp::core::ZipWriter w;
    w.add("manifest.json", bytes(R"({"id":"x"})"));
    w.add("data/", {});
    w.add("data/blob.bin", bytes(large));
    w.add("empty.txt", {});
    wisp::os::LargeBuffer zip = w.finish();
    ASSERT_TRUE(zip.ok());

    auto opened = wisp::core::ZipReader::open(zip.span());
    ASSERT_TRUE(opened.is_ok()) << opened.error().message;
    const auto& z = opened.value();
    ASSERT_EQ(z.entries().size(), 4u);
    EXPECT_EQ(z.entries()[0].path, "manifest.json");
    EXPECT_TRUE(z.entries()[1].is_dir);

    auto m = z.read(z.entries()[0]);
    ASSERT_TRUE(m.is_ok());
    EXPECT_EQ(text(m.value()), R"({"id":"x"})");
    auto b = z.read(z.entries()[2]);
    ASSERT_TRUE(b.is_ok()) << b.error().message;
    EXPECT_EQ(text(b.value()), large);
    auto e = z.read(z.entries()[3]);
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.value().size(), 0u);

    // The writer is reusable after finish().
    w.add("again.txt", bytes("again"));
    wisp::os::LargeBuffer second = w.finish();
    auto reopened = wisp::core::ZipReader::open(second.span());
    ASSERT_TRUE(reopened.is_ok()) << reopened.error().message;
    ASSERT_EQ(reopened.value().entries().size(), 1u);
}
