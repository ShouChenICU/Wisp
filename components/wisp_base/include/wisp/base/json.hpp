#pragma once

#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"

namespace wisp {

// Serializes `v` to compact JSON (no extra whitespace). Map keys are
// emitted in std::map's natural (lexicographic) order.
//
// For anything a browser or another program reads (HTTP bodies, WebSocket
// frames, machine-readable log lines): control bytes are escaped and any
// ill-formed UTF-8 in strings is replaced by U+FFFD, so the output is
// always valid UTF-8 that JSON.parse() accepts. That replacement is lossy.
std::string to_json(const Value& v);

// For persistence: the same, except string bytes that are not valid UTF-8
// are kept as they are, so from_json() gives back exactly what was stored
// (an SSID in GBK, for instance). Control bytes are still escaped. The
// output is not necessarily valid UTF-8; never send it to a browser.
std::string to_json_raw(const Value& v);

// Parses `s` as JSON into a Value. On a parse error, returns
// ErrorCode::InvalidArgument with ArduinoJson's error text in the message.
Result<Value> from_json(std::string_view s);

}  // namespace wisp
