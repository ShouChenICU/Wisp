#include "wisp/base/base64.hpp"

#include <cstddef>

namespace wisp {

namespace {

std::string encode(std::span<const uint8_t> data, const char* alphabet, bool pad) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(alphabet[(n >> 18) & 0x3f]);
        out.push_back(alphabet[(n >> 12) & 0x3f]);
        out.push_back(alphabet[(n >> 6) & 0x3f]);
        out.push_back(alphabet[n & 0x3f]);
    }
    const size_t rem = data.size() - i;
    if (rem == 0) return out;

    uint32_t n = uint32_t(data[i]) << 16;
    if (rem == 2) n |= uint32_t(data[i + 1]) << 8;
    out.push_back(alphabet[(n >> 18) & 0x3f]);
    out.push_back(alphabet[(n >> 12) & 0x3f]);
    if (rem == 2) {
        out.push_back(alphabet[(n >> 6) & 0x3f]);
        if (pad) out.push_back('=');
    } else if (pad) {
        out.append("==");
    }
    return out;
}

}  // namespace

std::string base64url_encode(std::span<const uint8_t> data) {
    return encode(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

std::string base64_encode(std::span<const uint8_t> data) {
    return encode(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

}  // namespace wisp
