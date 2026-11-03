#pragma once

// Fan-out of log lines and selected events to the admin WebSocket clients
// (spec 6.2). A client subscribes by sending {"sub":["log","events"]}
// (either or both; a later message replaces the set):
//   - "log": the current log ring is replayed first, one
//     {"t":"log","ts":0,"level":"I","tag":"","msg":<ring line>} per line,
//     then every new line streams as {"t":"log","ts","level","tag","msg"}.
//   - "events": events under net., power., storage. and system. arrive as
//     {"t":"event","topic","payload"}.
//
// Backpressure: each client has a bounded queue (max_queue messages). When
// it is full the oldest message is dropped and counted; the next send to
// that client is {"t":"dropped","count":N}. A send_text() that reports a
// full socket queue keeps the message at the head for the next pump().
// The replay is limited to the newest max_queue ring lines; older ones are
// skipped without being reported as dropped, since the client never fell
// behind on them.
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
//   - open()/on_message()/close() come from HTTP server threads.
// Lock order: the hub's mutex may be held while taking the log module's
// mutex (ring replay), never the other way round; the log module calls
// sinks outside its own lock.

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/http.hpp"
#include "wisp/os/os.hpp"

namespace wisp::admin {

class WsHub : public LogSink {
public:
    // Registers itself as a log sink and subscribes to the forwarded event
    // prefixes; does not start the sender task.
    WsHub(core::EventBus& bus, size_t max_clients = 2, size_t max_queue = 256);
    // Stops the sender task, unsubscribes and unregisters the log sink,
    // waiting out any write() or event handler still running.
    ~WsHub() override;

    WsHub(const WsHub&) = delete;
    WsHub& operator=(const WsHub&) = delete;

    // Spawns the sender task that calls pump() whenever something is
    // queued (and every 100 ms while a socket keeps refusing). Idempotent.
    void start();
    // Joins the sender task; a send_text() in progress is waited out.
    // Idempotent.
    void stop();

    // Admits a connection; false when max_clients are already open or the
    // id is taken. New connections are subscribed to nothing.
    bool open(std::shared_ptr<net::WsConnection> conn);
    // Handles a client's {"sub":[...]} message; anything else is ignored.
    void on_message(int id, std::string_view text);
    // Forgets the connection and drops whatever is still queued for it.
    void close(int id);

    // Only enqueues; never blocks on a connection.
    void write(LogLevel level, const char* tag, const char* msg) override;

    // Sends what is queued, per client up to one queue's worth plus the
    // dropped notice, stopping at that client's first refused send.
    void pump();

private:
    struct Client;

    void on_event(const core::Event& e);
    void enqueue_locked(Client& c, std::string msg);
    bool has_pending() const;
    void run();

    core::EventBus& bus_;
    const size_t max_clients_;
    const size_t max_queue_;

    mutable os::Mutex mutex_;  // guards clients_ and every Client's fields
    std::vector<std::shared_ptr<Client>> clients_;
    // Number of clients subscribed to each stream, so write() and the
    // event handler skip formatting when nobody listens. Changed under
    // mutex_; read without it.
    std::atomic<int> log_subscribers_{0};
    std::atomic<int> event_subscribers_{0};

    os::Mutex pump_mutex_;  // serializes pump()
    os::Semaphore wake_;    // given whenever something is enqueued
    os::Mutex lifecycle_mutex_;
    std::unique_ptr<os::Task> task_;
    std::atomic<bool> running_{false};

    std::vector<core::SubscriptionId> subscriptions_;
};

}  // namespace wisp::admin
