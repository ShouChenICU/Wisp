#pragma once

// The registry of every event topic the system publishes on the EventBus
// (spec 3.7 of the state-cells design). Publishers and subscribers name
// topics through these constants; docs/architecture.md's "Event topics"
// table lists the same names.
//
// Each topic has a kind, a delivery promise that tells a consumer what it
// may rely on (spec 3.1):
//   - CellMirror: the bus copy of a state cell. The event may be lost; a
//     consumer that must not miss a change follows the cell instead.
//   - Notification: may be lost; nobody's correctness depends on it (the
//     web pages, logs, refresh hints).
//   - Telemetry: like Notification, for counters and diagnostics.
//   - InputEdge: the observable copy of edges whose consumers take them
//     from an EdgeInbox, where every edge arrives.
// The kinds describe where the state-cells migration ends; until each
// owner and consumer has moved, today's code may still treat a topic as
// it always has. scene.state is a Notification: no cell mirrors it (spec
// 3.4: the scene overview cell and the lifecycle inbox serve consumers).
//
// `forwarded` says whether the admin WebSocket's "events" stream carries
// the topic; admin::WsHub::forwards() decides that, and a test keeps the
// two in agreement.
//
// Button topics are built per device: kButtonPrefix's "button." is the
// start of every button device id (chosen by the board), and the topic is
// "<device id>.<edge>", e.g. "button.power.short". The registry lists
// them as patterns: a row whose name contains '<' is a pattern, not a
// literal topic (tools/check_topics.py relies on this marker). A
// button's device id is "button.<name>", so the patterns read
// "button.<name>.<edge>": "<name>" is the part of the id after "button.",
// and the edge is one of kButtonDown/kButtonUp/kButtonShort/kButtonLong.
// A constant whose value contains a '.' and does not end with '.' is a topic
// name and must appear in kAll (tools/check_topics.py checks); the others are
// parts of a topic.
//
// Only constant data: no static constructors, nothing allocated; the
// arrays are inline variables, so one copy in read-only data however many
// files include this.

#include <cstdint>
#include <string_view>

namespace wisp::core::topics {

enum class Kind : uint8_t { CellMirror, Notification, Telemetry, InputEdge };

struct Topic {
    std::string_view name;
    Kind kind;
    bool forwarded;
};

// Prefix of button device ids, for subscriptions and checks; the topic itself is
// "<device id>.<edge>", so this is never prepended to an id.
inline constexpr char kButtonPrefix[] = "button.";
inline constexpr char kButtonDown[] = "down", kButtonUp[] = "up", kButtonShort[] = "short",
                      kButtonLong[] = "long";
inline constexpr char kPowerState[] = "power.state";
inline constexpr char kPowerWakelock[] = "power.wakelock";
inline constexpr char kPowerChargingChanged[] = "power.charging.changed";
inline constexpr char kPowerBatteryLow[] = "power.battery.low";
inline constexpr char kNotificationPosted[] = "notification.posted";
inline constexpr char kNotificationDismissed[] = "notification.dismissed";
inline constexpr char kStorageSdInserted[] = "storage.sd.inserted";
inline constexpr char kStorageSdRemoved[] = "storage.sd.removed";
inline constexpr char kSystemBootCompleted[] = "system.boot.completed";
inline constexpr char kSystemFactoryReset[] = "system.factory_reset";
inline constexpr char kSystemFirmware[] = "system.firmware";
inline constexpr char kSystemSettings[] = "system.settings";
inline constexpr char kSystemTime[] = "system.time";
inline constexpr char kNetWifiEnabled[] = "net.wifi.enabled";
inline constexpr char kNetWifiState[] = "net.wifi.state";
inline constexpr char kNetApState[] = "net.ap.state";
inline constexpr char kNetPairingCode[] = "net.pairing.code";
inline constexpr char kNetPairingFailed[] = "net.pairing.failed";
inline constexpr char kSceneState[] = "scene.state";
inline constexpr char kModelProfiles[] = "model.profiles";
inline constexpr char kAgentState[] = "agent.state";
inline constexpr char kVoiceState[] = "voice.state";
inline constexpr char kMemoryChanged[] = "memory.changed";
inline constexpr char kTodoChanged[] = "todo.changed";

inline constexpr Topic kAll[] = {
    {"button.<name>.down", Kind::InputEdge, false},
    {"button.<name>.up", Kind::InputEdge, false},
    {"button.<name>.short", Kind::InputEdge, false},
    {"button.<name>.long", Kind::InputEdge, false},
    {kPowerState, Kind::CellMirror, true},
    {kPowerWakelock, Kind::Telemetry, true},
    {kPowerChargingChanged, Kind::CellMirror, true},
    {kPowerBatteryLow, Kind::CellMirror, true},
    {kNotificationPosted, Kind::Notification, false},
    {kNotificationDismissed, Kind::Notification, false},
    {kStorageSdInserted, Kind::CellMirror, true},
    {kStorageSdRemoved, Kind::CellMirror, true},
    {kSystemBootCompleted, Kind::Notification, true},
    {kSystemFactoryReset, Kind::CellMirror, true},
    {kSystemFirmware, Kind::CellMirror, true},
    {kSystemSettings, Kind::CellMirror, true},
    {kSystemTime, Kind::CellMirror, true},
    {kNetWifiEnabled, Kind::CellMirror, true},
    {kNetWifiState, Kind::CellMirror, true},
    {kNetApState, Kind::CellMirror, true},
    {kNetPairingCode, Kind::CellMirror, true},
    {kNetPairingFailed, Kind::Notification, true},
    {kSceneState, Kind::Notification, true},
    {kModelProfiles, Kind::Notification, false},
    {kAgentState, Kind::CellMirror, false},
    {kVoiceState, Kind::CellMirror, false},
    {kMemoryChanged, Kind::Notification, true},
    {kTodoChanged, Kind::Notification, true},
};

// The registered entry named exactly `name` (a button topic matches only
// its pattern, e.g. "button.<name>.down"); nullptr when unregistered.
constexpr const Topic* find(std::string_view name) {
    for (const Topic& t : kAll) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

}  // namespace wisp::core::topics
