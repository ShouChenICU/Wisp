#include "wisp/shell/shell.hpp"

#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/shell/gestures.hpp"
#include "wisp/shell/home.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell {

namespace {
constexpr const char* kTag = "shell";
// A sub-page's title.
constexpr int kTitleFontPx = 28;

ui::StrId title_of(Page p) {
    switch (p) {
        case Page::Settings:
            return ui::StrId::PageSettings;
        case Page::Scenes:
            return ui::StrId::PageScenes;
        case Page::History:
        case Page::Home:
            break;
    }
    return ui::StrId::PageHistory;
}
}  // namespace

const char* to_string(Page p) {
    switch (p) {
        case Page::Home:
            return "home";
        case Page::Settings:
            return "settings";
        case Page::Scenes:
            return "scenes";
        case Page::History:
            return "history";
    }
    return "home";
}

Shell::Shell(ShellDeps deps) : deps_(std::move(deps)) {
    if (!deps_.now_ms) deps_.now_ms = os::now_ms;
}

Shell::~Shell() = default;

lv_obj_t* Shell::home_screen() const { return home_screen_; }

ui::WispView* Shell::wisp() const { return home_ ? home_->wisp() : nullptr; }

void Shell::start() {
    if (home_ != nullptr) return;
    alive_ = std::make_shared<bool>(true);
    prev_ = lv_screen_active();

    home_ = std::make_unique<Home>(HomeDeps{
        .ui = deps_.ui,
        .shape = deps_.shape,
        .bus = deps_.bus,
        .now_ms = deps_.now_ms,
        .admin_url = deps_.admin_url,
        .navigate = [this](Page p) { post_show(p); },
    });
    home_->start();
    home_->set_conversation(deps_.conversation);
    home_screen_ = home_->screen();

    lv_screen_load(home_screen_);
    show(Page::Home);
}

void Shell::stop() {
    if (home_ == nullptr) return;
    if (diag_ != nullptr) close_diagnostics();
    *alive_ = false;
    alive_.reset();
    if (lv_screen_active() == home_screen_ || (sub_ != nullptr && lv_screen_active() == sub_)) {
        // An active screen must not be deleted.
        lv_obj_t* back =
            prev_ != nullptr && lv_obj_is_valid(prev_) ? prev_ : lv_obj_create(nullptr);
        lv_screen_load(back);
    }
    delete_page();
    home_->stop();  // deletes the home screen and the wisp with it
    home_.reset();
    home_screen_ = nullptr;
    prev_ = nullptr;
    page_ = Page::Home;
}

void Shell::set_conversation(ConversationController* conversation) {
    deps_.conversation = conversation;
    if (home_ != nullptr) home_->set_conversation(conversation);
}

bool Shell::leave_scene() {
    if (!deps_.leave_scene) return true;
    Result<bool> r = deps_.leave_scene();
    if (!r) {
        WLOG_W(kTag, "scene in front not sent back (%s); staying", r.error().message.c_str());
        return false;
    }
    if (r.value()) {
        // Its view is about to leave the screen for one of the shell's;
        // the back gesture goes with it (the diagnostics set their own).
        deps_.layer.set_back_handler({});
        WLOG_I(kTag, "scene in front sent back");
    }
    return true;
}

void Shell::post_show(Page p) {
    // From inside an LVGL event of the screen it may delete.
    deps_.ui.post([this, p, alive = alive_] {
        if (*alive) show(p);
    });
}

void Shell::show(Page p) {
    if (home_ == nullptr) {
        page_ = p;
        WLOG_I(kTag, "page %s", to_string(p));
        return;
    }
    if (!leave_scene()) return;
    if (diag_ != nullptr) close_diagnostics();
    if (p == Page::Home) {
        if (lv_screen_active() != home_screen_) lv_screen_load(home_screen_);
        delete_page();
    } else if (p != page_ || sub_ == nullptr) {
        lv_obj_t* old = sub_;
        sub_ = build_page(p);
        lv_screen_load(sub_);
        if (old != nullptr) lv_obj_delete(old);
    } else if (lv_screen_active() != sub_) {
        lv_screen_load(sub_);
    }
    page_ = p;
    WLOG_I(kTag, "page %s", to_string(p));
}

lv_obj_t* Shell::build_page(Page p) {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    const hal::PanelKind panel = ui::display_panel(lv_obj_get_display(scr));
    lv_obj_set_style_bg_color(scr, ui::wisp_background(panel), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ui::default_font(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(scr, &Shell::page_event_cb, LV_EVENT_ALL, this);

    // The title, centred at the top of the safe area, below the status
    // capsule.
    const hal::Area safe = hal::safe_rect(deps_.shape);
    const hal::Area capsule = ui::SystemLayer::status_area(deps_.shape);
    lv_obj_t* title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, ui::font(kTitleFontPx), 0);
    lv_obj_set_style_text_color(
        title,
        hal::panel_is_colour(panel) ? lv_color_hex(ui::theme::kText) : ui::wisp_colour(panel), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, ui::tr(title_of(p)));
    const int32_t y = capsule.y2 + 1 + ui::theme::kGapSm;
    lv_obj_set_pos(title, safe.x1, y);
    lv_obj_set_width(title, safe.x2 - safe.x1 + 1);
    return scr;
}

void Shell::delete_page() {
    if (sub_ == nullptr) return;
    lv_obj_t* scr = sub_;
    sub_ = nullptr;
    page_tracking_ = false;
    lv_obj_delete(scr);
}

void Shell::page_event_cb(lv_event_t* e) {
    auto* self = static_cast<Shell*>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        self->page_tracking_ = true;
        self->page_start_ = p;
        self->page_top_zone_ = ui::SystemLayer::in_top_zone(self->deps_.shape, p.x, p.y);
    } else if (code == LV_EVENT_PRESS_LOST) {
        self->page_tracking_ = false;
    } else if (code == LV_EVENT_RELEASED && self->page_tracking_) {
        self->page_tracking_ = false;
        // Nothing on a sub-page scrolls yet: the history is at its bottom
        // end.
        const Swipe s = classify_swipe(self->page_start_, p);
        if (auto to = navigate(self->page_, s, self->page_top_zone_, true)) self->post_show(*to);
    }
}

void Shell::open_diagnostics() {
    if (home_ == nullptr || diag_ != nullptr || !deps_.open_diagnostics) return;
    if (!leave_scene()) return;
    // Over the page the user is on; a scene sent back leaves it the
    // active screen no longer.
    lv_obj_t* under = sub_ != nullptr ? sub_ : home_screen_;
    if (lv_screen_active() != under) lv_screen_load(under);
    lv_obj_t* before = lv_screen_active();
    deps_.open_diagnostics();
    lv_obj_t* now = lv_screen_active();
    if (now == before || now == home_screen_) {
        WLOG_W(kTag, "diagnostics not opened");
        return;
    }
    diag_ = now;
    lv_obj_add_event_cb(diag_, &Shell::diag_loaded_cb, LV_EVENT_SCREEN_LOADED, this);
    set_diag_back_handler();
    WLOG_I(kTag, "diagnostics open");
}

void Shell::set_diag_back_handler() {
    // Posted: the handler runs inside the pointer's read callback, and the
    // screen it would delete is the one under the finger.
    deps_.layer.set_back_handler([this, alive = alive_] {
        deps_.ui.post([this, alive] {
            if (*alive) show(Page::Home);
        });
    });
}

void Shell::diag_loaded_cb(lv_event_t* e) {
    // A scene opened over the diagnostics cleared the back handler when it
    // went; this screen is in front again.
    auto* self = static_cast<Shell*>(lv_event_get_user_data(e));
    if (self->diag_ != nullptr) self->set_diag_back_handler();
}

void Shell::close_diagnostics() {
    lv_obj_t* diag = diag_;
    diag_ = nullptr;
    if (lv_screen_active() == diag) {
        // Only the diagnostics' own gesture: a scene in front keeps its.
        deps_.layer.set_back_handler({});
        lv_screen_load(sub_ != nullptr ? sub_ : home_screen_);
    }
    lv_obj_remove_event_cb_with_user_data(diag, &Shell::diag_loaded_cb, this);
    if (deps_.close_diagnostics) deps_.close_diagnostics();
    WLOG_I(kTag, "diagnostics closed");
}

}  // namespace wisp::shell
