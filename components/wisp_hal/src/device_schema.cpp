#include "wisp/hal/device_schema.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "wisp/base/json.hpp"

namespace wisp::hal {

namespace {

constexpr size_t kMaxNameBytes = 32;

// int64 range as doubles: 2^63 is exact, and every double below it in
// magnitude converts without overflow.
constexpr double kInt64Limit = 9223372036854775808.0;

bool valid_name(const char* name) {
    if (name == nullptr) return false;
    const size_t len = std::strlen(name);
    if (len == 0 || len > kMaxNameBytes) return false;
    if (!(name[0] >= 'a' && name[0] <= 'z')) return false;
    for (size_t i = 1; i < len; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

std::string quoted(const char* name) { return std::string("'") + (name ? name : "") + "'"; }

bool has_range(const ParamSpec& p) { return !(p.min == 0 && p.max == 0); }

bool integral(double d) { return std::isfinite(d) && std::trunc(d) == d; }

bool fits_int64(double d) { return d >= -kInt64Limit && d < kInt64Limit; }

// A bound for a message: "10", "-1.5".
std::string format_number(double d) {
    char buf[32];
    if (integral(d) && fits_int64(d)) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
    } else {
        std::snprintf(buf, sizeof(buf), "%g", d);
    }
    return buf;
}

bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        if (static_cast<unsigned char>(s[i]) < 0x80) {
            ++i;
            continue;
        }
        size_t bad = 0;
        const size_t len = json_detail::utf8_sequence(s, i, &bad);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

Result<Value> type_error(const ParamSpec& p) {
    return Result<Value>::err(ErrorCode::InvalidArgument,
                              quoted(p.name) + " must be " + to_string(p.type));
}

Result<Value> range_check(const ParamSpec& p, double x, Value v) {
    if (has_range(p) && (x < p.min || x > p.max)) {
        return Result<Value>::err(ErrorCode::InvalidArgument, quoted(p.name) + " must be between " +
                                                                  format_number(p.min) + " and " +
                                                                  format_number(p.max));
    }
    return Result<Value>::ok(std::move(v));
}

// `v` checked against `p`, converted to its stored form.
Result<Value> check_value(const ParamSpec& p, const Value& v) {
    switch (p.type) {
        case ParamType::Bool:
            if (v.type() != Value::Type::Bool) return type_error(p);
            return Result<Value>::ok(v);
        case ParamType::Int: {
            int64_t x = 0;
            if (v.type() == Value::Type::Int) {
                x = v.as_int();
            } else if (v.type() == Value::Type::Double && integral(v.as_double()) &&
                       fits_int64(v.as_double())) {
                x = static_cast<int64_t>(v.as_double());
            } else {
                return type_error(p);
            }
            return range_check(p, static_cast<double>(x), Value(x));
        }
        case ParamType::Number:
            if (v.type() == Value::Type::Int) {
                return range_check(p, static_cast<double>(v.as_int()), v);
            }
            if (v.type() == Value::Type::Double && std::isfinite(v.as_double())) {
                return range_check(p, v.as_double(), v);
            }
            return type_error(p);
        case ParamType::String: {
            if (v.type() != Value::Type::String) return type_error(p);
            const std::string& s = v.as_string();
            if (!valid_utf8(s)) {
                return Result<Value>::err(ErrorCode::InvalidArgument,
                                          quoted(p.name) + " must be valid UTF-8");
            }
            if (s.size() > p.max_len) {
                return Result<Value>::err(
                    ErrorCode::InvalidArgument,
                    quoted(p.name) + " must be at most " + std::to_string(p.max_len) + " bytes");
            }
            return Result<Value>::ok(v);
        }
        case ParamType::Enum: {
            if (v.type() == Value::Type::String) {
                for (const char* choice : p.choices) {
                    if (v.as_string() == choice) return Result<Value>::ok(v);
                }
            }
            std::string message = quoted(p.name) + " must be one of: ";
            for (size_t i = 0; i < p.choices.size(); ++i) {
                if (i > 0) message += ", ";
                message += p.choices[i];
            }
            return Result<Value>::err(ErrorCode::InvalidArgument, std::move(message));
        }
    }
    return type_error(p);
}

Result<void> fail(const std::string& message) {
    return Result<void>::err(ErrorCode::InvalidArgument, message);
}

// `where` names the entry for the message: "prop 'x'", "cmd 'c' arg 'a'".
Result<void> check_param(const ParamSpec& p, const std::string& where, bool is_arg) {
    if (!valid_name(p.name)) {
        return fail(where + ": name must match [a-z][a-z0-9_]{0,31}");
    }
    if (p.desc == nullptr) return fail(where + ": desc is null");
    if (p.unit == nullptr) return fail(where + ": unit is null");
    if (has_range(p)) {
        if (p.type != ParamType::Int && p.type != ParamType::Number) {
            return fail(where + ": a range is only for int and number");
        }
        if (!(p.min <= p.max)) return fail(where + ": min > max");
        if (p.type == ParamType::Int &&
            !(integral(p.min) && integral(p.max) && fits_int64(p.min) && fits_int64(p.max))) {
            return fail(where + ": an int range needs integral bounds");
        }
    }
    if (p.type == ParamType::Enum) {
        if (p.choices.empty()) return fail(where + ": enum without choices");
        for (const char* choice : p.choices) {
            if (choice == nullptr) return fail(where + ": null choice");
        }
    } else if (!p.choices.empty()) {
        return fail(where + ": choices are only for enum");
    }
    if (p.optional && !is_arg) return fail(where + ": props are never optional");
    if (p.optional && !p.def.is_null()) {
        auto def = check_value(p, p.def);
        if (!def) return fail(where + ": bad default: " + def.error().message);
    }
    return Result<void>::ok();
}

// The first name in `params` that repeats, or nullptr.
const char* duplicate(std::span<const ParamSpec> params) {
    for (size_t i = 0; i < params.size(); ++i) {
        for (size_t j = i + 1; j < params.size(); ++j) {
            if (std::strcmp(params[i].name, params[j].name) == 0) return params[i].name;
        }
    }
    return nullptr;
}

Value render_param(const ParamSpec& p) {
    Map out{{"name", p.name}, {"type", to_string(p.type)}};
    if (p.desc[0] != '\0') out["desc"] = p.desc;
    if (has_range(p)) {
        if (p.type == ParamType::Int) {
            out["min"] = static_cast<int64_t>(p.min);
            out["max"] = static_cast<int64_t>(p.max);
        } else {
            out["min"] = p.min;
            out["max"] = p.max;
        }
    }
    if (p.unit[0] != '\0') out["unit"] = p.unit;
    if (p.type == ParamType::Enum) {
        List choices;
        for (const char* choice : p.choices) choices.emplace_back(choice);
        out["choices"] = std::move(choices);
    }
    if (p.type == ParamType::String) out["max_len"] = static_cast<int64_t>(p.max_len);
    if (p.optional) {
        out["optional"] = true;
        if (!p.def.is_null()) out["default"] = p.def;
    }
    return Value(std::move(out));
}

}  // namespace

const char* to_string(ParamType t) {
    switch (t) {
        case ParamType::Bool:
            return "bool";
        case ParamType::Int:
            return "int";
        case ParamType::Number:
            return "number";
        case ParamType::String:
            return "string";
        case ParamType::Enum:
            return "enum";
    }
    return "unknown";
}

Result<void> check_schema(const DeviceSchema& schema) {
    if (schema.props.size() > kMaxSchemaProps) {
        return fail(std::to_string(schema.props.size()) + " props (at most " +
                    std::to_string(kMaxSchemaProps) + ")");
    }
    if (schema.cmds.size() > kMaxSchemaCmds) {
        return fail(std::to_string(schema.cmds.size()) + " cmds (at most " +
                    std::to_string(kMaxSchemaCmds) + ")");
    }
    for (const ParamSpec& p : schema.props) {
        auto r = check_param(p, "prop " + quoted(p.name), false);
        if (!r) return r;
    }
    if (const char* name = duplicate(schema.props)) return fail("duplicate prop " + quoted(name));
    for (const CmdSpec& c : schema.cmds) {
        const std::string where = "cmd " + quoted(c.name);
        if (!valid_name(c.name)) return fail(where + ": name must match [a-z][a-z0-9_]{0,31}");
        if (c.desc == nullptr) return fail(where + ": desc is null");
        if (c.args.size() > kMaxCmdArgs) {
            return fail(where + ": " + std::to_string(c.args.size()) + " args (at most " +
                        std::to_string(kMaxCmdArgs) + ")");
        }
        for (const ParamSpec& a : c.args) {
            auto r = check_param(a, where + " arg " + quoted(a.name), true);
            if (!r) return r;
        }
        if (const char* name = duplicate(c.args)) {
            return fail(where + ": duplicate arg " + quoted(name));
        }
    }
    for (size_t i = 0; i < schema.cmds.size(); ++i) {
        for (size_t j = i + 1; j < schema.cmds.size(); ++j) {
            if (std::strcmp(schema.cmds[i].name, schema.cmds[j].name) == 0) {
                return fail("duplicate cmd " + quoted(schema.cmds[i].name));
            }
        }
    }
    return Result<void>::ok();
}

Value render_schema(const DeviceSchema& schema) {
    List props;
    for (const ParamSpec& p : schema.props) props.push_back(render_param(p));
    List cmds;
    for (const CmdSpec& c : schema.cmds) {
        List args;
        for (const ParamSpec& a : c.args) args.push_back(render_param(a));
        cmds.push_back(Value(Map{
            {"name", c.name},
            {"desc", c.desc},
            {"changes_state", c.changes_state},
            {"min_interval_ms", static_cast<int64_t>(c.min_interval_ms)},
            {"args", std::move(args)},
        }));
    }
    return Value(Map{{"props", std::move(props)}, {"cmds", std::move(cmds)}});
}

Result<Map> validate_args(const CmdSpec& cmd, const Value& args) {
    const Map empty;
    const Map* given = &empty;
    if (args.type() == Value::Type::Map) {
        given = &args.as_map();
    } else if (!args.is_null()) {
        return Result<Map>::err(ErrorCode::InvalidArgument, "args must be an object");
    }
    for (const auto& [key, value] : *given) {
        bool known = false;
        for (const ParamSpec& a : cmd.args) {
            if (key == a.name) known = true;
        }
        if (!known) {
            return Result<Map>::err(ErrorCode::InvalidArgument,
                                    "unknown argument " + quoted(key.c_str()));
        }
    }
    Map out;
    for (const ParamSpec& a : cmd.args) {
        auto it = given->find(a.name);
        if (it == given->end()) {
            if (!a.optional) {
                return Result<Map>::err(ErrorCode::InvalidArgument,
                                        "missing argument " + quoted(a.name));
            }
            if (!a.def.is_null()) out[a.name] = a.def;
            continue;
        }
        auto checked = check_value(a, it->second);
        if (!checked) return Result<Map>::err(checked.error().code, checked.error().message);
        out[a.name] = std::move(checked.value());
    }
    return Result<Map>::ok(std::move(out));
}

const ParamSpec* find_prop(const DeviceSchema& schema, std::string_view name) {
    for (const ParamSpec& p : schema.props) {
        if (name == p.name) return &p;
    }
    return nullptr;
}

const CmdSpec* find_cmd(const DeviceSchema& schema, std::string_view name) {
    for (const CmdSpec& c : schema.cmds) {
        if (name == c.name) return &c;
    }
    return nullptr;
}

Properties caps_of(const Device& device) {
    Properties caps = device.info().caps;
    if (const DeviceSchema* schema = device.describe()) caps["schema"] = render_schema(*schema);
    return caps;
}

}  // namespace wisp::hal
