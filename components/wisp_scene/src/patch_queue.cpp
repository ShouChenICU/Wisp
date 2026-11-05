// PatchQueue (see wisp/scene/patch_queue.hpp).

#include "wisp/scene/patch_queue.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

#include "wisp/scene/scene_view.hpp"

namespace wisp::scene {

PatchQueue::PatchQueue(size_t per_frame, size_t max_pending)
    : per_frame_(per_frame > 0 ? per_frame : 1), max_pending_(max_pending) {}

bool PatchQueue::take_refresh_request(uint32_t instance) {
    std::lock_guard<os::Mutex> lock(mutex_);
    return std::erase(refresh_, instance) > 0;
}

size_t PatchQueue::pending() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return items_.size();
}

void PatchQueue::clear(uint32_t instance) {
    std::lock_guard<os::Mutex> lock(mutex_);
    drop_instance_locked(instance);
    std::erase(refresh_, instance);
}

size_t PatchQueue::counted_locked(uint32_t instance) const {
    size_t n = 0;
    for (const Item& item : items_)
        if (item.instance == instance && !item.refresh) ++n;
    return n;
}

void PatchQueue::drop_instance_locked(uint32_t instance) {
    Items kept;
    for (Item& item : items_)
        if (item.instance != instance) kept.push_back(std::move(item));
    items_.swap(kept);
    std::erase_if(aside_, [&](const Item& item) { return item.instance == instance; });
    // Positions moved: nothing left may coalesce by its old sequence number.
    index_.clear();
}

void PatchQueue::submit(UiBatch batch) {
    std::lock_guard<os::Mutex> lock(mutex_);
    // Overflow is counted per instance: another scene's backlog is its own.
    size_t backlog = batch.refresh ? 0 : counted_locked(batch.instance);
    size_t added = 0;
    size_t first_new = items_.size();
    // While true, SetProps go with the Build queued last.
    bool after_build = false;
    for (UiOp& op : batch.ops) {
        auto* set = std::get_if<SetProp>(&op);
        if (set == nullptr) {
            if (std::holds_alternative<Build>(op)) {
                drop_instance_locked(batch.instance);
                backlog = 0;
                added = 0;
                first_new = items_.size();
                after_build = true;
            } else {
                after_build = false;
            }
            // Nothing coalesces across another op.
            index_.clear();
            Item item;
            item.instance = batch.instance;
            item.op = std::move(op);
            item.refresh = batch.refresh;
            items_.push_back(std::move(item));
            if (!batch.refresh) ++added;
            continue;
        }
        if (after_build) {
            items_.back().with.push_back(std::move(op));
            continue;
        }
        Key key{batch.instance, set->node, set->prop};
        auto it = index_.find(key);
        if (it != index_.end() && it->second >= head_seq_) {
            // Index entries only ever name unapplied SetProps: any other
            // op, a collapse and dropping an instance clear the index.
            std::get<SetProp>(items_[it->second - head_seq_].op).value = std::move(set->value);
            continue;
        }
        index_[key] = head_seq_ + items_.size();
        Item item;
        item.instance = batch.instance;
        item.op = std::move(op);
        item.refresh = batch.refresh;
        items_.push_back(std::move(item));
        if (!batch.refresh) ++added;
    }
    if (batch.refresh) {
        // The kept ScrollTos and Focuses land after the values they may
        // depend on (a list's rows, an input that was hidden). Another
        // instance's wait for that instance's own refresh.
        for (Item& item : aside_) {
            if (item.instance == batch.instance) items_.push_back(std::move(item));
        }
        std::erase_if(aside_, [&](const Item& item) { return item.instance == batch.instance; });
        index_.clear();
    } else if (backlog > 0 && backlog + added > max_pending_) {
        collapse_locked(first_new, batch.instance);
    }
}

void PatchQueue::keep_aside_locked(Item item) {
    const UiOp& op = item.op;
    const auto* scroll = std::get_if<ScrollTo>(&op);
    std::erase_if(aside_, [&](const Item& old) {
        if (old.instance != item.instance) return false;
        if (scroll != nullptr) {
            const auto* s = std::get_if<ScrollTo>(&old.op);
            return s != nullptr && s->node == scroll->node;
        }
        return std::holds_alternative<Focus>(old.op);
    });
    aside_.push_back(std::move(item));
}

void PatchQueue::collapse_locked(size_t end, uint32_t instance) {
    // Only `instance`'s items in [0, end) are collapsed; every other item
    // keeps its place. Its own last Build, if any, starts what is kept.
    size_t start = 0;
    for (size_t i = end; i-- > 0;) {
        if (items_[i].instance == instance && std::holds_alternative<Build>(items_[i].op)) {
            start = i;
            break;
        }
    }
    size_t own = 0;  // `instance`'s items kept below
    for (size_t i = 0; i < end; ++i) {
        const Item& item = items_[i];
        if (item.instance != instance || i < start) continue;
        if (!std::holds_alternative<SetProp>(item.op) &&
            !std::holds_alternative<ScrollTo>(item.op) && !std::holds_alternative<Focus>(item.op))
            ++own;
    }
    // Still over: only its Build stays.
    const bool only_build = own > max_pending_;
    Items kept;
    for (size_t i = 0; i < end; ++i) {
        Item& item = items_[i];
        if (item.instance != instance) {
            kept.push_back(std::move(item));
            continue;
        }
        if (i < start || std::holds_alternative<SetProp>(item.op)) continue;
        if (std::holds_alternative<ScrollTo>(item.op) || std::holds_alternative<Focus>(item.op)) {
            keep_aside_locked(std::move(item));
            continue;
        }
        if (only_build && !(i == start && std::holds_alternative<Build>(item.op))) continue;
        kept.push_back(std::move(item));
    }
    for (size_t i = end; i < items_.size(); ++i) kept.push_back(std::move(items_[i]));
    items_.swap(kept);
    index_.clear();
    if (std::find(refresh_.begin(), refresh_.end(), instance) == refresh_.end())
        refresh_.push_back(instance);
}

size_t PatchQueue::apply_frame(SceneView& view, uint32_t instance) {
    os::LargeVector<Item> taken;
    Items dropped;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        while (!items_.empty() && taken.size() < per_frame_) {
            Item& front = items_.front();
            if (const auto* set = std::get_if<SetProp>(&front.op)) {
                auto it = index_.find(Key{front.instance, set->node, set->prop});
                if (it != index_.end() && it->second == head_seq_) index_.erase(it);
            }
            if (front.instance == instance)
                taken.push_back(std::move(front));
            else
                dropped.push_back(std::move(front));
            items_.pop_front();
            ++head_seq_;
        }
    }
    // Applied without the lock: the scene task keeps submitting meanwhile.
    for (const Item& item : taken) {
        view.apply(item.op);
        for (const UiOp& op : item.with) view.apply(op);
    }
    return taken.size();
}

}  // namespace wisp::scene
