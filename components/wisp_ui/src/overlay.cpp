#include "wisp/ui/overlay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace wisp::ui {

namespace {

struct Overlay {
    lv_obj_t* obj = nullptr;
    // Whether the last frame its display rendered drew it covering its
    // core, and the box it was drawn in then. Only then do the panel's
    // pixels under the core belong to the overlay, so only then may
    // areas be cut there.
    bool covered = false;
    lv_area_t box{};
};

// The marked overlays of every display, in marking order. Only touched on
// the UI thread.
std::vector<Overlay>& overlays() {
    static std::vector<Overlay> list;
    return list;
}

// Set while the handler invalidates the extra pieces of a split area, so
// those pass straight through to the display's other handlers.
bool splitting = false;

// The display that is between LV_EVENT_RENDER_START and
// LV_EVENT_RENDER_READY. LVGL refreshes one display at a time.
lv_display_t* rendering = nullptr;

// Upper bounds that keep the handler off the heap. Cutting fewer
// overlays out of an area, or none, only redraws more, never less.
constexpr size_t kMaxCovering = 4;
constexpr size_t kMaxPieces = 16;

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

// Whether `obj` is drawn as itself, at full opacity and untransformed:
// what an object and each of its ancestors must be for the object's
// opaque background to land on the panel unchanged.
bool drawn_plainly(lv_obj_t* obj) {
    const lv_part_t m = LV_PART_MAIN;
    return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) &&
           lv_obj_get_style_opa(obj, m) == LV_OPA_COVER &&
           lv_obj_get_style_opa_layered(obj, m) == LV_OPA_COVER &&
           lv_obj_get_style_blend_mode(obj, m) == LV_BLEND_MODE_NORMAL &&
           lv_obj_get_style_bitmap_mask_src(obj, m) == nullptr &&
           lv_obj_get_style_transform_rotation(obj, m) == 0 &&
           lv_obj_get_style_transform_scale_x(obj, m) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_scale_y(obj, m) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_skew_x(obj, m) == 0 &&
           lv_obj_get_style_transform_skew_y(obj, m) == 0 &&
           lv_obj_get_style_transform_width(obj, m) == 0 &&
           lv_obj_get_style_transform_height(obj, m) == 0;
}

// Whether `obj` currently covers its core: it and every ancestor are shown
// and drawn plainly, and its own background is opaque everywhere.
bool covers(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (!drawn_plainly(o)) return false;
    }
    const lv_part_t m = LV_PART_MAIN;
    if (lv_obj_get_style_bg_opa(obj, m) != LV_OPA_COVER) return false;
    // A gradient descriptor can have translucent stops or leave parts
    // unpainted; such a background is not counted at all.
    if (lv_obj_get_style_bg_grad(obj, m) != nullptr) return false;
    return lv_obj_get_style_bg_grad_dir(obj, m) == LV_GRAD_DIR_NONE ||
           (lv_obj_get_style_bg_main_opa(obj, m) == LV_OPA_COVER &&
            lv_obj_get_style_bg_grad_opa(obj, m) == LV_OPA_COVER);
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
    return std::any_of(list.begin(), list.end(), [obj](const Overlay& o) { return o.obj == obj; });
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

// A fixed-capacity list of areas.
struct Pieces {
    std::array<lv_area_t, kMaxPieces> a{};
    size_t n = 0;

    bool push(const lv_area_t& area) {
        if (n == a.size()) return false;
        a[n++] = area;
        return true;
    }
};

// Appends `in` minus `cut` to `out`: up to four pieces (the rows above
// and below `cut`, and the parts left and right of it on its rows).
// False if `out` ran out of room.
bool subtract(const lv_area_t& in, const lv_area_t& cut, Pieces& out) {
    lv_area_t common;
    if (!intersect(in, cut, &common)) return out.push(in);
    bool ok = true;
    if (in.y1 < common.y1) ok = ok && out.push({in.x1, in.y1, in.x2, common.y1 - 1});
    if (in.y2 > common.y2) ok = ok && out.push({in.x1, common.y2 + 1, in.x2, in.y2});
    if (in.x1 < common.x1) ok = ok && out.push({in.x1, common.y1, common.x1 - 1, common.y2});
    if (in.x2 > common.x2) ok = ok && out.push({common.x2 + 1, common.y1, in.x2, common.y2});
    return ok;
}

// Runs before the display's other LV_EVENT_INVALIDATE_AREA handlers (it
// is registered as a preprocess handler), so each piece is then aligned
// and recorded like any other area.
void invalidate_cb(lv_event_t* e) {
    auto* disp = static_cast<lv_display_t*>(lv_event_get_current_target(e));
    // While rendering, the event only carries LVGL's probe for the band
    // height ({0, 0, 0, rows - 1}), which must reach the alignment
    // handlers unchanged; and nothing may be invalidated then (LVGL
    // halts in lv_inv_area()).
    if (splitting || rendering == disp) return;
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(e));

    std::array<lv_obj_t*, kMaxCovering> covering{};
    size_t n_covering = 0;
    for (const Overlay& o : overlays()) {
        if (!o.covered || lv_obj_get_display(o.obj) != disp || !covers(o.obj)) continue;
        if (inside(*area, box_of(o.obj))) return;
        if (n_covering < covering.size()) covering[n_covering++] = o.obj;
    }
    if (n_covering == 0) return;
    if (touches_other(lv_display_get_layer_top(disp), *area) ||
        touches_other(lv_display_get_layer_sys(disp), *area)) {
        return;
    }

    Pieces pieces;
    pieces.push(*area);
    for (size_t i = 0; i < n_covering; ++i) {
        lv_area_t core;
        if (!opaque_core(covering[i], &core)) continue;
        Pieces next;
        for (size_t k = 0; k < pieces.n; ++k) {
            if (!subtract(pieces.a[k], core, next)) return;  // too fragmented: redraw it whole
        }
        pieces = next;
    }
    // Wholly covered, yet not inside any one overlay's box: rare enough
    // that it is simply redrawn.
    if (pieces.n == 0) return;

    *area = pieces.a[0];
    // The system layer spans the display and is never hidden, so this
    // invalidates exactly the piece.
    splitting = true;
    lv_obj_t* sys = lv_display_get_layer_sys(disp);
    for (size_t i = 1; i < pieces.n; ++i) lv_obj_invalidate_area(sys, &pieces.a[i]);
    splitting = false;
}

// Before a refresh lays out and joins the invalidated areas: an overlay
// that covered its core in the last frame but no longer does (it, its
// layer or another ancestor was hidden, faded or transformed) has its
// box redrawn, since areas invalidated while it still covered may have
// had that part cut out, and LVGL invalidates a hidden object before
// hiding it.
void refr_start_cb(lv_event_t* e) {
    auto* disp = static_cast<lv_display_t*>(lv_event_get_current_target(e));
    lv_obj_t* sys = lv_display_get_layer_sys(disp);
    for (Overlay& o : overlays()) {
        if (!o.covered || lv_obj_get_display(o.obj) != disp || covers(o.obj)) continue;
        o.covered = false;
        lv_obj_invalidate_area(sys, &o.box);
    }
}

// After layout, when the frame's areas are final: records which overlays
// this frame draws covering their core, and where.
void render_start_cb(lv_event_t* e) {
    auto* disp = static_cast<lv_display_t*>(lv_event_get_current_target(e));
    for (Overlay& o : overlays()) {
        if (lv_obj_get_display(o.obj) != disp) continue;
        o.covered = covers(o.obj);
        if (o.covered) o.box = box_of(o.obj);
    }
    rendering = disp;
}

void render_ready_cb(lv_event_t*) { rendering = nullptr; }

void delete_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto& list = overlays();
    list.erase(
        std::remove_if(list.begin(), list.end(), [obj](const Overlay& o) { return o.obj == obj; }),
        list.end());
}

bool has_handler(lv_display_t* disp) {
    for (uint32_t i = 0; i < lv_display_get_event_count(disp); ++i) {
        if (lv_event_dsc_get_cb(lv_display_get_event_dsc(disp, i)) == &invalidate_cb) return true;
    }
    return false;
}

}  // namespace

void forget_drawn_overlays(lv_display_t* disp) {
    for (Overlay& o : overlays()) {
        if (lv_obj_get_display(o.obj) == disp) o.covered = false;
    }
}

void mark_opaque_overlay(lv_obj_t* obj) {
    if (obj == nullptr || marked(obj)) return;
    lv_display_t* disp = lv_obj_get_display(obj);
    if (!has_handler(disp)) {
        lv_display_add_event_cb(
            disp, &invalidate_cb,
            static_cast<lv_event_code_t>(LV_EVENT_INVALIDATE_AREA | LV_EVENT_PREPROCESS), nullptr);
        lv_display_add_event_cb(disp, &refr_start_cb, LV_EVENT_REFR_START, nullptr);
        lv_display_add_event_cb(disp, &render_start_cb, LV_EVENT_RENDER_START, nullptr);
        lv_display_add_event_cb(disp, &render_ready_cb, LV_EVENT_RENDER_READY, nullptr);
    }
    overlays().push_back(Overlay{obj, false, {}});
    lv_obj_add_event_cb(obj, &delete_cb, LV_EVENT_DELETE, nullptr);
}

}  // namespace wisp::ui
