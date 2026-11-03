#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"

namespace wisp::net {

struct AssetEntry {
    std::string path;  // always starts with "/"
    std::string mime;
    bool gzip = false;  // data is a gzip member; send with Content-Encoding: gzip
    std::string etag;   // "\"" + hex of the first 8 bytes of sha256(data) + "\""
    std::span<const uint8_t> data;
};

// Read-only view over a WispPack (spec 2.3; the byte layout is documented
// in tools/pack_assets.py, which produces it). Entries' `data` spans point
// into the bytes handed to parse() -- on the device a memory-mapped flash
// partition, served without copying -- so the caller must keep those bytes
// alive and unchanged for as long as the AssetPack (or any AssetEntry it
// returned) is in use.
class AssetPack {
public:
    AssetPack() = default;

    // Validates the header and every table record against `bytes` before
    // exposing anything: truncated, corrupt or hostile input yields
    // InvalidArgument and is never read out of range. Bytes past the
    // header's total_bytes are ignored (a partition is larger than its pack).
    static Result<AssetPack> parse(std::span<const uint8_t> bytes);

    // Exact path lookup; "/" maps to "/index.html". nullptr when absent.
    const AssetEntry* find(std::string_view path) const;
    size_t size() const { return entries_.size(); }

private:
    std::vector<AssetEntry> entries_;  // sorted by path
};

}  // namespace wisp::net
