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
// for both. The value moves ahead of SetProps for other props queued in
// between, so if a frame boundary falls there, one frame can show a mix
// of values the scene never had at once; the next frame catches up.
//
// A Build replaces the whole tree, so it drops every op of its instance
// queued before it.
//
// Overflow is about a backlog, never about one batch: a batch is always
// queued whole (its size is already capped by kMaxBatchBytes) and applied
// over as many frames as it takes. Only when ops left over from earlier
// batches and the new batch together pass `max_pending` is the backlog
// (the earlier part) collapsed: its SetProps go, everything before its
// last Build goes, and of its ScrollTos and Focuses only the newest per
// node (one Focus in all) is kept aside. If what remains of it is still
// over `max_pending`, only that Build stays. take_refresh_request() then
// reports true once, and the scene task answers with a batch marked
// UiBatch::refresh that re-sends every bound value
// (__wisp.refresh_all()); the kept ScrollTos and Focuses are queued
// right after it. A refresh batch never counts towards an overflow, so
// answering one can never ask for another, however many bindings the
// scene has.
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
        bool refresh = false;        // from a refresh batch: not counted for overflow
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

    // Drops every queued and kept-aside op of `instance`.
    void drop_instance_locked(uint32_t instance);
    // Collapses items_[0, end) (see Overflow above).
    void collapse_locked(size_t end);
    void keep_aside_locked(Item item);
    void count_locked();

    const size_t per_frame_;
    const size_t max_pending_;
    mutable os::Mutex mutex_;
    Items items_;
    Index index_;
    uint64_t head_seq_ = 0;  // sequence number of items_.front()
    size_t counted_ = 0;     // items_ not from a refresh batch
    // ScrollTos and Focuses kept from a collapse, queued after the
    // refresh batch.
    os::LargeVector<Item> aside_;
    bool refresh_ = false;
};

}  // namespace wisp::scene
