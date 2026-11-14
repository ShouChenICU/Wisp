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

// How much of an untrusted key an error message echoes.
constexpr size_t kMaxEchoBytes = 32;

// int64 range as doubles: 2^63 is exact, and every double below it in
// magnitude converts without overflow.
constexpr double kInt64Limit = 9223372036854775808.0;

// Lua 5.4's reserved words. A scene passes arguments as a table
// constructor ({ms = 150}), where a keyword is not a valid field name.
constexpr const char* kLuaKeywords[] = {
    "and",      "break",  "do",   "else", "elseif", "end",   "false", "for",
    "function", "goto",   "if",   "in",   "local",  "nil",   "not",   "or",
    "repeat",   "return", "then", "true", "until",  "while",
};

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

bool lua_keyword(const char* name) {
    for (const char* keyword : kLuaKeywords) {
        if (std::strcmp(name, keyword) == 0) return true;
    }
    return false;
}

// Null when the name is acceptable, else why not.
const char* name_problem(const char* name) {
    if (!valid_name(name)) return "name must match [a-z][a-z0-9_]{0,31}";
    if (lua_keyword(name)) return "name is a Lua keyword";
    return nullptr;
}

std::string quoted(std::string_view name) { return "'" + std::string(name) + "'"; }
std::string quoted(const char* name) { return quoted(std::string_view(name ? name : "")); }

// An untrusted key for a message, at most kMaxEchoBytes and never cut
// inside a UTF-8 sequence.
std::string echoed(std::string_view key) {
    if (key.size() <= kMaxEchoBytes) return quoted(key);
    size_t cut = kMaxEchoBytes;
    while (cut > 0 && (static_cast<unsigned char>(key[cut]) & 0xC0) == 0x80) --cut;
    return "'" + std::string(key.substr(0, cut)) + "...'";
}

bool has_range(const ParamSpec& p) { return !(p.min == 0 && p.max == 0); }

bool integral(double d) { return std::isfinite(d) && std::trunc(d) == d; }

bool fits_int64(double d) { return d >= -kInt64Limit && d < kInt64Limit; }

bool in_range(const ParamSpec& p, double x) { return !has_range(p) || (x >= p.min && x <= p.max); }

bool is_choice(const ParamSpec& p, std::string_view s) {
    for (const char* choice : p.choices) {
        if (s == choice) return true;
    }
    return false;
}

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

// The messages shared by argument and default validation; built only on
// the failure path.
std::string type_message(const ParamSpec& p) {
    return quoted(p.name) + " must be " + to_string(p.type);
}
std::string range_message(const ParamSpec& p) {
    return quoted(p.name) + " must be between " + format_number(p.min) + " and " +
           format_number(p.max);
}
std::string utf8_message(const ParamSpec& p) { return quoted(p.name) + " must be valid UTF-8"; }
std::string length_message(const ParamSpec& p) {
    return quoted(p.name) + " must be at most " + std::to_string(p.max_len) + " bytes";
}
std::string choices_message(const ParamSpec& p) {
    std::string message = quoted(p.name) + " must be one of: ";
    for (size_t i = 0; i < p.choices.size(); ++i) {
        if (i > 0) message += ", ";
        message += p.choices[i];
    }
    return message;
}

Result<Value> invalid(std::string message) {
    return Result<Value>::err(ErrorCode::InvalidArgument, std::move(message));
}

Result<Value> in_range_or_fail(const ParamSpec& p, double x, Value v) {
    if (!in_range(p, x)) return invalid(range_message(p));
    return Result<Value>::ok(std::move(v));
}

// `v` checked against `p`, converted to its stored form.
Result<Value> check_value(const ParamSpec& p, const Value& v) {
    switch (p.type) {
        case ParamType::Bool:
            if (v.type() != Value::Type::Bool) return invalid(type_message(p));
            return Result<Value>::ok(v);
        case ParamType::Int: {
            int64_t x = 0;
            if (v.type() == Value::Type::Int) {
                x = v.as_int();
            } else if (v.type() == Value::Type::Double && integral(v.as_double()) &&
                       fits_int64(v.as_double())) {
                x = static_cast<int64_t>(v.as_double());
            } else {
                return invalid(type_message(p));
            }
            return in_range_or_fail(p, static_cast<double>(x), Value(x));
        }
        case ParamType::Number:
            if (v.type() == Value::Type::Int) {
                return in_range_or_fail(p, static_cast<double>(v.as_int()), v);
            }
            if (v.type() == Value::Type::Double && std::isfinite(v.as_double())) {
                return in_range_or_fail(p, v.as_double(), v);
            }
            return invalid(type_message(p));
        case ParamType::String: {
            if (v.type() != Value::Type::String) return invalid(type_message(p));
            const std::string& s = v.as_string();
            if (!valid_utf8(s)) return invalid(utf8_message(p));
            if (s.size() > p.max_len) return invalid(length_message(p));
            return Result<Value>::ok(v);
        }
        case ParamType::Enum:
            if (v.type() == Value::Type::String && is_choice(p, v.as_string())) {
                return Result<Value>::ok(v);
            }
            return invalid(choices_message(p));
    }
    return invalid(type_message(p));
}

Value to_value(const ParamDefault& def) {
    switch (def.kind()) {
        case ParamDefault::Kind::None:
            return Value();
        case ParamDefault::Kind::Bool:
            return Value(def.as_bool());
        case ParamDefault::Kind::Int:
            return Value(def.as_int());
        case ParamDefault::Kind::Number:
            return Value(def.as_double());
        case ParamDefault::Kind::String:
            return def.as_string() ? Value(def.as_string()) : Value();
    }
    return Value();
}

Result<void> fail(std::string message) {
    return Result<void>::err(ErrorCode::InvalidArgument, std::move(message));
}

// The default of an optional arg, checked without building a Value.
Result<void> check_default(const ParamSpec& p) {
    using Kind = ParamDefault::Kind;
    const ParamDefault& def = p.def;
    switch (p.type) {
        case ParamType::Bool:
            if (def.kind() != Kind::Bool) return fail("default must be bool");
            return Result<void>::ok();
        case ParamType::Int:
            if (def.kind() != Kind::Int) return fail("default must be int");
            if (!in_range(p, static_cast<double>(def.as_int()))) {
                return fail("bad default: " + range_message(p));
            }
            return Result<void>::ok();
        case ParamType::Number: {
            double x = 0;
            if (def.kind() == Kind::Int) {
                x = static_cast<double>(def.as_int());
            } else if (def.kind() == Kind::Number && std::isfinite(def.as_double())) {
                x = def.as_double();
            } else {
                return fail("default must be number");
            }
            if (!in_range(p, x)) return fail("bad default: " + range_message(p));
            return Result<void>::ok();
        }
        case ParamType::String:
        case ParamType::Enum: {
            if (def.kind() != Kind::String) return fail("default must be string");
            if (def.as_string() == nullptr) return fail("default is null");
            const std::string_view s = def.as_string();
            if (p.type == ParamType::Enum) {
                if (!is_choice(p, s)) return fail("bad default: " + choices_message(p));
                return Result<void>::ok();
            }
            if (!valid_utf8(s)) return fail("bad default: " + utf8_message(p));
            if (s.size() > p.max_len) return fail("bad default: " + length_message(p));
            return Result<void>::ok();
        }
    }
    return fail("default must be " + std::string(to_string(p.type)));
}

// The rules for one prop or arg; the message leaves out which entry, so
// the caller builds that prefix only when something is wrong.
Result<void> check_param(const ParamSpec& p, bool is_arg) {
    if (const char* problem = name_problem(p.name)) return fail(problem);
    if (p.desc == nullptr) return fail("desc is null");
    if (p.unit == nullptr) return fail("unit is null");
    if (has_range(p)) {
        if (p.type != ParamType::Int && p.type != ParamType::Number) {
            return fail("a range is only for int and number");
        }
        if (!(p.min <= p.max)) return fail("min > max");
        if (p.type == ParamType::Int &&
            !(integral(p.min) && integral(p.max) && fits_int64(p.min) && fits_int64(p.max))) {
            return fail("an int range needs integral bounds");
        }
    }
    if (p.type == ParamType::Enum) {
        if (p.choices.empty()) return fail("enum without choices");
        for (size_t i = 0; i < p.choices.size(); ++i) {
            const char* choice = p.choices[i];
            if (choice == nullptr) return fail("null choice");
            if (choice[0] == '\0') return fail("empty choice");
            for (size_t j = 0; j < i; ++j) {
                if (std::strcmp(p.choices[j], choice) == 0) {
                    return fail("duplicate choice " + quoted(choice));
                }
            }
        }
    } else if (!p.choices.empty()) {
        return fail("choices are only for enum");
    }
    if (!is_arg) {
        if (p.optional) return fail("props are never optional");
        if (!p.def.is_none()) return fail("props take no default");
        return Result<void>::ok();
    }
    if (p.def.is_none()) return Result<void>::ok();
    if (!p.optional) return fail("default without optional");
    return check_default(p);
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
        if (!p.def.is_none()) out["default"] = to_value(p.def);
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
        if (auto r = check_param(p, false); !r) {
            return fail("prop " + quoted(p.name) + ": " + r.error().message);
        }
    }
    if (const char* name = duplicate(schema.props)) return fail("duplicate prop " + quoted(name));
    for (const CmdSpec& c : schema.cmds) {
        if (const char* problem = name_problem(c.name)) {
            return fail("cmd " + quoted(c.name) + ": " + problem);
        }
        if (c.desc == nullptr) return fail("cmd " + quoted(c.name) + ": desc is null");
        if (c.args.size() > kMaxCmdArgs) {
            return fail("cmd " + quoted(c.name) + ": " + std::to_string(c.args.size()) +
                        " args (at most " + std::to_string(kMaxCmdArgs) + ")");
        }
        for (const ParamSpec& a : c.args) {
            if (auto r = check_param(a, true); !r) {
                return fail("cmd " + quoted(c.name) + " arg " + quoted(a.name) + ": " +
                            r.error().message);
            }
        }
        if (const char* name = duplicate(c.args)) {
            return fail("cmd " + quoted(c.name) + ": duplicate arg " + quoted(name));
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
    for (const auto& entry : *given) {
        bool known = false;
        for (const ParamSpec& a : cmd.args) {
            if (entry.first == a.name) known = true;
        }
        if (!known) {
            return Result<Map>::err(ErrorCode::InvalidArgument,
                                    "unknown argument " + echoed(entry.first));
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
            if (!a.def.is_none()) out[a.name] = to_value(a.def);
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
