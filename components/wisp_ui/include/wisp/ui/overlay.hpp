#pragma once

// Opaque overlays: cards and dialogs on the top layer that hide whatever
// lies under them. LVGL redraws an invalidated area from the bottom up,
// so a list scrolling under a card would otherwise redraw the list and
// then the whole card over it on every frame, though neither visible
// pixel changed. Marking the card lets the display skip the part of such
// an area that the card covers.
//
// How it decides: an invalidated area that lies inside a marked
// overlay's box may be the overlay's own change and is left alone; so is
// one that touches any other visible object on the top or system layer
// (such as the performance label). Any other area comes from below the
// overlays, and the rows of each overlay that are opaque edge to edge
// (all but its rounded corners' rows) are cut out of it. An overlay
// counts only while it and every ancestor are shown, at full opacity,
// blended normally, unmasked and untransformed, its background is opaque
// (no gradient descriptor), and the last frame drew it so; otherwise
// nothing is cut. One that stops counting (hidden, faded or transformed,
// directly or through its layer) has its last drawn box redrawn on the
// next refresh. Nothing is cut while the display renders: the event
// then only carries LVGL's band-height probe.
//
// So everything drawn above a marked overlay must be inside a marked
// overlay itself, or be a separate object on the top or system layer, as
// the cards and the performance label are.
//
// UI thread only, like every lv_* call.

#include "lvgl.h"

namespace wisp::ui {

// Marks `obj` (a child of lv_layer_top() or lv_layer_sys()) as an opaque
// overlay of its display until it is deleted. Marking it again is a
// no-op.
void mark_opaque_overlay(lv_obj_t* obj);

// Tells the marked overlays of `disp` that the panel may no longer show
// them (a stripe failed to reach it): nothing is cut out of the areas
// invalidated before the next frame draws them again.
void forget_drawn_overlays(lv_display_t* disp);

}  // namespace wisp::ui
