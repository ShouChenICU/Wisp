#pragma once

// PatchQueue: the UI batches a scene task has sent and the UI thread has
// not applied yet (scene-runtime-core spec 3.2, 10). The scene task
// submits a batch at the end of each message; the UI thread's frame hook
// applies at most `per_frame` ops each loop iteration, so one busy scene
// cannot stall rendering.
//
// Counting: a Build is one op, and the SetProps that follow it in the
// same batch (the values its bindings had) go with it, uncounted, so a
// tree never shows half-set. Every other op counts one.
//
// Coalescing: a SetProp for a (node, prop) that already has an unapplied
// SetProp of the same instance replaces that one's value in place, as
// long as no other op was queued after it; the queue then holds one op
// for both.
//
// Overflow: when more than `max_pending` ops wait, the SetProps, ScrollTos
// and Focuses waiting are dropped, and everything before the last waiting
// Build (which replaces the whole tree anyway); if that still leaves too
// many, only that Build stays. take_refresh_request() then reports true
// once, and the scene task re-sends every bound value
// (__wisp.refresh_all()), which brings the dropped SetProps back at their
// current values. A dropped ScrollTo or Focus is not repeated.
//
// Everything queued lives in PSRAM (spec 2.3).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>

#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/ui_ops.hpp"

namespace wisp::scene {

class SceneView;

class PatchQueue {
public:
    explicit PatchQueue(size_t per_frame = 64, size_t max_pending = 1024);

    PatchQueue(const PatchQueue&) = delete;
    PatchQueue& operator=(const PatchQueue&) = delete;

    // Scene task: true once after an overflow dropped updates.
    bool take_refresh_request();
    // Any thread.
    void submit(UiBatch batch);
    // UI thread: applies up to per_frame ops of `instance` to `view` and
    // returns how many; ops of any other instance are dropped uncounted.
    size_t apply_frame(SceneView& view, uint32_t instance);
    // Ops waiting, counted as apply_frame() counts them.
    size_t pending() const;
    // Drops everything, including a refresh request.
    void clear();

private:
    struct Item {
        uint32_t instance = 0;
        UiOp op;
        os::LargeVector<UiOp> with;  // the SetProps that came with a Build
    };
    using Items = std::deque<Item, os::LargeAllocator<Item>>;
    // (instance, node, prop) -> sequence number of its unapplied SetProp.
    struct Key {
        uint32_t instance;
        NodeId node;
        PropId prop;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<uint64_t>()((static_cast<uint64_t>(k.node) << 16 | k.prop) ^
                                         (static_cast<uint64_t>(k.instance) << 48));
        }
    };
    using Index = std::unordered_map<Key, uint64_t, KeyHash, std::equal_to<Key>,
                                     os::LargeAllocator<std::pair<const Key, uint64_t>>>;

    void push_locked(Item item);
    void collapse_locked();

    const size_t per_frame_;
    const size_t max_pending_;
    mutable os::Mutex mutex_;
    Items items_;
    Index index_;
    uint64_t head_seq_ = 0;  // sequence number of items_.front()
    bool refresh_ = false;
};

}  // namespace wisp::scene
