// The JSON Schema subset tool arguments are checked against (see
// wisp/agent/tool_registry.hpp).

#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/scene_manifest.hpp"

namespace wisp::agent {

namespace {

bool is_number(const Value& v) {
    return v.type() == Value::Type::Int || v.type() == Value::Type::Double;
}

double number_of(const Value& v) {
    return v.type() == Value::Type::Int ? static_cast<double>(v.as_int()) : v.as_double();
}

size_t code_points(std::string_view s) {
    size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool has_type(const Value& v, std::string_view type) {
    if (type == "object") return v.type() == Value::Type::Map;
    if (type == "array") return v.type() == Value::Type::List;
    if (type == "string") return v.type() == Value::Type::String;
    if (type == "boolean") return v.type() == Value::Type::Bool;
    if (type == "null") return v.type() == Value::Type::Null;
    if (type == "number") return is_number(v);
    if (type == "integer") {
        if (v.type() == Value::Type::Int) return true;
        return v.type() == Value::Type::Double && std::isfinite(v.as_double()) &&
               std::floor(v.as_double()) == v.as_double();
    }
    return false;
}

std::string join(const std::string& path, std::string_view key) {
    if (path.empty()) return std::string(key);
    std::string out = path;
    out.push_back('.');
    out.append(key);
    return out;
}

std::string number_text(double d) { return to_json(Value(d)); }

std::optional<std::string> fail(const std::string& path, std::string* why, std::string reason) {
    if (why != nullptr) *why = std::move(reason);
    return path;
}

std::optional<std::string> check(const Value& schema, const Value& v, const std::string& path,
                                 std::string* why) {
    const std::string label = path.empty() ? std::string("arguments") : "'" + path + "'";
    if (const Value* type = schema.find("type")) {
        if (type->type() == Value::Type::List) {
            bool any = false;
            std::string names;
            for (const Value& t : type->as_list()) {
                any = any || has_type(v, t.as_string());
                if (!names.empty()) names += " or ";
                names += t.as_string();
            }
            if (!any) return fail(path, why, label + " must be " + names);
        } else if (!has_type(v, type->as_string())) {
            return fail(path, why, label + " must be " + type->as_string());
        }
    }
    if (const Value* e = schema.find("enum")) {
        bool found = false;
        for (const Value& option : e->as_list()) {
            if (option == v) {
                found = true;
                break;
            }
        }
        if (!found) return fail(path, why, label + " must be one of " + to_json(*e));
    }
    if (is_number(v)) {
        const double d = number_of(v);
        if (const Value* min = schema.find("minimum"); min != nullptr && d < number_of(*min)) {
            return fail(path, why, label + " must be at least " + number_text(number_of(*min)));
        }
        if (const Value* max = schema.find("maximum"); max != nullptr && d > number_of(*max)) {
            return fail(path, why, label + " must be at most " + number_text(number_of(*max)));
        }
    }
    if (v.type() == Value::Type::String) {
        const Value* max = schema.find("maxLength");
        if (max != nullptr && code_points(v.as_string()) > static_cast<size_t>(max->as_int())) {
            return fail(path, why,
                        label + " is longer than " + std::to_string(max->as_int()) + " characters");
        }
    }
    if (v.type() == Value::Type::Map) {
        if (const Value* required = schema.find("required")) {
            for (const Value& name : required->as_list()) {
                if (v.find(name.as_string()) == nullptr) {
                    const std::string field = join(path, name.as_string());
                    return fail(field, why, "'" + field + "' is required");
                }
            }
        }
        if (const Value* props = schema.find("properties")) {
            for (const auto& [name, sub] : props->as_map()) {
                const Value* arg = v.find(name);
                if (arg == nullptr) continue;
                if (auto bad = check(sub, *arg, join(path, name), why)) return bad;
            }
        }
    }
    if (v.type() == Value::Type::List) {
        if (const Value* items = schema.find("items")) {
            const auto& list = v.as_list();
            for (size_t i = 0; i < list.size(); ++i) {
                if (auto bad = check(*items, list[i], path + "[" + std::to_string(i) + "]", why)) {
                    return bad;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> validate_args(const Value& schema, const Value& args, std::string* why) {
    return check(schema, args, std::string(), why);
}

bool schema_supported(const Value& schema, std::string* why) {
    return core::scene_tool_schema_supported(schema, why);
}

}  // namespace wisp::agent
