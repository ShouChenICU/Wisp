#pragma once

// Records, in publish order, the frames the admin WebSocket sends an
// "events" client (admin::event_frame(), the builder WsHub uses) for the
// topics whose wire format a test pins: those wisp/core/topics.hpp marks
// CellMirror and that WsHub::forwards(). Those are the events a state
// cell's mirror publishes, so a golden list of these frames shows that
// moving a publisher behind a cell left the wire byte-identical.
// The recorder sees the same events in the same order as the hub, by
// construction: every publish goes through the bus's one FIFO and is handed
// to every subscriber by its one dispatcher thread.
//
// One field depends on the clock: a non-zero "expires_ms" of
// net.pairing.code (now + the code's lifetime) is replaced with the string
// kNormalisedExpiry. Zero (no code) and a value that is not an integer are
// kept, so a change of either still shows.

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/admin/ws_hub.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/topics.hpp"

namespace wisp::test {

inline constexpr char kNormalisedExpiry[] = "<now+ttl>";

// Whether the recorder keeps `topic`: a CellMirror topic that the
// WebSocket forwards, except the media topics. Those were born behind
// their cells (media spec 9), so there is no earlier wire format to keep,
// and the library's frames at boot race the Wi-Fi ones on another task;
// app/media_wiring_test.cpp pins their payloads.
inline bool pinned_topic(std::string_view topic) {
    const core::topics::Topic* t = core::topics::find(topic);
    return t != nullptr && t->kind == core::topics::Kind::CellMirror &&
           admin::WsHub::forwards(topic) && !topic.starts_with("media.");
}

// The pinned topics, in registry order.
inline std::vector<std::string> pinned_topics() {
    std::vector<std::string> out;
    for (const core::topics::Topic& t : core::topics::kAll) {
        if (pinned_topic(t.name)) out.emplace_back(t.name);
    }
    return out;
}

// `e` as the recorder frames it: a non-zero integer expires_ms of
// net.pairing.code becomes kNormalisedExpiry.
inline core::Event normalised(core::Event e) {
    if (e.topic != core::topics::kNetPairingCode) return e;
    auto it = e.payload.find("expires_ms");
    if (it != e.payload.end() && it->second.type() == Value::Type::Int &&
        it->second.as_int() != 0) {
        it->second = Value(kNormalisedExpiry);
    }
    return e;
}

class ForwardedRecorder {
public:
    // Subscribes to every topic on construction.
    explicit ForwardedRecorder(core::EventBus& bus) : bus_(bus) {
        sub_ = bus_.subscribe("", [this](const core::Event& e) {
            if (!pinned_topic(e.topic)) return;
            std::string frame = admin::event_frame(normalised(e));
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(std::move(frame));
        });
    }
    ~ForwardedRecorder() { bus_.unsubscribe(sub_); }

    ForwardedRecorder(const ForwardedRecorder&) = delete;
    ForwardedRecorder& operator=(const ForwardedRecorder&) = delete;

    // The frames so far, cleared. Call after bus.wait_idle(), or the last
    // events may still be on their way.
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.swap(frames_);
        return out;
    }

    // How many frames are waiting to be taken.
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

private:
    core::EventBus& bus_;
    core::SubscriptionId sub_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> frames_;
};

}  // namespace wisp::test
