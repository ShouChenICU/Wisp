#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace wisp {

// RFC 4648 section 5 alphabet ('-' and '_'), without '=' padding, so the
// result can go into URLs and cookies unescaped.
std::string base64url_encode(std::span<const uint8_t> data);

// RFC 4648 section 4 standard alphabet with '=' padding; the WebSocket
// handshake (RFC 6455 Sec-WebSocket-Accept) requires this exact form.
std::string base64_encode(std::span<const uint8_t> data);

// The length of base64_encode()'s result for `n` input bytes.
constexpr size_t base64_encoded_size(size_t n) { return (n + 2) / 3 * 4; }

// base64_encode() into a caller's buffer, so a large payload (an image in
// a request body) can be encoded in place without an intermediate string.
// Writes exactly base64_encoded_size(data.size()) chars to `out`; no
// terminator.
void base64_encode_into(std::span<const uint8_t> data, char* out);

}  // namespace wisp
