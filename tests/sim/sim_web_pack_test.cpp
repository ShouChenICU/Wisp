#include "wisp/port/sim/sim_web_pack.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "support/temp_dir.hpp"
#include "wisp/net/asset_pack.hpp"

using wisp::net::AssetPack;
using wisp::port::sim::pack_web_dir;

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string gunzip(std::span<const uint8_t> data) {
    z_stream zs{};
    EXPECT_EQ(inflateInit2(&zs, 16 + MAX_WBITS), Z_OK);
    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = static_cast<uInt>(data.size());
    std::string out;
    char buf[4096];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    }
    EXPECT_EQ(rc, Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

}  // namespace

TEST(SimWebPack, PacksADirectoryWithThePackerRules) {
    const std::filesystem::path dir = WISP_FIXTURE_WEB_DIR;
    auto bytes = pack_web_dir(dir.string());
    ASSERT_TRUE(bytes) << bytes.error().message;
    auto pack = AssetPack::parse(bytes.value());
    ASSERT_TRUE(pack) << pack.error().message;
    EXPECT_EQ(pack.value().size(), 4u);

    const auto* index = pack.value().find("/");
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->path, "/index.html");
    EXPECT_EQ(index->mime, "text/html; charset=utf-8");
    EXPECT_TRUE(index->gzip);
    EXPECT_EQ(gunzip(index->data), read_file(dir / "index.html"));

    const auto* js = pack.value().find("/app/main.js");
    ASSERT_NE(js, nullptr);
    EXPECT_EQ(js->mime, "text/javascript; charset=utf-8");
    EXPECT_TRUE(js->gzip);
    EXPECT_EQ(gunzip(js->data), read_file(dir / "app" / "main.js"));

    const auto* png = pack.value().find("/logo.png");
    ASSERT_NE(png, nullptr);
    EXPECT_EQ(png->mime, "image/png");
    EXPECT_FALSE(png->gzip);
    EXPECT_EQ(std::string(png->data.begin(), png->data.end()), read_file(dir / "logo.png"));
}

TEST(SimWebPack, EmptyDirectoryGivesAnEmptyPack) {
    auto dir = wisp::test::temp_dir();
    auto bytes = pack_web_dir(dir.string());
    ASSERT_TRUE(bytes);
    auto pack = AssetPack::parse(bytes.value());
    ASSERT_TRUE(pack);
    EXPECT_EQ(pack.value().size(), 0u);
}

TEST(SimWebPack, MissingDirectoryIsNotFound) {
    auto bytes = pack_web_dir((wisp::test::temp_dir() / "nope").string());
    ASSERT_FALSE(bytes);
    EXPECT_EQ(bytes.error().code, wisp::ErrorCode::NotFound);
}
