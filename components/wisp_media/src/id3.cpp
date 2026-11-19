// ID3v2.3/2.4 and ID3v1 tag reading (media spec 3.3). Only the three
// fields the library indexes are decoded; everything else is skipped by
// its size.

#include <algorithm>
#include <cstring>

#include "wisp/media/tags.hpp"
#include "wisp/os/large.hpp"

namespace wisp::media {
namespace {

constexpr size_t kHeaderBytes = 10;
// A text frame never needs more input than this to produce
// kMaxTagFieldBytes of UTF-8 (UTF-16 ASCII is the widest at 2:1, plus the
// encoding byte and a BOM), so per-frame resynchronisation copies at most
// this much, on the stack.
constexpr size_t kMaxTextInput = 2 * kMaxTagFieldBytes + 4;

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

// False when a byte has its high bit set, which a syncsafe integer never
// does.
bool syncsafe32(const uint8_t* p, uint32_t& out) {
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80) return false;
    out = (uint32_t{p[0]} << 21) | (uint32_t{p[1]} << 14) | (uint32_t{p[2]} << 7) | p[3];
    return true;
}

// Undoes unsynchronisation (0xFF 0x00 -> 0xFF) of `in` into `out`, which
// has room for in.size() bytes; returns the bytes written.
size_t resync(std::span<const uint8_t> in, uint8_t* out) {
    size_t n = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        out[n++] = in[i];
        if (in[i] == 0xFF && i + 1 < in.size() && in[i + 1] == 0x00) ++i;
    }
    return n;
}

// Length of the well-formed UTF-8 sequence starting at s[i] (no overlong
// forms, no surrogates, nothing above U+10FFFF), 0 when there is none.
size_t utf8_seq(std::span<const uint8_t> s, size_t i) {
    const uint8_t c = s[i];
    if (c < 0x80) return 1;
    size_t len;
    uint8_t lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if (s.size() - i < len) return 0;
    if (s[i + 1] < lo || s[i + 1] > hi) return 0;
    for (size_t k = 2; k < len; ++k) {
        if (s[i + k] < 0x80 || s[i + k] > 0xBF) return 0;
    }
    return len;
}

// Bytes up to the first NUL, read as UTF-8. ISO-8859-1 text goes through
// here too: bytes that form valid UTF-8 are a common mis-tagging and are
// kept; anything else with bytes >= 0x80 is typically GBK, which the device
// cannot convert, so the field is dropped (spec 3.3).
std::string decode_utf8(std::span<const uint8_t> s) {
    std::string out;
    for (size_t i = 0; i < s.size() && s[i] != 0;) {
        const size_t len = utf8_seq(s, i);
        if (len == 0) return {};
        if (out.size() + len > kMaxTagFieldBytes) break;
        out.append(reinterpret_cast<const char*>(s.data() + i), len);
        i += len;
    }
    return out;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

size_t utf8_len(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

// UTF-16 up to the first NUL unit. Encoding 1 starts with a BOM (missing:
// little-endian, what most taggers write); encoding 2 is big-endian. An
// unpaired surrogate makes the field undecodable.
std::string decode_utf16(std::span<const uint8_t> s, bool big_endian) {
    size_t i = 0;
    if (s.size() >= 2) {
        if (s[0] == 0xFF && s[1] == 0xFE) {
            big_endian = false;
            i = 2;
        } else if (s[0] == 0xFE && s[1] == 0xFF) {
            big_endian = true;
            i = 2;
        }
    }
    auto unit = [&](size_t at) -> uint32_t {
        return big_endian ? (uint32_t{s[at]} << 8) | s[at + 1] : (uint32_t{s[at + 1]} << 8) | s[at];
    };
    std::string out;
    while (s.size() - i >= 2) {
        uint32_t cp = unit(i);
        if (cp == 0) break;
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (s.size() - i < 2) return {};
            const uint32_t lo = unit(i);
            if (lo < 0xDC00 || lo > 0xDFFF) return {};
            i += 2;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return {};
        }
        if (out.size() + utf8_len(cp) > kMaxTagFieldBytes) break;
        append_utf8(out, cp);
    }
    return out;
}

// A text frame body: one encoding byte, then the text. Multiple values
// (v2.4, NUL-separated) yield the first.
std::string decode_text(std::span<const uint8_t> body) {
    if (body.empty()) return {};
    const auto s = body.subspan(1);
    switch (body[0]) {
        case 0:
        case 3:
            return decode_utf8(s);
        case 1:
            return decode_utf16(s, false);
        case 2:
            return decode_utf16(s, true);
        default:
            return {};
    }
}

std::string* field_for(TrackTags& t, const uint8_t* id) {
    if (std::memcmp(id, "TIT2", 4) == 0) return &t.title;
    if (std::memcmp(id, "TPE1", 4) == 0) return &t.artist;
    if (std::memcmp(id, "TALB", 4) == 0) return &t.album;
    return nullptr;
}

bool frame_id_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

// Walks the frames of `frames` (already resynchronised for v2.3).
// `all_unsync` is the v2.4 tag-level flag: every frame is unsynchronised.
void read_frames(std::span<const uint8_t> frames, uint8_t version, bool all_unsync,
                 TrackTags& tags) {
    size_t pos = 0;
    while (frames.size() - pos >= kHeaderBytes) {
        const uint8_t* h = frames.data() + pos;
        // Padding, or junk where the next frame should be.
        if (!frame_id_char(h[0]) || !frame_id_char(h[1]) || !frame_id_char(h[2]) ||
            !frame_id_char(h[3])) {
            return;
        }
        uint32_t size = 0;
        if (version == 4) {
            if (!syncsafe32(h + 4, size)) return;
        } else {
            size = be32(h + 4);
        }
        const uint8_t flags = h[9];
        pos += kHeaderBytes;
        if (size > frames.size() - pos) return;
        auto body = frames.subspan(pos, size);
        pos += size;

        std::string* field = field_for(tags, h);
        if (field == nullptr || !field->empty()) continue;

        bool unsync = false;
        size_t extra = 0;
        if (version == 4) {
            if (flags & 0x0C) continue;    // compressed or encrypted
            if (flags & 0x40) extra += 1;  // group id
            if (flags & 0x01) extra += 4;  // data length indicator
            unsync = all_unsync || (flags & 0x02);
        } else {
            if (flags & 0xC0) continue;    // compressed or encrypted
            if (flags & 0x20) extra += 1;  // group id
        }
        if (extra > body.size()) continue;
        body = body.subspan(extra);

        if (unsync) {
            uint8_t buf[kMaxTextInput];
            const size_t n = resync(body.first(std::min(body.size(), sizeof buf)), buf);
            *field = decode_text(std::span<const uint8_t>(buf, n));
        } else {
            *field = decode_text(body);
        }
    }
}

// ID3v1 fields are fixed-width, NUL- or space-padded.
std::string v1_field(std::span<const uint8_t> raw) {
    size_t n = 0;
    while (n < raw.size() && raw[n] != 0) ++n;
    while (n > 0 && raw[n - 1] == ' ') --n;
    return decode_utf8(raw.first(n));
}

}  // namespace

size_t id3v2_size(std::span<const uint8_t> head) {
    if (head.size() < kHeaderBytes) return 0;
    if (head[0] != 'I' || head[1] != 'D' || head[2] != '3') return 0;
    if (head[3] < 2 || head[3] > 4 || head[4] == 0xFF) return 0;
    uint32_t size = 0;
    if (!syncsafe32(head.data() + 6, size)) return 0;
    const bool footer = head[3] == 4 && (head[5] & 0x10);
    return kHeaderBytes + size + (footer ? kHeaderBytes : 0);
}

TrackTags parse_id3v2(std::span<const uint8_t> tag) {
    TrackTags tags;
    if (id3v2_size(tag) == 0) return tags;
    const uint8_t version = tag[3];
    if (version != 3 && version != 4) return tags;  // v2.2 uses 3-byte frame ids
    const uint8_t flags = tag[5];
    uint32_t declared = 0;
    syncsafe32(tag.data() + 6, declared);
    auto body = tag.subspan(kHeaderBytes, std::min<size_t>(declared, tag.size() - kHeaderBytes));

    // v2.3 unsynchronises the whole tag, extended header included, and its
    // frame sizes count resynchronised bytes, so the body is resynchronised
    // before any size is read. v2.4 does it per frame.
    os::LargeBuffer resynced;
    if (version == 3 && (flags & 0x80)) {
        resynced = os::LargeBuffer::allocate(body.size());
        if (!resynced.ok()) return tags;
        body = std::span<const uint8_t>(resynced.data(), resync(body, resynced.data()));
    }

    if (flags & 0x40) {
        if (body.size() < 4) return tags;
        uint64_t ext = 0;
        if (version == 3) {
            ext = uint64_t{4} + be32(body.data());  // the size excludes itself
        } else {
            uint32_t s = 0;
            if (!syncsafe32(body.data(), s) || s < 6) return tags;
            ext = s;
        }
        if (ext > body.size()) return tags;
        body = body.subspan(static_cast<size_t>(ext));
    }

    read_frames(body, version, version == 4 && (flags & 0x80), tags);
    return tags;
}

TrackTags parse_id3v1(std::span<const uint8_t> last128) {
    TrackTags tags;
    if (last128.size() < 128) return tags;
    const auto t = last128.last(128);
    if (t[0] != 'T' || t[1] != 'A' || t[2] != 'G') return tags;
    tags.title = v1_field(t.subspan(3, 30));
    tags.artist = v1_field(t.subspan(33, 30));
    tags.album = v1_field(t.subspan(63, 30));
    return tags;
}

std::string title_from_file_name(std::string_view path) {
    const size_t slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
    return std::string(name);
}

}  // namespace wisp::media
