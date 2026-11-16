#pragma once

// Finding a credential that a server's text quotes (spec 4.4
// "Scrubbing"): an LLM provider's error code may quote its API key, an
// MCP server's message one of its header values.

#include <span>
#include <string_view>

namespace wisp::net::detail {

// Whether `text` holds any of `secrets` whole, or any run of 8 or more
// bytes of one. One pass over `text` (a lookup per 8-byte window in a table
// of the secrets' windows), so its cost does not grow with the secrets'
// length times the text's.
bool holds_any_secret_piece(std::string_view text, std::span<const std::string_view> secrets);

// holds_any_secret_piece() for one secret.
bool holds_secret_piece(std::string_view text, std::string_view secret);

// The part of an Authorization or Proxy-Authorization value that is the
// credential (other headers have no scheme): after an auth-scheme
// prefix ("Bearer ", "Basic ", "Token ": an RFC 9110 token and spaces)
// when that part is at least 8 bytes, else the whole value. Looking for
// pieces of the scheme would withhold any message that merely names it
// ("Bearer token expired" holds "Bearer t").
std::string_view credential_part(std::string_view value);

inline constexpr std::string_view kWithheldMessage = "(message withheld: it quoted a credential)";

}  // namespace wisp::net::detail
