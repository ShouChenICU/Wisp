#pragma once

// Finding a credential that a server's text quotes (spec 4.4
// "Scrubbing"): an LLM provider's error code may quote its API key, an
// MCP server's message one of its header values.

#include <string_view>

namespace wisp::net::detail {

// Whether `text` holds `secret` whole, or any run of 8 or more bytes of it.
bool holds_secret_piece(std::string_view text, std::string_view secret);

inline constexpr std::string_view kWithheldMessage = "(message withheld: it quoted a credential)";

}  // namespace wisp::net::detail
