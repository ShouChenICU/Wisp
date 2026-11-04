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

}  // namespace

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
    Result<Value> parsed = from_json(json);
    if (!parsed) {
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
    if (schema == nullptr || schema->type() != Value::Type::Int || schema->as_int() != 1) {
        return invalid("schema_version must be 1");
    }
    m.schema_version = 1;

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

    return Result<SceneManifest>::ok(std::move(m));
}

}  // namespace wisp::core
