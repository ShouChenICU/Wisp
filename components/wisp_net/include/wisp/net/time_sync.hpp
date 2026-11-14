#pragma once

// Network time (connectivity spec 5.6): sets the wall clock and the
// hardware RTC from SNTP while the station has an IP.
//
// Schedule: an attempt runs when the station gets an IP, again 6 h after a
// success, and after a failure 30 s later, doubling up to 30 min, until one
// succeeds or the link goes. Without a link nothing is scheduled; the next
// IP starts over, even when it comes while an attempt for the old one is
// still running. sync_now() runs one at once. An attempt asks each server
// of the ntp_servers setting in turn and stops at the first answer.
//
// On an answer the wall clock is set (TimeSyncDeps::set_system_time; if
// it refuses, the attempt fails) and the RTC written (a failure there is
// only a warning). Every finished attempt then sets result_cell(), whose
// mirror publishes system.time: {"state":"synced","server":s,"time":unix},
// or for a failure {"state":"failed","server":<last one asked>}.
// Everything is UTC; displays apply utc_offset_min themselves.
//
// Threading: tick() runs on the "wisp.net" task and never blocks on the
// network: when an attempt is due it reads the server list there (Settings
// is flash) and returns true, and the caller has run_attempt() run the
// queries -- several seconds per unreachable server -- on a worker that
// does not touch flash. sync_now(), status() and now_unix() may be called
// from any thread. One mutex guards the state; it is never held across a
// query, a callback or a publish, and the result cell is set after it is
// released (lock order: cell -> bus; mutex_ is never held with the cell's).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/time_sync_result.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

// One SNTP request (the platform port: wisp_port_esp over lwIP, the
// simulator from the host clock).
class Sntp {
public:
    virtual ~Sntp() = default;
    // Asks `host` (a name or an IPv4 address) for the time, waiting at most
    // about `timeout_ms` for the answer (name lookup aside); UTC seconds
    // since the epoch.
    virtual Result<int64_t> query(const std::string& host, uint32_t timeout_ms) = 0;
};

struct TimeSyncConfig {
    uint32_t resync_ms = 6 * 60 * 60 * 1000;
    uint32_t backoff_min_ms = 30 * 1000;
    uint32_t backoff_max_ms = 30 * 60 * 1000;
    uint32_t query_timeout_ms = 3000;
};

struct TimeSyncDeps {
    Sntp& sntp;
    core::EventBus& bus;
    // The ntp_servers setting ("a,b"), read once per attempt by tick().
    std::function<std::string()> servers;
    // Sets the system wall clock to UTC seconds; an error fails the
    // attempt. Empty: left alone (the simulator, whose host clock is
    // already right).
    std::function<Result<void>(int64_t)> set_system_time;
    // clock.main, written after each sync; null without one.
    std::shared_ptr<hal::Clock> rtc;
    // Monotonic milliseconds; tests inject their own.
    std::function<uint64_t()> now_ms = [] { return os::now_ms(); };
};

struct TimeSyncStatus {
    // A sync succeeded during this boot.
    bool synced = false;
    // The time that sync set (UTC seconds) and the server that gave it;
    // 0 and "" until then.
    int64_t last_sync = 0;
    std::string server;
    // Attempts finished during this boot (a cancelled one does not count)
    // and the last one's outcome: "synced", "failed", or "" before any.
    uint64_t attempts = 0;
    std::string last_result;
};

// The entries of an ntp_servers value, empty ones dropped.
std::vector<std::string> split_servers(std::string_view list);

class TimeSyncService {
public:
    explicit TimeSyncService(TimeSyncDeps deps, TimeSyncConfig config = {});

    TimeSyncService(const TimeSyncService&) = delete;
    TimeSyncService& operator=(const TimeSyncService&) = delete;

    // Called when sync_now() has queued an attempt, so the "wisp.net" task
    // can tick at once. Set before the service is shared.
    void set_wake(std::function<void()> wake) { wake_ = std::move(wake); }

    // The "wisp.net" task's tick; `online` is whether the station has an
    // IP. True when it has prepared an attempt for run_attempt(); no new
    // one is prepared until that has finished.
    bool tick(bool online);
    // Runs the prepared attempt, if any, blocking for the queries.
    void run_attempt();

    // Asks for an attempt now. nullopt when the station has no IP;
    // otherwise status().attempts as it stood, so the answer is the first
    // attempt to finish after that many. An attempt already prepared or
    // running counts as the one asked for.
    std::optional<uint64_t> sync_now();

    // Shutdown: a running attempt stops before its next server, publishing
    // nothing, and no further attempt runs. Any thread.
    void cancel() { cancelled_.store(true); }

    TimeSyncStatus status() const;

    // The last attempt's result, as system.time announces it: it moves
    // exactly when that event is published. Followers must release their
    // subscription before the service is destroyed (NetWiring::stop(),
    // before the bus stops; wisp/core/state_cell.hpp).
    const core::StateCell<core::TimeSyncResult>& result_cell() const { return result_cell_; }

    // A clock set by other means (the simulator's debug command): counted
    // in the cell as a successful attempt from `server` at `time`, as a
    // sync would be, so followers catch up on the jump. status() is left
    // alone: the count sync_now() hands out is answered only by a real
    // attempt. Any thread.
    void note_external_sync(std::string_view server, int64_t time);
    // The current UTC time as the last sync implies it; nullopt before
    // any.
    std::optional<int64_t> now_unix() const;

private:
    void finish(bool ok, int64_t unix_time, const std::string& server);

    TimeSyncDeps deps_;
    TimeSyncConfig config_;
    std::function<void()> wake_;

    mutable os::Mutex mutex_;
    bool online_ = false;
    bool requested_ = false;
    // Prepared by tick(), not yet finished by run_attempt().
    bool in_flight_ = false;
    // The link went (and maybe came back) while in flight: the outcome
    // belongs to the old link and must not set the new one's schedule.
    bool link_changed_ = false;
    std::atomic<bool> cancelled_{false};
    std::vector<std::string> job_;
    std::optional<uint64_t> next_attempt_ms_;
    uint32_t backoff_ms_;
    TimeSyncStatus status_;
    uint64_t synced_at_ms_ = 0;
    // Set outside mutex_, once per finished attempt (finish()) or external
    // sync. Declared after deps_ (the bus its constructor uses); the
    // service is destroyed before the bus stops (NetWiring::stop()).
    core::StateCell<core::TimeSyncResult> result_cell_;
};

}  // namespace wisp::net
