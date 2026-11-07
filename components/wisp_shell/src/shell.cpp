#include "wisp/shell/shell.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include "lvgl_private.h"  // lv_indev_t's long-press stamps
#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/shell/gestures.hpp"
#include "wisp/shell/history.hpp"
#include "wisp/shell/home.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/shell/scene_recents.hpp"
#include "wisp/shell/settings_page.hpp"
#include "wisp/shell/talk_overlay.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell {

namespace {
constexpr const char* kTag = "shell";
// The talk overlay's tick(), as often as Home's own refresh timer.
constexpr uint32_t kTalkTickMs = 250;

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

std::optional<voice::VoiceState> parse_voice(const std::string& s) {
    using voice::VoiceState;
    for (VoiceState v : {VoiceState::Idle, VoiceState::Listening, VoiceState::Transcribing,
                         VoiceState::Speaking, VoiceState::Error}) {
        if (s == voice::to_string(v)) return v;
    }
    return std::nullopt;
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

Shell::Shell(ShellDeps deps) : deps_(std::move(deps)), track_(std::make_unique<DragTrack>()) {
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
    });
    home_->start();
    home_->set_conversation(deps_.conversation);
    home_screen_ = home_->screen();
    // After Home's own handler: Home has seen the press when may_drag()
    // asks about it.
    lv_obj_add_event_cb(home_screen_, &Shell::press_cb, LV_EVENT_ALL, this);

    lv_screen_load(home_screen_);
    listing_ = std::make_shared<std::atomic<bool>>(false);
    icons_ = std::make_shared<IconCache>();
    // Always subscribed: a scene.state event hides the talk overlay (the
    // scene that was in front may no longer be) whether or not the scene
    // list is also listening for it.
    scene_sub_ = deps_.bus.subscribe("scene.state", [this, alive = alive_](const core::Event&) {
        // On the bus's thread: the list is read again from the UI
        // thread, which knows whether it is open.
        deps_.ui.post([this, alive] {
            if (!*alive) return;
            if (talk_) talk_->hide();
            if (scenes_) list_scenes();
        });
    });
    talk_ =
        std::make_unique<TalkOverlay>(deps_.layer, deps_.shape, [this] { post_show(Page::Home); });
    // on_voice_bus() only touches atomics directly; request_talk() (which
    // it calls) does its own posting with its own alive_ check.
    talk_voice_sub_ =
        deps_.bus.subscribe("voice.state", [this](const core::Event& e) { on_voice_bus(e); });
    talk_timer_ = lv_timer_create(&Shell::talk_timer_cb, kTalkTickMs, this);
    if (deps_.conversation != nullptr) {
        talk_listener_ =
            deps_.conversation->add_listener([this](const TurnView&) { request_talk(); });
    }
    show(Page::Home);
}

void Shell::stop() {
    if (home_ == nullptr) return;
    if (scene_sub_ != 0) {
        deps_.bus.unsubscribe(scene_sub_);
        scene_sub_ = 0;
    }
    if (talk_voice_sub_ != 0) {
        deps_.bus.unsubscribe(talk_voice_sub_);
        talk_voice_sub_ = 0;
    }
    if (talk_timer_ != nullptr) {
        lv_timer_delete(talk_timer_);
        talk_timer_ = nullptr;
    }
    if (deps_.conversation != nullptr && talk_listener_ != 0)
        deps_.conversation->remove_listener(talk_listener_);
    talk_listener_ = 0;
    abort_drag();
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
    talk_.reset();
    home_->stop();  // deletes the home screen and the wisp with it
    home_.reset();
    home_screen_ = nullptr;
    prev_ = nullptr;
    page_ = Page::Home;
}

void Shell::set_conversation(ConversationController* conversation) {
    if (deps_.conversation != nullptr && talk_listener_ != 0)
        deps_.conversation->remove_listener(talk_listener_);
    talk_listener_ = 0;
    deps_.conversation = conversation;
    if (home_ != nullptr) home_->set_conversation(conversation);
    if (deps_.conversation != nullptr) {
        talk_listener_ =
            deps_.conversation->add_listener([this](const TurnView&) { request_talk(); });
    }
    request_talk();
}

bool Shell::scene_in_front() const {
    if (home_ == nullptr) return false;
    lv_obj_t* act = lv_screen_active();
    return act != home_screen_ && act != sub_ && act != diag_;
}

void Shell::request_talk() {
    if (!talk_) return;
    if (talk_posted_.exchange(true)) return;
    // A full queue: nothing is lost (the next request_talk() tries again),
    // but the flag must come back down or every later call would see it
    // still set and never post again.
    if (!deps_.ui.post([this, alive = alive_] {
            talk_posted_.store(false);
            if (*alive) update_talk();
        }))
        talk_posted_.store(false);
}

void Shell::update_talk() {
    if (!talk_ || !scene_in_front()) return;
    const TurnView view =
        deps_.conversation != nullptr ? deps_.conversation->current() : TurnView{};
    talk_->on_turn(view, voice_state_.load(), voice_level_.load(), deps_.now_ms());
}

void Shell::on_voice_bus(const core::Event& e) {
    auto it = e.payload.find("state");
    const std::string state = it != e.payload.end() && it->second.type() == Value::Type::String
                                  ? it->second.as_string()
                                  : std::string();
    if (auto v = parse_voice(state)) {
        voice_state_.store(*v);
        auto lvl = e.payload.find("level");
        voice_level_.store(
            lvl != e.payload.end() && lvl->second.type() == Value::Type::Int
                ? static_cast<uint8_t>(std::clamp<int64_t>(lvl->second.as_int(), 0, 100))
                : 0);
    }
    request_talk();
}

void Shell::talk_timer_cb(lv_timer_t* t) {
    auto* self = static_cast<Shell*>(lv_timer_get_user_data(t));
    if (self->talk_) self->talk_->tick(self->deps_.now_ms());
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
    abort_drag();
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
    lv_obj_add_event_cb(f.screen, &Shell::press_cb, LV_EVENT_ALL, this);
    page_alive_ = std::make_shared<bool>(true);
    listing_ = std::make_shared<std::atomic<bool>>(false);
    sub_root_ = f.root;
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
    sub_root_ = nullptr;
    history_.reset();
    scenes_.reset();
    settings_.reset();
}

struct Shell::IconCache {
    struct Entry {
        uint64_t hash = 0;
        size_t size = 0;
        std::shared_ptr<const SceneIcon> icon;  // null: the file did not decode
    };

    // Fills each scene's `icon` from its `icon_png`, decoding only what
    // changed, and drops the file bytes. The bytes are hashed rather than
    // compared by pointer: every listing reads the files again.
    void resolve(std::vector<ShellScene>& scenes, int32_t side) {
        std::lock_guard<os::Mutex> lock(mu);
        std::map<std::string, Entry> next;
        for (ShellScene& sc : scenes) {
            if (!sc.icon_png) continue;
            const os::LargeBuffer& png = *sc.icon_png;
            const uint64_t h = fnv1a(png);
            auto it = by_id.find(sc.id);
            Entry e;
            if (it != by_id.end() && it->second.hash == h && it->second.size == png.size()) {
                e = it->second;
            } else {
                e = {h, png.size(), decode_scene_icon(png, side)};
                if (!e.icon)
                    WLOG_W(kTag, "%s: icon not shown (not a PNG it can draw)", sc.id.c_str());
            }
            sc.icon = e.icon;
            sc.icon_png.reset();
            next[sc.id] = std::move(e);
        }
        by_id = std::move(next);  // deleted scenes and dropped icons go
    }

    static uint64_t fnv1a(const os::LargeBuffer& b) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < b.size(); ++i) {
            h ^= b.data()[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    os::Mutex mu;
    std::map<std::string, Entry> by_id;
};

void Shell::list_scenes() {
    if (!scenes_) return;
    if (!deps_.list_scenes || !deps_.submit_io) {
        scenes_->set_scenes({});
        return;
    }
    // One listing waiting in the queue answers every event before it ran.
    if (listing_->exchange(true)) return;
    auto job = [this, list = deps_.list_scenes, recents = deps_.recents, ui = &deps_.ui,
                queued = listing_, alive = page_alive_, icons = icons_,
                side = SceneList::icon_side(deps_.shape)] {
        queued->store(false);
        std::vector<ShellScene> got = list();
        icons->resolve(got, side);
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
    pressing_ = false;
    drop_pages();
    lv_obj_delete(scr);
}

void Shell::press_cb(lv_event_t* e) { static_cast<Shell*>(lv_event_get_user_data(e))->on_press(e); }

namespace {

// Whether `obj` can still scroll the way a finger moving `dir` scrolls it
// (as LVGL's own search for a scroll decides it).
bool scrolls_toward(lv_obj_t* obj, Swipe dir) {
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) return false;
    const lv_dir_t can = lv_obj_get_scroll_dir(obj);
    switch (dir) {
        case Swipe::Right:
            return (can & LV_DIR_LEFT) != 0 && lv_obj_get_scroll_left(obj) > 0;
        case Swipe::Left:
            return (can & LV_DIR_RIGHT) != 0 && lv_obj_get_scroll_right(obj) > 0;
        case Swipe::Down:
            return (can & LV_DIR_TOP) != 0 && lv_obj_get_scroll_top(obj) > 0;
        case Swipe::Up:
            return (can & LV_DIR_BOTTOM) != 0 && lv_obj_get_scroll_bottom(obj) > 0;
        case Swipe::None:
            break;
    }
    return false;
}

// The press is the drag's: no long press on what it started on. Through
// LVGL's private struct, because lv_indev_reset_long_press() stamps
// lv_tick_get(), which can be a tick past the read being processed, and
// LVGL's tick difference then wraps round into a long press at once.
void hold_off_long_press(lv_indev_t* indev) {
    indev->pr_timestamp = indev->timestamp;
    indev->long_pr_sent = 0;
}

}  // namespace

void Shell::on_press(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST)
        return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_obj_t* scr = lv_event_get_current_target_obj(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const uint64_t now = deps_.now_ms();
    switch (code) {
        case LV_EVENT_PRESSED:
            pressing_ = false;
            track_->reset();
            // A press while the pages still slide waits for the next one.
            if (drag_.phase != DragPhase::Idle || scr != lv_screen_active()) return;
            pressing_ = true;
            press_target_ = lv_event_get_target_obj(e);
            press_top_zone_ = ui::SystemLayer::in_top_zone(deps_.shape, p.x, p.y);
            track_->press(p, now);
            return;
        case LV_EVENT_PRESSING: {
            if (drag_.phase == DragPhase::Following) {
                if (scr != drag_.host) return;
                track_->moved(p, now);
                place_drag(track_->offset());
                hold_off_long_press(indev);
                return;
            }
            if (!pressing_) return;
            const Swipe dir = track_->moved(p, now);
            if (dir == Swipe::None) return;
            pressing_ = false;
            const bool at_bottom = !history_ || history_->at_bottom();
            const std::optional<Page> to = navigate(page_, dir, press_top_zone_, at_bottom);
            if (!to || !may_drag(scr, press_target_, dir)) {
                track_->reset();
                return;
            }
            begin_drag(*to, dir, press_target_);
            hold_off_long_press(indev);
            track_->moved(p, now);
            place_drag(track_->offset());
            return;
        }
        case LV_EVENT_RELEASED:
            pressing_ = false;
            if (drag_.phase == DragPhase::Following && scr == drag_.host) {
                const bool complete = track_->release(p, now);
                // No click for what the drag started on.
                lv_indev_reset(indev, nullptr);
                settle_drag(complete);
            } else {
                track_->reset();
            }
            return;
        case LV_EVENT_PRESS_LOST:
            pressing_ = false;
            track_->reset();
            if (drag_.phase == DragPhase::Following && scr == drag_.host) settle_drag(false);
            return;
        default:
            return;
    }
}

bool Shell::may_drag(lv_obj_t* scr, lv_obj_t* target, Swipe dir) const {
    lv_obj_t* root = page_ == Page::Home ? (home_ ? home_->root() : nullptr) : sub_root_;
    if (root == nullptr || lv_obj_get_screen(root) != scr) return false;
    if (target != nullptr && target != scr) {
        bool in_root = false;
        for (lv_obj_t* o = target; o != nullptr && o != scr; o = lv_obj_get_parent(o)) {
            if (o == root) in_root = true;
            if (lv_obj_has_flag(o, detail::kNoSwipe) || scrolls_toward(o, dir)) return false;
        }
        // Not the page's own (a card over it).
        if (!in_root) return false;
    }
    return page_ != Page::Home || home_->may_drag();
}

void Shell::begin_drag(Page to, Swipe dir, lv_obj_t* target) {
    Drag& d = drag_;
    d = Drag{};
    d.to = to;
    d.host = lv_screen_active();
    d.from_root = page_ == Page::Home ? home_->root() : sub_root_;
    if (to == Page::Home) {
        d.to_root = home_->root();
    } else {
        sub_ = build_page(to);
        d.to_root = sub_root_;
        if (to == Page::Scenes) list_scenes();
    }
    lv_obj_set_parent(d.to_root, d.host);
    // LVGL would otherwise scroll these along with the drag: it looks for
    // something to scroll after this event, on the same read.
    for (lv_obj_t* o = target; o != nullptr && o != d.host; o = lv_obj_get_parent(o)) {
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) continue;
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        d.frozen.push_back(o);
    }
    if (target != nullptr && target != d.host) lv_obj_remove_state(target, LV_STATE_PRESSED);
    if (page_ == Page::Home) home_->drag_started();
    d.ux = dir == Swipe::Right ? 1 : dir == Swipe::Left ? -1 : 0;
    d.uy = dir == Swipe::Down ? 1 : dir == Swipe::Up ? -1 : 0;
    d.extent = d.ux != 0 ? deps_.shape.width : deps_.shape.height;
    d.phase = DragPhase::Following;
    track_->start(dir, d.extent);
    WLOG_D(kTag, "drag %s -> %s", to_string(page_), to_string(to));
}

void Shell::place_drag(int32_t offset) {
    Drag& d = drag_;
    d.offset = offset;
    // Only positions: the pages keep their layout.
    lv_obj_set_pos(d.from_root, d.ux * offset, d.uy * offset);
    lv_obj_set_pos(d.to_root, d.ux * (offset - d.extent), d.uy * (offset - d.extent));
}

void Shell::settle_drag(bool complete) {
    Drag& d = drag_;
    d.phase = DragPhase::Sliding;
    d.complete = complete;
    const int32_t end = complete ? d.extent : 0;
    const int32_t left = std::abs(end - d.offset);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_values(&a, d.offset, end);
    // At least a millisecond: the end may delete the screen this release
    // came from, which must happen on the timer and not inside its event.
    lv_anim_set_duration(
        &a, std::max<uint32_t>(static_cast<uint32_t>(int64_t{kSlideMs} * left / d.extent), 1));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, &Shell::slide_cb);
    lv_anim_set_user_data(&a, this);
    lv_anim_set_completed_cb(&a, &Shell::slide_done_cb);
    lv_anim_start(&a);
}

void Shell::slide_cb(void* var, int32_t v) { static_cast<Shell*>(var)->place_drag(v); }

void Shell::slide_done_cb(lv_anim_t* a) {
    static_cast<Shell*>(lv_anim_get_user_data(a))->finish_drag();
}

void Shell::finish_drag() {
    if (drag_.phase == DragPhase::Idle) return;
    Drag d = std::move(drag_);
    drag_ = Drag{};
    for (lv_obj_t* o : d.frozen)
        if (lv_obj_is_valid(o)) lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(d.from_root, 0, 0);
    lv_obj_set_pos(d.to_root, 0, 0);
    // A screen that came in front meanwhile (a scene) stays there.
    const bool complete = d.complete && lv_screen_active() == d.host;
    lv_obj_t* own = d.to == Page::Home ? home_screen_ : sub_;
    lv_obj_set_parent(d.to_root, own);
    // Under anything put over the page on its screen.
    lv_obj_move_to_index(d.to_root, 0);
    if (!complete) {
        if (d.to != Page::Home) delete_page();
        WLOG_D(kTag, "drag to %s snapped back", to_string(d.to));
        return;
    }
    lv_screen_load(own);
    if (d.to == Page::Home) delete_page();
    page_ = d.to;
    WLOG_I(kTag, "page %s", to_string(page_));
}

void Shell::abort_drag() {
    if (drag_.phase == DragPhase::Idle) return;
    lv_anim_delete(this, &Shell::slide_cb);
    pressing_ = false;
    track_->reset();
    drag_.complete = false;
    finish_drag();
}

void Shell::open_diagnostics() {
    if (home_ == nullptr || diag_ != nullptr || !deps_.open_diagnostics) return;
    if (!leave_scene()) return;
    abort_drag();
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
