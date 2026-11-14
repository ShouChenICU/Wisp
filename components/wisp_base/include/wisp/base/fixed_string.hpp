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
// included; truncation counts bytes.
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
    bool assign(std::string_view s) {
        size_t n = std::min(s.size(), N);
        if (n != 0) {  // an empty string_view may hold a null data(), which memcpy must not see
            std::memcpy(bytes_.data(), s.data(), n);
        }
        std::memset(bytes_.data() + n, 0, N - n);
        size_ = static_cast<uint8_t>(n);
        return n == s.size();
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
    std::array<char, N> bytes_{};
    uint8_t size_ = 0;
};

}  // namespace wisp
