#pragma once

// The device's one look: the tokens of wisp/ui/theme_tokens.hpp, the few
// that need LVGL, and the button roles built on them.
//
// UiRuntime installs an LVGL theme made from these tokens, so stock
// widgets (buttons, switches, sliders, textareas, message boxes ...) take
// the look without any styling of their own. The scene theme
// (wisp/scene/theme.hpp) maps its colour tokens onto the same values.

#include <cstdint>

#include "lvgl.h"
#include "wisp/ui/theme_tokens.hpp"

namespace wisp::ui::theme {

// Badges, chips and toggles.
inline constexpr int32_t kRadiusPill = LV_RADIUS_CIRCLE;

// The ring around a widget a key focuses (wisp/ui/key_nav.hpp): its
// width and its gap to the widget. It is drawn outside the widget, so
// whatever holds focusable widgets leaves this much room around them.
inline constexpr int32_t kFocusRingWidth = 2, kFocusRingPad = 2;

}  // namespace wisp::ui::theme

namespace wisp::ui {

// What a button does, which sets its colours. A plain lv_button is
// Primary through the theme already.
//   - Primary: the action the screen is for (accent fill, dark text).
//   - Secondary: the way out or a lesser action (raised fill, light text).
//   - Danger: an action that destroys data (error fill, dark text).
enum class ButtonRole : uint8_t { Primary, Secondary, Danger };

// Gives `btn` the colours of `role`, for every state it can be in.
// UI thread only, like every lv_* call; UiRuntime must be running.
void set_button_role(lv_obj_t* btn, ButtonRole role);

}  // namespace wisp::ui
