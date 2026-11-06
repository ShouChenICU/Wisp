#include "wisp/base/json.hpp"

// ArduinoJson's default (2 bytes of length) stops strings at 65535 bytes,
// but a stored tool result or a file written by the agent can be 64 KiB
// or a little more. This is the only file that includes ArduinoJson, so
// the setting cannot differ between two users of its templates.
#define ARDUINOJSON_STRING_LENGTH_SIZE 4
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace wisp {

namespace {

void write_value(JsonVariant dst, const Value& v) {
    switch (v.type()) {
        case Value::Type::Null:
            // JsonVariant starts out null; nothing to do.
            break;
        case Value::Type::Bool:
            dst.set(v.as_bool());
            break;
        case Value::Type::Int:
            dst.set(v.as_int());
            break;
        case Value::Type::Double:
            dst.set(v.as_double());
            break;
        case Value::Type::String:
            dst.set(v.as_string());
            break;
        case Value::Type::List: {
            JsonArray arr = dst.to<JsonArray>();
            for (const Value& item : v.as_list()) {
                write_value(arr.add<JsonVariant>(), item);
            }
            break;
        }
        case Value::Type::Map: {
            JsonObject obj = dst.to<JsonObject>();
            for (const auto& [key, value] : v.as_map()) {
                write_value(obj[key].to<JsonVariant>(), value);
            }
            break;
        }
    }
}

Value read_value(JsonVariantConst src) {
    if (src.isNull()) {
        return Value();
    }
    if (src.is<JsonArrayConst>()) {
        List list;
        for (JsonVariantConst item : src.as<JsonArrayConst>()) {
            list.push_back(read_value(item));
        }
        return Value(std::move(list));
    }
    if (src.is<JsonObjectConst>()) {
        Map map;
        for (JsonPairConst kv : src.as<JsonObjectConst>()) {
            map.emplace(kv.key().c_str(), read_value(kv.value()));
        }
        return Value(std::move(map));
    }
    // Order matters: an integer value also satisfies is<double>(), so Int
    // must be checked first to keep Int/Double distinct on round-trip.
    if (src.is<int64_t>()) {
        return Value(src.as<int64_t>());
    }
    if (src.is<double>()) {
        return Value(src.as<double>());
    }
    if (src.is<bool>()) {
        return Value(src.as<bool>());
    }
    if (src.is<const char*>()) {
        return Value(std::string(src.as<const char*>()));
    }
    return Value();
}

// ArduinoJson escapes only the control bytes JSON gives a short form to
// (and NUL), and copies other bytes as they are. JSON.parse() rejects the
// remaining raw control characters, so every byte below 0x20 becomes a
// 6-character escape (backslash, 'u', four hex digits); from_json() turns
// it back into the same byte. With `replace_invalid_utf8`, every
// ill-formed UTF-8 sequence also becomes U+FFFD: a WebSocket text frame
// that is not valid UTF-8 drops the connection (RFC 6455), and strings
// come from logs cut at a byte limit, SSIDs and other device data.
// Compact output has no whitespace and multi-byte text only occurs inside
// strings, so a byte-level pass cannot touch the JSON structure. Output
// that needs no change is returned as is; otherwise the copy starts at
// the first byte that does, keeping the pass linear.
std::string fix_up(std::string json, bool replace_invalid_utf8) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    bool copying = false;
    // Switches to building `out`, seeded with the unchanged bytes before i.
    auto start_copy = [&](size_t i) {
        if (copying) return;
        out.reserve(json.size() + 16);
        out.append(json, 0, i);
        copying = true;
    };
    size_t i = 0;
    while (i < json.size()) {
        const auto b = static_cast<uint8_t>(json[i]);
        if (b < 0x20) {
            start_copy(i);
            const char esc[] = {'\\', 'u', '0', '0', kHex[b >> 4], kHex[b & 0xF]};
            out.append(esc, sizeof(esc));
            ++i;
            continue;
        }
        size_t keep = 1;  // bytes at i that pass through unchanged
        if (b >= 0x80) {
            size_t bad = 0;
            keep = json_detail::utf8_sequence(json, i, &bad);
            if (keep == 0 && replace_invalid_utf8) {
                start_copy(i);
                out += "\xEF\xBF\xBD";
                i += bad;
                continue;
            }
            if (keep == 0) keep = 1;
        }
        if (copying) out.append(json, i, keep);
        i += keep;
    }
    return copying ? out : json;
}

std::string serialize(const Value& v) {
    JsonDocument doc;
    write_value(doc.to<JsonVariant>(), v);
    std::string out;
    serializeJson(doc, out);
    return out;
}

}  // namespace

size_t json_detail::utf8_sequence(std::string_view s, size_t i, size_t* bad) {
    const auto b = static_cast<uint8_t>(s[i]);
    size_t len = 0;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        len = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
        len = 3;
        if (b == 0xE0) lo = 0xA0;  // overlong
        if (b == 0xED) hi = 0x9F;  // UTF-16 surrogates
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4;
        if (b == 0xF0) lo = 0x90;  // overlong
        if (b == 0xF4) hi = 0x8F;  // past U+10FFFF
    } else {
        *bad = 1;
        return 0;
    }
    for (size_t k = 1; k < len; ++k) {
        const uint8_t min = k == 1 ? lo : 0x80;
        const uint8_t max = k == 1 ? hi : 0xBF;
        if (i + k >= s.size() || static_cast<uint8_t>(s[i + k]) < min ||
            static_cast<uint8_t>(s[i + k]) > max) {
            *bad = k;
            return 0;
        }
    }
    return len;
}

std::string to_json(const Value& v) { return fix_up(serialize(v), /*replace_invalid_utf8=*/true); }

std::string to_json_raw(const Value& v) {
    return fix_up(serialize(v), /*replace_invalid_utf8=*/false);
}

Result<Value> from_json(std::string_view s) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s.data(), s.size());
    if (err) {
        return Result<Value>::err(ErrorCode::InvalidArgument, err.c_str());
    }
    return Result<Value>::ok(read_value(doc.as<JsonVariantConst>()));
}

Result<Value> from_json(std::string_view s, uint8_t max_depth) {
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, s.data(), s.size(), DeserializationOption::NestingLimit(max_depth));
    if (err == DeserializationError::TooDeep) {
        return Result<Value>::err(ErrorCode::QuotaExceeded,
                                  "nested deeper than " + std::to_string(max_depth) + " levels");
    }
    if (err) {
        return Result<Value>::err(ErrorCode::InvalidArgument, err.c_str());
    }
    // The document is at most max_depth deep, and so is this recursion.
    return Result<Value>::ok(read_value(doc.as<JsonVariantConst>()));
}

}  // namespace wisp
