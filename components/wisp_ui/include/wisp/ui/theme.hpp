#pragma once

// The device's one look: the colour, radius and spacing tokens every
// screen draws with, and the button roles built on them.
//
// The values are the web console's dark set (web/styles/tokens.css): a
// night-blue base with the wisp's pale blue glow. The device is dark only
// (an AMOLED panel lights only what it draws), so there is no light set.
// The console's translucent tokens (--border, the -soft tints) become
// opaque colours here, blended onto `kSurface`, where they are drawn:
// LVGL then fills them without blending on every frame.
//
// UiRuntime installs an LVGL theme made from these tokens, so stock
// widgets (buttons, switches, sliders, textareas, message boxes ...) take
// the look without any styling of their own. The scene theme
// (wisp/scene/theme.hpp) maps its colour tokens onto the same values.

#include <cstdint>

#include "lvgl.h"

namespace wisp::ui::theme {

// `fg` laid over `bg` at `percent` (0..100) opacity, as 0xRRGGBB.
constexpr uint32_t blend(uint32_t fg, uint32_t bg, uint32_t percent) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const uint32_t f = (fg >> shift) & 0xFF;
        const uint32_t b = (bg >> shift) & 0xFF;
        out |= ((f * percent + b * (100 - percent) + 50) / 100) << shift;
    }
    return out;
}

// --- Colours, as 0xRRGGBB ---

// The page behind everything, and a band raised a little off it.
inline constexpr uint32_t kBg = 0x0B0F17;
inline constexpr uint32_t kBgRaised = 0x0E1420;
// Cards, and the two steps above a card (a card on a card, a control).
inline constexpr uint32_t kSurface = 0x121926;
inline constexpr uint32_t kSurface2 = 0x19212F;
inline constexpr uint32_t kSurface3 = 0x212B3C;

// The console's border hue, at its two strengths.
inline constexpr uint32_t kBorderHue = 0x94AACD;
inline constexpr uint32_t kBorder = blend(kBorderHue, kSurface, 12);
inline constexpr uint32_t kBorderStrong = blend(kBorderHue, kSurface, 22);

inline constexpr uint32_t kText = 0xE6EDF7;
// Secondary text: headings of cards, table keys, hints.
inline constexpr uint32_t kTextDim = 0x93A0B4;
// The quietest text that still reads (WCAG AA on every surface).
inline constexpr uint32_t kTextFaint = 0x8A94A5;

inline constexpr uint32_t kAccent = 0x5AB4FF;
inline constexpr uint32_t kAccentStrong = 0x84C7FF;
// Text on an accent (or another bright status) fill.
inline constexpr uint32_t kAccentInk = 0x04121F;
inline constexpr uint32_t kAccentSoft = blend(kAccent, kSurface, 12);

inline constexpr uint32_t kOk = 0x3DD68C;
inline constexpr uint32_t kWarn = 0xF5B74F;
inline constexpr uint32_t kErr = 0xFF6B6B;
// A status badge's fill; its text is the status colour itself.
inline constexpr uint32_t kOkSoft = blend(kOk, kSurface, 13);
inline constexpr uint32_t kWarnSoft = blend(kWarn, kSurface, 13);
inline constexpr uint32_t kErrSoft = blend(kErr, kSurface, 13);
inline constexpr uint32_t kMutedSoft = blend(kBorderHue, kSurface, 10);

// A pressed control: its own text colour laid over its fill, so a press
// reads on a bright fill (it darkens) and on a dark one (it lightens).
inline constexpr uint32_t kPressPercent = 25;
constexpr uint32_t pressed(uint32_t fill, uint32_t ink) { return blend(ink, fill, kPressPercent); }

// --- Shapes and spacing, in pixels ---

// Cards.
inline constexpr int32_t kRadius = 14;
// Buttons, inputs and other controls.
inline constexpr int32_t kRadiusSm = 9;
// Dialogs and the cards that float over a screen.
inline constexpr int32_t kRadiusLg = 20;
// Badges, chips and toggles.
inline constexpr int32_t kRadiusPill = LV_RADIUS_CIRCLE;

inline constexpr int32_t kGapXs = 4;
inline constexpr int32_t kGapSm = 8;
inline constexpr int32_t kGapMd = 12;
inline constexpr int32_t kGap = 16;
inline constexpr int32_t kGapLg = 24;
inline constexpr int32_t kGapXl = 32;

// The width of a border.
inline constexpr int32_t kBorderWidth = 1;

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
