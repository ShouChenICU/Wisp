// PatchQueue (see wisp/scene/patch_queue.hpp).

#include "wisp/scene/patch_queue.hpp"

#include <mutex>
#include <utility>
#include <variant>
#include <vector>

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
    os::LargeVector<Item> aside;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        dropped.swap(items_);
        aside.swap(aside_);
        index_.clear();
        head_seq_ = 0;
        counted_ = 0;
        refresh_ = false;
    }
}

void PatchQueue::count_locked() {
    counted_ = 0;
    for (const Item& item : items_)
        if (!item.refresh) ++counted_;
}

void PatchQueue::drop_instance_locked(uint32_t instance) {
    Items kept;
    for (Item& item : items_)
        if (item.instance != instance) kept.push_back(std::move(item));
    items_.swap(kept);
    std::erase_if(aside_, [&](const Item& item) { return item.instance == instance; });
    // Positions moved: nothing left may coalesce by its old sequence number.
    index_.clear();
    count_locked();
}

void PatchQueue::submit(UiBatch batch) {
    std::lock_guard<os::Mutex> lock(mutex_);
    size_t backlog = counted_;
    size_t first_new = items_.size();
    // While true, SetProps go with the Build queued last.
    bool after_build = false;
    for (UiOp& op : batch.ops) {
        auto* set = std::get_if<SetProp>(&op);
        if (set == nullptr) {
            if (std::holds_alternative<Build>(op)) {
                drop_instance_locked(batch.instance);
                backlog = counted_;
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
            if (!batch.refresh) ++counted_;
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
        if (!batch.refresh) ++counted_;
    }
    if (batch.refresh) {
        // The kept ScrollTos and Focuses land after the values they may
        // depend on (a list's rows, an input that was hidden).
        for (Item& item : aside_) {
            if (item.instance != batch.instance) continue;
            items_.push_back(std::move(item));
            ++counted_;
        }
        aside_.clear();
        index_.clear();
    } else if (backlog > 0 && counted_ > max_pending_) {
        collapse_locked(first_new);
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

void PatchQueue::collapse_locked(size_t end) {
    size_t start = 0;
    for (size_t i = end; i-- > 0;) {
        if (std::holds_alternative<Build>(items_[i].op)) {
            start = i;
            break;
        }
    }
    Items kept;
    for (size_t i = start; i < end; ++i) {
        Item& item = items_[i];
        if (std::holds_alternative<SetProp>(item.op)) continue;
        if (std::holds_alternative<ScrollTo>(item.op) || std::holds_alternative<Focus>(item.op)) {
            keep_aside_locked(std::move(item));
            continue;
        }
        kept.push_back(std::move(item));
    }
    if (kept.size() > max_pending_) {
        bool build = !kept.empty() && std::holds_alternative<Build>(kept.front().op);
        kept.resize(build ? 1 : 0);
    }
    for (size_t i = end; i < items_.size(); ++i) kept.push_back(std::move(items_[i]));
    items_.swap(kept);
    index_.clear();
    count_locked();
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
            if (!front.refresh) --counted_;
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
