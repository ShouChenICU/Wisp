#include "wisp/shell/shell.hpp"

#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell {

namespace {
constexpr const char* kTag = "shell";
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

Shell::Shell(ShellDeps deps) : deps_(std::move(deps)) {}

Shell::~Shell() = default;

void Shell::start() {
    if (home_ != nullptr) return;
    alive_ = std::make_shared<bool>(true);
    prev_ = lv_screen_active();

    home_ = lv_obj_create(nullptr);
    // No theme styles: the screen is one flat surface the wisp is drawn on
    // (it draws no background of its own).
    lv_obj_remove_style_all(home_);
    lv_obj_set_style_bg_color(home_,
                              ui::wisp_background(ui::display_panel(lv_obj_get_display(home_))), 0);
    lv_obj_set_style_bg_opa(home_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(home_, LV_OBJ_FLAG_SCROLLABLE);

    // Centred in the safe area (spec 5.6), which on an asymmetric shape is
    // not the screen's centre.
    const hal::Area safe = hal::safe_rect(deps_.shape);
    wisp_ = ui::WispView::create(home_, ui::wisp_size_px(safe, kWispIdlePercent));
    if (wisp_ != nullptr) {
        const int32_t dx = (safe.x1 + safe.x2 + 1) / 2 - deps_.shape.width / 2;
        const int32_t dy = (safe.y1 + safe.y2 + 1) / 2 - deps_.shape.height / 2;
        lv_obj_align(wisp_->obj(), LV_ALIGN_CENTER, dx, dy);
    } else {
        WLOG_E(kTag, "no memory for the wisp; the home screen is empty");
    }

    lv_screen_load(home_);
    show(Page::Home);
}

void Shell::stop() {
    if (home_ == nullptr) return;
    if (diag_ != nullptr) close_diagnostics();
    *alive_ = false;
    alive_.reset();
    if (lv_screen_active() == home_) {
        // An active screen must not be deleted.
        lv_obj_t* back =
            prev_ != nullptr && lv_obj_is_valid(prev_) ? prev_ : lv_obj_create(nullptr);
        lv_screen_load(back);
    }
    lv_obj_delete(home_);  // frees the wisp with its object
    home_ = nullptr;
    wisp_ = nullptr;
    prev_ = nullptr;
    page_ = Page::Home;
}

void Shell::show(Page p) {
    page_ = p;
    if (p == Page::Home && home_ != nullptr) {
        if (diag_ != nullptr) close_diagnostics();
        if (lv_screen_active() != home_) lv_screen_load(home_);
    }
    WLOG_I(kTag, "page %s", to_string(p));
}

void Shell::open_diagnostics() {
    if (home_ == nullptr || diag_ != nullptr || !deps_.open_diagnostics) return;
    lv_obj_t* before = lv_screen_active();
    deps_.open_diagnostics();
    lv_obj_t* now = lv_screen_active();
    if (now == before || now == home_) {
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
        lv_screen_load(home_);
    }
    lv_obj_remove_event_cb_with_user_data(diag, &Shell::diag_loaded_cb, this);
    if (deps_.close_diagnostics) deps_.close_diagnostics();
    WLOG_I(kTag, "diagnostics closed");
}

}  // namespace wisp::shell
