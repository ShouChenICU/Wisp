#pragma once

// The URL and header rules the services that store HTTP endpoints share:
// ModelProfileService (base URLs and their keys) and McpServerService
// (server URLs and their header values). They live in wisp_core because
// those services cannot reach wisp_net; net::SceneHttp's header syntax
// check delegates here, so the device has one definition of each rule.
// None of these log or echo their input.

#include <string>
#include <string_view>

namespace wisp::core {

// Whether `s` holds a space, a control byte (0x00-0x1f) or DEL: none of
// these belong in a URL or a header name.
bool has_control_or_space(std::string_view s);

// An http:// or https:// URL without whitespace or control bytes, with a
// host left once user info and the port are taken off. Accepts the URL as
// it is: no trailing '/' is stripped, no placeholder is checked for.
bool valid_http_url(std::string_view url);

// "scheme://host:port" of a URL valid_http_url() accepted, with the scheme
// and host lowercased, user info dropped and the default port made
// explicit, so two URLs that reach the same server compare equal. "" for
// "" (no endpoint yet). A secret stored for one origin is never sent to
// another.
std::string url_origin(std::string_view url);

// Whether the URL's authority carries user info ("user:pass@host").
bool url_has_userinfo(std::string_view url);

// A header name that is an RFC 9110 token (non-empty) and a value without
// CR, LF or NUL: what stops header injection. No allowlist policy.
bool http_header_syntax_ok(std::string_view name, std::string_view value);

}  // namespace wisp::core
