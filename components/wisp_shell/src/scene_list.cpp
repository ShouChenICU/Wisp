#include "wisp/shell/scene_list.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "lvgl_private.h"  // lv_image_decoder_dsc_t
#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/theme.hpp"
#include "wisp/ui/theme_tokens.hpp"

namespace wisp::shell {

namespace {

constexpr const char* kTag = "shell";
constexpr int32_t kGap = ui::theme::kGapSm;
// The first character of a name without an icon.
constexpr int kLetterFontPx = 40;
// A greyed-out card.
constexpr lv_opa_t kUnavailableOpa = LV_OPA_40;
// The long-press card's widest.
constexpr int32_t kPopupMaxPx = 320;

// The first UTF-8 character of `s`.
std::string first_char(const std::string& s) {
    if (s.empty()) return "?";
    const auto lead = static_cast<unsigned char>(s[0]);
    size_t n = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    return s.substr(0, std::min(n, s.size()));
}

int32_t area_w(const hal::Area& a) { return a.x2 - a.x1 + 1; }

}  // namespace

SceneList::SceneList(SceneListDeps deps) : deps_(std::move(deps)) {
    const int32_t w = area_w(hal::safe_rect(deps_.shape));
    columns_ = std::max<int32_t>(1, (w + kGap) / (kCardMinPx + kGap));
    card_px_ = (w - (columns_ - 1) * kGap) / columns_;
}

SceneList::~SceneList() = default;

void SceneList::build(lv_obj_t* list) {
    list_ = list;
    grid_ = detail::make_box(list_);
    lv_obj_set_flex_flow(grid_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid_, kGap, 0);
    lv_obj_set_style_pad_row(grid_, kGap, 0);
    detail::bubble_tree(list_);
}

void SceneList::set_scenes(std::vector<ShellScene> scenes) {
    close_popup();
    scenes_ = std::move(scenes);
    const int32_t y = lv_obj_get_scroll_y(list_);
    build_cards();
    lv_obj_update_layout(list_);
    lv_obj_scroll_to_y(list_, std::min<int32_t>(y, y + lv_obj_get_scroll_bottom(list_)),
                       LV_ANIM_OFF);
}

void SceneList::build_cards() {
    lv_obj_clean(grid_);
    cards_.clear();
    icons_.clear();
    if (empty_ != nullptr) {
        lv_obj_delete(empty_);
        empty_ = nullptr;
    }
    if (scenes_.empty()) {
        empty_ = detail::make_text(list_, ui::tr(ui::StrId::SceneListEmpty), true);
        lv_obj_set_style_text_align(empty_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(empty_, LV_OBJ_FLAG_EVENT_BUBBLE);
        return;
    }
    const bool colour = detail::colour_panel(list_);
    const int32_t icon_px = card_px_ / 2;
    for (const ShellScene& s : scenes_) {
        Card c;
        c.card = lv_obj_create(grid_);
        lv_obj_remove_style_all(c.card);
        lv_obj_set_size(c.card, card_px_, card_px_);
        lv_obj_remove_flag(c.card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c.card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(c.card, ui::theme::kRadius, 0);
        if (colour) {
            lv_obj_set_style_bg_color(c.card, lv_color_hex(ui::theme::kSurface), 0);
            lv_obj_set_style_bg_opa(c.card, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(c.card, lv_color_hex(ui::theme::kSurface3), LV_STATE_PRESSED);
        } else {
            lv_obj_set_style_border_color(c.card, detail::text_colour(list_), 0);
            lv_obj_set_style_border_width(c.card, ui::theme::kBorderWidth, 0);
        }
        lv_obj_set_style_pad_all(c.card, kGap, 0);
        lv_obj_set_flex_flow(c.card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c.card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(c.card, ui::theme::kGapXs, 0);
        if (!s.available) lv_obj_set_style_opa(c.card, kUnavailableOpa, 0);
        lv_obj_add_event_cb(c.card, &SceneList::card_cb, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(c.card, &SceneList::card_cb, LV_EVENT_LONG_PRESSED, this);

        std::unique_ptr<Icon> icon = s.icon_png ? decode(*s.icon_png) : nullptr;
        if (icon) {
            c.icon = lv_image_create(c.card);
            lv_obj_set_size(c.icon, icon_px, icon_px);
            lv_image_set_inner_align(c.icon, LV_IMAGE_ALIGN_CONTAIN);
            lv_image_set_src(c.icon, &icon->image);
            icons_.push_back(std::move(icon));
        } else {
            lv_obj_t* letter = lv_label_create(c.card);
            lv_obj_set_style_text_font(letter, ui::font(kLetterFontPx), 0);
            lv_label_set_text(letter, first_char(s.name).c_str());
        }
        c.name = lv_label_create(c.card);
        lv_obj_set_width(c.name, LV_PCT(100));
        lv_obj_set_style_text_align(c.name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(c.name, LV_LABEL_LONG_DOT);
        lv_label_set_text(c.name, s.name.c_str());
        if (s.background) {
            c.mark = lv_label_create(c.card);
            lv_obj_add_flag(c.mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_style_text_color(
                c.mark, colour ? lv_color_hex(ui::theme::kAccent) : detail::text_colour(list_), 0);
            lv_label_set_text(c.mark, LV_SYMBOL_PLAY);
            lv_obj_align(c.mark, LV_ALIGN_TOP_RIGHT, 0, 0);
        }
        cards_.push_back(c);
    }
    detail::bubble_tree(grid_);
}

std::unique_ptr<SceneList::Icon> SceneList::decode(const os::LargeBuffer& png) const {
    // The file's bytes as an image source LVGL's decoders recognise by
    // their content (see SceneView::set_image()).
    lv_image_dsc_t src{};
    src.header.magic = LV_IMAGE_HEADER_MAGIC;
    src.header.cf = LV_COLOR_FORMAT_UNKNOWN;
    src.data = png.data();
    src.data_size = static_cast<uint32_t>(png.size());
    lv_image_header_t header;
    if (lv_image_decoder_get_info(&src, &header) != LV_RESULT_OK || header.w == 0 ||
        header.h == 0 || header.w > kIconMaxSide || header.h > kIconMaxSide) {
        return nullptr;
    }
    lv_image_decoder_args_t args{};
    args.no_cache = true;
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, &src, &args) != LV_RESULT_OK) return nullptr;
    std::unique_ptr<Icon> out;
    const lv_draw_buf_t* d = dsc.decoded;
    if (d != nullptr && d->header.w > 0 && d->header.h > 0) {
        const size_t size = static_cast<size_t>(d->header.stride) * d->header.h;
        if (size <= d->data_size) {
            auto icon = std::make_unique<Icon>();
            icon->pixels = os::LargeBuffer::allocate(size);
            if (icon->pixels.ok()) {
                std::memcpy(icon->pixels.data(), d->data, size);
                icon->image.header = d->header;
                icon->image.header.flags = 0;
                icon->image.data = icon->pixels.data();
                icon->image.data_size = static_cast<uint32_t>(size);
                out = std::move(icon);
            }
        }
    }
    lv_image_decoder_close(&dsc);
    return out;
}

size_t SceneList::index_of(lv_obj_t* card) const {
    for (size_t i = 0; i < cards_.size(); ++i)
        if (cards_[i].card == card) return i;
    return cards_.size();
}

void SceneList::card_cb(lv_event_t* e) {
    auto* self = static_cast<SceneList*>(lv_event_get_user_data(e));
    const size_t i = self->index_of(lv_event_get_current_target_obj(e));
    if (i >= self->cards_.size()) return;
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        self->long_press(i);
    } else {
        self->tap(i);
    }
}

void SceneList::tap(size_t i) {
    if (i >= scenes_.size() || !scenes_[i].available || !deps_.open_scene) return;
    // A copy: opening may replace the scenes (a refresh) before it returns.
    const std::string id = scenes_[i].id;
    WLOG_I(kTag, "scene %s opened from the list", id.c_str());
    deps_.open_scene(id);
}

void SceneList::long_press(size_t i) {
    if (i >= scenes_.size() || !scenes_[i].available || !deps_.delete_scene) return;
    close_popup();
    const ShellScene& s = scenes_[i];
    pop_id_ = s.id;
    lv_obj_t* scr = lv_obj_get_screen(list_);
    // A backdrop over the whole page: a tap outside the card closes it.
    popup_ = lv_obj_create(scr);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, deps_.shape.width, deps_.shape.height);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_remove_flag(popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, &SceneList::popup_cb, LV_EVENT_CLICKED, this);

    const hal::Area safe = hal::safe_rect(deps_.shape);
    lv_obj_t* card = detail::make_card(popup_);
    lv_obj_set_width(card, std::min(area_w(safe), kPopupMaxPx));
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);  // a tap on it is not outside
    lv_obj_t* name = detail::make_text(card, s.name.c_str());
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    pop_data_ = nullptr;
    if (s.has_data) {
        pop_data_ = lv_checkbox_create(card);
        lv_checkbox_set_text(pop_data_, ui::tr(ui::StrId::SceneDeleteData));
        lv_obj_set_style_min_height(pop_data_, detail::kMinTouchPx, 0);
        lv_obj_set_flex_flow(pop_data_, LV_FLEX_FLOW_ROW);
    }
    pop_delete_ = lv_button_create(card);
    lv_obj_set_size(pop_delete_, LV_PCT(100), detail::kMinTouchPx);
    ui::set_button_role(pop_delete_, ui::ButtonRole::Danger);
    lv_obj_t* label = lv_label_create(pop_delete_);
    lv_label_set_text(label, ui::tr(ui::StrId::SceneDeleteButton));
    lv_obj_center(label);
    lv_obj_add_event_cb(pop_delete_, &SceneList::popup_cb, LV_EVENT_CLICKED, this);
    // Centred in the safe area, which on a round screen is not the screen's
    // centre row by row but is by its box.
    lv_obj_update_layout(card);
    lv_obj_set_pos(card, (safe.x1 + safe.x2 + 1 - lv_obj_get_width(card)) / 2,
                   (safe.y1 + safe.y2 + 1 - lv_obj_get_height(card)) / 2);
    detail::bubble_tree(popup_);
}

void SceneList::popup_cb(lv_event_t* e) {
    auto* self = static_cast<SceneList*>(lv_event_get_user_data(e));
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* current = lv_event_get_current_target_obj(e);
    if (current == self->popup_) {
        // Only a tap on the backdrop itself.
        if (target == self->popup_) self->close_popup();
        return;
    }
    if (current != self->pop_delete_) return;
    const bool with_data =
        self->pop_data_ != nullptr && lv_obj_has_state(self->pop_data_, LV_STATE_CHECKED);
    const std::string id = self->pop_id_;
    self->close_popup();
    if (Result<void> r = self->deps_.delete_scene(id, with_data); !r) {
        WLOG_W(kTag, "scene %s not deleted: %s", id.c_str(), r.error().message.c_str());
    }
}

void SceneList::close_popup() {
    if (popup_ == nullptr) return;
    // From inside one of its own events: deleted once the event is over.
    lv_obj_delete_async(popup_);
    popup_ = pop_delete_ = pop_data_ = nullptr;
    pop_id_.clear();
}

}  // namespace wisp::shell
