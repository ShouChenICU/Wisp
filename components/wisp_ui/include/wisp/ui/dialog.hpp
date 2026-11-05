#pragma once

// The action buttons of the system's dialogs and cards: the error panel,
// the confirm dialogs and the network cards. They are big enough to hit
// with a thumb on a small screen, and the buttons of one row share its
// width equally, so a single button spans the whole row.
//
// UI thread only, like every lv_* call.

#include <cstdint>

#include "lvgl.h"

namespace wisp::ui {

// The least height of an action button.
inline constexpr int32_t kDialogButtonMinHeight = 56;
// The space between two buttons of a row.
inline constexpr int32_t kDialogButtonGap = 12;

// A row for action buttons, as wide as `parent`'s content and as tall as
// its buttons. It takes no touches and does not scroll.
lv_obj_t* make_dialog_actions(lv_obj_t* parent);

// Adds a button reading `text` to `row` (from make_dialog_actions()). It
// shares the row's width equally with the row's other buttons, is at
// least kDialogButtonMinHeight tall and darkens while pressed. Its label
// keeps the inherited body font: a larger one would leave CJK text to a
// fallback of another size.
lv_obj_t* add_dialog_button(lv_obj_t* row, const char* text);

}  // namespace wisp::ui
