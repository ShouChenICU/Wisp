#include "wisp/shell/scene_list.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/theme.hpp"
#include "wisp/ui/theme_tokens.hpp"

// LVGL's bundled lodepng (LV_USE_LODEPNG), compiled as C. Declared here
// because its header, included from C++, also declares C++ overloads
// inside its extern "C" block, which does not compile. LVGL's copy hands
// back an lv_draw_buf_t* in `out` (not raw bytes), its pixels R, G, B, A.
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h,
                                     const unsigned char* in, size_t insize);

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
// A loading placeholder.
constexpr lv_opa_t kPlaceholderOpa = LV_OPA_40;

// What a card shows: the same scene, the same way.
bool same_card(const ShellScene& a, const ShellScene& b) {
    return a.id == b.id && a.name == b.name && a.icon == b.icon && a.background == b.background &&
           a.available == b.available;
}

// The first UTF-8 character of `s`.
std::string first_char(const std::string& s) {
    if (s.empty()) return "?";
    const auto lead = static_cast<unsigned char>(s[0]);
    size_t n = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    return s.substr(0, std::min(n, s.size()));
}

int32_t area_w(const hal::Area& a) { return a.x2 - a.x1 + 1; }

struct Grid {
    int32_t columns;
    int32_t card_px;
};

Grid grid_of(const hal::ScreenShape& shape) {
    const int32_t w = area_w(hal::safe_rect(shape));
    const int32_t columns = std::max<int32_t>(1, (w + kGap) / (SceneList::kCardMinPx + kGap));
    return {columns, (w - (columns - 1) * kGap) / columns};
}

// A PNG's size from its IHDR chunk, read before decoding so an oversized
// image is refused without its pixels ever being allocated.
bool png_size(const os::LargeBuffer& png, uint32_t& w, uint32_t& h) {
    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (png.size() < 8 + 25 || std::memcmp(png.data(), kSig, 8) != 0 ||
        std::memcmp(png.data() + 12, "IHDR", 4) != 0) {
        return false;
    }
    const auto be32 = [&](size_t at) {
        const uint8_t* p = png.data() + at;
        return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
    };
    w = be32(16);
    h = be32(20);
    return true;
}

}  // namespace

uint64_t icon_hash(std::span<const uint8_t> bytes) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h != 0 ? h : 1;
}

std::shared_ptr<const SceneIcon> decode_scene_icon(const os::LargeBuffer& png, int32_t side) {
    uint32_t w = 0, h = 0;
    if (side <= 0 || !png_size(png, w, h) || w == 0 || h == 0 || w > kIconMaxSide ||
        h > kIconMaxSide) {
        return nullptr;
    }
    // Off the UI thread: lodepng and lv_draw_buf_create_ex() only allocate,
    // through lv_malloc(), which here is os::alloc (lv_mem_wisp.cpp), and
    // read the draw-buffer handlers lv_init() set; they change no LVGL
    // state (no cache, no display).
    unsigned char* out = nullptr;
    unsigned dw = 0, dh = 0;
    const unsigned err = lodepng_decode32(&out, &dw, &dh, png.data(), png.size());
    auto* decoded = reinterpret_cast<lv_draw_buf_t*>(out);
    if (err != 0 || decoded == nullptr || dw != w || dh != h || decoded->header.stride < w * 4) {
        if (decoded != nullptr) lv_draw_buf_destroy(decoded);
        return nullptr;
    }
    const uint8_t* rgba = decoded->data;
    const uint32_t src_stride = decoded->header.stride;
    // Fit the square, keeping the aspect.
    const auto out_w =
        static_cast<uint32_t>(std::max<int64_t>(1, w >= h ? side : int64_t{side} * w / h));
    const auto out_h =
        static_cast<uint32_t>(std::max<int64_t>(1, h >= w ? side : int64_t{side} * h / w));
    auto icon = std::make_shared<SceneIcon>();
    const uint32_t stride = out_w * 4;
    icon->pixels = os::LargeBuffer::allocate(size_t{stride} * out_h);
    if (!icon->pixels.ok()) {
        lv_draw_buf_destroy(decoded);
        return nullptr;
    }
    uint8_t* dst = icon->pixels.data();
    for (uint32_t y = 0; y < out_h; ++y) {
        // The source rows and columns this pixel covers: a box average
        // when shrinking, the nearest pixel when growing.
        const uint32_t y0 = y * h / out_h;
        const uint32_t y1 = std::max(y0 + 1, (y + 1) * h / out_h);
        for (uint32_t x = 0; x < out_w; ++x) {
            const uint32_t x0 = x * w / out_w;
            const uint32_t x1 = std::max(x0 + 1, (x + 1) * w / out_w);
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* p = rgba + size_t{sy} * src_stride + size_t{x0} * 4;
                for (uint32_t sx = x0; sx < x1; ++sx, p += 4) {
                    // Colour weighted by alpha, so transparent pixels do
                    // not darken the edges.
                    r += uint32_t{p[0]} * p[3];
                    g += uint32_t{p[1]} * p[3];
                    b += uint32_t{p[2]} * p[3];
                    a += p[3];
                    ++n;
                }
            }
            uint8_t* q = dst + size_t{y} * stride + size_t{x} * 4;
            // LVGL's ARGB8888 is B, G, R, A in memory.
            q[0] = static_cast<uint8_t>(a ? b / a : 0);
            q[1] = static_cast<uint8_t>(a ? g / a : 0);
            q[2] = static_cast<uint8_t>(a ? r / a : 0);
            q[3] = static_cast<uint8_t>(a / n);
        }
    }
    lv_draw_buf_destroy(decoded);
    lv_image_dsc_t& img = icon->image;
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_ARGB8888;
    img.header.w = out_w;
    img.header.h = out_h;
    img.header.stride = stride;
    img.data = icon->pixels.data();
    img.data_size = static_cast<uint32_t>(icon->pixels.size());
    return icon;
}

int32_t SceneList::icon_side(const hal::ScreenShape& shape) { return grid_of(shape).card_px / 2; }

SceneList::SceneList(SceneListDeps deps) : deps_(std::move(deps)) {
    const Grid g = grid_of(deps_.shape);
    columns_ = g.columns;
    card_px_ = g.card_px;
}

SceneList::~SceneList() { *alive_ = false; }

void SceneList::build(lv_obj_t* list) {
    list_ = list;
    grid_ = detail::make_box(list_);
    lv_obj_set_flex_flow(grid_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid_, kGap, 0);
    lv_obj_set_style_pad_row(grid_, kGap, 0);
    show_loading();
    detail::bubble_tree(list_);
}

void SceneList::show_loading() {
    // Rows wholly inside the safe area below the list's top, so every
    // placeholder is on the visible screen.
    lv_obj_update_layout(list_);
    lv_area_t list_area;
    lv_obj_get_coords(list_, &list_area);
    const hal::Area safe = hal::safe_rect(deps_.shape);
    const int32_t room =
        safe.y2 + 1 - (list_area.y1 + lv_obj_get_style_pad_top(list_, LV_PART_MAIN));
    const int32_t rows = std::max<int32_t>(1, (room + kGap) / (card_px_ + kGap));
    const bool colour = detail::colour_panel(list_);
    for (int32_t i = 0; i < rows * columns_; ++i) {
        lv_obj_t* p = lv_obj_create(grid_);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, card_px_, card_px_);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(p, ui::theme::kRadius, 0);
        if (colour) {
            lv_obj_set_style_bg_color(p, lv_color_hex(ui::theme::kSurface), 0);
            lv_obj_set_style_bg_opa(p, kPlaceholderOpa, 0);
        } else {
            lv_obj_set_style_border_color(p, detail::dim_colour(list_), 0);
            lv_obj_set_style_border_width(p, ui::theme::kBorderWidth, 0);
            lv_obj_set_style_border_opa(p, kPlaceholderOpa, 0);
        }
    }
    placeholders_ = static_cast<size_t>(rows * columns_);
}

void SceneList::set_scenes(std::vector<ShellScene> scenes) {
    const auto still_there = [&] {
        for (const ShellScene& s : scenes)
            if (s.id == pop_id_) return s.available;
        return false;
    };
    // A refresh (any scene.state) must not take the card away from a user
    // still deciding.
    if (popup_ != nullptr && !still_there()) close_popup();
    // A refresh with nothing new (the cached listing shown at once, then
    // the same one from storage) keeps the cards as they are.
    if (listed_ && scenes.size() == scenes_.size() &&
        std::equal(scenes.begin(), scenes.end(), scenes_.begin(), same_card)) {
        scenes_ = std::move(scenes);
        return;
    }
    listed_ = true;
    scenes_ = std::move(scenes);
    const int32_t y = lv_obj_get_scroll_y(list_);
    build_cards();
    lv_obj_update_layout(list_);
    lv_obj_scroll_to_y(list_, std::min<int32_t>(y, y + lv_obj_get_scroll_bottom(list_)),
                       LV_ANIM_OFF);
}

void SceneList::build_cards() {
    lv_obj_clean(grid_);
    placeholders_ = 0;
    ++card_builds_;
    cards_.clear();
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

        put_icon(c, s);
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

// The icon or, without one, the first character, first in the card.
void SceneList::put_icon(Card& c, const ShellScene& s) {
    if (s.icon) {
        // Already icon_side() across: drawn as is, centred. scenes_ holds
        // the pixels for as long as the card shows them.
        const int32_t icon_px = icon_side(deps_.shape);
        c.icon = lv_image_create(c.card);
        lv_obj_set_size(c.icon, icon_px, icon_px);
        lv_image_set_inner_align(c.icon, LV_IMAGE_ALIGN_CENTER);
        lv_image_set_src(c.icon, &s.icon->image);
        lv_obj_move_to_index(c.icon, 0);
    } else {
        c.letter = lv_label_create(c.card);
        lv_obj_set_style_text_font(c.letter, ui::font(kLetterFontPx), 0);
        lv_label_set_text(c.letter, first_char(s.name).c_str());
        lv_obj_move_to_index(c.letter, 0);
    }
}

void SceneList::set_icon(const std::string& id, std::shared_ptr<const SceneIcon> icon) {
    for (size_t i = 0; i < scenes_.size() && i < cards_.size(); ++i) {
        ShellScene& s = scenes_[i];
        if (s.id != id) continue;
        if (s.icon == icon) return;
        Card& c = cards_[i];
        lv_obj_t* old = c.icon != nullptr ? c.icon : c.letter;
        // The old pixels stay until the image showing them is gone.
        const std::shared_ptr<const SceneIcon> keep = std::move(s.icon);
        c.icon = c.letter = nullptr;
        s.icon = std::move(icon);
        put_icon(c, s);
        if (old != nullptr) lv_obj_delete(old);
        detail::bubble_tree(c.card);
        ++icon_swaps_;
        return;
    }
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
    pop_card_ = card;
    lv_obj_set_width(card, std::min(area_w(safe), kPopupMaxPx));
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);  // a tap on it is not outside
    lv_obj_t* name = detail::make_text(card, s.name.c_str());
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    pop_data_ = nullptr;
    pop_delete_ = lv_button_create(card);
    lv_obj_set_size(pop_delete_, LV_PCT(100), detail::kMinTouchPx);
    ui::set_button_role(pop_delete_, ui::ButtonRole::Danger);
    lv_obj_t* label = lv_label_create(pop_delete_);
    lv_label_set_text(label, ui::tr(ui::StrId::SceneDeleteButton));
    lv_obj_center(label);
    lv_obj_add_event_cb(pop_delete_, &SceneList::popup_cb, LV_EVENT_CLICKED, this);
    place_popup();
    detail::bubble_tree(popup_);
    // Without the option until the answer comes; deleting before it does
    // keeps the data, as leaving the box unticked would.
    pop_pending_ = static_cast<bool>(deps_.has_data);
    if (pop_pending_) {
        const uint32_t serial = ++pop_serial_;
        deps_.has_data(pop_id_, [this, alive = alive_, serial](bool has) {
            if (*alive) on_has_data(serial, has);
        });
    }
}

void SceneList::on_has_data(uint32_t serial, bool has) {
    if (popup_ == nullptr || serial != pop_serial_ || !pop_pending_) return;
    pop_pending_ = false;
    if (!has) return;
    pop_data_ = lv_checkbox_create(pop_card_);
    lv_checkbox_set_text(pop_data_, ui::tr(ui::StrId::SceneDeleteData));
    lv_obj_set_style_min_height(pop_data_, detail::kMinTouchPx, 0);
    lv_obj_set_flex_flow(pop_data_, LV_FLEX_FLOW_ROW);
    // Above the delete button, as if it had been there from the start.
    lv_obj_move_to_index(pop_data_, lv_obj_get_index(pop_delete_));
    detail::bubble_tree(pop_data_);
    place_popup();
}

void SceneList::place_popup() {
    // Centred in the safe area, which on a round screen is not the screen's
    // centre row by row but is by its box.
    const hal::Area safe = hal::safe_rect(deps_.shape);
    lv_obj_update_layout(pop_card_);
    lv_obj_set_pos(pop_card_, (safe.x1 + safe.x2 + 1 - lv_obj_get_width(pop_card_)) / 2,
                   (safe.y1 + safe.y2 + 1 - lv_obj_get_height(pop_card_)) / 2);
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
    popup_ = pop_card_ = pop_delete_ = pop_data_ = nullptr;
    pop_pending_ = false;
    pop_id_.clear();
}

}  // namespace wisp::shell
