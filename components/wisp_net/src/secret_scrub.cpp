#include "secret_scrub.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "wisp/os/large.hpp"

namespace wisp::net::detail {

namespace {

constexpr size_t kMinPiece = 8;

uint64_t window_at(const char* p) {
    uint64_t w = 0;
    std::memcpy(&w, p, sizeof(w));
    return w;
}

size_t slot_of(uint64_t w, size_t mask) {
    return static_cast<size_t>((w * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
}

}  // namespace

// A longer run holds an 8-byte one, so those are all that need looking
// for. A shorter piece is not enough: short real words ("invalid") can
// occur in a secret by chance and say nothing about it. A secret shorter
// than that is looked for whole.
bool holds_any_secret_piece(std::string_view text, std::span<const std::string_view> secrets) {
    size_t windows = 0;
    for (const std::string_view s : secrets) {
        if (s.empty()) continue;
        if (s.size() < kMinPiece) {
            if (text.find(s) != std::string_view::npos) return true;
            continue;
        }
        windows += s.size() - kMinPiece + 1;
    }
    if (windows == 0 || text.size() < kMinPiece) return false;

    // Open addressing at most half full. 0 marks an empty slot, so the
    // all-NUL window (never in a header value, but possible elsewhere) is
    // kept aside.
    size_t slots = 16;
    while (slots < windows * 2) slots *= 2;
    const size_t mask = slots - 1;
    os::LargeVector<uint64_t> table(slots, 0);
    bool zero = false;
    for (const std::string_view s : secrets) {
        if (s.size() < kMinPiece) continue;
        for (size_t i = 0; i + kMinPiece <= s.size(); ++i) {
            const uint64_t w = window_at(s.data() + i);
            if (w == 0) {
                zero = true;
                continue;
            }
            size_t at = slot_of(w, mask);
            while (table[at] != 0 && table[at] != w) at = (at + 1) & mask;
            table[at] = w;
        }
    }
    for (size_t i = 0; i + kMinPiece <= text.size(); ++i) {
        const uint64_t w = window_at(text.data() + i);
        if (w == 0) {
            if (zero) return true;
            continue;
        }
        for (size_t at = slot_of(w, mask); table[at] != 0; at = (at + 1) & mask)
            if (table[at] == w) return true;
    }
    return false;
}

bool holds_secret_piece(std::string_view text, std::string_view secret) {
    return holds_any_secret_piece(text, std::span<const std::string_view>(&secret, 1));
}

std::string_view credential_part(std::string_view value) {
    constexpr size_t kMinPart = 8;
    constexpr std::string_view kTokenPunct = "!#$%&'*+-.^_`|~";
    auto token_char = [&](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               kTokenPunct.find(c) != std::string_view::npos;
    };
    size_t i = 0;
    while (i < value.size() && token_char(value[i])) ++i;
    if (i == 0 || i == value.size() || value[i] != ' ') return value;
    while (i < value.size() && value[i] == ' ') ++i;
    const std::string_view rest = value.substr(i);
    return rest.size() >= kMinPart ? rest : value;
}

}  // namespace wisp::net::detail
