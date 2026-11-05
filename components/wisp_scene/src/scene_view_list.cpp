// SceneView: virtualized lists (see wisp/scene/scene_view.hpp).

#include <algorithm>
#include <cinttypes>
#include <unordered_set>

#include "scene_view_internal.hpp"
#include "wisp/core/log.hpp"

namespace wisp::scene {

using namespace view;

namespace {

// The tree and every row built share kMaxNodes (spec 4.5), as
// build_list_rows() counts them.
constexpr size_t kMaxViewNodes = kMaxNodes;

}  // namespace

void SceneView::apply_list_height(Node& n) {
    if (n.filler == nullptr) return;
    int64_t total = static_cast<int64_t>(n.rows) * n.row_h;
    lv_obj_set_height(n.filler, static_cast<int32_t>(std::min<int64_t>(total, LV_COORD_MAX)));
    // A new row_h moves and resizes every row, and changes the window.
    for (const Node::Row& r : n.built)
        if (Node* root = find(r.ids[0])) place_row(*root);
    if (!building_ && !n.resetting) {
        lv_obj_update_layout(n.obj);
        update_window(n, false);
    }
}

void SceneView::apply_list_reset(const ListReset& r) {
    Node* n = find(r.list);
    if (n == nullptr || n->widget != Widget::List) {
        WLOG_D(kTag, "list reset for node %" PRIu32 " that is not a list", r.list);
        return;
    }
    // Scroll events from the clamping below would ask for a window of the
    // old rows; the one request at the end covers them.
    n->resetting = true;
    drop_rows(*n);
    n->rows = r.count;
    apply_list_height(*n);
    lv_obj_update_layout(n->obj);
    // LVGL keeps the scroll position when the content shrinks: hold it to
    // what the new content allows.
    const int32_t bottom = lv_obj_get_scroll_bottom(n->obj);
    if (bottom < 0) {
        const int32_t y = std::max<int32_t>(0, lv_obj_get_scroll_y(n->obj) + bottom);
        lv_obj_scroll_to_y(n->obj, y, LV_ANIM_OFF);
    }
    n->resetting = false;
    update_window(*n, true);
}

void SceneView::update_window(Node& list, bool force) {
    // Rows start below the list's top padding, and the padding scrolls
    // with them: the viewport over the rows is the whole list, offset by
    // that padding (a list padded to clear a rounded screen's corners).
    const ListWindow w = visible_rows(
        lv_obj_get_scroll_y(list.obj) - lv_obj_get_style_pad_top(list.obj, LV_PART_MAIN),
        lv_obj_get_height(list.obj), list.row_h, list.rows);
    if (!force && w == list.window) return;
    list.window = w;
    for (size_t i = list.built.size(); i-- > 0;) {
        const uint32_t index = list.built[i].index;
        if (index < w.first || index - w.first >= w.count) drop_row(list, i);
    }
    if (cb_.on_list_range) cb_.on_list_range(list.id, w.first, w.count);
}

void SceneView::list_event_cb(lv_event_t* e) {
    auto* n = static_cast<Node*>(lv_event_get_user_data(e));
    // Not skipped while an op is applied: a scroll the scene asked for
    // (ScrollTo) needs its rows like one the user made.
    if (n->resetting) return;
    n->view->update_window(*n, false);
}

bool SceneView::row_ok(const Node& list, const os::LargeVector<NodeDesc>& row,
                       const os::LargeVector<NodeId>* old) const {
    if (row.empty() || row[0].parent != list.id) return false;
    const size_t old_count = old ? old->size() : 0;
    if (nodes_.size() - old_count + row.size() > kMaxViewNodes) return false;
    std::unordered_map<NodeId, WidgetId, std::hash<NodeId>, std::equal_to<NodeId>,
                       os::LargeAllocator<std::pair<const NodeId, WidgetId>>>
        seen;
    seen.reserve(row.size());
    for (size_t i = 0; i < row.size(); ++i) {
        const NodeDesc& d = row[i];
        if (d.id == 0 || d.widget >= kWidgetCount) return false;
        const auto w = static_cast<Widget>(d.widget);
        if (w == Widget::Screen || w == Widget::List) return false;
        if (i > 0) {
            auto parent = seen.find(d.parent);
            if (parent == seen.end()) return false;
            if (!widget_spec(static_cast<Widget>(parent->second)).children) return false;
        }
        if (nodes_.count(d.id) != 0 &&
            (old == nullptr || std::find(old->begin(), old->end(), d.id) == old->end()))
            return false;
        if (!seen.emplace(d.id, d.widget).second) return false;
    }
    return true;
}

void SceneView::apply_list_rows(const ListRows& r) {
    Node* list = find(r.list);
    if (list == nullptr || list->widget != Widget::List) {
        WLOG_D(kTag, "rows for node %" PRIu32 " that is not a list", r.list);
        return;
    }
    for (size_t k = 0; k < r.rows.size(); ++k) {
        if (r.first > UINT32_MAX - k) break;
        const auto index = static_cast<uint32_t>(r.first + k);
        // Rows for indexes that scrolled away while the scene built them.
        if (index < list->window.first || index - list->window.first >= list->window.count)
            continue;
        size_t slot = list->built.size();
        for (size_t i = 0; i < list->built.size(); ++i)
            if (list->built[i].index == index) slot = i;
        const bool replaces = slot < list->built.size();
        if (!row_ok(*list, r.rows[k], replaces ? &list->built[slot].ids : nullptr)) {
            WLOG_W(kTag, "malformed row %" PRIu32 " of list %" PRIu32 " dropped", index, list->id);
            continue;
        }
        if (replaces) drop_row(*list, slot);
        build_row(*list, index, r.rows[k]);
    }
}

void SceneView::build_row(Node& list, uint32_t index, const os::LargeVector<NodeDesc>& row) {
    os::LargeVector<os::LargeUnique<Node>> fresh;
    fresh.reserve(row.size());
    for (size_t i = 0; i < row.size(); ++i) {
        fresh.push_back(os::make_large<Node>());
        if (!fresh.back()) {
            WLOG_E(kTag, "out of memory building row %" PRIu32 " of list %" PRIu32, index, list.id);
            return;
        }
    }
    Node::Row built;
    built.index = index;
    built.ids.reserve(row.size());
    const bool was_building = building_;
    building_ = true;
    for (size_t i = 0; i < row.size(); ++i) {
        const NodeDesc& d = row[i];
        Node& n = *fresh[i];
        Node* parent = i == 0 ? &list : find(d.parent);
        if (i == 0) {
            n.row_of = list.id;
            n.row_index = index;
        }
        nodes_.emplace(d.id, std::move(fresh[i]));
        built.ids.push_back(d.id);
        init_node(n, d, parent);
    }
    building_ = was_building;
    for (NodeId id : built.ids)
        if (Node* n = find(id); n && n->widget == Widget::Grid) layout_grid(*n);
    list.built.push_back(std::move(built));
}

void SceneView::place_row(Node& root) {
    Node* list = find(root.row_of);
    if (list == nullptr) return;
    const int64_t y = static_cast<int64_t>(root.row_index) * list->row_h;
    lv_obj_set_pos(root.obj, 0, static_cast<int32_t>(std::min<int64_t>(y, LV_COORD_MAX)));
    lv_obj_set_size(root.obj, lv_pct(100), list->row_h);
    lv_obj_set_style_flex_grow(root.obj, 0, LV_PART_MAIN);
}

void SceneView::drop_row(Node& list, size_t slot) {
    Node::Row row = std::move(list.built[slot]);
    list.built.erase(list.built.begin() + static_cast<std::ptrdiff_t>(slot));
    if (Node* root = find(row.ids[0])) {
        // Its objects first: the nodes hold what they draw (an image's
        // pixels, a canvas buffer).
        ++applying_;
        lv_obj_delete(root->obj);
        --applying_;
    }
    for (NodeId id : row.ids) nodes_.erase(id);
}

void SceneView::drop_rows(Node& list) {
    while (!list.built.empty()) drop_row(list, list.built.size() - 1);
}

}  // namespace wisp::scene
