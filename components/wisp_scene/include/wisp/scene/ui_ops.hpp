#pragma once

// The UI operations a scene sends from its task to the UI thread
// (scene-runtime-core spec 3.1-3.2). Everything here lives in PSRAM
// (spec 2.3): widget tree descriptions and patch queues can be large.

#include <cstdint>
#include <utility>
#include <variant>

#include "wisp/os/large.hpp"

namespace wisp::scene {

using NodeId = uint32_t;
using PropId = uint16_t;
using WidgetId = uint16_t;

using PropValue = std::variant<std::monostate, bool, int64_t, double, os::LargeString>;

// Event bits of NodeDesc::events.
inline constexpr uint32_t kEventClick = 1;
inline constexpr uint32_t kEventLongPress = 2;
inline constexpr uint32_t kEventChange = 4;

struct NodeDesc {
    NodeId id = 0;
    NodeId parent = 0;
    WidgetId widget = 0;
    uint32_t events = 0;  // kEvent* bits
    os::LargeVector<std::pair<PropId, PropValue>> props;
    int line = 0;  // the line of the entry file that built it
};

struct SetProp {
    NodeId node = 0;
    PropId prop = 0;
    PropValue value;
};

struct Build {
    os::LargeVector<NodeDesc> nodes;  // nodes[0] is the root
};

struct ListReset {
    NodeId list = 0;
    uint32_t count = 0;
};

struct ListRows {
    NodeId list = 0;
    uint32_t first = 0;
    os::LargeVector<os::LargeVector<NodeDesc>> rows;
};

struct CanvasDraw {
    NodeId canvas = 0;
    os::LargeVector<uint8_t> commands;
};

struct ScrollTo {
    NodeId node = 0;
    uint32_t index = 0;  // 0-based
};

// Focuses an input (a ui.ref handle's :focus()).
struct Focus {
    NodeId node = 0;
};

using UiOp = std::variant<SetProp, Build, ListReset, ListRows, CanvasDraw, ScrollTo, Focus>;

struct UiBatch {
    uint32_t instance = 0;
    os::LargeVector<UiOp> ops;
    // Set on the batch that answers PatchQueue::take_refresh_request(): it
    // re-sends every bound value (__wisp.refresh_all()).
    bool refresh = false;
};

}  // namespace wisp::scene
