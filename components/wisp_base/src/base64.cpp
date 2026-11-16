#include "wisp/base/base64.hpp"

#include <cstddef>

namespace wisp {

namespace {

constexpr const char kStandard[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr const char kUrl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Encodes `data` into `out` and returns the number of chars written:
// base64_encoded_size(data.size()) with `pad`, fewer for a partial final
// group without it.
size_t encode_to(std::span<const uint8_t> data, const char* alphabet, bool pad, char* out) {
    char* p = out;
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        *p++ = alphabet[(n >> 18) & 0x3f];
        *p++ = alphabet[(n >> 12) & 0x3f];
        *p++ = alphabet[(n >> 6) & 0x3f];
        *p++ = alphabet[n & 0x3f];
    }
    const size_t rem = data.size() - i;
    if (rem == 0) return size_t(p - out);

    uint32_t n = uint32_t(data[i]) << 16;
    if (rem == 2) n |= uint32_t(data[i + 1]) << 8;
    *p++ = alphabet[(n >> 18) & 0x3f];
    *p++ = alphabet[(n >> 12) & 0x3f];
    if (rem == 2) {
        *p++ = alphabet[(n >> 6) & 0x3f];
        if (pad) *p++ = '=';
    } else if (pad) {
        *p++ = '=';
        *p++ = '=';
    }
    return size_t(p - out);
}

}  // namespace

void base64_encode_into(std::span<const uint8_t> data, char* out) {
    encode_to(data, kStandard, true, out);
}

std::string base64url_encode(std::span<const uint8_t> data) {
    std::string out(base64_encoded_size(data.size()), '\0');
    out.resize(encode_to(data, kUrl, false, out.data()));
    return out;
}

std::string base64_encode(std::span<const uint8_t> data) {
    std::string out(base64_encoded_size(data.size()), '\0');
    base64_encode_into(data, out.data());
    return out;
}

}  // namespace wisp
