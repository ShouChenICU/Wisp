#include "wisp/admin/ws_hub.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"

namespace wisp::admin {

namespace {

constexpr const char* kTag = "ws";

// Event prefixes forwarded to "events" subscribers (spec 6.2).
constexpr const char* kForwardedPrefixes[] = {"net.", "power.", "storage.", "system."};

// While a socket keeps refusing, the sender retries this often.
constexpr uint32_t kRetryMs = 100;

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "D";
        case LogLevel::Info:
            return "I";
        case LogLevel::Warn:
            return "W";
        case LogLevel::Error:
            return "E";
    }
    return "I";
}

std::string log_json(int64_t ts, const char* level, std::string tag, std::string msg) {
    return to_json(Value(Map{{"t", Value("log")},
                             {"ts", Value(ts)},
                             {"level", Value(level)},
                             {"tag", Value(std::move(tag))},
                             {"msg", Value(std::move(msg))}}));
}

}  // namespace

struct WsHub::Client {
    std::shared_ptr<net::WsConnection> conn;
    int id = 0;
    std::string client_id;  // the paired client whose token opened it
    bool log = false;
    bool events = false;
    // Set once the client is forgotten (close(), close_client(),
    // close_all()); pump() may still hold a reference to this client.
    bool closed = false;
    std::deque<std::string> queue;
    size_t queued_bytes = 0;  // sum of queue's message sizes
    uint64_t dropped = 0;
};

WsHub::WsHub(core::EventBus& bus, size_t max_clients, size_t max_queue, size_t max_queue_bytes)
    : bus_(bus),
      max_clients_(max_clients),
      max_queue_(max_queue),
      max_queue_bytes_(max_queue_bytes),
      wake_(1, 0) {
    for (const char* prefix : kForwardedPrefixes) {
        subscriptions_.push_back(
            bus_.subscribe(prefix, [this](const core::Event& e) { on_event(e); }));
    }
    log::add_sink(this);
}

WsHub::~WsHub() {
    stop();
    // Both calls wait out a write()/handler still running on another
    // thread, so nothing calls into this object once they return.
    log::remove_sink(this);
    for (core::SubscriptionId id : subscriptions_) bus_.unsubscribe(id);
}

void WsHub::start() {
    std::lock_guard<os::Mutex> lock(lifecycle_mutex_);
    if (task_) return;
    running_ = true;
    task_ =
        os::Task::spawn({.name = "wisp.ws", .stack_bytes = 4096, .priority = 3}, [this] { run(); });
}

void WsHub::stop() {
    std::unique_ptr<os::Task> task;
    {
        std::lock_guard<os::Mutex> lock(lifecycle_mutex_);
        if (!task_) return;
        running_ = false;
        task = std::move(task_);
    }
    wake_.give();
    task->join();
}

void WsHub::run() {
    while (running_) {
        // Sleep until something is enqueued; poll only while a socket is
        // refusing, so an idle hub costs no wake-ups.
        wake_.take(has_pending() ? kRetryMs : os::kForever);
        if (!running_) break;
        pump();
    }
}

bool WsHub::open(std::shared_ptr<net::WsConnection> conn, std::string client_id) {
    if (!conn) return false;
    const int id = conn->id();
    enum class Outcome { Accepted, IdTaken, Full } outcome = Outcome::Accepted;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        const bool taken = std::any_of(clients_.begin(), clients_.end(),
                                       [id](const auto& c) { return c->id == id; });
        if (taken) {
            outcome = Outcome::IdTaken;
        } else if (clients_.size() >= max_clients_) {
            outcome = Outcome::Full;
        } else {
            auto c = std::make_shared<Client>();
            c->conn = std::move(conn);
            c->id = id;
            c->client_id = std::move(client_id);
            clients_.push_back(std::move(c));
        }
    }
    // Logged outside mutex_: write() takes it.
    switch (outcome) {
        case Outcome::Accepted:
            WLOG_I(kTag, "client %d connected", id);
            break;
        case Outcome::IdTaken:
            WLOG_W(kTag, "client %d refused: id already open", id);
            break;
        case Outcome::Full:
            WLOG_W(kTag, "client %d refused: at most %u connections", id,
                   static_cast<unsigned>(max_clients_));
            break;
    }
    return outcome == Outcome::Accepted;
}

void WsHub::on_message(int id, std::string_view text) {
    auto parsed = from_json(text);
    if (!parsed) return;
    const Value* sub = parsed.value().find("sub");
    if (sub == nullptr || sub->type() != Value::Type::List) return;
    bool want_log = false;
    bool want_events = false;
    for (const Value& v : sub->as_list()) {
        if (v.type() != Value::Type::String) continue;
        if (v.as_string() == "log") want_log = true;
        if (v.as_string() == "events") want_events = true;
    }

    bool queued = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        auto it = std::find_if(clients_.begin(), clients_.end(),
                               [id](const auto& c) { return c->id == id; });
        if (it == clients_.end()) return;
        Client& c = **it;
        if (want_events != c.events) {
            c.events = want_events;
            event_subscribers_ += want_events ? 1 : -1;
        }
        if (want_log != c.log) {
            c.log = want_log;
            log_subscribers_ += want_log ? 1 : -1;
            if (want_log) {
                // The subscription is visible to write() before the ring
                // is read, and write() waits on mutex_ until the replay is
                // queued: a line racing the subscription may arrive twice
                // but is never lost, and the replay always comes first.
                const std::string ring = log::ring_snapshot();
                std::vector<std::string_view> lines;
                size_t start = 0;
                while (start < ring.size()) {
                    size_t nl = ring.find('\n', start);
                    if (nl == std::string::npos) nl = ring.size();
                    lines.push_back(std::string_view(ring).substr(start, nl - start));
                    start = nl + 1;
                }
                // Newest first, as many as an empty queue holds.
                std::vector<std::string> replay;
                size_t bytes = 0;
                for (size_t i = lines.size(); i > 0 && replay.size() < max_queue_; --i) {
                    std::string msg = log_json(0, "I", std::string(), std::string(lines[i - 1]));
                    if (bytes + msg.size() > max_queue_bytes_) break;
                    bytes += msg.size();
                    replay.push_back(std::move(msg));
                }
                for (auto it = replay.rbegin(); it != replay.rend(); ++it) {
                    enqueue_locked(c, std::move(*it));
                    queued = true;
                }
            }
        }
    }
    if (queued) wake_.give();
}

void WsHub::forget_locked(Client& c) {
    if (c.log) --log_subscribers_;
    if (c.events) --event_subscribers_;
    c.closed = true;
    c.queue.clear();
    c.queued_bytes = 0;
}

void WsHub::close(int id) {
    bool found = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        auto it = std::find_if(clients_.begin(), clients_.end(),
                               [id](const auto& c) { return c->id == id; });
        if (it != clients_.end()) {
            forget_locked(**it);
            clients_.erase(it);
            found = true;
        }
    }
    if (found) WLOG_I(kTag, "client %d disconnected", id);
}

void WsHub::close_matching(const std::function<bool(const Client&)>& match) {
    std::vector<std::shared_ptr<Client>> closing;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (match(**it)) {
                forget_locked(**it);
                closing.push_back(std::move(*it));
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Outside mutex_: a server may report the close straight back through
    // close(), and the log line goes through write().
    for (const auto& c : closing) {
        c->conn->close();
        WLOG_I(kTag, "client %d closed: its pairing was revoked", c->id);
    }
}

void WsHub::close_client(std::string_view client_id) {
    if (client_id.empty()) return;
    close_matching([client_id](const Client& c) { return c.client_id == client_id; });
}

void WsHub::close_all() {
    close_matching([](const Client&) { return true; });
}

void WsHub::write(LogLevel level, const char* tag, const char* msg) {
    // Runs on the logging thread: format, append, signal. No logging and no
    // connection calls from here.
    if (log_subscribers_.load() == 0) return;
    const std::string json = log_json(static_cast<int64_t>(os::now_ms()), level_name(level),
                                      tag != nullptr ? tag : "", msg != nullptr ? msg : "");
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        for (auto& c : clients_) {
            if (c->log) enqueue_locked(*c, json);
        }
    }
    wake_.give();
}

void WsHub::on_event(const core::Event& e) {
    if (event_subscribers_.load() == 0) return;
    const std::string json = to_json(Value(
        Map{{"t", Value("event")}, {"topic", Value(e.topic)}, {"payload", Value(e.payload)}}));
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        for (auto& c : clients_) {
            if (c->events) enqueue_locked(*c, json);
        }
    }
    wake_.give();
}

void WsHub::enqueue_locked(Client& c, std::string msg) {
    if (msg.size() > max_queue_bytes_) {
        ++c.dropped;
        return;
    }
    while (!c.queue.empty() &&
           (c.queue.size() >= max_queue_ || c.queued_bytes + msg.size() > max_queue_bytes_)) {
        c.queued_bytes -= c.queue.front().size();
        c.queue.pop_front();
        ++c.dropped;
    }
    c.queued_bytes += msg.size();
    c.queue.push_back(std::move(msg));
}

bool WsHub::has_pending() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return std::any_of(clients_.begin(), clients_.end(),
                       [](const auto& c) { return !c->queue.empty() || c->dropped > 0; });
}

void WsHub::pump() {
    std::lock_guard<os::Mutex> serialize(pump_mutex_);
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        clients = clients_;
    }
    for (const auto& c : clients) {
        // Bounded so a client that keeps up with a log flood cannot keep
        // the sender here forever; the next wake continues.
        for (size_t n = 0; n <= max_queue_; ++n) {
            std::string msg;
            uint64_t reported = 0;
            {
                std::lock_guard<os::Mutex> lock(mutex_);
                if (c->closed) break;
                if (c->dropped > 0) {
                    reported = c->dropped;
                    msg = to_json(Value(Map{{"t", Value("dropped")},
                                            {"count", Value(static_cast<int64_t>(reported))}}));
                } else if (!c->queue.empty()) {
                    msg = std::move(c->queue.front());
                    c->queue.pop_front();
                    c->queued_bytes -= msg.size();
                } else {
                    break;
                }
            }
            // send_text() runs without mutex_ so write() never waits on a
            // socket.
            const bool sent = c->conn->send_text(msg);
            std::lock_guard<os::Mutex> lock(mutex_);
            if (reported > 0) {
                // More may have been dropped while sending; keep those.
                if (sent) c->dropped -= reported;
            } else if (!sent && !c->closed) {
                // Back to the head. If write() filled the queue meanwhile,
                // this message is now the oldest one and is the one dropped.
                if (c->queue.size() >= max_queue_ ||
                    c->queued_bytes + msg.size() > max_queue_bytes_) {
                    ++c->dropped;
                } else {
                    c->queued_bytes += msg.size();
                    c->queue.push_front(std::move(msg));
                }
            }
            if (!sent) break;
        }
    }
}

}  // namespace wisp::admin
