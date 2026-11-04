#include "wisp/scene/patch_batch.hpp"

#include <cassert>
#include <utility>

namespace wisp::scene {

namespace {

size_t value_bytes(const PropValue& v) {
    const auto* s = std::get_if<os::LargeString>(&v);
    return s ? s->size() : 0;
}

size_t node_bytes(const NodeDesc& n) {
    size_t total = kOpOverheadBytes;
    for (const auto& [prop, value] : n.props) total += kOpOverheadBytes + value_bytes(value);
    return total;
}

size_t op_bytes(const UiOp& op) {
    size_t total = kOpOverheadBytes;
    if (const auto* set = std::get_if<SetProp>(&op)) {
        total += value_bytes(set->value);
    } else if (const auto* build = std::get_if<Build>(&op)) {
        for (const auto& n : build->nodes) total += node_bytes(n);
    } else if (const auto* rows = std::get_if<ListRows>(&op)) {
        for (const auto& row : rows->rows)
            for (const auto& n : row) total += node_bytes(n);
    } else if (const auto* draw = std::get_if<CanvasDraw>(&op)) {
        total += draw->commands.size();
    }
    return total;
}

Result<void> too_large() {
    return Result<void>::err(ErrorCode::QuotaExceeded, "ui update too large");
}

}  // namespace

Result<void> PatchBatch::set_prop(NodeId node, PropId prop, PropValue value) {
    size_t size = value_bytes(value);
    if (size > kMaxPropStringBytes)
        return Result<void>::err(ErrorCode::InvalidArgument, "value too large");
    uint64_t key = (static_cast<uint64_t>(node) << 16) | prop;
    auto it = index_.find(key);
    if (it != index_.end()) {
        // The index only ever points at SetProps: ops are only appended,
        // and take()/clear() drop the index with them.
        auto* set = std::get_if<SetProp>(&ops_[it->second]);
        assert(set != nullptr);
        size_t old = value_bytes(set->value);
        if (bytes_ - old + size > kMaxBatchBytes) return too_large();
        bytes_ = bytes_ - old + size;
        set->value = std::move(value);
        return Result<void>::ok();
    }
    size_t added = kOpOverheadBytes + size;
    if (added > kMaxBatchBytes - bytes_) return too_large();
    index_.emplace(key, ops_.size());
    ops_.emplace_back(SetProp{node, prop, std::move(value)});
    bytes_ += added;
    return Result<void>::ok();
}

Result<void> PatchBatch::push(UiOp op) {
    if (auto* set = std::get_if<SetProp>(&op))
        return set_prop(set->node, set->prop, std::move(set->value));
    size_t added = op_bytes(op);
    if (added > kMaxBatchBytes - bytes_) return too_large();
    ops_.push_back(std::move(op));
    bytes_ += added;
    return Result<void>::ok();
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
    bytes_ = 0;
}

}  // namespace wisp::scene
