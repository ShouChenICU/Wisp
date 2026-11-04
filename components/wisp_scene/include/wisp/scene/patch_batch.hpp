#pragma once

// The UI operations one scene message produces, collected on the scene
// task and handed to the UI thread as one batch when the message ends
// (scene-runtime-core spec 3.2). A SetProp for a (node, prop) that is
// already in the batch only replaces its value: it keeps the position of
// the first one, so a property set three times in one message is one
// patch. Every other operation is appended in order. Ops and the index
// live in PSRAM (spec 2.3).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

#include "wisp/os/large.hpp"
#include "wisp/scene/ui_ops.hpp"

namespace wisp::scene {

class PatchBatch {
public:
    void set_prop(NodeId node, PropId prop, PropValue value);
    // A SetProp goes through set_prop(); anything else is appended.
    void push(UiOp op);
    // Moves the ops out (tagged with `instance`) and empties the batch.
    UiBatch take(uint32_t instance);
    size_t size() const { return ops_.size(); }
    void clear();

private:
    using Index = std::unordered_map<uint64_t, size_t, std::hash<uint64_t>, std::equal_to<uint64_t>,
                                     os::LargeAllocator<std::pair<const uint64_t, size_t>>>;

    os::LargeVector<UiOp> ops_;
    Index index_;  // (node << 16 | prop) -> position of its SetProp in ops_
};

}  // namespace wisp::scene
