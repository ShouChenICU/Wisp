#pragma once

// Shared look of the self-check screen: its colours and the few widget
// shapes every section is built from (titled cards, plain rows, aligned
// "label  value" tables). UI thread only, like every lv_* call.

#include <cstdint>

#include "lvgl.h"
#include "self_check_model.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

// Status colours, as 0xRRGGBB: the device dots, the summary chips and the
// "passed" marks.
inline constexpr uint32_t kColorGood = 0x43A047;
inline constexpr uint32_t kColorWarning = 0xF9A825;
inline constexpr uint32_t kColorError = 0xE53935;
inline constexpr uint32_t kColorMissing = 0x9E9E9E;
// A neutral chip's background (its text stays dark).
inline constexpr uint32_t kColorNeutralChip = 0xE0E0E0;
// Secondary text: driver names, reasons, the header's info line, the
// card headings and "untested".
inline constexpr uint32_t kColorDimText = 0x757575;
inline constexpr uint32_t kColorCard = 0xF2F3F5;

// The dot colour of a device in `state`.
uint32_t state_color(core::DeviceState state);
// A summary chip's background colour.
uint32_t level_color(SummaryLevel level);

// Appends a titled section card to `col`: a rounded, borderless, filled
// column whose first child is the heading label. Content goes into the
// returned card, below the heading.
lv_obj_t* make_card(lv_obj_t* col, ui::StrId heading);

// A full-width label that wraps.
lv_obj_t* make_wrap_label(lv_obj_t* parent);

// A transparent, unscrollable, full-width flex row without padding.
lv_obj_t* make_plain_row(lv_obj_t* parent, lv_flex_flow_t flow);

// A two-column grid of `rows` "label  value" rows (rows <= 2): the labels
// in a column as wide as the widest one, so the values line up. Fill it
// with add_table_row().
lv_obj_t* make_table(lv_obj_t* parent, int rows);
// Puts `label` (dimmed) into row `row` of `table`, column 0.
void add_table_key(lv_obj_t* table, int row, const char* label);
// add_table_key(), then a value label next to it that wraps within the
// rest of the width; returns the value label.
lv_obj_t* add_table_row(lv_obj_t* table, int row, const char* label);

// Sets the label's text only if it differs, so an unchanged readout is
// never invalidated (and redrawn) again.
void set_text_if_changed(lv_obj_t* label, const char* text);
// The same for the text colour and for the background colour.
void set_text_color_if_changed(lv_obj_t* obj, uint32_t rgb);
void set_bg_color_if_changed(lv_obj_t* obj, uint32_t rgb);

}  // namespace wisp::app
