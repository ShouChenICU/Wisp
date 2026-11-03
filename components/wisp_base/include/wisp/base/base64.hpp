#pragma once

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

}  // namespace wisp
