#pragma once

// A minimal zip reader and writer for scene packages. The reader treats
// every archive as hostile: open() validates the whole central directory
// (paths, flags, methods, sizes) before anything is decompressed, and
// read() re-checks the local header, the bounds and the CRC. Only what
// scene packages need is supported: stored (0) and deflated (8) entries,
// no encryption, no Zip64, no multi-disk archives.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "wisp/base/result.hpp"
#include "wisp/os/large.hpp"

namespace wisp::core {

struct ZipLimits {
    size_t max_entries = 256;
    size_t max_total = 512 * 1024;  // sum of the declared uncompressed sizes
    size_t max_path = 128;          // bytes
};

// The entry list and its paths live in Mem::Large: their size is chosen by
// whoever built the (untrusted) archive, and small allocations would
// otherwise come from internal RAM (AGENTS.md section 8).
struct ZipEntry {
    os::LargeString path;  // directories keep their trailing '/'
    uint32_t crc32 = 0;
    uint32_t size = 0;
    uint32_t comp_size = 0;
    uint16_t method = 0;
    uint32_t local_offset = 0;
    bool is_dir = false;
};

class ZipReader {
public:
    // Parses and validates the central directory of `zip`, which must
    // outlive the reader (it is not copied). Errors: InvalidArgument for a
    // malformed archive or an unsafe entry (the message names the entry's
    // path), QuotaExceeded when the declared sizes exceed limits.max_total.
    static Result<ZipReader> open(std::span<const uint8_t> zip, ZipLimits limits = {});

    const os::LargeVector<ZipEntry>& entries() const { return entries_; }

    // Returns the entry's contents in Mem::Large, checked against its
    // declared size and CRC. Errors: InvalidArgument for a damaged entry,
    // NoMemory when the buffer cannot be allocated.
    Result<os::LargeBuffer> read(const ZipEntry& e) const;

private:
    ZipReader(std::span<const uint8_t> zip, os::LargeVector<ZipEntry> entries, size_t cd_offset,
              size_t max_total)
        : zip_(zip), entries_(std::move(entries)), cd_offset_(cd_offset), max_total_(max_total) {}

    std::span<const uint8_t> zip_;
    os::LargeVector<ZipEntry> entries_;
    // read() takes any ZipEntry, not only one from entries(), so it
    // re-checks against these instead of trusting the entry: entry data
    // must end before the central directory, and no entry may need a
    // buffer larger than the archive limit.
    size_t cd_offset_;
    size_t max_total_;
};

// Builds a zip of stored entries in Mem::Large. A path ending in '/' is
// written as a directory entry. Paths are written as given; the caller is
// responsible for passing names ZipReader would accept.
class ZipWriter {
public:
    void add(std::string_view path, std::span<const uint8_t> data);

    // Returns the finished archive; a buffer with !ok() means Mem::Large
    // was exhausted. The writer is left empty.
    os::LargeBuffer finish();

private:
    os::LargeVector<uint8_t> body_;
    os::LargeVector<uint8_t> central_;
    size_t count_ = 0;
};

}  // namespace wisp::core
