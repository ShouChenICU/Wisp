#include "wisp/scene/patch_batch.hpp"

#include <utility>

namespace wisp::scene {

void PatchBatch::set_prop(NodeId node, PropId prop, PropValue value) {
    uint64_t key = (static_cast<uint64_t>(node) << 16) | prop;
    auto [it, added] = index_.try_emplace(key, ops_.size());
    if (!added) {
        // The index only ever points at SetProps.
        if (auto* set = std::get_if<SetProp>(&ops_[it->second])) {
            set->value = std::move(value);
            return;
        }
        it->second = ops_.size();
    }
    ops_.emplace_back(SetProp{node, prop, std::move(value)});
}

void PatchBatch::push(UiOp op) {
    if (auto* set = std::get_if<SetProp>(&op)) {
        set_prop(set->node, set->prop, std::move(set->value));
        return;
    }
    ops_.push_back(std::move(op));
}

UiBatch PatchBatch::take(uint32_t instance) {
    UiBatch out;
    out.instance = instance;
    out.ops = std::move(ops_);
    clear();
    return out;
}

void PatchBatch::clear() {
    ops_.clear();
    index_.clear();
}

}  // namespace wisp::scene
