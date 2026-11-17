#pragma once

// Whether text holds a secret or any 8-byte piece of it: the check every
// write-only secret test uses (an API key, an MCP header value), so a
// leak of part of a value, cut or wrapped, is caught too.

#include <cstddef>
#include <string_view>

namespace wisp::test {

inline constexpr size_t kSecretPieceBytes = 8;

inline bool holds_secret_piece(std::string_view text, std::string_view secret) {
    if (secret.size() < kSecretPieceBytes) return text.find(secret) != std::string_view::npos;
    for (size_t i = 0; i + kSecretPieceBytes <= secret.size(); ++i) {
        if (text.find(secret.substr(i, kSecretPieceBytes)) != std::string_view::npos) return true;
    }
    return false;
}

}  // namespace wisp::test
