#include "wisp/net/pairing_service.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <span>
#include <utility>

#include "wisp/base/base64.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/sha256.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "pairing";
constexpr const char* kSource = "pairing";
constexpr const char* kClientsFile = "/net/clients.json";
constexpr size_t kTokenBytes = 32;
constexpr size_t kIdChars = 12;
constexpr size_t kHashChars = 64;
constexpr size_t kMaxLabelBytes = 64;
// A real token is 43 characters; anything far longer is rejected before
// hashing so a request cannot make the device hash arbitrary input.
constexpr size_t kMaxTokenChars = 128;
// Largest multiple of 10^6 that fits in 32 bits: drawing below it and
// reducing mod 10^6 gives every code the same probability.
constexpr uint32_t kCodeDrawLimit = 4294000000u;
constexpr int kCodeDrawTries = 16;
// Ids are 48-bit hash prefixes; a repeat means the random source is
// broken, so give up instead of looping.
constexpr int kTokenDrawTries = 4;

// Compares without an early exit, so the time taken does not reveal how
// many leading characters matched. Only the lengths (public: codes are 6
// digits, hashes 64 hex chars) influence timing.
bool ct_equal(std::string_view a, std::string_view b) {
    size_t n = std::max(a.size(), b.size());
    unsigned diff = a.size() == b.size() ? 0u : 1u;
    for (size_t i = 0; i < n; ++i) {
        unsigned ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
        unsigned cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

std::string hash_hex(std::string_view token) {
    auto digest = sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(token.data()), token.size()));
    return to_hex(digest);
}

bool is_lower_hex(std::string_view s) {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

// A short, human-readable name for the paired-clients list. Order matters:
// Edge and Opera UAs also say "Chrome", Chrome's says "Safari", Android's
// says "Linux" and iOS's says "Mac OS X".
std::string label_for(std::string_view ua) {
    const char* browser = nullptr;
    if (contains(ua, "Edg/") || contains(ua, "EdgA/") || contains(ua, "EdgiOS/")) {
        browser = "Edge";
    } else if (contains(ua, "OPR/")) {
        browser = "Opera";
    } else if (contains(ua, "Firefox/") || contains(ua, "FxiOS/")) {
        browser = "Firefox";
    } else if (contains(ua, "Chrome/") || contains(ua, "CriOS/")) {
        browser = "Chrome";
    } else if (contains(ua, "Safari/")) {
        browser = "Safari";
    }

    const char* os = nullptr;
    if (contains(ua, "Android")) {
        os = "Android";
    } else if (contains(ua, "iPhone")) {
        os = "iPhone";
    } else if (contains(ua, "iPad")) {
        os = "iPad";
    } else if (contains(ua, "Windows")) {
        os = "Windows";
    } else if (contains(ua, "Mac OS X") || contains(ua, "Macintosh")) {
        os = "macOS";
    } else if (contains(ua, "CrOS")) {
        os = "ChromeOS";
    } else if (contains(ua, "Linux")) {
        os = "Linux";
    }

    if (browser != nullptr && os != nullptr) return std::string(browser) + " · " + os;
    if (browser != nullptr) return browser;
    if (os != nullptr) return os;
    return "浏览器";
}

}  // namespace

PairingService::PairingService(core::StorageManager& storage, core::EventBus& bus, RandomFn random,
                               std::function<int64_t()> unix_now, PairingConfig config)
    : storage_(storage),
      bus_(bus),
      random_(std::move(random)),
      unix_now_(std::move(unix_now)),
      config_(config) {}

// ---------------------------------------------------------------------------
// Persistence

std::string PairingService::clients_path() const {
    auto root = storage_.write_root(core::Area::System);
    return root ? root.value() + kClientsFile : std::string();
}

Result<void> PairingService::persist_locked(const std::vector<Entry>& clients) {
    std::string path = clients_path();
    if (path.empty()) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    }
    List list;
    for (const auto& e : clients) {
        list.push_back(Map{{"id", e.info.id},
                           {"hash", e.hash},
                           {"label", e.info.label},
                           {"created", e.info.created_unix},
                           {"last_seen", e.info.last_seen_unix}});
    }
    std::string json = to_json(Map{{"clients", std::move(list)}});
    auto r = storage_.write_atomic(path, std::vector<uint8_t>(json.begin(), json.end()));
    if (!r) {
        WLOG_E(kTag, "saving paired clients failed: %s", r.error().message.c_str());
        return r;
    }
    // Whatever was written includes every pending last_seen update.
    last_flush_ms_ = latest_now_ms_;
    last_seen_dirty_ = false;
    return r;
}

Result<void> PairingService::init() {
    std::lock_guard<os::Mutex> lock(mutex_);
    clients_.clear();
    last_seen_dirty_ = false;

    Result<void> result = Result<void>::ok();
    std::string path = clients_path();
    if (path.empty()) {
        result = Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    } else if (auto bytes = storage_.read_file(path); !bytes) {
        if (bytes.error().code != ErrorCode::NotFound) {
            result = Result<void>::err(bytes.error().code,
                                       "reading net/clients.json: " + bytes.error().message);
        }
    } else {
        auto doc = from_json(std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                                              bytes.value().size()));
        const Value* list = doc ? doc.value().find("clients") : nullptr;
        if (list == nullptr || list->type() != Value::Type::List) {
            result = Result<void>::err(ErrorCode::InvalidArgument, "net/clients.json is malformed");
        } else {
            size_t skipped = 0;
            for (const auto& item : list->as_list()) {
                const Value* id = item.find("id");
                const Value* hash = item.find("hash");
                const Value* label = item.find("label");
                const Value* created = item.find("created");
                const Value* seen = item.find("last_seen");
                bool valid =
                    id != nullptr && hash != nullptr && label != nullptr && created != nullptr &&
                    seen != nullptr && id->type() == Value::Type::String &&
                    hash->type() == Value::Type::String && label->type() == Value::Type::String &&
                    created->type() == Value::Type::Int && seen->type() == Value::Type::Int &&
                    hash->as_string().size() == kHashChars && is_lower_hex(hash->as_string()) &&
                    id->as_string() == hash->as_string().substr(0, kIdChars) &&
                    label->as_string().size() <= kMaxLabelBytes &&
                    clients_.size() < config_.max_clients &&
                    std::none_of(
                        clients_.begin(), clients_.end(),
                        [&](const Entry& e) { return e.info.id == id->as_string(); });
                if (!valid) {
                    ++skipped;
                    continue;
                }
                clients_.push_back(Entry{PairedClient{id->as_string(), label->as_string(),
                                                      created->as_int(), seen->as_int()},
                                         hash->as_string()});
            }
            if (skipped > 0) {
                WLOG_W(kTag, "ignored %zu invalid paired client entr%s", skipped,
                       skipped == 1 ? "y" : "ies");
            }
        }
    }
    if (!result) {
        WLOG_W(kTag, "%s; starting with no paired clients", result.error().message.c_str());
    }
    WLOG_I(kTag, "%zu paired client(s)", clients_.size());
    return result;
}

// ---------------------------------------------------------------------------
// Codes and lockout

void PairingService::observe_locked(uint64_t now_ms) {
    latest_now_ms_ = std::max(latest_now_ms_, now_ms);
    if (code_ && now_ms >= code_expires_ms_) {
        WLOG_I(kTag, "pairing code expired");
        clear_code_locked();
    }
    if (locked_until_ms_ && now_ms >= *locked_until_ms_) {
        WLOG_I(kTag, "pairing unlocked");
        locked_until_ms_.reset();
        failures_ = 0;
    }
}

void PairingService::clear_code_locked() {
    if (!code_) return;
    code_.reset();
    code_expires_ms_ = 0;
    bus_.publish(core::Event{"net.pairing.code", kSource, 0,
                             Properties{{"active", false}, {"expires_ms", int64_t{0}}}});
}

std::string PairingService::draw_code_locked() {
    uint32_t v = 0;
    for (int i = 0; i < kCodeDrawTries; ++i) {
        std::array<uint8_t, 4> b{};
        random_(b.data(), b.size());
        v = (uint32_t{b[0]} << 24) | (uint32_t{b[1]} << 16) | (uint32_t{b[2]} << 8) | b[3];
        if (v < kCodeDrawLimit) break;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", static_cast<unsigned>(v % 1000000u));
    return buf;
}

Result<void> PairingService::request_code(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    observe_locked(now_ms);
    if (locked_until_ms_) {
        return Result<void>::err(ErrorCode::RateLimited, "too many wrong codes; try again later");
    }
    code_ = draw_code_locked();
    code_expires_ms_ = now_ms + config_.code_ttl_ms;
    WLOG_I(kTag, "pairing code shown on screen (valid %u s)", config_.code_ttl_ms / 1000);
    bus_.publish(core::Event{
        "net.pairing.code", kSource, 0,
        Properties{{"active", true}, {"expires_ms", static_cast<int64_t>(code_expires_ms_)}}});
    return Result<void>::ok();
}

std::optional<std::string> PairingService::active_code(uint64_t now_ms) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!code_ || now_ms >= code_expires_ms_) return std::nullopt;
    return code_;
}

void PairingService::tick(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    observe_locked(now_ms);
}

Result<std::string> PairingService::confirm(std::string_view code, const ClientInfo& client,
                                            uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    observe_locked(now_ms);
    if (locked_until_ms_) {
        return Result<std::string>::err(ErrorCode::RateLimited,
                                        "too many wrong codes; try again later");
    }
    // Compare even when no code is active, so a missing code and a wrong
    // one take the same time and both count toward the lock.
    bool match = ct_equal(code_ ? std::string_view(*code_) : std::string_view(), code);
    if (!match || !code_) {
        ++failures_;
        bool now_locked = failures_ >= config_.max_attempts;
        WLOG_W(kTag, "wrong pairing code (%d/%d)", failures_, config_.max_attempts);
        if (now_locked) {
            locked_until_ms_ = now_ms + config_.lock_ms;
            WLOG_W(kTag, "pairing locked for %u s", config_.lock_ms / 1000);
            clear_code_locked();
        }
        bus_.publish(core::Event{"net.pairing.failed", kSource, 0,
                                 Properties{{"attempts", failures_}, {"locked", now_locked}}});
        return Result<std::string>::err(ErrorCode::PermissionDenied,
                                        "wrong or expired pairing code");
    }
    // If saving fails the code stays valid, so the browser can retry.
    auto token = add_client_locked(client, "code");
    if (!token) return token;
    failures_ = 0;
    clear_code_locked();
    return token;
}

// ---------------------------------------------------------------------------
// Clients

Result<std::string> PairingService::add_client_locked(const ClientInfo& client, const char* how) {
    std::string token;
    std::string hash;
    for (int i = 0;; ++i) {
        if (i == kTokenDrawTries) {
            return Result<std::string>::err(ErrorCode::IoError, "random source is not random");
        }
        std::array<uint8_t, kTokenBytes> bytes{};
        random_(bytes.data(), bytes.size());
        token = base64url_encode(bytes);
        hash = hash_hex(token);
        std::string_view id = std::string_view(hash).substr(0, kIdChars);
        if (std::none_of(clients_.begin(), clients_.end(),
                         [&](const Entry& e) { return e.info.id == id; })) {
            break;
        }
    }

    int64_t unix_now = unix_now_ ? unix_now_() : 0;
    Entry entry{
        PairedClient{hash.substr(0, kIdChars), label_for(client.user_agent), unix_now, unix_now},
        hash};

    std::vector<Entry> updated = clients_;
    std::string evicted;
    if (!updated.empty() && updated.size() >= config_.max_clients) {
        auto victim =
            std::min_element(updated.begin(), updated.end(), [](const Entry& a, const Entry& b) {
                if (a.info.last_seen_unix != b.info.last_seen_unix) {
                    return a.info.last_seen_unix < b.info.last_seen_unix;
                }
                return a.info.created_unix < b.info.created_unix;
            });
        evicted = victim->info.id;
        updated.erase(victim);
    }
    updated.push_back(entry);
    auto saved = persist_locked(updated);
    if (!saved) return Result<std::string>::err(saved.error().code, saved.error().message);
    clients_ = std::move(updated);

    if (!evicted.empty()) {
        WLOG_I(kTag, "evicted least recently seen client %s", evicted.c_str());
    }
    WLOG_I(kTag, "paired client %s (%s) via %s", entry.info.id.c_str(), entry.info.label.c_str(),
           how);
    return Result<std::string>::ok(std::move(token));
}

Result<std::string> PairingService::pair_from_ap(const ClientInfo& client, uint64_t now_ms) {
    if (client.iface != Iface::Ap) {
        return Result<std::string>::err(ErrorCode::PermissionDenied,
                                        "pairing without a code is only allowed on the hotspot");
    }
    std::lock_guard<os::Mutex> lock(mutex_);
    observe_locked(now_ms);
    return add_client_locked(client, "hotspot");
}

Result<std::string> PairingService::authenticate(std::string_view token, uint64_t now_ms) {
    if (token.empty() || token.size() > kMaxTokenChars) {
        return Result<std::string>::err(ErrorCode::PermissionDenied, "unknown token");
    }
    std::string hash = hash_hex(token);

    std::lock_guard<os::Mutex> lock(mutex_);
    observe_locked(now_ms);
    // Visit every entry so the lookup time does not depend on where (or
    // whether) the match sits.
    Entry* found = nullptr;
    for (auto& e : clients_) {
        if (ct_equal(e.hash, hash)) found = &e;
    }
    if (found == nullptr) {
        return Result<std::string>::err(ErrorCode::PermissionDenied, "unknown token");
    }

    // 0 means the RTC is unset; keep the last known time rather than
    // making this client look like the least recently seen one.
    int64_t unix_now = unix_now_ ? unix_now_() : 0;
    if (unix_now != 0 && unix_now != found->info.last_seen_unix) {
        found->info.last_seen_unix = unix_now;
        last_seen_dirty_ = true;
    }
    std::string id = found->info.id;
    if (last_seen_dirty_ && now_ms >= last_flush_ms_ &&
        now_ms - last_flush_ms_ >= config_.last_seen_flush_ms) {
        if (!persist_locked(clients_)) {
            // persist_locked() logged the cause; wait a full interval
            // before retrying rather than retrying on every request.
            last_flush_ms_ = now_ms;
        }
    }
    return Result<std::string>::ok(std::move(id));
}

std::vector<PairedClient> PairingService::clients() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<PairedClient> out;
    out.reserve(clients_.size());
    for (const auto& e : clients_) out.push_back(e.info);
    return out;
}

Result<void> PairingService::revoke(std::string_view id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    auto it = std::find_if(clients_.begin(), clients_.end(),
                           [&](const Entry& e) { return e.info.id == id; });
    if (it == clients_.end()) {
        return Result<void>::err(ErrorCode::NotFound, "no such paired client");
    }
    std::string removed = it->info.id;
    // Forget it in memory first: a revoked token must stop working even
    // if the file cannot be rewritten.
    clients_.erase(it);
    WLOG_I(kTag, "revoked client %s", removed.c_str());
    return persist_locked(clients_);
}

void PairingService::revoke_all() {
    std::lock_guard<os::Mutex> lock(mutex_);
    clients_.clear();
    if (!persist_locked(clients_)) {
        WLOG_E(kTag, "factory reset of paired clients was not persisted");
        return;
    }
    WLOG_I(kTag, "all paired clients revoked");
}

}  // namespace wisp::net
