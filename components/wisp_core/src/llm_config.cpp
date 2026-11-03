#include "wisp/core/llm_config.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "llm_config";
constexpr const char* kFile = "/secrets/llm.json";
constexpr size_t kMaxModelBytes = 128;

bool has_control_or_space(std::string_view s) {
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return true;
    }
    return false;
}

bool has_control(std::string_view s) {
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return true;
    }
    return false;
}

// Returns the normalized URL, or nullopt if it is not an acceptable
// http(s) base URL. Never logs or echoes its input in an error.
std::optional<std::string> normalize_base_url(std::string_view url) {
    std::string_view rest;
    if (url.starts_with("http://")) {
        rest = url.substr(7);
    } else if (url.starts_with("https://")) {
        rest = url.substr(8);
    } else {
        return std::nullopt;
    }
    if (has_control_or_space(url)) return std::nullopt;
    size_t host_end = rest.find_first_of("/?#");
    if (host_end == 0 || rest.empty()) return std::nullopt;

    std::string out(url);
    while (out.ends_with('/')) out.pop_back();
    return out;
}

// "scheme://host:port" of a URL normalize_base_url() accepted, with the
// scheme and host lowercased, user info dropped and the default port made
// explicit, so two URLs that reach the same server compare equal. "" for
// "" (no endpoint configured yet).
std::string origin_of(std::string_view url) {
    if (url.empty()) return {};
    const bool https = url.starts_with("https://");
    std::string_view authority = url.substr(https ? 8 : 7);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (size_t at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);
    }
    std::string_view host = authority;
    std::string_view port;
    size_t colon = authority.rfind(':');
    size_t bracket = authority.rfind(']');
    if (colon != std::string_view::npos && (bracket == std::string_view::npos || colon > bracket)) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (port.empty()) port = https ? "443" : "80";
    std::string out = https ? "https://" : "http://";
    for (char c : host) out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    out += ':';
    out += port;
    return out;
}

// API keys go into an Authorization header verbatim: only visible ASCII
// (no spaces, CR, LF or other control bytes) is accepted.
bool valid_api_key(std::string_view key) {
    for (char c : key) {
        auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f) return false;
    }
    return true;
}

std::string string_field(const Value& doc, std::string_view name) {
    const Value* v = doc.find(name);
    return v != nullptr && v->type() == Value::Type::String ? v->as_string() : std::string();
}

}  // namespace

LlmConfigService::LlmConfigService(StorageManager& storage) : storage_(storage) {}

std::string LlmConfigService::file_path() const {
    auto root = storage_.write_root(Area::System);
    return root ? root.value() + kFile : std::string();
}

Result<void> LlmConfigService::init() {
    std::lock_guard<os::Mutex> lock(mutex_);
    current_ = Stored{};

    std::string path = file_path();
    if (path.empty()) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    }
    auto bytes = storage_.read_file(path);
    if (!bytes) {
        if (bytes.error().code == ErrorCode::NotFound) return Result<void>::ok();
        return Result<void>::err(bytes.error().code,
                                 "reading secrets/llm.json: " + bytes.error().message);
    }
    auto doc = from_json(std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                                          bytes.value().size()));
    if (!doc || doc.value().type() != Value::Type::Map) {
        // The parse error could quote part of the file, so only say that
        // it failed.
        WLOG_W(kTag, "secrets/llm.json is corrupt; starting with an empty config");
        return Result<void>::ok();
    }

    // Re-validate what was stored so a hand-edited file cannot smuggle in
    // a value set() would have refused.
    Stored loaded;
    if (auto url = normalize_base_url(string_field(doc.value(), "base_url"))) {
        loaded.base_url = std::move(*url);
    }
    std::string model = string_field(doc.value(), "model");
    if (model.size() <= kMaxModelBytes && !has_control(model)) loaded.model = std::move(model);
    std::string key = string_field(doc.value(), "api_key");
    if (valid_api_key(key)) loaded.api_key = std::move(key);
    current_ = std::move(loaded);
    WLOG_I(kTag, "loaded LLM config (key %s)", current_.api_key.empty() ? "unset" : "set");
    return Result<void>::ok();
}

LlmConfig LlmConfigService::get() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return LlmConfig{current_.base_url, current_.model, !current_.api_key.empty()};
}

Result<void> LlmConfigService::set(const LlmConfigUpdate& u) {
    std::lock_guard<os::Mutex> lock(mutex_);
    Stored next = current_;
    if (u.base_url) {
        auto url = normalize_base_url(*u.base_url);
        if (!url) {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     "base_url must be an http:// or https:// URL");
        }
        next.base_url = std::move(*url);
        // The key belongs to the server it was saved for. Moving to another
        // origin without a new key forgets it rather than sending it to a
        // host the owner never gave it to.
        if (!u.api_key && origin_of(next.base_url) != origin_of(current_.base_url)) {
            next.api_key.clear();
        }
    }
    if (u.model) {
        if (u.model->size() > kMaxModelBytes || has_control(*u.model)) {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     "model must be at most 128 bytes of text");
        }
        next.model = *u.model;
    }
    if (u.api_key) {
        if (!valid_api_key(*u.api_key)) {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     "api_key must be printable ASCII without spaces");
        }
        next.api_key = *u.api_key;
    }

    WISP_TRY(persist(next));
    bool key_changed = next.api_key != current_.api_key;
    current_ = std::move(next);
    WLOG_I(kTag, "LLM config updated (key %s%s)", current_.api_key.empty() ? "unset" : "set",
           key_changed ? ", changed" : "");
    return Result<void>::ok();
}

Result<void> LlmConfigService::persist(const Stored& next) {
    std::string path = file_path();
    if (path.empty()) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    }
    std::string json = to_json_raw(
        Map{{"base_url", next.base_url}, {"model", next.model}, {"api_key", next.api_key}});
    auto r = storage_.write_atomic(path, std::vector<uint8_t>(json.begin(), json.end()));
    if (!r) {
        WLOG_E(kTag, "saving secrets/llm.json failed: %s", r.error().message.c_str());
    }
    return r;
}

std::string LlmConfigService::authorization_for_request() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return current_.api_key.empty() ? std::string() : "Bearer " + current_.api_key;
}

void LlmConfigService::clear() {
    std::lock_guard<os::Mutex> lock(mutex_);
    current_ = Stored{};
    std::string path = file_path();
    if (path.empty()) return;
    // ".new" first: read_file() would promote a leftover ".new" if the
    // target were gone, resurrecting the old key.
    for (const std::string& p : {path + ".new", path + ".tmp", path}) {
        if (std::remove(p.c_str()) != 0 && errno != ENOENT) {
            WLOG_W(kTag, "deleting %s failed (errno %d)", p.c_str(), errno);
        }
    }
    WLOG_I(kTag, "LLM config cleared");
}

}  // namespace wisp::core
