#pragma once

// Fan-out of log lines and selected events to the admin WebSocket clients
// (spec 6.2). Every connection first receives {"t":"hello","boot_id":<id>},
// the id of this boot (also /info's "boot_id"), before anything else. A
// client subscribes by sending {"sub":["log","events"]} (either or both; a
// later message replaces the set):
//   - "log": the lines still in the log ring are replayed first, one
//     {"t":"log","seq","ts","level","tag","msg","replay":true} per line,
//     then every new line streams as {"t":"log","seq","ts","level","tag",
//     "msg"}. seq and ts are the log module's own (wisp::LogRecord), so
//     (boot_id, seq) names a line exactly: a browser drops any copy it
//     has already shown, e.g. the replay after a re-subscribe. A message that
//     contains '\n' is still one line.
//   - "events": events under net., power., storage. and system. arrive as
//     {"t":"event","topic","payload"}.
//   - "agent": the agent's frames {"t":"agent","k",...} (agent-core spec
//     7.2), as the owner hands them to send_agent(); nothing is replayed.
//
// Backpressure: each client has a bounded queue, at most max_queue
// messages and max_queue_bytes bytes in total (on ESP these small strings
// come from internal RAM). When a message does not fit, the oldest ones
// are dropped and counted until it does; a message larger than the whole
// byte budget is itself dropped and counted. The next send to that client
// is {"t":"dropped","count":N}. A send_text() that reports a full socket
// queue keeps the message at the head for the next pump(). The replay is
// limited to the newest ring lines that fit in an empty queue; older ones
// are skipped without being reported as dropped, since the client never
// fell behind on them.
//
// Wake lock: with set_wake_locks(), the hub holds one network lock
// ("system.admin", "admin session") while at least one connection is
// open, so deep standby does not turn Wi-Fi off under an open admin page
// (scene-device spec 4.3).
//
// Keepalive: a peer that vanished without closing (left Wi-Fi range, a
// laptop lid shut, a router reboot) never reports a close, and its lock
// would keep Wi-Fi on until reboot. keepalive(), called periodically by
// the owner, sends every client {"t":"ping"}; the page answers
// {"t":"pong"}, and any message from a client counts as hearing from it.
// A client not heard from for kKeepaliveMisses calls in a row is closed
// ("client N closed: silent"). The server's TCP keepalive (where the port
// has one) catches the same peers at the socket level.
//
// Revocation: each connection remembers the paired client whose token
// opened it; close_client() ends that client's connections when its
// token is revoked (spec 6.4).
//
// Threading:
//   - write() (LogSink) runs on whichever thread logs. It only formats the
//     line and appends it to the queues under the hub's mutex; it never
//     touches a connection and never logs (a log call from a sink is not
//     delivered anywhere, see wisp::LogSink).
//   - Event handlers run on the EventBus dispatcher and enqueue the same
//     way.
//   - pump() drains the queues through send_text() without holding the
//     hub's mutex, so a slow socket never stalls a logging thread. In
//     production it runs on the hub's own "wisp.ws" task (start()/stop());
//     tests call it directly. Concurrent pump() calls are serialized.
//   - open()/on_message()/close() come from HTTP server threads,
//     close_client()/close_all() from whichever thread revokes a pairing.
//     The latter call WsConnection::close() without holding the hub's
//     mutex, so a server may report the close back through close().
// Lock order: the hub's mutex may be held while taking the log module's
// mutex (ring replay), never the other way round; the log module calls
// sinks outside its own lock.

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/http.hpp"
#include "wisp/os/os.hpp"

namespace wisp::admin {

class WsHub : public LogSink {
public:
    // Registers itself as a log sink and subscribes to the forwarded event
    // prefixes; does not start the sender task. `boot_id` goes into every
    // connection's hello.
    WsHub(core::EventBus& bus, std::string boot_id = {}, size_t max_clients = 2,
          size_t max_queue = 256, size_t max_queue_bytes = 32768);
    // Stops the sender task, unsubscribes and unregisters the log sink,
    // waiting out any write() or event handler still running.
    ~WsHub() override;

    WsHub(const WsHub&) = delete;
    WsHub& operator=(const WsHub&) = delete;

    // The registry for the "system.admin" lock; set once, before the first
    // open(). It must outlive the hub.
    void set_wake_locks(core::WakeLockRegistry* locks) { locks_ = locks; }

    // Spawns the sender task that calls pump() whenever something is
    // queued (and every 100 ms while a socket keeps refusing). Idempotent.
    void start();
    // Joins the sender task; a send_text() in progress is waited out.
    // Idempotent.
    void stop();

    // Admits a connection opened with `client_id`'s token; false when
    // max_clients are already open or the id is taken. New connections are
    // subscribed to nothing.
    bool open(std::shared_ptr<net::WsConnection> conn, std::string client_id = {});
    // Handles a client's {"sub":[...]} message; anything else is ignored
    // (but still counts as hearing from the client, see keepalive()).
    void on_message(int id, std::string_view text);
    // The server reports a closed connection: forgets it and drops
    // whatever is still queued for it. Unknown ids are ignored.
    void close(int id);
    // Closes (WsConnection::close()) and forgets every connection opened
    // by that paired client; an empty id matches nothing. No send to them
    // starts afterwards; one already in progress may finish.
    void close_client(std::string_view client_id);
    // The same for every connection.
    void close_all();

    // Only enqueues; never blocks on a connection.
    void write(const LogRecord& record) override;

    // Queues one agent frame (JSON text) for every "agent" subscriber.
    // Any thread; only enqueues, like write().
    void send_agent(std::string json);

    // Waits up to timeout_ms until nothing is owed to any client (hello,
    // dropped notice, queued messages) and no send is in progress; true
    // once that holds, false on timeout. Only waits: the sender task (or a
    // caller's pump()) does the sending. A factory reset uses it so its
    // final event leaves before the revocation closes the sockets.
    bool flush(uint32_t timeout_ms);

    // One keepalive round: pings every client and closes those not heard
    // from for kKeepaliveMisses rounds. Only enqueues and closes; the
    // sender task sends the pings.
    void keepalive();
    static constexpr int kKeepaliveMisses = 3;

    // Sends what is owed, per client the hello (until it goes through),
    // the dropped notice and up to one queue's worth, in that order,
    // stopping at that client's first refused send.
    void pump();

private:
    struct Client;

    void on_event(const core::Event& e);
    void enqueue_locked(Client& c, std::string msg);
    // Detaches the matching clients and closes their connections.
    // `why` ends the log line "client N closed: <why>".
    void close_matching(const std::function<bool(const Client&)>& match, const char* why);
    void forget_locked(Client& c);
    bool has_pending() const;
    void run();
    // Holds the admin lock exactly while a connection is open. Called
    // without mutex_ (taking a lock publishes an event, which the hub's own
    // handler receives).
    void sync_lock();

    core::EventBus& bus_;
    const std::string hello_;  // the hello frame, built once
    const size_t max_clients_;
    const size_t max_queue_;
    const size_t max_queue_bytes_;

    mutable os::Mutex mutex_;  // guards clients_ and every Client's fields
    std::vector<std::shared_ptr<Client>> clients_;
    // Number of clients subscribed to each stream, so write() and the
    // event handler skip formatting when nobody listens. Changed under
    // mutex_; read without it.
    std::atomic<int> log_subscribers_{0};
    std::atomic<int> event_subscribers_{0};
    std::atomic<int> agent_subscribers_{0};

    os::Mutex pump_mutex_;  // serializes pump()
    os::Semaphore wake_;    // given whenever something is enqueued
    os::Mutex lifecycle_mutex_;
    std::unique_ptr<os::Task> task_;
    std::atomic<bool> running_{false};

    std::vector<core::SubscriptionId> subscriptions_;

    core::WakeLockRegistry* locks_ = nullptr;
    // Serializes sync_lock(); taken before mutex_.
    os::Mutex lock_mutex_;
    core::WakeLock admin_lock_;
};

}  // namespace wisp::admin
