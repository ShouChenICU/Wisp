#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace wisp {

// Up to N bytes stored inline, so a struct of these copies without
// allocating (state cells, spec 3.3 of the state-cells design). Bytes,
// not characters: an SSID is raw bytes and may hold any value, NUL
// included; truncation counts bytes. Display text uses assign_utf8();
// byte-exact identifiers (SSIDs, ids) use assign().
template <size_t N>
class FixedString {
    static_assert(N > 0 && N <= 255, "the size is kept in one byte");

public:
    constexpr FixedString() = default;
    // Truncates to N bytes, as assign() does.
    FixedString(std::string_view s) { assign(s); }  // NOLINT(google-explicit-constructor)

    // Returns false when bytes past N were dropped. The tail past the new
    // size is zero-filled so that two equal strings are also equal byte
    // for byte, whatever they held before.
    bool assign(std::string_view s) { return store(s, std::min(s.size(), N)); }

    // As assign(), but a cut at N bytes moves back so that it never splits
    // a UTF-8 sequence it can recognise. The rule, on bytes only (nothing
    // is validated, so invalid input is copied as bytes up to the cut):
    // when s is longer than N and byte N (the first dropped) is a
    // continuation byte (10xxxxxx), look back at most three bytes past
    // continuation bytes for a lead byte (110xxxxx, 1110xxxx, 11110xxx);
    // if there is one and the length it declares runs past N, the cut
    // moves to that lead byte. Otherwise the cut stays at N. Returns false
    // when anything was dropped.
    bool assign_utf8(std::string_view s) {
        size_t n = std::min(s.size(), N);
        if (n < s.size() && is_continuation(s[n])) {
            size_t k = n;
            while (k > 0 && n - k < 3 && is_continuation(s[k - 1])) {
                --k;
            }
            if (k > 0) {
                size_t lead = k - 1;
                size_t len = sequence_length(s[lead]);
                if (len != 0 && lead + len > n) {
                    n = lead;
                }
            }
        }
        return store(s, n);
    }

    std::string_view view() const { return std::string_view(bytes_.data(), size_); }
    std::string str() const { return std::string(view()); }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    static constexpr size_t capacity() { return N; }

    friend bool operator==(const FixedString& a, const FixedString& b) {
        return a.view() == b.view();
    }
    friend bool operator==(const FixedString& a, std::string_view b) { return a.view() == b; }

private:
    static bool is_continuation(char c) { return (static_cast<uint8_t>(c) & 0xC0) == 0x80; }
    // The sequence length a lead byte declares; 0 for any other byte.
    static size_t sequence_length(char c) {
        auto b = static_cast<uint8_t>(c);
        if ((b & 0xE0) == 0xC0) return 2;
        if ((b & 0xF0) == 0xE0) return 3;
        if ((b & 0xF8) == 0xF0) return 4;
        return 0;
    }

    // Keeps the first n bytes of s (n <= N); true when that is all of s.
    // memmove: s may be this string's own view (fs.assign(fs.view().substr(1))).
    bool store(std::string_view s, size_t n) {
        if (n != 0) {  // an empty string_view may hold a null data(), which memmove must not see
            std::memmove(bytes_.data(), s.data(), n);
        }
        std::memset(bytes_.data() + n, 0, N - n);
        size_ = static_cast<uint8_t>(n);
        return n == s.size();
    }

    std::array<char, N> bytes_{};
    uint8_t size_ = 0;
};

}  // namespace wisp
