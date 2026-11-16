#include "secret_scrub.hpp"

#include <cstddef>

namespace wisp::net::detail {

// A longer run holds an 8-byte one, so those are all that need looking
// for. A shorter piece is not enough: short real words ("invalid") can
// occur in a secret by chance and say nothing about it.
bool holds_secret_piece(std::string_view text, std::string_view secret) {
    constexpr size_t kMinPiece = 8;
    if (secret.empty()) return false;
    if (text.find(secret) != std::string_view::npos) return true;
    for (size_t i = 0; i + kMinPiece <= text.size(); ++i) {
        if (secret.find(text.substr(i, kMinPiece)) != std::string_view::npos) return true;
    }
    return false;
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
