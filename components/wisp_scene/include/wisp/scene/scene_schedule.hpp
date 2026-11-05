#pragma once

// Which scenes in the background close to keep within the background's
// count and the VM budget all scenes share (scene-device spec 3.5). A pure
// function; SceneRuntime calls it at its checkpoints (opening a scene,
// sending one to the background, before each message while a scene is in
// front) and closes what it picks.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wisp::scene {

// At most this many scenes run in the background.
inline constexpr size_t kMaxBackground = 3;

struct SlotInfo {
    std::string id;
    bool foreground = false;
    // os::now_ms() when it last left the front; 0: never in front.
    uint64_t last_front_ms = 0;
    // What its VM holds now.
    size_t vm_bytes = 0;
};

struct Eviction {
    std::string id;
    bool memory = false;  // closed for the budget, else for the count
};

// Background scenes to close, least recently in front first (equal times
// in the order given): past max_background, then (when a foreground scene
// exists or `opening`) while the background's bytes exceed budget -
// foreground_reserve (0 when the reserve is the whole budget or more).
// The foreground's own bytes never count: it is guaranteed its reserve,
// whatever it holds now.
std::vector<Eviction> pick_evictions(std::span<const SlotInfo> slots, bool opening,
                                     size_t max_background, size_t budget,
                                     size_t foreground_reserve);

}  // namespace wisp::scene
