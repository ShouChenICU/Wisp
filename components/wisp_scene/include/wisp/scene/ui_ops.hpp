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
inline constexpr uint32_t kEventSwipe = 8;

struct NodeDesc {
    NodeId id = 0;
    NodeId parent = 0;
    WidgetId widget = 0;
    uint32_t events = 0;  // kEvent* bits
    os::LargeVector<std::pair<PropId, PropValue>> props;
    int line = 0;  // the line of the entry file that built it
};

// pad = {top, right, bottom, left} travels as one integer, so PropValue
// needs no table type: 10 bits per side (each is 0..512, schema
// prop_range()) under a tag bit that no plain pad (also 0..512) has.
struct PadSides {
    int32_t top = 0, right = 0, bottom = 0, left = 0;
};

inline constexpr int64_t kPadSidesTag = int64_t{1} << 40;

constexpr int64_t encode_pad_sides(int32_t top, int32_t right, int32_t bottom, int32_t left) {
    return kPadSidesTag | (int64_t{top & 0x3ff} << 30) | (int64_t{right & 0x3ff} << 20) |
           (int64_t{bottom & 0x3ff} << 10) | int64_t{left & 0x3ff};
}

// False for anything encode_pad_sides() did not make (a plain pad).
constexpr bool decode_pad_sides(int64_t v, PadSides& out) {
    if ((v & ~(kPadSidesTag - 1)) != kPadSidesTag) return false;
    out = {static_cast<int32_t>((v >> 30) & 0x3ff), static_cast<int32_t>((v >> 20) & 0x3ff),
           static_cast<int32_t>((v >> 10) & 0x3ff), static_cast<int32_t>(v & 0x3ff)};
    return true;
}

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
