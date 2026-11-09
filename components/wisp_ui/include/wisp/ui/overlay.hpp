#pragma once

// Opaque overlays: cards and dialogs on the top layer that hide whatever
// lies under them. LVGL redraws an invalidated area from the bottom up,
// so a list scrolling under a card would otherwise redraw the list and
// then the whole card over it on every frame, though neither visible
// pixel changed. Marking the card lets the display skip the part of such
// an area that the card covers.
//
// How it decides: an invalidated area that lies inside a marked
// overlay's box may be the overlay's own change and is left alone (for an
// OwnChanges::InOwnEvents overlay, only while it handles an event); so is
// one that touches any other visible object on the top or system layer
// (such as the performance label). Any other area comes from below the
// overlays, and the rows of each overlay that are opaque edge to edge
// (all but its rounded corners' rows) are cut out of it; an area they
// cover wholly is dropped. An overlay
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

// Which invalidated areas may be an overlay's own change, and so are never
// cut.
enum class OwnChanges {
    // Any area inside its box. Right for an overlay with children, or one
    // that animates: anything may change it at any time.
    InsideBox,
    // An area inside its box only while LVGL sends an event to it, or to
    // another overlay marked so. For an overlay without children that
    // changes only in its own events (input, value, size or style
    // changes) and in code run from them, such as the on-screen keyboard.
    // It may span the screen: content scrolling under it lies inside its
    // box, and is cut all the same. Add its other event callbacks before
    // marking it; whatever they change counts as its own.
    InOwnEvents,
};

// Marks `obj` (a child of lv_layer_top() or lv_layer_sys()) as an opaque
// overlay of its display until it is deleted. Marking it again is a
// no-op.
void mark_opaque_overlay(lv_obj_t* obj, OwnChanges own = OwnChanges::InsideBox);

// Pauses or resumes a marked overlay. Paused, it counts as not covering
// anything, like a hidden one: nothing is cut for it and its last drawn
// box is redrawn on the next refresh. For an overlay that moves (the
// pull-down panel following the finger): an area invalidated before the
// move would otherwise be cut by where the overlay still is, though the
// move uncovers it. Resumed, it counts again from the next frame that
// draws it. No-op on an unmarked object.
void pause_opaque_overlay(lv_obj_t* obj, bool paused);

// Tells the marked overlays of `disp` that the panel may no longer show
// them (a stripe failed to reach it): nothing is cut out of the areas
// invalidated before the next frame draws them again.
void forget_drawn_overlays(lv_display_t* disp);

}  // namespace wisp::ui
