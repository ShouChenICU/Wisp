#include "wisp/ui/overlay.hpp"

#include <algorithm>
#include <vector>

namespace wisp::ui {

namespace {

// The marked overlays of every display, in marking order. Only touched on
// the UI thread.
std::vector<lv_obj_t*>& overlays() {
    static std::vector<lv_obj_t*> list;
    return list;
}

// Set while the handler invalidates the extra pieces of a split area, so
// those pass straight through to the display's other handlers.
bool splitting = false;

bool intersect(const lv_area_t& a, const lv_area_t& b, lv_area_t* out) {
    *out = {std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

bool inside(const lv_area_t& a, const lv_area_t& box) {
    return a.x1 >= box.x1 && a.y1 >= box.y1 && a.x2 <= box.x2 && a.y2 <= box.y2;
}

// The box an object's own invalidations stay within: its coordinates
// plus what it draws outside them (shadow, outline). The extra size is
// asked of the object the way LVGL itself does, as its cached value is
// not public.
lv_area_t box_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t ext = 0;
    lv_obj_send_event(obj, LV_EVENT_REFR_EXT_DRAW_SIZE, &ext);
    lv_area_increase(&a, ext, ext);
    return a;
}

bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

bool opaque(lv_obj_t* obj) {
    const lv_part_t m = LV_PART_MAIN;
    if (lv_obj_get_style_opa(obj, m) != LV_OPA_COVER) return false;
    if (lv_obj_get_style_opa_layered(obj, m) != LV_OPA_COVER) return false;
    if (lv_obj_get_style_bg_opa(obj, m) != LV_OPA_COVER) return false;
    if (lv_obj_get_style_bg_grad_dir(obj, m) != LV_GRAD_DIR_NONE &&
        (lv_obj_get_style_bg_main_opa(obj, m) != LV_OPA_COVER ||
         lv_obj_get_style_bg_grad_opa(obj, m) != LV_OPA_COVER)) {
        return false;
    }
    if (lv_obj_get_style_blend_mode(obj, m) != LV_BLEND_MODE_NORMAL) return false;
    return lv_obj_get_style_transform_rotation(obj, m) == 0 &&
           lv_obj_get_style_transform_scale_x(obj, m) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_scale_y(obj, m) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_skew_x(obj, m) == 0 &&
           lv_obj_get_style_transform_skew_y(obj, m) == 0;
}

// The rows of `obj` its background covers edge to edge: all of them but
// the rounded corners'. False if there are none.
bool opaque_core(lv_obj_t* obj, lv_area_t* core) {
    lv_obj_get_coords(obj, core);
    const int32_t w = lv_area_get_width(core);
    const int32_t h = lv_area_get_height(core);
    const int32_t r = std::min({lv_obj_get_style_radius(obj, LV_PART_MAIN), w / 2, h / 2});
    core->y1 += r;
    core->y2 -= r;
    return core->y1 <= core->y2;
}

bool marked(lv_obj_t* obj) {
    const auto& list = overlays();
    return std::find(list.begin(), list.end(), obj) != list.end();
}

// Whether `area` touches a shown, unmarked object on `layer`.
bool touches_other(lv_obj_t* layer, const lv_area_t& area) {
    if (layer == nullptr) return false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(layer); ++i) {
        lv_obj_t* child = lv_obj_get_child(layer, static_cast<int32_t>(i));
        if (marked(child) || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        const lv_area_t box = box_of(child);
        lv_area_t common;
        if (intersect(area, box, &common)) return true;
    }
    return false;
}

// Appends `in` minus `cut` to `out`: up to four pieces (the rows above
// and below `cut`, and the parts left and right of it on its rows).
void subtract(const lv_area_t& in, const lv_area_t& cut, std::vector<lv_area_t>& out) {
    lv_area_t common;
    if (!intersect(in, cut, &common)) {
        out.push_back(in);
        return;
    }
    if (in.y1 < common.y1) out.push_back({in.x1, in.y1, in.x2, common.y1 - 1});
    if (in.y2 > common.y2) out.push_back({in.x1, common.y2 + 1, in.x2, in.y2});
    if (in.x1 < common.x1) out.push_back({in.x1, common.y1, common.x1 - 1, common.y2});
    if (in.x2 > common.x2) out.push_back({common.x2 + 1, common.y1, in.x2, common.y2});
}

// Runs before the display's other LV_EVENT_INVALIDATE_AREA handlers (it
// is registered as a preprocess handler), so each piece is then aligned
// and recorded like any other area.
void invalidate_cb(lv_event_t* e) {
    if (splitting) return;
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(e));
    auto* disp = static_cast<lv_display_t*>(lv_event_get_current_target(e));

    std::vector<lv_obj_t*> covering;
    for (lv_obj_t* o : overlays()) {
        if (lv_obj_get_display(o) != disp || !shown(o) || !opaque(o)) continue;
        const lv_area_t box = box_of(o);
        if (inside(*area, box)) return;
        covering.push_back(o);
    }
    if (covering.empty()) return;
    if (touches_other(lv_display_get_layer_top(disp), *area) ||
        touches_other(lv_display_get_layer_sys(disp), *area)) {
        return;
    }

    std::vector<lv_area_t> pieces{*area};
    for (lv_obj_t* o : covering) {
        lv_area_t core;
        if (!opaque_core(o, &core)) continue;
        std::vector<lv_area_t> next;
        for (const lv_area_t& p : pieces) subtract(p, core, next);
        pieces.swap(next);
    }
    // Wholly covered, yet not inside any one overlay's box: rare enough
    // that it is simply redrawn.
    if (pieces.empty()) return;

    *area = pieces.front();
    // The system layer spans the display and is never hidden, so this
    // invalidates exactly the piece.
    splitting = true;
    lv_obj_t* sys = lv_display_get_layer_sys(disp);
    for (size_t i = 1; i < pieces.size(); ++i) lv_obj_invalidate_area(sys, &pieces[i]);
    splitting = false;
}

void delete_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto& list = overlays();
    list.erase(std::remove(list.begin(), list.end(), obj), list.end());
}

bool has_handler(lv_display_t* disp) {
    for (uint32_t i = 0; i < lv_display_get_event_count(disp); ++i) {
        if (lv_event_dsc_get_cb(lv_display_get_event_dsc(disp, i)) == &invalidate_cb) return true;
    }
    return false;
}

}  // namespace

void mark_opaque_overlay(lv_obj_t* obj) {
    if (obj == nullptr || marked(obj)) return;
    lv_display_t* disp = lv_obj_get_display(obj);
    if (!has_handler(disp)) {
        lv_display_add_event_cb(
            disp, &invalidate_cb,
            static_cast<lv_event_code_t>(LV_EVENT_INVALIDATE_AREA | LV_EVENT_PREPROCESS), nullptr);
    }
    overlays().push_back(obj);
    lv_obj_add_event_cb(obj, &delete_cb, LV_EVENT_DELETE, nullptr);
}

}  // namespace wisp::ui
