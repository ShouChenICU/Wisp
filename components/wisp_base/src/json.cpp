#include "wisp/base/json.hpp"

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
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

// Length of the well-formed UTF-8 sequence starting at s[i], or 0 when
// there is none. When 0, *bad is set to how many bytes form the maximal
// ill-formed prefix (at least 1), which is replaced by one U+FFFD as the
// Unicode standard recommends.
size_t utf8_sequence(const std::string& s, size_t i, size_t* bad) {
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

// ArduinoJson escapes only the control bytes JSON gives a short form to
// (and NUL), and copies other bytes as they are. Browsers need more: a
// WebSocket text frame that is not valid UTF-8 drops the connection
// (RFC 6455), and JSON.parse() rejects raw control characters. Strings
// come from logs cut at a byte limit, SSIDs and other device data, so the
// output is fixed up here: every byte below 0x20 becomes a 6-character
// escape (backslash, 'u', four hex digits) and every ill-formed UTF-8
// sequence becomes U+FFFD. Compact output has no
// whitespace and multi-byte text only occurs inside strings, so a
// byte-level pass cannot touch the JSON structure.
void make_browser_safe(std::string& json) {
    size_t i = 0;
    while (i < json.size()) {
        const auto b = static_cast<uint8_t>(json[i]);
        if (b >= 0x20 && b < 0x80) {
            ++i;
            continue;
        }
        if (b >= 0x80) {
            size_t bad = 0;
            const size_t len = utf8_sequence(json, i, &bad);
            if (len > 0) {
                i += len;
                continue;
            }
            json.replace(i, bad, "\xEF\xBF\xBD");
            i += 3;
            continue;
        }
        static constexpr char kHex[] = "0123456789abcdef";
        const char esc[] = {'\\', 'u', '0', '0', kHex[b >> 4], kHex[b & 0xF]};
        json.replace(i, 1, esc, sizeof(esc));
        i += sizeof(esc);
    }
}

}  // namespace

std::string to_json(const Value& v) {
    JsonDocument doc;
    write_value(doc.to<JsonVariant>(), v);
    std::string out;
    serializeJson(doc, out);
    make_browser_safe(out);
    return out;
}

Result<Value> from_json(std::string_view s) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s.data(), s.size());
    if (err) {
        return Result<Value>::err(ErrorCode::InvalidArgument, err.c_str());
    }
    return Result<Value>::ok(read_value(doc.as<JsonVariantConst>()));
}

}  // namespace wisp
