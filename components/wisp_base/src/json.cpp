#include "wisp/base/json.hpp"

#include <ArduinoJson.h>

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

}  // namespace

std::string to_json(const Value& v) {
    JsonDocument doc;
    write_value(doc.to<JsonVariant>(), v);
    std::string out;
    serializeJson(doc, out);
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
