#include "wisp/core/scene_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"

namespace wisp::core {

namespace {

// scene.json is a few hundred bytes; the cap keeps a hostile package from
// making the parser build a large Value tree in internal RAM.
constexpr size_t kMaxManifestBytes = 16 * 1024;
constexpr size_t kMaxNameBytes = 32;
// SemVer sets no limit; the version is shown in lists and logs.
constexpr size_t kMaxVersionBytes = 64;
constexpr size_t kMaxHosts = 16;
constexpr size_t kMaxHostBytes = 253;
constexpr int64_t kMaxSchemaVersion = 1000;
// The manifest needs three levels (permissions.http[]), plus one more for
// headroom, plus up to four more for a tool's "parameters" JSON-Schema
// shape (tools[].parameters.properties.<name>.type is 4 levels below
// "parameters" itself). Parsing recurses once per level, and the storage
// worker parses every manifest at boot on a stack of a few KB
// (wisp/core/storage_worker.hpp): without a cap, a package with a deeply
// nested unknown field would overflow it on every boot; 8 levels of
// recursion is still trivially cheap on that stack.
constexpr uint8_t kMaxManifestDepth = 8;
// ^[a-z][a-z0-9_]{0,31}$, so 1-32 bytes.
constexpr size_t kMaxToolNameBytes = 32;
constexpr size_t kMinToolDescriptionCodePoints = 1;
constexpr size_t kMaxToolDescriptionCodePoints = 512;
constexpr int64_t kMinToolTimeoutMs = 1000;
constexpr int64_t kMaxToolTimeoutMs = 120000;
constexpr uint32_t kDefaultToolTimeoutMs = 30000;

Result<SceneManifest> invalid(const std::string& what) {
    return Result<SceneManifest>::err(ErrorCode::InvalidArgument, "scene.json: " + what);
}

// Strict UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF.
bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<uint8_t>(s[i]);
        size_t n = 0;
        uint32_t cp = 0;
        uint32_t min = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
            min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
            min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
            min = 0x10000;
        } else {
            return false;
        }
        if (n >= s.size() - i) {
            return false;
        }
        for (size_t k = 1; k <= n; ++k) {
            const auto cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool all_digits(std::string_view s) {
    for (char c : s) {
        if (!is_digit(c)) {
            return false;
        }
    }
    return !s.empty();
}

// A SemVer numeric identifier: "0" or digits without a leading zero.
bool numeric_id(std::string_view s) { return all_digits(s) && (s.size() == 1 || s[0] != '0'); }

bool ident_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

// Dot-separated, non-empty [0-9A-Za-z-] identifiers; in a pre-release, a
// purely numeric identifier must not have a leading zero.
bool dotted_ids(std::string_view s, bool pre_release) {
    if (s.empty()) {
        return false;
    }
    size_t start = 0;
    while (true) {
        size_t dot = s.find('.', start);
        std::string_view id = s.substr(start, dot == std::string_view::npos ? s.npos : dot - start);
        if (id.empty()) {
            return false;
        }
        for (char c : id) {
            if (!ident_char(c)) {
                return false;
            }
        }
        if (pre_release && all_digits(id) && !numeric_id(id)) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        start = dot + 1;
    }
}

// SemVer 2.0: MAJOR.MINOR.PATCH[-pre.release][+build.meta].
bool valid_semver(std::string_view v) {
    std::string_view build;
    if (size_t plus = v.find('+'); plus != std::string_view::npos) {
        build = v.substr(plus + 1);
        if (!dotted_ids(build, false)) {
            return false;
        }
        v = v.substr(0, plus);
    }
    if (size_t dash = v.find('-'); dash != std::string_view::npos) {
        if (!dotted_ids(v.substr(dash + 1), true)) {
            return false;
        }
        v = v.substr(0, dash);
    }
    size_t a = v.find('.');
    if (a == std::string_view::npos) {
        return false;
    }
    size_t b = v.find('.', a + 1);
    if (b == std::string_view::npos) {
        return false;
    }
    return numeric_id(v.substr(0, a)) && numeric_id(v.substr(a + 1, b - a - 1)) &&
           numeric_id(v.substr(b + 1));
}

// A plain file name: no separators, control bytes or '.'/'..'.
bool plain_name(std::string_view s) {
    if (s.empty() || s == "." || s == "..") {
        return false;
    }
    for (char ch : s) {
        const auto c = static_cast<uint8_t>(ch);
        if (c < 0x20 || c == 0x7F || ch == '/' || ch == '\\' || ch == ':') {
            return false;
        }
    }
    return valid_utf8(s);
}

// "assets/<name>[/<name>...]"
bool valid_icon(std::string_view icon) {
    constexpr std::string_view kPrefix = "assets/";
    if (icon.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    std::string_view rest = icon.substr(kPrefix.size());
    while (true) {
        size_t slash = rest.find('/');
        if (!plain_name(rest.substr(0, slash))) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        rest = rest.substr(slash + 1);
    }
}

bool valid_host(std::string_view h) {
    if (h.empty() || h.size() > kMaxHostBytes) {
        return false;
    }
    for (char c : h) {
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

// ^[a-z][a-z0-9_]{0,31}$ (agent-tools spec 5.1).
bool valid_tool_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxToolNameBytes) {
        return false;
    }
    if (name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    for (size_t i = 1; i < name.size(); ++i) {
        const char c = name[i];
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '_')) {
            return false;
        }
    }
    return true;
}

// The number of Unicode code points in `s`, which must already be valid
// UTF-8: counts the bytes that start a sequence (everything but a
// continuation byte, 10xxxxxx).
size_t count_code_points(std::string_view s) {
    size_t n = 0;
    for (char c : s) {
        if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

// Reads an optional string field into `out`. False if it is present but not
// a string.
bool string_field(const Value& root, std::string_view key, std::string& out, bool& present) {
    const Value* v = root.find(key);
    present = v != nullptr;
    if (v == nullptr) {
        return true;
    }
    if (v->type() != Value::Type::String) {
        return false;
    }
    out = v->as_string();
    return true;
}

bool json_number(const Value& value) {
    return value.type() == Value::Type::Int || value.type() == Value::Type::Double;
}

bool supported_tool_schema(const Value& schema, const std::string& where, std::string* why) {
    auto no = [&](const std::string& reason) {
        if (why != nullptr) *why = (where.empty() ? std::string("schema") : where) + ": " + reason;
        return false;
    };
    if (schema.type() != Value::Type::Map) return no("not an object");
    for (const auto& [key, value] : schema.as_map()) {
        (void)value;
        bool accepted = false;
        for (const char* known : {"type", "properties", "required", "enum", "minimum", "maximum",
                                  "maxLength", "items"}) {
            if (key == known) {
                accepted = true;
                break;
            }
        }
        if (!accepted) return no("unsupported keyword " + key);
    }
    auto known_type = [](const Value& type) {
        if (type.type() != Value::Type::String) return false;
        for (std::string_view known :
             {"object", "string", "integer", "number", "boolean", "array", "null"}) {
            if (type.as_string() == known) return true;
        }
        return false;
    };
    const Value* type = schema.find("type");
    if (type == nullptr) return no("no type");
    if (type->type() == Value::Type::List) {
        if (where.empty()) return no("type list at the root");
        if (type->as_list().empty()) return no("empty type list");
        for (const Value& item : type->as_list()) {
            if (!known_type(item)) return no("unsupported type in list");
        }
    } else if (type->type() != Value::Type::String) {
        return no("no type");
    } else if (!known_type(*type)) {
        return no("unsupported type " + type->as_string());
    }

    if (const Value* properties = schema.find("properties")) {
        if (properties->type() != Value::Type::Map) return no("properties is not an object");
        for (const auto& [name, child] : properties->as_map()) {
            const std::string path = where.empty() ? name : where + "." + name;
            if (!supported_tool_schema(child, path, why)) return false;
        }
    }
    if (const Value* required = schema.find("required")) {
        if (required->type() != Value::Type::List) return no("required is not an array");
        for (const Value& name : required->as_list()) {
            if (name.type() != Value::Type::String) return no("required holds a non-string");
        }
    }
    if (const Value* values = schema.find("enum");
        values != nullptr && (values->type() != Value::Type::List || values->as_list().empty())) {
        return no("enum is not a non-empty array");
    }
    for (const char* key : {"minimum", "maximum"}) {
        if (const Value* value = schema.find(key); value != nullptr && !json_number(*value)) {
            return no(std::string(key) + " is not a number");
        }
    }
    if (const Value* max_length = schema.find("maxLength");
        max_length != nullptr &&
        (max_length->type() != Value::Type::Int || max_length->as_int() < 0)) {
        return no("maxLength is not a non-negative integer");
    }
    if (const Value* items = schema.find("items")) {
        if (!supported_tool_schema(*items, where + "[]", why)) return false;
    }
    return true;
}

}  // namespace

bool scene_tool_schema_supported(const Value& schema, std::string* why) {
    return supported_tool_schema(schema, {}, why);
}

Result<void> validate_scene_tool_schemas(const SceneManifest& manifest) {
    for (size_t i = 0; i < manifest.tools.size(); ++i) {
        const SceneToolDecl& tool = manifest.tools[i];
        if (tool.parameters.empty()) continue;

        const std::string prefix = "tools[" + std::to_string(i) + "].parameters";
        auto schema = from_json(tool.parameters, kMaxManifestDepth);
        if (!schema) {
            return Result<void>::err(
                ErrorCode::InvalidArgument,
                prefix + ": schema is not valid JSON: " + schema.error().message);
        }
        std::string why;
        if (!scene_tool_schema_supported(schema.value(), &why)) {
            return Result<void>::err(ErrorCode::InvalidArgument, prefix + ": " + why);
        }
        const Value* type = schema.value().find("type");
        if (type == nullptr || type->type() != Value::Type::String ||
            type->as_string() != "object") {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     prefix + " must describe an object");
        }
    }
    return Result<void>::ok();
}

bool valid_scene_id(std::string_view id) {
    if (id.empty() || id.size() > kMaxSceneIdBytes) {
        return false;
    }
    for (char c : id) {
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

Result<SceneManifest> parse_scene_manifest(std::string_view json) {
    if (json.size() > kMaxManifestBytes) {
        return invalid("larger than 16 KB");
    }
    Result<Value> parsed = from_json(json, kMaxManifestDepth);
    if (!parsed) {
        if (parsed.error().code == ErrorCode::QuotaExceeded) {
            return invalid("nested deeper than " + std::to_string(kMaxManifestDepth) + " levels");
        }
        return invalid("not valid JSON: " + parsed.error().message);
    }
    const Value& root = parsed.value();
    if (root.type() != Value::Type::Map) {
        return invalid("not a JSON object");
    }

    SceneManifest m;
    bool present = false;

    if (!string_field(root, "id", m.id, present) || !present) {
        return invalid("id must be a string");
    }
    if (!valid_scene_id(m.id)) {
        return invalid("id must be 1-32 characters of a-z, 0-9, '_' or '-'");
    }

    if (!string_field(root, "name", m.name, present) || !present) {
        return invalid("name must be a string");
    }
    if (m.name.empty() || m.name.size() > kMaxNameBytes || !valid_utf8(m.name)) {
        return invalid("name must be 1-32 bytes of UTF-8");
    }

    if (!string_field(root, "version", m.version, present) || !present) {
        return invalid("version must be a string");
    }
    if (m.version.size() > kMaxVersionBytes || !valid_semver(m.version)) {
        return invalid("version must be a semantic version such as 1.0.0, at most 64 bytes");
    }

    const Value* schema = root.find("schema_version");
    if (schema == nullptr || schema->type() != Value::Type::Int || schema->as_int() < 1 ||
        schema->as_int() > kMaxSchemaVersion) {
        return invalid("schema_version must be an integer from 1 to 1000");
    }
    m.schema_version = static_cast<int>(schema->as_int());

    if (!string_field(root, "entry", m.entry, present)) {
        return invalid("entry must be a string");
    }
    if (m.entry.size() <= 4 || m.entry.substr(m.entry.size() - 4) != ".lua" ||
        !plain_name(m.entry)) {
        return invalid("entry must be a top-level .lua file");
    }

    if (!string_field(root, "icon", m.icon, present)) {
        return invalid("icon must be a string");
    }
    if (!m.icon.empty() && !valid_icon(m.icon)) {
        return invalid("icon must be a file under assets/");
    }

    if (const Value* perms = root.find("permissions")) {
        if (perms->type() != Value::Type::Map) {
            return invalid("permissions must be an object");
        }
        if (const Value* http = perms->find("http")) {
            if (http->type() != Value::Type::List) {
                return invalid("permissions.http must be an array of host names");
            }
            if (http->as_list().size() > kMaxHosts) {
                return invalid("permissions.http lists more than 16 hosts");
            }
            for (const Value& h : http->as_list()) {
                if (h.type() != Value::Type::String || !valid_host(h.as_string())) {
                    return invalid(
                        "permissions.http entries must be lower-case host names (a-z, 0-9, '.', "
                        "'-'), without scheme, port or path");
                }
                m.http_hosts.push_back(h.as_string());
            }
        }
    }

    if (const Value* bg = root.find("background")) {
        if (bg->type() != Value::Type::Bool) {
            return invalid("background must be a boolean");
        }
        m.background = bg->as_bool();
    }

    if (const Value* f = root.find("files")) {
        const std::string_view v = f->type() == Value::Type::String
                                       ? std::string_view(f->as_string())
                                       : std::string_view();
        if (v == "auto") {
            m.files = FilesWant::Auto;
        } else if (v == "sd") {
            m.files = FilesWant::Sd;
        } else if (v == "int") {
            m.files = FilesWant::Int;
        } else {
            return invalid("files must be \"auto\", \"sd\" or \"int\"");
        }
    }

    if (const Value* tools = root.find("tools")) {
        if (tools->type() != Value::Type::List) {
            return invalid("tools must be an array");
        }
        const List& list = tools->as_list();
        if (list.size() > kMaxSceneTools) {
            return invalid("tools lists more than 16 tools");
        }
        for (size_t i = 0; i < list.size(); ++i) {
            const Value& item = list[i];
            const std::string prefix = "tools[" + std::to_string(i) + "]";
            if (item.type() != Value::Type::Map) {
                return invalid(prefix + " must be an object");
            }

            SceneToolDecl decl;
            bool field_present = false;
            if (!string_field(item, "name", decl.name, field_present) || !field_present) {
                return invalid(prefix + ".name must be a string");
            }
            if (!valid_tool_name(decl.name)) {
                return invalid(prefix + ".name must match ^[a-z][a-z0-9_]{0,31}$");
            }
            for (const SceneToolDecl& existing : m.tools) {
                if (existing.name == decl.name) {
                    return invalid(prefix + ".name '" + decl.name + "' is already used");
                }
            }

            if (!string_field(item, "description", decl.description, field_present) ||
                !field_present) {
                return invalid(prefix + ".description must be a string");
            }
            const size_t description_cps = count_code_points(decl.description);
            if (!valid_utf8(decl.description) || description_cps < kMinToolDescriptionCodePoints ||
                description_cps > kMaxToolDescriptionCodePoints) {
                return invalid(prefix + ".description must be 1-512 code points of valid UTF-8");
            }

            if (const Value* params = item.find("parameters")) {
                if (params->type() != Value::Type::Map) {
                    return invalid(prefix + ".parameters must be an object");
                }
                decl.parameters = to_json(*params);
                if (decl.parameters.size() > kMaxToolSchemaBytes) {
                    return invalid(prefix + ".parameters is larger than 4096 bytes serialized");
                }
            }

            decl.timeout_ms = kDefaultToolTimeoutMs;
            if (const Value* timeout = item.find("timeout_ms")) {
                if (timeout->type() != Value::Type::Int || timeout->as_int() < kMinToolTimeoutMs ||
                    timeout->as_int() > kMaxToolTimeoutMs) {
                    return invalid(prefix + ".timeout_ms must be an integer from 1000 to 120000");
                }
                decl.timeout_ms = static_cast<uint32_t>(timeout->as_int());
            }

            m.tools.push_back(std::move(decl));
        }
    }

    return Result<SceneManifest>::ok(std::move(m));
}

const char* to_string(FilesWant w) {
    switch (w) {
        case FilesWant::Sd:
            return "sd";
        case FilesWant::Int:
            return "int";
        case FilesWant::Auto:
            break;
    }
    return "auto";
}

}  // namespace wisp::core
