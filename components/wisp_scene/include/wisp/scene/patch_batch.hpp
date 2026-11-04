#pragma once

// The UI operations one scene message produces, collected on the scene
// task and handed to the UI thread as one batch when the message ends
// (scene-runtime-core spec 3.2). A SetProp for a (node, prop) that is
// already in the batch only replaces its value: it keeps the position of
// the first one, so a property set three times in one message is one
// patch. Every other operation is appended in order. Ops and the index
// live in PSRAM (spec 2.3).
//
// Limits (spec 5.6): a string property value is at most
// kMaxPropStringBytes ("value too large"), and one batch at most
// kMaxBatchBytes ("ui update too large"). The batch size is an estimate:
// kOpOverheadBytes for every op, every NodeDesc in a Build or ListRows and
// every prop of those nodes, plus the bytes of every string value and of
// CanvasDraw commands. A SetProp that replaces a coalesced value counts
// only the new value. A refused op leaves the batch as it was. Scene code
// can bind one Lua string to many widgets, and every op copies it, so
// without the cap a single message could exhaust PSRAM, where
// os::LargeAllocator aborts. Every producer (emit, and the Build,
// ListRows, ScrollTo, Focus and CanvasDraw bindings) goes through
// set_prop()/push() and turns a refusal into a Lua error; the scene task
// then discards the batch.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

#include "wisp/base/result.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/ui_ops.hpp"

namespace wisp::scene {

inline constexpr size_t kMaxPropStringBytes = 64 * 1024;
inline constexpr size_t kMaxBatchBytes = 1024 * 1024;
// Roughly a UiOp, NodeDesc or prop pair plus its share of the index.
inline constexpr size_t kOpOverheadBytes = 64;

class PatchBatch {
public:
    // Errors: InvalidArgument "value too large", QuotaExceeded "ui update
    // too large".
    Result<void> set_prop(NodeId node, PropId prop, PropValue value);
    // A SetProp goes through set_prop(); anything else is appended.
    Result<void> push(UiOp op);
    // The estimated size, never above kMaxBatchBytes.
    size_t bytes() const { return bytes_; }
    // Moves the ops out (tagged with `instance`) and empties the batch.
    UiBatch take(uint32_t instance);
    size_t size() const { return ops_.size(); }
    void clear();

private:
    using Index = std::unordered_map<uint64_t, size_t, std::hash<uint64_t>, std::equal_to<uint64_t>,
                                     os::LargeAllocator<std::pair<const uint64_t, size_t>>>;

    os::LargeVector<UiOp> ops_;
    Index index_;  // (node << 16 | prop) -> position of its SetProp in ops_
    size_t bytes_ = 0;
};

}  // namespace wisp::scene
