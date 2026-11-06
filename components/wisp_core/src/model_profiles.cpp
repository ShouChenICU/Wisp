#include "wisp/core/model_profiles.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/flash_guard.hpp"
#include "wisp/os/large.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "models";
constexpr const char* kFile = "/secrets/models.json";
// Early sub-project 2 builds kept a single LLM config here.
constexpr const char* kLegacyFile = "/secrets/llm.json";
constexpr int64_t kFileVersion = 2;
constexpr size_t kIdBytes = 4;  // 8 hex characters

using Profile = ModelProfile;

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }

template <typename T>
Result<T> fail(const Error& e) {
    return Result<T>::err(e.code, e.message);
}

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

// Well-formed UTF-8: no stray continuation bytes, overlong forms,
// surrogates or code points past U+10FFFF.
bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// The normalized URL, or nullopt if it is not an acceptable http(s) base
// URL. Never logs or echoes its input.
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
    // A host must remain once user info and the port are taken off, the
    // same way origin_of() reads it.
    std::string_view host = rest.substr(0, host_end);
    if (size_t at = host.rfind('@'); at != std::string_view::npos) host.remove_prefix(at + 1);
    size_t colon = host.rfind(':');
    size_t bracket = host.rfind(']');
    if (colon != std::string_view::npos && (bracket == std::string_view::npos || colon > bracket)) {
        host = host.substr(0, colon);
    }
    if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
    if (host.empty()) return std::nullopt;

    std::string out(url);
    while (out.ends_with('/')) out.pop_back();
    return out;
}

// "scheme://host:port" of a URL normalize_base_url() accepted, with the
// scheme and host lowercased, user info dropped and the default port made
// explicit, so two URLs that reach the same server compare equal. "" for
// "" (no endpoint yet).
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

bool valid_id(std::string_view id) {
    if (id.size() != 2 * kIdBytes) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

Result<void> check_name(std::string_view name) {
    if (name.empty() || name.size() > ModelProfileService::kMaxNameBytes || has_control(name) ||
        !valid_utf8(name)) {
        return fail<void>(invalid("name must be 1-32 bytes of UTF-8 text"));
    }
    return Result<void>::ok();
}

Result<void> check_model(std::string_view model) {
    if (model.size() > ModelProfileService::kMaxModelBytes || has_control(model) ||
        !valid_utf8(model)) {
        return fail<void>(invalid("model must be at most 128 bytes of text"));
    }
    return Result<void>::ok();
}

// A decimal string in kMinContextWindow..kMaxContextWindow, digits only.
std::optional<uint32_t> parse_context_window(std::string_view s) {
    if (s.empty() || s.size() > 7) return std::nullopt;
    uint32_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + static_cast<uint32_t>(c - '0');
    }
    if (n < kMinContextWindow || n > kMaxContextWindow) return std::nullopt;
    return n;
}

Result<void> check_options(const ModelOptions& options) {
    if (options.size() > ModelProfileService::kMaxOptions) {
        return fail<void>(invalid("options may have at most 8 keys"));
    }
    for (const auto& [k, v] : options) {
        if (k.empty() || k.size() > ModelProfileService::kMaxOptionKeyBytes ||
            has_control_or_space(k) || !valid_utf8(k)) {
            return fail<void>(invalid("option keys must be 1-15 bytes without spaces"));
        }
        if (v.size() > ModelProfileService::kMaxOptionValueBytes || has_control(v) ||
            !valid_utf8(v)) {
            return fail<void>(invalid("option values must be at most 64 bytes of text"));
        }
    }
    if (auto it = options.find(kOptVision); it != options.end()) {
        if (it->second != "true" && it->second != "false")
            return fail<void>(invalid("vision must be \"true\" or \"false\""));
    }
    if (auto it = options.find(kOptContextWindow); it != options.end()) {
        if (!parse_context_window(it->second)) {
            return fail<void>(
                invalid("context_window must be a whole number from 4096 to 1048576"));
        }
    }
    return Result<void>::ok();
}

Result<std::string> check_base_url(std::string_view url) {
    if (url.find('<') != std::string_view::npos) {
        return fail<std::string>(
            invalid("base_url still contains a placeholder; put in the real address"));
    }
    auto normalized = normalize_base_url(url);
    if (!normalized) {
        return fail<std::string>(invalid("base_url must be an http:// or https:// URL"));
    }
    return Result<std::string>::ok(std::move(*normalized));
}

Result<void> check_api_key(std::string_view key) {
    if (!valid_api_key(key)) {
        return fail<void>(invalid("api_key must be printable ASCII without spaces"));
    }
    return Result<void>::ok();
}

const ModelPreset* preset_for(std::string_view id, ModelKind kind) {
    const ModelPreset* p = find_model_preset(id);
    return p != nullptr && p->supports(kind) ? p : nullptr;
}

std::string string_field(const Value& doc, std::string_view name) {
    const Value* v = doc.find(name);
    return v != nullptr && v->type() == Value::Type::String ? v->as_string() : std::string();
}

Value active_value(const std::array<std::string, kModelKindCount>& active) {
    Map m;
    for (ModelKind k : kModelKinds) m[to_string(k)] = Value(active[static_cast<size_t>(k)]);
    return Value(std::move(m));
}

// Deletes a JSON file written by write_atomic() along with its leftovers.
// ".new" first: read_file() would promote a leftover ".new" if the target
// were gone, resurrecting what was deleted.
// Raw std::remove(), since StorageManager reserves the ".new"/".tmp" names,
// but flash all the same: flash thread only.
void remove_file(const std::string& path) {
    WISP_ASSERT_FLASH_THREAD("ModelProfileService remove_file");
    for (const std::string& p : {path + ".new", path + ".tmp", path}) {
        if (std::remove(p.c_str()) != 0 && errno != ENOENT) {
            WLOG_W(kTag, "deleting %s failed (errno %d)", p.c_str(), errno);
        }
    }
}

}  // namespace

bool profile_vision(const ModelOptions& options) {
    auto it = options.find(kOptVision);
    return it != options.end() && it->second == "true";
}

uint32_t profile_context_window(const ModelOptions& options) {
    auto it = options.find(kOptContextWindow);
    if (it == options.end()) return kDefaultContextWindow;
    return parse_context_window(it->second).value_or(kDefaultContextWindow);
}

const char* to_string(ModelKind kind) {
    switch (kind) {
        case ModelKind::Llm:
            return "llm";
        case ModelKind::Asr:
            return "asr";
        case ModelKind::Tts:
            return "tts";
    }
    return "llm";
}

std::optional<ModelKind> parse_model_kind(std::string_view s) {
    for (ModelKind k : kModelKinds) {
        if (s == to_string(k)) return k;
    }
    return std::nullopt;
}

const char* to_string(ModelFailure failure) {
    switch (failure) {
        case ModelFailure::None:
            return "";
        case ModelFailure::LimitReached:
            return "limit_reached";
        case ModelFailure::ActiveProfile:
            return "active_profile";
        case ModelFailure::KeyOriginMismatch:
            return "key_origin_mismatch";
    }
    return "";
}

Value to_value(const ModelProfile& p) {
    Map options;
    for (const auto& [k, v] : p.options) options[k] = Value(v);
    return Value(Map{
        {"id", Value(p.id)},
        {"kind", Value(to_string(p.kind))},
        {"name", Value(p.name)},
        {"preset", Value(p.preset)},
        {"base_url", Value(p.base_url)},
        {"model", Value(p.model)},
        {"options", Value(std::move(options))},
        {"has_key", Value(p.has_key)},
    });
}

Value to_value(const ProfilesState& state) {
    List profiles;
    for (const ModelProfile& p : state.profiles) profiles.push_back(to_value(p));
    return Value(
        Map{{"active", active_value(state.active)}, {"profiles", Value(std::move(profiles))}});
}

Value to_value(const ModelPreset& preset) {
    List kinds;
    for (ModelKind k : kModelKinds) {
        if (preset.supports(k)) kinds.push_back(Value(to_string(k)));
    }
    return Value(Map{
        {"id", Value(std::string(preset.id))},
        {"name", Value(std::string(preset.name))},
        {"kinds", Value(std::move(kinds))},
        {"base_url", Value(std::string(preset.base_url))},
        {"key_required", Value(preset.key_required)},
    });
}

Map profiles_event_payload(const ProfilesState& state) {
    return Map{{"active", active_value(state.active)},
               {"count", Value(static_cast<int64_t>(state.profiles.size()))}};
}

ModelProfileService::ModelProfileService(StorageManager& storage, EventBus& bus,
                                         ModelRandomFn random, LatestSave::SubmitIo submit_io)
    : storage_(storage), bus_(bus), random_(std::move(random)), saver_(std::move(submit_io)) {}

std::string ModelProfileService::system_root() const {
    auto root = storage_.write_root(Area::System);
    return root ? root.value() : std::string();
}

ModelProfileService::Stored* ModelProfileService::find(State& state, std::string_view id) {
    for (Stored& s : state.profiles) {
        if (s.profile.id == id) return &s;
    }
    return nullptr;
}

const ModelProfileService::Stored* ModelProfileService::find(const State& state,
                                                             std::string_view id) const {
    for (const Stored& s : state.profiles) {
        if (s.profile.id == id) return &s;
    }
    return nullptr;
}

std::string ModelProfileService::new_id(const State& state) {
    static constexpr char kHex[] = "0123456789abcdef";
    auto to_hex = [](const uint8_t (&bytes)[kIdBytes]) {
        std::string id;
        for (uint8_t b : bytes) {
            id += kHex[b >> 4];
            id += kHex[b & 0x0F];
        }
        return id;
    };
    // Random ids, but only a few tries: a broken random source that keeps
    // colliding must not spin under the lock.
    constexpr int kRandomTries = 8;
    for (int i = 0; random_ && i < kRandomTries; ++i) {
        uint8_t bytes[kIdBytes] = {};
        random_(bytes, sizeof(bytes));
        std::string id = to_hex(bytes);
        if (find(state, id) == nullptr) return id;
    }
    // Ids only need to be unique on this device, not unguessable. The mix
    // is a bijection on 32 bits, so consecutive counter values give
    // distinct ids and at most kMaxProfiles + 1 tries find a free one.
    if (fallback_counter_ == 0) fallback_counter_ = static_cast<uint32_t>(os::now_ms()) | 1u;
    for (;;) {
        uint32_t x = fallback_counter_++;
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        uint8_t bytes[kIdBytes] = {};
        for (uint8_t& b : bytes) {
            b = static_cast<uint8_t>(x);
            x >>= 8;
        }
        std::string id = to_hex(bytes);
        if (find(state, id) == nullptr) return id;
    }
}

Result<bool> ModelProfileService::load_models(const std::vector<uint8_t>& bytes) {
    auto doc =
        from_json(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    const Value* list = doc ? doc.value().find("profiles") : nullptr;
    if (!doc || list == nullptr || list->type() != Value::Type::List) {
        // The parse error could quote part of the file (keys included), so
        // only say that it failed.
        WLOG_W(kTag, "secrets/models.json is corrupt; starting without model profiles");
        return Result<bool>::ok(false);
    }

    // Re-validate everything so a hand-edited file cannot smuggle in what
    // create() would have refused.
    State loaded;
    size_t dropped = 0;
    for (const Value& e : list->as_list()) {
        if (loaded.profiles.size() >= kMaxProfiles) {
            ++dropped;
            continue;
        }
        Stored s;
        Profile& p = s.profile;
        p.id = string_field(e, "id");
        auto kind = parse_model_kind(string_field(e, "kind"));
        p.name = string_field(e, "name");
        p.preset = string_field(e, "preset");
        std::string url = string_field(e, "base_url");
        p.model = string_field(e, "model");
        s.api_key = string_field(e, "api_key");
        bool ok = valid_id(p.id) && find(loaded, p.id) == nullptr && kind.has_value() &&
                  check_name(p.name) && preset_for(p.preset, *kind) != nullptr &&
                  check_model(p.model) && check_api_key(s.api_key);
        if (ok && !url.empty()) {
            auto normalized = check_base_url(url);
            ok = static_cast<bool>(normalized);
            if (ok) p.base_url = std::move(normalized.value());
        }
        if (ok) {
            if (const Value* opts = e.find("options"); opts != nullptr) {
                if (opts->type() != Value::Type::Map) ok = false;
                for (const auto& [k, v] : ok ? opts->as_map() : Map{}) {
                    if (v.type() != Value::Type::String) {
                        ok = false;
                        break;
                    }
                    p.options[k] = v.as_string();
                }
                ok = ok && check_options(p.options);
            }
        }
        if (!ok) {
            ++dropped;
            continue;
        }
        p.kind = *kind;
        p.has_key = !s.api_key.empty();
        loaded.profiles.push_back(std::move(s));
    }
    if (dropped > 0) WLOG_W(kTag, "dropped %u invalid model profiles", unsigned(dropped));

    const Value* active = doc.value().find("active");
    for (ModelKind k : kModelKinds) {
        std::string id = active != nullptr ? string_field(*active, to_string(k)) : std::string();
        const Stored* s = find(loaded, id);
        if (s == nullptr || s->profile.kind != k) {
            // A kind with profiles always has an active one.
            id.clear();
            for (const Stored& c : loaded.profiles) {
                if (c.profile.kind == k) {
                    id = c.profile.id;
                    break;
                }
            }
        }
        loaded.active[static_cast<size_t>(k)] = std::move(id);
    }
    state_ = std::move(loaded);
    return Result<bool>::ok(true);
}

Result<void> ModelProfileService::migrate_legacy(const std::string& root, bool over_corrupt) {
    const std::string legacy = root + kLegacyFile;
    auto bytes = storage_.read_file(legacy);
    if (!bytes) {
        if (bytes.error().code == ErrorCode::NotFound) return Result<void>::ok();
        return Result<void>::err(bytes.error().code,
                                 "reading secrets/llm.json: " + bytes.error().message);
    }
    if (over_corrupt) {
        WLOG_W(kTag, "replacing the unreadable secrets/models.json from secrets/llm.json");
    }
    auto doc = from_json(std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                                          bytes.value().size()));
    State next;
    if (!doc || doc.value().type() != Value::Type::Map) {
        WLOG_W(kTag, "secrets/llm.json is corrupt; not migrating it");
    } else {
        Stored s;
        Profile& p = s.profile;
        // A URL create() would refuse (a placeholder, say) is left out
        // rather than migrated and then dropped on the next load; the key
        // then has no origin until the first base_url is saved.
        if (auto url = check_base_url(string_field(doc.value(), "base_url"))) {
            p.base_url = std::move(url.value());
        }
        std::string model = string_field(doc.value(), "model");
        if (check_model(model)) p.model = std::move(model);
        std::string key = string_field(doc.value(), "api_key");
        if (valid_api_key(key)) s.api_key = std::move(key);
        if (!p.base_url.empty() || !p.model.empty() || !s.api_key.empty()) {
            p.id = new_id(next);
            p.kind = ModelKind::Llm;
            p.name = "Default";
            p.preset = "custom";
            p.has_key = !s.api_key.empty();
            next.active[static_cast<size_t>(ModelKind::Llm)] = p.id;
            next.profiles.push_back(std::move(s));
        }
    }
    // The old file goes only once the new one holds its contents; until
    // then the next boot migrates again.
    (void)persist(next, legacy);
    state_ = std::move(next);
    return Result<void>::ok();
}

Result<void> ModelProfileService::init() {
    std::lock_guard<os::Mutex> lock(mutex_);
    state_ = State{};

    const std::string root = system_root();
    if (root.empty()) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    }
    auto bytes = storage_.read_file(root + kFile);
    if (!bytes) {
        if (bytes.error().code == ErrorCode::NotFound) return migrate_legacy(root);
        return Result<void>::err(bytes.error().code,
                                 "reading secrets/models.json: " + bytes.error().message);
    }
    auto parsed = load_models(bytes.value());
    if (!parsed) return Result<void>::err(parsed.error().code, parsed.error().message);
    // An unreadable new file next to a legacy one: the legacy file may hold
    // the only good key, so migrate it again over the unreadable file. With
    // no legacy file the unreadable one is left alone.
    if (!parsed.value()) return migrate_legacy(root, /*over_corrupt=*/true);
    // A migration interrupted after writing the new file.
    remove_file(root + kLegacyFile);
    WLOG_I(kTag, "loaded %u model profiles", unsigned(state_.profiles.size()));
    return Result<void>::ok();
}

ProfilesState ModelProfileService::get() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    ProfilesState out;
    out.active = state_.active;
    for (const Stored& s : state_.profiles) out.profiles.push_back(s.profile);
    return out;
}

Result<ModelProfile> ModelProfileService::create(const ProfileInput& in, ModelFailure* why) {
    if (why != nullptr) *why = ModelFailure::None;
    using R = Result<ModelProfile>;
    if (in.api_key && in.api_key_from) {
        return fail<ModelProfile>(invalid("give api_key or api_key_from, not both"));
    }
    const ModelPreset* preset = preset_for(in.preset, in.kind);
    if (preset == nullptr)
        return fail<ModelProfile>(invalid("preset is unknown or does not support this kind"));
    if (auto r = check_name(in.name); !r) return fail<ModelProfile>(r.error());
    if (in.model) {
        if (auto r = check_model(*in.model); !r) return fail<ModelProfile>(r.error());
    }
    if (in.options) {
        if (auto r = check_options(*in.options); !r) return fail<ModelProfile>(r.error());
    }
    if (in.api_key) {
        if (auto r = check_api_key(*in.api_key); !r) return fail<ModelProfile>(r.error());
    }
    const std::string_view raw_url =
        in.base_url ? std::string_view(*in.base_url) : preset->base_url;
    if (raw_url.empty()) return fail<ModelProfile>(invalid("base_url is required for this preset"));
    auto url = check_base_url(raw_url);
    if (!url) return fail<ModelProfile>(url.error());

    std::lock_guard<os::Mutex> lock(mutex_);
    if (state_.profiles.size() >= kMaxProfiles) {
        if (why != nullptr) *why = ModelFailure::LimitReached;
        return R::err(ErrorCode::QuotaExceeded, "at most 16 model profiles");
    }
    State next = state_;
    Stored s;
    Profile& p = s.profile;
    p.kind = in.kind;
    p.name = in.name;
    p.preset = in.preset;
    p.base_url = std::move(url.value());
    if (in.model) p.model = *in.model;
    if (in.options) p.options = *in.options;
    if (in.api_key) s.api_key = *in.api_key;
    if (in.api_key_from) {
        const Stored* src = find(state_, *in.api_key_from);
        if (src == nullptr || src->api_key.empty() ||
            origin_of(src->profile.base_url) != origin_of(p.base_url)) {
            if (why != nullptr) *why = ModelFailure::KeyOriginMismatch;
            return fail<ModelProfile>(
                invalid("api_key_from must name a profile with a key on the same origin"));
        }
        s.api_key = src->api_key;
    }
    p.has_key = !s.api_key.empty();
    p.id = new_id(next);
    std::string& active = next.active[static_cast<size_t>(p.kind)];
    if (active.empty()) active = p.id;
    Profile out = p;
    next.profiles.push_back(std::move(s));

    WISP_TRY(persist(next));
    state_ = std::move(next);
    WLOG_I(kTag, "created %s profile %s (key %s)", to_string(out.kind), out.id.c_str(),
           out.has_key ? "set" : "unset");
    publish(state_);
    return R::ok(std::move(out));
}

Result<ModelProfile> ModelProfileService::update(std::string_view id, const ProfileUpdate& u,
                                                 ModelFailure* why) {
    if (why != nullptr) *why = ModelFailure::None;
    using R = Result<ModelProfile>;
    if (u.api_key && u.api_key_from) {
        return fail<ModelProfile>(invalid("give api_key or api_key_from, not both"));
    }
    if (u.name) {
        if (auto r = check_name(*u.name); !r) return fail<ModelProfile>(r.error());
    }
    if (u.model) {
        if (auto r = check_model(*u.model); !r) return fail<ModelProfile>(r.error());
    }
    if (u.options) {
        if (auto r = check_options(*u.options); !r) return fail<ModelProfile>(r.error());
    }
    if (u.api_key) {
        if (auto r = check_api_key(*u.api_key); !r) return fail<ModelProfile>(r.error());
    }
    std::optional<std::string> url;
    if (u.base_url) {
        auto checked = check_base_url(*u.base_url);
        if (!checked) return fail<ModelProfile>(checked.error());
        url = std::move(checked.value());
    }

    std::lock_guard<os::Mutex> lock(mutex_);
    State next = state_;
    Stored* s = find(next, id);
    if (s == nullptr) return R::err(ErrorCode::NotFound, "no such model profile");
    Profile& p = s->profile;
    if (u.preset) {
        if (preset_for(*u.preset, p.kind) == nullptr) {
            return fail<ModelProfile>(invalid("preset is unknown or does not support this kind"));
        }
        p.preset = *u.preset;
    }
    if (u.name) p.name = *u.name;
    if (u.model) p.model = *u.model;
    if (u.options) p.options = *u.options;
    const std::string old_key = s->api_key;
    if (url) {
        // The key belongs to the server it was saved for. Moving to another
        // origin without a new key forgets it rather than sending it to a
        // host the owner never gave it to.
        if (!u.api_key && !u.api_key_from && origin_of(*url) != origin_of(p.base_url)) {
            s->api_key.clear();
        }
        p.base_url = std::move(*url);
    }
    if (u.api_key_from) {
        const Stored* src = find(state_, *u.api_key_from);
        if (src == nullptr || src->api_key.empty() || p.base_url.empty() ||
            origin_of(src->profile.base_url) != origin_of(p.base_url)) {
            if (why != nullptr) *why = ModelFailure::KeyOriginMismatch;
            return fail<ModelProfile>(
                invalid("api_key_from must name a profile with a key on the same origin"));
        }
        s->api_key = src->api_key;
    }
    if (u.api_key) s->api_key = *u.api_key;
    p.has_key = !s->api_key.empty();
    Profile out = p;
    const bool key_changed = s->api_key != old_key;

    WISP_TRY(persist(next));
    state_ = std::move(next);
    WLOG_I(kTag, "updated profile %s (key %s%s)", out.id.c_str(), out.has_key ? "set" : "unset",
           key_changed ? ", changed" : "");
    publish(state_);
    return R::ok(std::move(out));
}

Result<void> ModelProfileService::remove(std::string_view id, ModelFailure* why) {
    if (why != nullptr) *why = ModelFailure::None;
    std::lock_guard<os::Mutex> lock(mutex_);
    const Stored* s = find(state_, id);
    if (s == nullptr) return Result<void>::err(ErrorCode::NotFound, "no such model profile");
    const ModelKind kind = s->profile.kind;
    State next = state_;
    std::string& active = next.active[static_cast<size_t>(kind)];
    size_t same_kind = 0;
    for (const Stored& c : state_.profiles) same_kind += c.profile.kind == kind ? 1 : 0;
    if (active == id) {
        if (same_kind > 1) {
            if (why != nullptr) *why = ModelFailure::ActiveProfile;
            return Result<void>::err(ErrorCode::Busy,
                                     "this is the active profile; activate another one first");
        }
        active.clear();
    }
    std::erase_if(next.profiles, [&](const Stored& c) { return c.profile.id == id; });

    WISP_TRY(persist(next));
    state_ = std::move(next);
    WLOG_I(kTag, "removed profile %.*s", static_cast<int>(id.size()), id.data());
    publish(state_);
    return Result<void>::ok();
}

Result<void> ModelProfileService::activate(std::string_view id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    const Stored* s = find(state_, id);
    if (s == nullptr) return Result<void>::err(ErrorCode::NotFound, "no such model profile");
    // `s` points into state_, which the move below frees.
    const ModelKind kind = s->profile.kind;
    State next = state_;
    next.active[static_cast<size_t>(kind)] = s->profile.id;

    WISP_TRY(persist(next));
    state_ = std::move(next);
    WLOG_I(kTag, "activated %s profile %.*s", to_string(kind), static_cast<int>(id.size()),
           id.data());
    publish(state_);
    return Result<void>::ok();
}

void ModelProfileService::set_prober(ModelProber prober) {
    std::lock_guard<os::Mutex> lock(mutex_);
    prober_ = std::move(prober);
}

Result<ModelProbe> ModelProfileService::test(std::string_view id) const {
    ModelProber prober;
    ResolvedProfile target;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        const Stored* s = find(state_, id);
        if (s == nullptr)
            return Result<ModelProbe>::err(ErrorCode::NotFound, "no such model profile");
        if (!prober_) {
            return Result<ModelProbe>::err(ErrorCode::Unsupported,
                                           "no HTTP client on this platform");
        }
        prober = prober_;
        target = ResolvedProfile{s->profile.id, s->profile.base_url, s->profile.model,
                                 s->profile.options, s->api_key};
    }
    // Up to the prober's timeout (10 s): never under the lock.
    return Result<ModelProbe>::ok(prober(target));
}

std::optional<ResolvedProfile> ModelProfileService::active_for_request(ModelKind kind) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    const Stored* s = find(state_, state_.active[static_cast<size_t>(kind)]);
    if (s == nullptr) return std::nullopt;
    return ResolvedProfile{s->profile.id, s->profile.base_url, s->profile.model, s->profile.options,
                           s->api_key};
}

void ModelProfileService::clear() {
    std::lock_guard<os::Mutex> lock(mutex_);
    state_ = State{};
    // Through the saver, so a write still queued cannot bring the keys back
    // after the files are gone.
    (void)saver_.save([&storage = storage_]() -> Result<void> {
        auto root = storage.write_root(Area::System);
        if (root) {
            remove_file(root.value() + kFile);
            remove_file(root.value() + kLegacyFile);
        }
        return Result<void>::ok();
    });
    WLOG_I(kTag, "model profiles cleared");
    publish(state_);
}

Result<void> ModelProfileService::persist(const State& next, std::string migrated_from) {
    List profiles;
    for (const Stored& s : next.profiles) {
        Map options;
        for (const auto& [k, v] : s.profile.options) options[k] = Value(v);
        profiles.push_back(Value(Map{
            {"id", Value(s.profile.id)},
            {"kind", Value(to_string(s.profile.kind))},
            {"name", Value(s.profile.name)},
            {"preset", Value(s.profile.preset)},
            {"base_url", Value(s.profile.base_url)},
            {"model", Value(s.profile.model)},
            {"options", Value(std::move(options))},
            {"api_key", Value(s.api_key)},
        }));
    }
    const std::string json = to_json_raw(Map{{"version", Value(kFileVersion)},
                                             {"active", active_value(next.active)},
                                             {"profiles", Value(std::move(profiles))}});
    // The write runs on the flash thread, possibly after this service is
    // gone: it owns its snapshot and finds the path itself.
    return saver_.save([&storage = storage_, snapshot = os::LargeString(json.begin(), json.end()),
                        migrated_from = std::move(migrated_from)]() -> Result<void> {
        auto root = storage.write_root(Area::System);
        if (!root) {
            WLOG_E(kTag, "saving secrets/models.json failed: system storage unavailable");
            return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
        }
        auto r = storage.write_atomic(
            root.value() + kFile,
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(snapshot.data()),
                                     snapshot.size()));
        if (!r) {
            WLOG_E(kTag, "saving secrets/models.json failed: %s", r.error().message.c_str());
            return r;
        }
        if (!migrated_from.empty()) {
            remove_file(migrated_from);
            WLOG_I(kTag, "migrated secrets/llm.json to secrets/models.json");
        }
        return r;
    });
}

void ModelProfileService::publish(const State& state) {
    ProfilesState view;
    view.active = state.active;
    view.profiles.resize(state.profiles.size());
    bus_.publish({"model.profiles", "models", 0, profiles_event_payload(view)});
}

}  // namespace wisp::core
