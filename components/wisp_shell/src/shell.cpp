#include "wisp/shell/shell.hpp"

#include <utility>

#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/shell/gestures.hpp"
#include "wisp/shell/history.hpp"
#include "wisp/shell/home.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/shell/scene_recents.hpp"
#include "wisp/shell/settings_page.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell {

namespace {
constexpr const char* kTag = "shell";

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
    listing_ = std::make_shared<std::atomic<bool>>(false);
    if (deps_.list_scenes) {
        scene_sub_ = deps_.bus.subscribe("scene.state", [this, alive = alive_](const core::Event&) {
            // On the bus's thread: the list is read again from the UI
            // thread, which knows whether it is open.
            deps_.ui.post([this, alive] {
                if (*alive && scenes_) list_scenes();
            });
        });
    }
    show(Page::Home);
}

void Shell::stop() {
    if (home_ == nullptr) return;
    if (scene_sub_ != 0) {
        deps_.bus.unsubscribe(scene_sub_);
        scene_sub_ = 0;
    }
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
        sub_ = nullptr;
        if (old != nullptr) drop_pages();
        sub_ = build_page(p);
        page_ = p;
        lv_screen_load(sub_);
        if (old != nullptr) lv_obj_delete(old);
        // Once its screen is loaded: the listing may answer at once.
        if (p == Page::Scenes) list_scenes();
    } else if (lv_screen_active() != sub_) {
        lv_screen_load(sub_);
    }
    page_ = p;
    WLOG_I(kTag, "page %s", to_string(p));
}

lv_obj_t* Shell::build_page(Page p) {
    detail::PageFrame f = detail::make_page(deps_.shape, title_of(p));
    lv_obj_add_event_cb(f.screen, &Shell::page_event_cb, LV_EVENT_ALL, this);
    page_alive_ = std::make_shared<bool>(true);
    page_list_ = f.list;
    switch (p) {
        case Page::History:
            history_ = std::make_unique<HistoryPage>(
                HistoryDeps{.ui = deps_.ui, .layer = deps_.layer, .agent = deps_.agent});
            history_->build(f.list);
            break;
        case Page::Scenes:
            scenes_ = std::make_unique<SceneList>(SceneListDeps{
                .shape = deps_.shape,
                .open_scene = deps_.open_scene,
                .delete_scene = deps_.delete_scene,
            });
            scenes_->build(f.list);
            break;
        case Page::Settings:
            settings_ = std::make_unique<SettingsPage>(SettingsDeps{
                .settings = deps_.settings,
                .set_settings = deps_.set_settings,
                .wifi_status = deps_.wifi_status,
                .admin_url = deps_.admin_url,
                .firmware_version = deps_.firmware_version,
                .start_wifi_setup = deps_.start_wifi_setup,
                // Posted: it loads another screen from inside this one's
                // event.
                .open_diagnostics =
                    [this, alive = alive_] {
                        deps_.ui.post([this, alive] {
                            if (*alive) open_diagnostics();
                        });
                    },
                .submit_io = deps_.submit_io,
            });
            settings_->build(f.list);
            break;
        case Page::Home:
            break;
    }
    return f.screen;
}

void Shell::drop_pages() {
    if (page_alive_) *page_alive_ = false;
    page_alive_.reset();
    page_list_ = nullptr;
    history_.reset();
    scenes_.reset();
    settings_.reset();
}

void Shell::list_scenes() {
    if (!scenes_) return;
    if (!deps_.list_scenes || !deps_.submit_io) {
        scenes_->set_scenes({});
        return;
    }
    // One listing waiting in the queue answers every event before it ran.
    if (listing_->exchange(true)) return;
    auto job = [this, list = deps_.list_scenes, recents = deps_.recents, ui = &deps_.ui,
                queued = listing_, alive = page_alive_] {
        queued->store(false);
        std::vector<ShellScene> got = list();
        if (recents != nullptr) {
            std::vector<std::string> ids;
            ids.reserve(got.size());
            for (const ShellScene& sc : got) ids.push_back(sc.id);
            std::vector<ShellScene> sorted;
            sorted.reserve(got.size());
            for (const std::string& id : recents->order(ids)) {
                for (ShellScene& sc : got) {
                    if (sc.id == id && !sc.id.empty()) {
                        sorted.push_back(std::move(sc));
                        sc.id.clear();
                        break;
                    }
                }
            }
            got = std::move(sorted);
        }
        if (!ui->post([this, alive, got = std::move(got)]() mutable {
                if (*alive && scenes_) scenes_->set_scenes(std::move(got));
            })) {
            WLOG_W(kTag, "scene list not shown (UI queue full)");
        }
    };
    if (!deps_.submit_io(std::move(job))) {
        listing_->store(false);
        WLOG_W(kTag, "scene list not read (storage queue full)");
    }
}

void Shell::delete_page() {
    if (sub_ == nullptr) return;
    lv_obj_t* scr = sub_;
    sub_ = nullptr;
    page_tracking_ = false;
    drop_pages();
    lv_obj_delete(scr);
}

void Shell::page_event_cb(lv_event_t* e) {
    auto* self = static_cast<Shell*>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_RELEASED)
        return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        lv_obj_t* target = lv_event_get_target_obj(e);
        self->page_tracking_ = target == nullptr || !lv_obj_has_flag(target, detail::kNoSwipe);
        self->page_start_ = p;
        self->page_top_zone_ = ui::SystemLayer::in_top_zone(self->deps_.shape, p.x, p.y);
        self->page_at_bottom_ = !self->history_ || self->history_->at_bottom();
        self->page_scroll_y_ = self->page_list_ ? lv_obj_get_scroll_y(self->page_list_) : 0;
    } else if (code == LV_EVENT_PRESS_LOST) {
        self->page_tracking_ = false;
    } else if (self->page_tracking_) {
        self->page_tracking_ = false;
        // A drag that scrolled the list is no swipe. LVGL starts a scroll
        // (SCROLL_BEGIN) even at an end it cannot move past, so the offset
        // itself decides.
        if (self->page_list_ && lv_obj_get_scroll_y(self->page_list_) != self->page_scroll_y_)
            return;
        const Swipe s = classify_swipe(self->page_start_, p);
        if (auto to = navigate(self->page_, s, self->page_top_zone_, self->page_at_bottom_)) {
            // No click for what the swipe started on.
            lv_indev_reset(indev, nullptr);
            self->post_show(*to);
        }
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
