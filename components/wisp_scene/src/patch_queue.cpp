// PatchQueue (see wisp/scene/patch_queue.hpp).

#include "wisp/scene/patch_queue.hpp"

#include <mutex>
#include <utility>
#include <variant>

#include "wisp/scene/scene_view.hpp"

namespace wisp::scene {

PatchQueue::PatchQueue(size_t per_frame, size_t max_pending)
    : per_frame_(per_frame > 0 ? per_frame : 1), max_pending_(max_pending) {}

bool PatchQueue::take_refresh_request() {
    std::lock_guard<os::Mutex> lock(mutex_);
    return std::exchange(refresh_, false);
}

size_t PatchQueue::pending() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return items_.size();
}

void PatchQueue::clear() {
    Items dropped;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        dropped.swap(items_);
        index_.clear();
        head_seq_ = 0;
        refresh_ = false;
    }
}

void PatchQueue::submit(UiBatch batch) {
    std::lock_guard<os::Mutex> lock(mutex_);
    // While true, SetProps go with the Build queued last.
    bool after_build = false;
    for (UiOp& op : batch.ops) {
        auto* set = std::get_if<SetProp>(&op);
        if (set == nullptr) {
            // Nothing coalesces across another op.
            index_.clear();
            after_build = std::holds_alternative<Build>(op);
            Item item;
            item.instance = batch.instance;
            item.op = std::move(op);
            items_.push_back(std::move(item));
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
            // op clears the index, and so does collapse_locked().
            std::get<SetProp>(items_[it->second - head_seq_].op).value = std::move(set->value);
            continue;
        }
        index_[key] = head_seq_ + items_.size();
        Item item;
        item.instance = batch.instance;
        item.op = std::move(op);
        items_.push_back(std::move(item));
    }
    if (items_.size() > max_pending_) collapse_locked();
}

void PatchQueue::collapse_locked() {
    size_t start = 0;
    for (size_t i = items_.size(); i-- > 0;) {
        if (std::holds_alternative<Build>(items_[i].op)) {
            start = i;
            break;
        }
    }
    Items kept;
    for (size_t i = start; i < items_.size(); ++i) {
        const UiOp& op = items_[i].op;
        if (std::holds_alternative<SetProp>(op) || std::holds_alternative<ScrollTo>(op) ||
            std::holds_alternative<Focus>(op))
            continue;
        kept.push_back(std::move(items_[i]));
    }
    if (kept.size() > max_pending_) {
        bool build = !kept.empty() && std::holds_alternative<Build>(kept.front().op);
        kept.resize(build ? 1 : 0);
    }
    items_.swap(kept);
    index_.clear();
    refresh_ = true;
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
