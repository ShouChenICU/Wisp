#include "wisp/port/sim/png_writer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "wisp/base/error.hpp"

namespace wisp::port::sim {

namespace {

const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    const auto& table = crc_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1;
    uint32_t b = 0;
    constexpr uint32_t kMod = 65521;
    constexpr size_t kNMax = 5552;  // largest chunk that can't overflow b before the mod below
    size_t i = 0;
    while (i < len) {
        size_t chunk = std::min(len - i, kNMax);
        for (size_t j = 0; j < chunk; ++j, ++i) {
            a += data[i];
            b += a;
        }
        a %= kMod;
        b %= kMod;
    }
    return (b << 16) | a;
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void put_le16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// Appends one PNG chunk (length + type + data + CRC-32-over-type-and-data)
// to `out`.
void write_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put_be32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> type_and_data(type, type + 4);
    type_and_data.insert(type_and_data.end(), data.begin(), data.end());
    out.insert(out.end(), type_and_data.begin(), type_and_data.end());
    put_be32(out, crc32(type_and_data.data(), type_and_data.size()));
}

// zlib-wraps `raw` as a sequence of uncompressed ("stored") deflate
// blocks, each at most 65535 bytes (deflate's own stored-block length
// limit -- see RFC 1951 section 3.2.4), followed by the Adler-32
// trailer zlib requires (RFC 1950).
std::vector<uint8_t> zlib_stored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    out.push_back(0x78);  // CMF: deflate, 32K window
    out.push_back(0x01);  // FLG: fastest/no compression, (CMF<<8|FLG) % 31 == 0

    constexpr size_t kMaxBlock = 65535;
    size_t pos = 0;
    if (raw.empty()) {
        // A single, final, zero-length stored block for an empty image.
        out.push_back(0x01);
        put_le16(out, 0);
        put_le16(out, 0xFFFF);
    }
    while (pos < raw.size()) {
        size_t remaining = raw.size() - pos;
        size_t block_len = std::min(remaining, kMaxBlock);
        bool is_final = (pos + block_len) >= raw.size();
        out.push_back(is_final ? 0x01 : 0x00);
        put_le16(out, static_cast<uint16_t>(block_len));
        put_le16(out, static_cast<uint16_t>(~static_cast<uint16_t>(block_len)));
        out.insert(out.end(), raw.begin() + static_cast<ptrdiff_t>(pos),
                   raw.begin() + static_cast<ptrdiff_t>(pos + block_len));
        pos += block_len;
    }

    put_be32(out, adler32(raw.data(), raw.size()));
    return out;
}

}  // namespace

Result<void> write_png_rgb888(const std::string& path, const uint8_t* rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> file;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    file.insert(file.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // color type: RGB (truecolor, no alpha)
    ihdr.push_back(0);  // compression method: deflate (the only one PNG defines)
    ihdr.push_back(0);  // filter method: adaptive (only "none" is actually used per-row below)
    ihdr.push_back(0);  // interlace method: none
    write_chunk(file, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter type 0: none
        const uint8_t* row = rgb + static_cast<size_t>(y) * w * 3;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 3);
    }
    write_chunk(file, "IDAT", zlib_stored(raw));

    write_chunk(file, "IEND", {});

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return Result<void>::err(ErrorCode::IoError, "failed to open " + path);
    }
    size_t written = std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    if (written != file.size()) {
        return Result<void>::err(ErrorCode::IoError, "failed to write " + path);
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::sim
