#pragma once

// Shared look of the self-check screen: its colours, the few widget
// shapes every section is built from (titled cards, plain rows, aligned
// "label  value" tables) and the page's own language (PageText). UI thread
// only, like every lv_* call.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
// Secondary text: driver names, reasons, the table keys, the
// card headings and "untested".
inline constexpr uint32_t kColorDimText = 0x757575;
inline constexpr uint32_t kColorCard = 0xF2F3F5;

// The self-check page's language and its fixed labels. The page has a
// language of its own, apart from the system one (ui::lang(), which the
// system layer's dialogs follow): it starts in English every time the page
// is built, is never stored, and only the page's switcher changes it. Every
// text on the page comes from tr() here; the network cards and dialogs over
// it are system UI and use the system language. A label whose whole text is one
// string is bound once and re-texted by set_lang(); the sections re-render
// the rest themselves (see SelfCheck::set_page_lang()).
class PageText {
public:
    ui::Lang lang() const { return lang_; }
    const char* tr(ui::StrId id) const { return ui::tr(id, lang_); }
    std::string tr_fill(ui::StrId id, std::string_view value) const {
        return ui::tr_fill(id, lang_, value);
    }

    // Sets `label` to `prefix` + tr(id), now and after every set_lang().
    // Only for labels that live as long as the page: nothing unbinds one.
    void bind(lv_obj_t* label, ui::StrId id, std::string prefix = {});

    // Makes `lang` the page's language and re-texts every bound label.
    // False, touching nothing, if it already was.
    bool set_lang(ui::Lang lang);

private:
    struct Bound {
        lv_obj_t* label;
        ui::StrId id;
        std::string prefix;
    };
    void render(const Bound& bound) const;

    ui::Lang lang_ = ui::Lang::En;
    std::vector<Bound> bound_;
};

// The dot colour of a device in `state`.
uint32_t state_color(core::DeviceState state);
// A summary chip's background colour.
uint32_t level_color(SummaryLevel level);

// Appends a titled section card to `col`: a rounded, borderless, filled
// column whose first child is the heading label (bound in `text`). Content
// goes into the returned card, below the heading.
lv_obj_t* make_card(lv_obj_t* col, ui::StrId heading, PageText& text);

// A full-width label that wraps.
lv_obj_t* make_wrap_label(lv_obj_t* parent);

// A transparent, unscrollable, full-width flex row without padding.
lv_obj_t* make_plain_row(lv_obj_t* parent, lv_flex_flow_t flow);

// A two-column grid of `rows` "label  value" rows (rows <= kMaxTableRows):
// the labels in a column as wide as the widest one, so the values line up.
// Fill it with add_table_row().
inline constexpr int kMaxTableRows = 4;
lv_obj_t* make_table(lv_obj_t* parent, int rows);
// Puts the label `key` (dimmed, bound in `text`) into row `row` of
// `table`, column 0.
void add_table_key(lv_obj_t* table, int row, ui::StrId key, PageText& text);
// add_table_key(), then a value label next to it that wraps within the
// rest of the width; returns the value label.
lv_obj_t* add_table_row(lv_obj_t* table, int row, ui::StrId key, PageText& text);

// Sets the label's text only if it differs, so an unchanged readout is
// never invalidated (and redrawn) again.
void set_text_if_changed(lv_obj_t* label, const char* text);
// The same for the text colour and for the background colour.
void set_text_color_if_changed(lv_obj_t* obj, uint32_t rgb);
void set_bg_color_if_changed(lv_obj_t* obj, uint32_t rgb);

}  // namespace wisp::app
