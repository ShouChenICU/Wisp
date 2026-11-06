#pragma once

// Browser pairing for the admin web UI (spec 5.2, 6.3).
//
// A browser pairs either by typing the 6-digit code shown on the device's
// screen (request_code() + confirm()) or, during Wi-Fi provisioning, by
// being on the setup hotspot (pair_from_ap()). Either way it receives a
// long-lived bearer token (32 random bytes, base64url) that it presents
// on every later request through authenticate().
//
// Secrets: the pairing code is only handed out through active_code() for
// the device's own screen; neither the code nor a token ever appears in
// a log line, an event payload or an error message. Tokens are not kept
// at all -- memory and <System>/net/clients.json only hold their SHA-256.
//
// Brute force: wrong codes are counted; the max_attempts-th consecutive
// failure locks code pairing (request_code() and confirm() return
// RateLimited) for lock_ms and drops the active code. Codes and token
// hashes are compared in constant time.
//
// Flash wear: authenticate() runs on every authenticated HTTP request, so
// it only updates last_seen in memory and writes clients.json at most
// once per last_seen_flush_ms. Adding or revoking a client writes at
// once (on the flash thread when there is a submit_io). A reboot can
// therefore lose up to that long of last_seen progress, which only affects
// which client is evicted first.
//
// Threading: confirm()/authenticate() run on HTTP server threads while
// request_code()/active_code()/tick() run on the UI or console; one
// mutex guards all state. EventBus::publish() never blocks on handlers,
// so events are published while holding it. Revoke listeners have a lock
// of their own and are called without the state mutex (see
// add_revoke_listener()).
//
// Time: every `now_ms` is the monotonic os::now_ms() timebase. Wall-clock
// stamps (created/last_seen) come from `unix_now`, which returns 0 while
// the RTC is unset.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/latest_save.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/random.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

struct ClientInfo {
    std::string user_agent;
    std::string remote_ip;
    Iface iface = Iface::Sta;
};

struct PairedClient {
    std::string id;     // first 12 hex chars of the token's SHA-256
    std::string label;  // e.g. "Chrome · Android", summarised from the User-Agent; may be ""
    int64_t created_unix = 0;
    int64_t last_seen_unix = 0;
};

// Why confirm() refused a code, for an error the browser can act on
// (spec 6.1): an expired code needs a fresh one, a wrong one a retry.
enum class ConfirmFailure { None, WrongCode, ExpiredCode, Locked };

struct PairingConfig {
    uint32_t code_ttl_ms = 120000;
    // At most one new code per this long (see request_code()).
    uint32_t min_code_interval_ms = 10000;
    int max_attempts = 5;
    uint32_t lock_ms = 300000;
    size_t max_clients = 8;
    uint32_t last_seen_flush_ms = 3600000;
};

class PairingService {
public:
    // `submit_io` queues the writes of clients.json on the flash thread
    // (core::LatestSave); empty: every change is written on the calling
    // thread.
    PairingService(core::StorageManager& storage, core::EventBus& bus, RandomFn random,
                   std::function<int64_t()> unix_now, PairingConfig config = {},
                   core::LatestSave::SubmitIo submit_io = {});

    PairingService(const PairingService&) = delete;
    PairingService& operator=(const PairingService&) = delete;

    // Loads <System>/net/clients.json. A missing file is not an error. A
    // corrupt or unreadable one is reported, but the service stays usable
    // with no paired clients (the file is rewritten on the next pairing).
    Result<void> init();

    // Issues a fresh code valid for code_ttl_ms, replacing any active one,
    // and publishes net.pairing.code {active:true, expires_ms} where
    // expires_ms is the deadline in the now_ms timebase. RateLimited while
    // locked. The request route needs no authentication, so a valid code
    // is reused for its whole lifetime: the call succeeds and publishes
    // nothing, so repeated requests can neither replace the code the owner
    // is typing nor wake the screen. Only without a valid code (expired,
    // cancelled, used, or dropped by a lock) is a new one issued, at most
    // one per min_code_interval_ms; sooner is RateLimited (code_wait_ms()
    // says for how long).
    Result<void> request_code(uint64_t now_ms);

    // The current code, for the device's screen only. Empty when none is
    // active, it has expired, or pairing is locked. With a code, *expires_ms
    // (when given) is its deadline in the now_ms timebase -- the same value
    // net.pairing.code carried -- so the screen can resync from the service
    // alone when it missed that event.
    std::optional<std::string> active_code(uint64_t now_ms, uint64_t* expires_ms = nullptr) const;

    // Drops the active code at once (the device's "reject" button) and
    // publishes net.pairing.code {active:false}, as an expiry would. Typing
    // it afterwards is a wrong code, not an expired one: nobody should be
    // told to fetch a fresh code for a request the device turned down. A
    // no-op without an active code.
    void cancel_code();

    // Publishes net.pairing.code {active:false} once an active code has
    // expired. Every other method taking now_ms does the same, so a caller
    // that needs the event promptly (the screen) drives this periodically.
    void tick(uint64_t now_ms);

    // Pairs a browser that typed the screen code; returns its token. A
    // wrong, expired or missing code is PermissionDenied and counts toward
    // the lock (publishing net.pairing.failed {attempts, locked}); while
    // locked, RateLimited. Success consumes the code and resets the count.
    // On failure, *why (when given) says which: ExpiredCode only for the
    // code that most recently timed out, while no newer code has been
    // issued and pairing has not locked since.
    Result<std::string> confirm(std::string_view code, const ClientInfo& client, uint64_t now_ms,
                                ConfirmFailure* why = nullptr);

    // How long code pairing stays locked, 0 when it is not.
    uint64_t lock_remaining_ms(uint64_t now_ms) const;

    // How long until request_code() may issue a new code; 0 when it may
    // now or when a valid code would be reused. Ignores the lock.
    uint64_t code_wait_ms(uint64_t now_ms) const;

    // Pairs without a code; only for requests that arrived on the setup
    // hotspot (client.iface == Iface::Ap), else PermissionDenied.
    Result<std::string> pair_from_ap(const ClientInfo& client, uint64_t now_ms);

    // Returns the client id for a valid token (PermissionDenied otherwise)
    // and records the visit; see the file header for when that is written.
    Result<std::string> authenticate(std::string_view token, uint64_t now_ms);

    // In pairing order.
    std::vector<PairedClient> clients() const;

    // NotFound for an unknown id. The client is forgotten in memory even
    // if writing clients.json fails (the error is still returned).
    Result<void> revoke(std::string_view id);

    // Called once after LatestSave::kFailuresToReport failed writes in a
    // row (see core::LatestSave). Set it before the first change.
    void set_on_persist_failed(std::function<void()> fn) {
        saver_.set_on_persist_failed(std::move(fn));
    }

    // Factory reset: forgets every client and writes an empty list. If
    // that write fails, clients.json is deleted instead so no old hash
    // survives a reboot.
    void revoke_all();

    // Told about every revocation, so whoever holds sessions opened with a
    // token (the admin WebSocket) can end them (spec 6.4). Gets the
    // revoked id from revoke() and from a pairing that evicts the least
    // recently seen client, and nullopt from revoke_all() (every client). Called on the revoking
    // thread once the client is already forgotten, outside the service's lock (so it may call back
    // into the service) but under the listeners' own lock: it must not add or remove listeners.
    using RevokeListener = std::function<void(const std::optional<std::string>& id)>;
    // Returns a handle for remove_revoke_listener().
    int add_revoke_listener(RevokeListener fn);
    // Waits out a call to that listener still running on another thread,
    // so what it captured may be destroyed once this returns.
    void remove_revoke_listener(int handle);

private:
    struct Entry {
        PairedClient info;
        std::string hash;  // lowercase hex SHA-256 of the token
    };

    std::string clients_path() const;
    // Saves `clients` as clients.json (see core::LatestSave): Ok once the
    // write is queued, or, without submit_io, once it is done; either way
    // the pending last_seen updates count as flushed. With
    // `remove_on_failure` a failed write deletes the file instead, so no
    // old hash survives a reboot.
    Result<void> persist_locked(const std::vector<Entry>& clients, bool remove_on_failure = false);
    // Records the latest time seen, expires the code and lifts the lock
    // when due.
    void observe_locked(uint64_t now_ms);
    void clear_code_locked();
    std::string draw_code_locked();
    // Sets *evicted to the id dropped to stay within max_clients, if any;
    // the caller notifies revoke listeners once mutex_ is released.
    Result<std::string> add_client_locked(const ClientInfo& client, const char* how,
                                          std::string* evicted);
    // confirm() minus the listener call, under mutex_.
    Result<std::string> confirm_unnotified(std::string_view code, const ClientInfo& client,
                                           uint64_t now_ms, std::string* evicted,
                                           ConfirmFailure* why);

    core::StorageManager& storage_;
    core::EventBus& bus_;
    RandomFn random_;
    std::function<int64_t()> unix_now_;
    PairingConfig config_;
    core::LatestSave saver_;

    mutable os::Mutex mutex_;

    std::vector<Entry> clients_;
    // Disk and memory agree right after init(), so boot counts as a flush.
    uint64_t last_flush_ms_ = 0;
    bool last_seen_dirty_ = false;
    // Largest now_ms passed in so far; revoke()/revoke_all() take no time
    // argument but still restart the flush interval with their write.
    uint64_t latest_now_ms_ = 0;

    std::optional<std::string> code_;
    uint64_t code_expires_ms_ = 0;
    std::optional<uint64_t> code_issued_ms_;  // the last request_code() that drew
    // The code that last timed out, so typing it late reads as "expired"
    // rather than "wrong"; dropped by a new code, a lock or a pairing.
    std::optional<std::string> expired_code_;
    int failures_ = 0;
    std::optional<uint64_t> locked_until_ms_;

    void notify_revoked(const std::optional<std::string>& id);

    // Separate from mutex_ so listeners run without it.
    os::Mutex listeners_mutex_;
    std::vector<std::pair<int, RevokeListener>> listeners_;
    int next_listener_ = 1;
};

}  // namespace wisp::net
