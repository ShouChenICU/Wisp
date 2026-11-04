// Zip reading and writing per the PKWARE APPNOTE (sections 4.3.6-4.3.16:
// local file header, central directory header, end of central directory
// record). Only the subset scene packages use is accepted; see zip.hpp.
#include "wisp/core/zip.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include "wisp/base/crc32.hpp"
#include "wisp/base/inflate.hpp"

namespace wisp::core {

namespace {

constexpr uint32_t kLocalSig = 0x04034B50;
constexpr uint32_t kCentralSig = 0x02014B50;
constexpr uint32_t kEndSig = 0x06054B50;
constexpr uint32_t kZip64LocatorSig = 0x07064B50;
constexpr size_t kLocalSize = 30;
constexpr size_t kCentralSize = 46;
constexpr size_t kEndSize = 22;
constexpr size_t kZip64LocatorSize = 20;
constexpr uint16_t kZip64ExtraId = 0x0001;
constexpr uint16_t kFlagEncrypted = 0x0001;
constexpr uint16_t kFlagUtf8 = 0x0800;
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflated = 8;
constexpr uint32_t kModeTypeMask = 0170000;
constexpr uint32_t kModeSymlink = 0120000;
constexpr uint32_t kModeRegular = 0100644;
constexpr uint32_t kModeDir = 040755;
constexpr uint32_t kDosDirAttr = 0x10;
constexpr uint16_t kVersion = 20;          // 2.0: deflate and directories
constexpr uint16_t kMadeByUnix = 3 << 8;   // upper byte: host system 3 (Unix)
constexpr uint16_t kDosDate1980 = 0x0021;  // 1980-01-01, so output is reproducible

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Length of the well-formed UTF-8 sequence at s[i] (RFC 3629: no overlong
// forms, no surrogates, nothing above U+10FFFF), or 0 if it is ill-formed.
size_t utf8_len(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
    const uint8_t c = b(i);
    if (c < 0x80) {
        return 1;
    }
    size_t n = 0;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        lo = c == 0xE0 ? 0xA0 : 0x80;
        hi = c == 0xED ? 0x9F : 0xBF;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        lo = c == 0xF0 ? 0x90 : 0x80;
        hi = c == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0;
    }
    if (s.size() - i < n || b(i + 1) < lo || b(i + 1) > hi) {
        return 0;
    }
    for (size_t k = 2; k < n; ++k) {
        if (b(i + k) < 0x80 || b(i + k) > 0xBF) {
            return 0;
        }
    }
    return n;
}

// The path as it appears in error messages: well-formed UTF-8 is kept,
// control and ill-formed bytes become \xNN so the message (which reaches
// logs and JSON responses) stays valid UTF-8 and printable.
std::string printable(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string r;
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<uint8_t>(s[i]);
        const size_t n = utf8_len(s, i);
        if (n == 0 || c < 0x20 || c == 0x7F) {
            r += "\\x";
            r += kHex[c >> 4];
            r += kHex[c & 0xF];
            ++i;
        } else {
            r.append(s.substr(i, n));
            i += n;
        }
    }
    return r;
}

// Why `path` is not a safe relative path, or nullptr if it is. A single
// trailing '/' marks a directory and is not an empty segment.
const char* path_problem(std::string_view path, size_t max_path) {
    if (path.empty()) {
        return "empty path";
    }
    if (path.size() > max_path) {
        return "path too long";
    }
    if (path.front() == '/') {
        return "absolute path";
    }
    for (size_t i = 0; i < path.size();) {
        const auto c = static_cast<uint8_t>(path[i]);
        if (c == '\\' || c == ':') {
            return "path contains '\\' or ':'";
        }
        if (c < 0x20 || c == 0x7F) {
            return "path contains a control character";
        }
        const size_t n = utf8_len(path, i);
        if (n == 0) {
            return "path is not valid UTF-8";
        }
        i += n;
    }
    std::string_view rest = path;
    if (rest.back() == '/') {
        rest.remove_suffix(1);
    }
    while (true) {
        const size_t slash = rest.find('/');
        const std::string_view seg = rest.substr(0, slash);
        if (seg.empty()) {
            return "path has an empty segment";
        }
        if (seg == "." || seg == "..") {
            return "path has a '.' or '..' segment";
        }
        if (slash == std::string_view::npos) {
            return nullptr;
        }
        rest.remove_prefix(slash + 1);
    }
}

std::string_view without_slash(std::string_view p) {
    if (!p.empty() && p.back() == '/') {
        p.remove_suffix(1);
    }
    return p;
}

// An entry name can be up to 64 KB; messages show only its start so a
// hostile name cannot inflate them.
constexpr size_t kMaxShownPath = 160;

template <class T>
Result<T> bad(std::string_view path, const char* why) {
    std::string shown = printable(path.substr(0, kMaxShownPath));
    if (path.size() > kMaxShownPath) {
        shown += "...";
    }
    return Result<T>::err(ErrorCode::InvalidArgument,
                          "zip entry '" + shown + "': " + std::string(why));
}

template <class T>
Result<T> bad_archive(const char* why) {
    return Result<T>::err(ErrorCode::InvalidArgument, std::string("zip: ") + why);
}

// True if a central-directory extra field carries a Zip64 record.
bool has_zip64_extra(const uint8_t* p, size_t n) {
    while (n >= 4) {
        const uint16_t id = rd16(p);
        const size_t len = rd16(p + 2);
        if (id == kZip64ExtraId) {
            return true;
        }
        if (len > n - 4) {
            return false;  // malformed tail; nothing else reads extra fields
        }
        p += 4 + len;
        n -= 4 + len;
    }
    return false;
}

void put16(os::LargeVector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(os::LargeVector<uint8_t>& v, uint32_t x) {
    put16(v, x & 0xFFFF);
    put16(v, x >> 16);
}

}  // namespace

Result<ZipReader> ZipReader::open(std::span<const uint8_t> zip, ZipLimits limits) {
    using R = Result<ZipReader>;
    if (zip.size() < kEndSize) {
        return bad_archive<ZipReader>("too short for an end of central directory record");
    }
    // The end record is last, followed only by its comment (<= 64 KB).
    // Requiring the comment length to reach exactly the end of the data
    // keeps a signature inside the comment from being mistaken for it.
    size_t end = SIZE_MAX;
    const size_t lowest = zip.size() - kEndSize > 0xFFFF ? zip.size() - kEndSize - 0xFFFF : 0;
    for (size_t pos = zip.size() - kEndSize + 1; pos-- > lowest;) {
        const uint8_t* p = zip.data() + pos;
        if (rd32(p) == kEndSig && pos + kEndSize + rd16(p + 20) == zip.size()) {
            end = pos;
            break;
        }
    }
    if (end == SIZE_MAX) {
        return bad_archive<ZipReader>("no end of central directory record");
    }
    const uint8_t* e = zip.data() + end;
    const uint16_t disk = rd16(e + 4);
    const uint16_t cd_disk = rd16(e + 6);
    const uint16_t disk_entries = rd16(e + 8);
    const uint16_t total_entries = rd16(e + 10);
    const uint32_t cd_size = rd32(e + 12);
    const uint32_t cd_offset = rd32(e + 16);
    if (total_entries == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF ||
        (end >= kZip64LocatorSize &&
         rd32(zip.data() + end - kZip64LocatorSize) == kZip64LocatorSig)) {
        return bad_archive<ZipReader>("Zip64 archives are not supported");
    }
    if (disk != 0 || cd_disk != 0 || disk_entries != total_entries) {
        return bad_archive<ZipReader>("multi-disk archives are not supported");
    }
    if (total_entries > limits.max_entries) {
        return R::err(ErrorCode::InvalidArgument, "zip: " + std::to_string(total_entries) +
                                                      " entries, limit " +
                                                      std::to_string(limits.max_entries));
    }
    if (static_cast<uint64_t>(cd_offset) + cd_size > end) {
        return bad_archive<ZipReader>("central directory out of bounds");
    }

    os::LargeVector<ZipEntry> entries;
    entries.reserve(total_entries);
    uint64_t total = 0;
    size_t pos = cd_offset;
    const size_t cd_end = static_cast<size_t>(cd_offset) + cd_size;
    for (uint16_t i = 0; i < total_entries; ++i) {
        if (cd_end - pos < kCentralSize || rd32(zip.data() + pos) != kCentralSig) {
            return bad_archive<ZipReader>("damaged central directory");
        }
        const uint8_t* h = zip.data() + pos;
        const size_t name_len = rd16(h + 28);
        const size_t extra_len = rd16(h + 30);
        const size_t comment_len = rd16(h + 32);
        if (cd_end - pos - kCentralSize < name_len + extra_len + comment_len) {
            return bad_archive<ZipReader>("damaged central directory");
        }
        // Validated in place: only a path that passes (and so is at most
        // max_path bytes) is copied.
        const std::string_view name(reinterpret_cast<const char*>(h + kCentralSize), name_len);
        if (const char* why = path_problem(name, limits.max_path)) {
            return bad<ZipReader>(name, why);
        }
        ZipEntry entry;
        entry.path.assign(name);
        const uint16_t flags = rd16(h + 8);
        entry.method = rd16(h + 10);
        entry.crc32 = rd32(h + 16);
        entry.comp_size = rd32(h + 20);
        entry.size = rd32(h + 24);
        const uint16_t start_disk = rd16(h + 34);
        const uint32_t external = rd32(h + 38);
        entry.local_offset = rd32(h + 42);
        pos += kCentralSize + name_len + extra_len + comment_len;

        entry.is_dir = entry.path.back() == '/';
        if ((flags & kFlagEncrypted) != 0) {
            return bad<ZipReader>(entry.path, "encrypted entries are not supported");
        }
        if (entry.method != kMethodStored && entry.method != kMethodDeflated) {
            return bad<ZipReader>(entry.path, "compression method is neither stored nor deflate");
        }
        // The upper half of the external attributes holds the Unix mode
        // whenever an archiver records one; other hosts leave it zero.
        if (((external >> 16) & kModeTypeMask) == kModeSymlink) {
            return bad<ZipReader>(entry.path, "symbolic links are not allowed");
        }
        if (entry.size == 0xFFFFFFFF || entry.comp_size == 0xFFFFFFFF ||
            entry.local_offset == 0xFFFFFFFF ||
            has_zip64_extra(h + kCentralSize + name_len, extra_len)) {
            return bad<ZipReader>(entry.path, "Zip64 entries are not supported");
        }
        if (start_disk != 0) {
            return bad<ZipReader>(entry.path, "multi-disk archives are not supported");
        }
        if (entry.method == kMethodStored && entry.comp_size != entry.size) {
            return bad<ZipReader>(entry.path, "stored entry with mismatched sizes");
        }
        if (entry.is_dir && entry.size != 0) {
            return bad<ZipReader>(entry.path, "directory entry with data");
        }
        // Local headers and their data precede the central directory.
        if (static_cast<uint64_t>(entry.local_offset) + kLocalSize + name_len + entry.comp_size >
            cd_offset) {
            return bad<ZipReader>(entry.path, "entry data out of bounds");
        }
        for (const ZipEntry& prev : entries) {
            if (without_slash(prev.path) == without_slash(entry.path)) {
                return bad<ZipReader>(entry.path, "duplicate path");
            }
        }
        total += entry.size;
        if (total > limits.max_total) {
            return R::err(
                ErrorCode::QuotaExceeded,
                "zip: declared contents exceed " + std::to_string(limits.max_total) + " bytes");
        }
        entries.push_back(std::move(entry));
    }
    return R::ok(ZipReader(zip, std::move(entries), cd_offset, limits.max_total));
}

Result<os::LargeBuffer> ZipReader::read(const ZipEntry& e) const {
    using R = Result<os::LargeBuffer>;
    if (e.method != kMethodStored && e.method != kMethodDeflated) {
        return bad<os::LargeBuffer>(e.path, "compression method is neither stored nor deflate");
    }
    if (e.size > max_total_) {
        return bad<os::LargeBuffer>(e.path, "entry larger than the archive limit");
    }
    if (static_cast<uint64_t>(e.local_offset) + kLocalSize > cd_offset_ ||
        rd32(zip_.data() + e.local_offset) != kLocalSig) {
        return bad<os::LargeBuffer>(e.path, "missing local header");
    }
    const uint8_t* h = zip_.data() + e.local_offset;
    // Sizes and CRC come from the central directory: with general purpose
    // bit 3 set the local header carries zeros and the real values trail
    // the data.
    const uint16_t flags = rd16(h + 6);
    const uint16_t method = rd16(h + 8);
    const size_t name_len = rd16(h + 26);
    const size_t extra_len = rd16(h + 28);
    const uint64_t data_start =
        static_cast<uint64_t>(e.local_offset) + kLocalSize + name_len + extra_len;
    if ((flags & kFlagEncrypted) != 0) {
        return bad<os::LargeBuffer>(e.path, "encrypted entries are not supported");
    }
    // The data is decoded with the central directory's method either way;
    // a disagreeing local header means some other unzipper would read the
    // entry differently, so the archive is rejected rather than trusted.
    if (method != e.method) {
        return bad<os::LargeBuffer>(e.path, "local header has a different compression method");
    }
    if (data_start + e.comp_size > cd_offset_) {
        return bad<os::LargeBuffer>(e.path, "entry data out of bounds");
    }
    if (name_len != e.path.size() || std::memcmp(h + kLocalSize, e.path.data(), name_len) != 0) {
        return bad<os::LargeBuffer>(e.path, "local header names a different path");
    }
    const std::span<const uint8_t> data =
        zip_.subspan(static_cast<size_t>(data_start), e.comp_size);

    os::LargeBuffer out = os::LargeBuffer::allocate(e.size);
    if (!out.ok()) {
        return R::err(ErrorCode::NoMemory,
                      "zip entry '" + printable(std::string_view(e.path).substr(0, kMaxShownPath)) +
                          "': " + std::to_string(e.size) + " bytes");
    }
    if (e.method == kMethodStored) {
        if (e.comp_size != e.size) {
            return bad<os::LargeBuffer>(e.path, "stored entry with mismatched sizes");
        }
        if (e.size != 0) {
            std::memcpy(out.data(), data.data(), e.size);
        }
    } else {
        auto n = inflate_raw(data, out.span());
        if (!n) {
            return bad<os::LargeBuffer>(e.path, "corrupt deflate data or wrong size");
        }
        if (n.value() != e.size) {
            return bad<os::LargeBuffer>(e.path, "decompressed size differs from the header");
        }
    }
    if (crc32(out.span()) != e.crc32) {
        return bad<os::LargeBuffer>(e.path, "CRC-32 mismatch");
    }
    return R::ok(std::move(out));
}

void ZipWriter::add(std::string_view path, std::span<const uint8_t> data) {
    const bool dir = !path.empty() && path.back() == '/';
    const uint32_t crc = crc32(data);
    const auto size = static_cast<uint32_t>(data.size());
    const auto name_len = static_cast<uint32_t>(path.size());
    const auto offset = static_cast<uint32_t>(body_.size());
    const uint16_t flags = kFlagUtf8;

    put32(body_, kLocalSig);
    put16(body_, kVersion);
    put16(body_, flags);
    put16(body_, kMethodStored);
    put16(body_, 0);  // time 00:00:00
    put16(body_, kDosDate1980);
    put32(body_, crc);
    put32(body_, size);
    put32(body_, size);
    put16(body_, name_len);
    put16(body_, 0);  // extra field length
    body_.insert(body_.end(), path.begin(), path.end());
    body_.insert(body_.end(), data.begin(), data.end());

    put32(central_, kCentralSig);
    put16(central_, kMadeByUnix | kVersion);
    put16(central_, kVersion);
    put16(central_, flags);
    put16(central_, kMethodStored);
    put16(central_, 0);
    put16(central_, kDosDate1980);
    put32(central_, crc);
    put32(central_, size);
    put32(central_, size);
    put16(central_, name_len);
    put16(central_, 0);  // extra field length
    put16(central_, 0);  // comment length
    put16(central_, 0);  // disk number start
    put16(central_, 0);  // internal attributes
    put32(central_, dir ? (kModeDir << 16) | kDosDirAttr : kModeRegular << 16);
    put32(central_, offset);
    central_.insert(central_.end(), path.begin(), path.end());
    ++count_;
}

os::LargeBuffer ZipWriter::finish() {
    const auto cd_offset = static_cast<uint32_t>(body_.size());
    const auto cd_size = static_cast<uint32_t>(central_.size());
    os::LargeVector<uint8_t> tail;
    put32(tail, kEndSig);
    put16(tail, 0);  // this disk
    put16(tail, 0);  // disk holding the central directory
    put16(tail, static_cast<uint32_t>(count_));
    put16(tail, static_cast<uint32_t>(count_));
    put32(tail, cd_size);
    put32(tail, cd_offset);
    put16(tail, 0);  // comment length

    os::LargeBuffer out = os::LargeBuffer::allocate(body_.size() + central_.size() + tail.size());
    if (out.ok()) {
        // An empty writer has empty vectors whose data() may be null, which
        // memcpy does not accept even for zero bytes.
        uint8_t* p = out.data();
        if (!body_.empty()) {
            std::memcpy(p, body_.data(), body_.size());
            p += body_.size();
            std::memcpy(p, central_.data(), central_.size());
            p += central_.size();
        }
        std::memcpy(p, tail.data(), tail.size());
    }
    body_ = {};
    central_ = {};
    count_ = 0;
    return out;
}

}  // namespace wisp::core
