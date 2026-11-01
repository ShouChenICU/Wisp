#pragma once

#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"

namespace wisp {

// Serializes `v` to compact JSON (no extra whitespace). Map keys are
// emitted in std::map's natural (lexicographic) order.
std::string to_json(const Value& v);

// Parses `s` as JSON into a Value. On a parse error, returns
// ErrorCode::InvalidArgument with ArduinoJson's error text in the message.
Result<Value> from_json(std::string_view s);

}  // namespace wisp
