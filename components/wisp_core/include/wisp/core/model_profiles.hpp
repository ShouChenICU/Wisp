#pragma once

// ModelProfileService (spec 5.3): the saved model profiles -- endpoint,
// model, extra options and API key -- for each kind of model the device
// uses: "llm" (chat), "asr" (speech recognition) and "tts" (speech
// synthesis). Every kind has its own active profile; activating one never
// touches the other kinds.
//
// Everything, keys included, lives in the System area as
// "secrets/models.json":
//   {"version":2, "active":{"llm":id,"asr":id,"tts":id},
//    "profiles":[{id,kind,name,preset,base_url,model,options,api_key}]}
// A "secrets/llm.json" from early sub-project 2 builds
// ({base_url, model, api_key}) is migrated at init() into one active llm
// profile (preset "custom", name "Default"); the old file is deleted once
// the new one is written.
//
// API keys are write-only. Nothing returned by a public method carries a
// key or any part of it, except active_for_request(), which exists only
// for the clients that send model requests (the sub-project 4 agent, the
// sub-project 5 voice path) -- REST routes, Lua bindings and agent tools
// must never call it. test() hands the key to the prober and nowhere
// else. Nothing here logs a key, a file's contents or a base_url+key
// pair, and no error message echoes a key.
//
// A key belongs to the origin (scheme://host:port; scheme and host
// case-insensitive, default ports made explicit, path ignored) of the
// base_url it was saved with. An update() that moves base_url to another
// origin without a new api_key clears the profile's key, as does the
// first base_url given to a profile whose key was stored without one
// (only possible for a migrated legacy config). api_key_from copies
// another profile's key inside the device, only between profiles on the
// same origin.
//
// Every successful change publishes "model.profiles" {active, count} --
// no keys, no addresses.
//
// Thread-safe: the admin HTTP handlers and request-sending code may call
// in concurrently. test() runs the prober outside the lock.
//
// Saving (spec 3.5 of docs/superpowers/specs/2026-11-06-flash-thread-design.md):
// with a `submit_io`, a change is applied in memory and its file write
// queued on the flash thread (core::LatestSave), the newest state winning;
// init() reads flash and must itself run on the flash thread or before it
// starts.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/latest_save.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class ModelKind : uint8_t { Llm, Asr, Tts };
constexpr size_t kModelKindCount = 3;
constexpr std::array<ModelKind, kModelKindCount> kModelKinds = {ModelKind::Llm, ModelKind::Asr,
                                                                ModelKind::Tts};

// "llm", "asr", "tts".
const char* to_string(ModelKind kind);
std::optional<ModelKind> parse_model_kind(std::string_view s);

// One row of the compiled-in platform table (spec 5.3). `name` is the
// platform's brand name and is not translated. A base_url containing '<'
// is a placeholder the user must replace before saving.
struct ModelPreset {
    std::string_view id;
    std::string_view name;
    uint8_t kinds;  // bit (1 << ModelKind)
    std::string_view base_url;
    bool key_required;

    bool supports(ModelKind kind) const {
        return (kinds & (1u << static_cast<unsigned>(kind))) != 0;
    }
};

// In display order; "custom" first.
std::span<const ModelPreset> model_presets();
const ModelPreset* find_model_preset(std::string_view id);

// Extra per-platform fields: flat, string values. Spec 5.3 defines tts
// "voice"; the agent adds the two llm options below. Unknown keys are kept
// as they are.
using ModelOptions = std::map<std::string, std::string>;

// Whether the model takes images: "true" or "false" (absent: false).
inline constexpr const char* kOptVision = "vision";
// The model's context window in tokens: a decimal string,
// kMinContextWindow..kMaxContextWindow (absent: kDefaultContextWindow).
inline constexpr const char* kOptContextWindow = "context_window";
inline constexpr uint32_t kMinContextWindow = 4096;
inline constexpr uint32_t kMaxContextWindow = 1048576;
inline constexpr uint32_t kDefaultContextWindow = 65536;

bool profile_vision(const ModelOptions& options);
uint32_t profile_context_window(const ModelOptions& options);

// What the outside world may see of a profile: everything but the key.
struct ModelProfile {
    std::string id;
    ModelKind kind = ModelKind::Llm;
    std::string name;
    std::string preset;
    std::string base_url;
    std::string model;
    ModelOptions options;
    bool has_key = false;
};

struct ProfilesState {
    // Indexed by ModelKind; "" when that kind has no profile.
    std::array<std::string, kModelKindCount> active;
    std::vector<ModelProfile> profiles;

    const std::string& active_for(ModelKind kind) const {
        return active[static_cast<size_t>(kind)];
    }
};

// create(). base_url left out takes the preset's. api_key "" means no key.
// api_key and api_key_from are mutually exclusive.
struct ProfileInput {
    ModelKind kind = ModelKind::Llm;
    std::string name;
    std::string preset;
    std::optional<std::string> base_url;
    std::optional<std::string> model;
    std::optional<ModelOptions> options;
    std::optional<std::string> api_key;
    std::optional<std::string> api_key_from;
};

// update(): fields left out keep their value. api_key "" clears the key;
// options replaces the whole map. The kind never changes.
struct ProfileUpdate {
    std::optional<std::string> name;
    std::optional<std::string> preset;
    std::optional<std::string> base_url;
    std::optional<std::string> model;
    std::optional<ModelOptions> options;
    std::optional<std::string> api_key;
    std::optional<std::string> api_key_from;
};

// Failures with their own error code in the API (spec 6.1); None for the
// generic ones (invalid_argument, not_found, ...).
enum class ModelFailure { None, LimitReached, ActiveProfile, KeyOriginMismatch };

// "limit_reached", "active_profile", "key_origin_mismatch"; "" for None.
const char* to_string(ModelFailure failure);

// Result of test(). `error` is "" on success, otherwise the category:
// "dns", "connect", "tls", "timeout", "other" (no request could be made),
// "no_model_list" (HTTP 404: the platform has no /models) or "http" (any
// other HTTP status, or a 200 that is not a model list).
struct ModelProbe {
    bool ok = false;
    std::string error;
    int http_status = 0;  // 0 when no HTTP response arrived
    uint32_t latency_ms = 0;
    std::vector<std::string> models;
};

// What a model client needs to send a request: internal, see the header.
struct ResolvedProfile {
    std::string id;
    std::string base_url;
    std::string model;
    ModelOptions options;
    std::string api_key;  // "" when none is set
};

// Sends the connectivity test (GET {base_url}/models) for `target`; the
// HTTP client lives in wisp_net, which wisp_core cannot depend on, so App
// injects it (net::probe_models).
using ModelProber = std::function<ModelProbe(const ResolvedProfile& target)>;

// Random bytes for profile ids. Empty: a weak fallback, still unique.
using ModelRandomFn = std::function<void(uint8_t* out, size_t len)>;

class ModelProfileService {
public:
    static constexpr size_t kMaxProfiles = 16;
    static constexpr size_t kMaxNameBytes = 32;
    static constexpr size_t kMaxModelBytes = 128;
    static constexpr size_t kMaxOptions = 8;
    static constexpr size_t kMaxOptionKeyBytes = 15;
    static constexpr size_t kMaxOptionValueBytes = 64;

    // `submit_io` queues the file writes on the flash thread; empty: every
    // change is written on the calling thread.
    ModelProfileService(StorageManager& storage, EventBus& bus, ModelRandomFn random = nullptr,
                        LatestSave::SubmitIo submit_io = {});

    ModelProfileService(const ModelProfileService&) = delete;
    ModelProfileService& operator=(const ModelProfileService&) = delete;

    // Loads secrets/models.json, or migrates secrets/llm.json (see the file
    // header). A missing file means no profiles. A corrupt file logs a
    // warning and is treated as empty (the next change rewrites it);
    // entries a hand edit made invalid are dropped. A storage read failure
    // is returned, leaving the service empty.
    Result<void> init();

    ProfilesState get() const;
    std::span<const ModelPreset> presets() const { return model_presets(); }

    // Validates everything before changing anything: on any error nothing
    // changes, in memory or on disk. The first profile of a kind becomes
    // its active one. Rules: name 1-32 bytes of UTF-8 text; preset known
    // and supporting `kind`; base_url http(s) with a host and without
    // whitespace, control characters or a '<' placeholder (trailing '/'
    // stripped); model at most 128 bytes of text; at most 8 options, keys
    // 1-15 bytes, values at most 64 bytes, no control characters, and
    // kOptVision/kOptContextWindow in their documented forms; api_key
    // printable ASCII without spaces (it goes into an HTTP header).
    // Failures with a dedicated code also set *why: LimitReached (17th
    // profile, QuotaExceeded) and KeyOriginMismatch (api_key_from naming a
    // profile that does not exist, has no key or is on another origin;
    // InvalidArgument).
    Result<ModelProfile> create(const ProfileInput& in, ModelFailure* why = nullptr);

    // Same rules; NotFound for an unknown id. See the file header for what
    // a base_url on another origin does to the key.
    Result<ModelProfile> update(std::string_view id, const ProfileUpdate& u,
                                ModelFailure* why = nullptr);

    // NotFound for an unknown id. The active profile of a kind that has
    // others is refused (ActiveProfile, Busy): activate another first. The
    // last profile of a kind is removed and the kind's active becomes "".
    Result<void> remove(std::string_view id, ModelFailure* why = nullptr);

    // Makes the profile its kind's active one. NotFound for an unknown id.
    Result<void> activate(std::string_view id);

    // Runs the prober against the profile. NotFound for an unknown id,
    // Unsupported when no prober was set (no HTTP client on this
    // platform). The result never contains the key.
    Result<ModelProbe> test(std::string_view id) const;

    void set_prober(ModelProber prober);

    // Internal: the kind's active profile with its key, nullopt when the
    // kind has none. Only for the model clients -- see the file header.
    std::optional<ResolvedProfile> active_for_request(ModelKind kind) const;

    // Forgets every profile and deletes the files (factory reset), in
    // place of any write still queued. Failures to delete are logged, not
    // returned: the caller reboots regardless.
    void clear();

private:
    struct Stored {
        ModelProfile profile;  // has_key mirrors !api_key.empty()
        std::string api_key;
    };
    struct State {
        std::array<std::string, kModelKindCount> active;
        std::vector<Stored> profiles;
    };

    std::string system_root() const;  // "" if the System area is unavailable
    // Saves `next` (see LatestSave). `migrated_from`, the legacy file
    // `next` was migrated from, is deleted once the write has succeeded.
    Result<void> persist(const State& next, std::string migrated_from = {});
    void publish(const State& state);
    std::string new_id(const State& state);
    Stored* find(State& state, std::string_view id);
    const Stored* find(const State& state, std::string_view id) const;
    // false: the file did not parse and the state is empty.
    Result<bool> load_models(const std::vector<uint8_t>& bytes);
    // Migrates secrets/llm.json if it exists; `over_corrupt` when that
    // replaces an unreadable secrets/models.json.
    Result<void> migrate_legacy(const std::string& root, bool over_corrupt = false);

    StorageManager& storage_;
    EventBus& bus_;
    ModelRandomFn random_;
    LatestSave saver_;
    mutable os::Mutex mutex_;
    State state_;
    ModelProber prober_;
    uint32_t fallback_counter_ = 0;
};

// The public JSON shapes (spec 5.3/6.1), with no key in them:
// a profile is {id, kind, name, preset, base_url, model, options, has_key};
// the state is {active:{llm,asr,tts}, profiles:[...]}; a preset is
// {id, name, kinds:[...], base_url, key_required}.
Value to_value(const ModelProfile& profile);
Value to_value(const ProfilesState& state);
Value to_value(const ModelPreset& preset);
// The model.profiles event payload: {active:{llm,asr,tts}, count}.
Map profiles_event_payload(const ProfilesState& state);

}  // namespace wisp::core
