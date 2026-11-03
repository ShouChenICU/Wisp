#include "wisp/net/asset_pack.hpp"

#include <algorithm>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/base/sha256.hpp"  // to_hex

namespace wisp::net {

namespace {

constexpr size_t kHeaderBytes = 16;
constexpr uint16_t kVersion = 1;
constexpr uint8_t kFlagGzip = 0x01;
constexpr size_t kDigestBytes = 32;
constexpr size_t kEtagDigestBytes = 8;

// Sequential little-endian reader that refuses to step past `end`. Every
// read is checked, so a lying length field can only produce a failure.
class Reader {
public:
    Reader(std::span<const uint8_t> bytes, size_t pos, size_t end)
        : bytes_(bytes), pos_(pos), end_(end) {}

    bool u8(uint8_t& v) {
        if (!have(1)) return false;
        v = bytes_[pos_++];
        return true;
    }
    bool u16(uint16_t& v) {
        if (!have(2)) return false;
        v = static_cast<uint16_t>(bytes_[pos_] | (bytes_[pos_ + 1] << 8));
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t& v) {
        if (!have(4)) return false;
        v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | bytes_[pos_ + i];
        pos_ += 4;
        return true;
    }
    bool str(size_t n, std::string& out) {
        if (!have(n)) return false;
        out.assign(reinterpret_cast<const char*>(bytes_.data() + pos_), n);
        pos_ += n;
        return true;
    }
    bool bytes(size_t n, std::span<const uint8_t>& out) {
        if (!have(n)) return false;
        out = bytes_.subspan(pos_, n);
        pos_ += n;
        return true;
    }
    size_t pos() const { return pos_; }

private:
    bool have(size_t n) const { return pos_ <= end_ && n <= end_ - pos_; }

    std::span<const uint8_t> bytes_;
    size_t pos_;
    size_t end_;
};

Result<AssetPack> corrupt(std::string what) {
    return Result<AssetPack>::err(ErrorCode::InvalidArgument, "asset pack: " + std::move(what));
}

std::string make_etag(std::span<const uint8_t> digest) {
    return "\"" + to_hex(digest.first(kEtagDigestBytes)) + "\"";
}

}  // namespace

Result<AssetPack> AssetPack::parse(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes) return corrupt("truncated header");
    if (bytes[0] != 'W' || bytes[1] != 'P' || bytes[2] != 'A' || bytes[3] != 'K') {
        return corrupt("bad magic");
    }
    Reader header(bytes, 4, kHeaderBytes);
    uint16_t version = 0;
    uint16_t count = 0;
    uint32_t table_bytes = 0;
    uint32_t total_bytes = 0;
    header.u16(version);
    header.u16(count);
    header.u32(table_bytes);
    header.u32(total_bytes);
    if (version != kVersion) return corrupt("unsupported version");
    if (total_bytes < kHeaderBytes || total_bytes > bytes.size()) {
        return corrupt("total size out of range");
    }
    // 64-bit arithmetic: table_bytes is attacker-controlled and must not wrap.
    const uint64_t table_end = uint64_t{kHeaderBytes} + table_bytes;
    if (table_end > total_bytes) return corrupt("table out of range");

    AssetPack pack;
    pack.entries_.reserve(count);
    Reader table(bytes, kHeaderBytes, static_cast<size_t>(table_end));
    for (uint16_t i = 0; i < count; ++i) {
        AssetEntry entry;
        uint16_t path_len = 0;
        uint8_t mime_len = 0;
        uint8_t flags = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
        std::span<const uint8_t> digest;
        if (!table.u16(path_len) || !table.str(path_len, entry.path) || !table.u8(mime_len) ||
            !table.str(mime_len, entry.mime) || !table.u8(flags) || !table.u32(offset) ||
            !table.u32(length) || !table.bytes(kDigestBytes, digest)) {
            return corrupt("table record truncated");
        }
        if (entry.path.empty() || entry.path.front() != '/') return corrupt("bad path");
        if (offset % 4 != 0) return corrupt("misaligned data: " + entry.path);
        if (offset < table_end || uint64_t{offset} + length > total_bytes) {
            return corrupt("data out of range: " + entry.path);
        }
        entry.gzip = (flags & kFlagGzip) != 0;
        entry.data = bytes.subspan(offset, length);
        // The packer's digest is used as-is rather than rehashing every file
        // out of flash at boot; a wrong digest can only cost a cache miss.
        entry.etag = make_etag(digest);
        pack.entries_.push_back(std::move(entry));
    }
    if (table.pos() != table_end) return corrupt("table size mismatch");

    std::sort(pack.entries_.begin(), pack.entries_.end(),
              [](const AssetEntry& a, const AssetEntry& b) { return a.path < b.path; });
    auto dup = std::adjacent_find(
        pack.entries_.begin(), pack.entries_.end(),
        [](const AssetEntry& a, const AssetEntry& b) { return a.path == b.path; });
    if (dup != pack.entries_.end()) return corrupt("duplicate path: " + dup->path);
    return Result<AssetPack>::ok(std::move(pack));
}

const AssetEntry* AssetPack::find(std::string_view path) const {
    if (path == "/") path = "/index.html";
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), path,
        [](const AssetEntry& e, std::string_view p) { return std::string_view(e.path) < p; });
    if (it == entries_.end() || it->path != path) return nullptr;
    return &*it;
}

}  // namespace wisp::net
