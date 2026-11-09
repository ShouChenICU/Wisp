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

// One page open's listing, for its timing line: written on the worker
// before each post that carries it on, and on the UI thread after.
struct Shell::ListTiming {
    uint64_t asked_us = 0;
    uint32_t list_ms = 0, icons_ms = 0;
    uint64_t build_us = 0;
    size_t scenes = 0, decoded = 0;
    bool cached = false;
};

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
    lang_ = ui::lang();

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
    close_choice();
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
    return act != home_screen_ && act != sub_ && act != diag_ && act != choice_screen_;
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
    self->check_drag();
    self->follow_lang();
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
    close_choice();
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
                .has_data = scene_has_data_query(),
            });
            {
                open_timing_ = std::make_shared<ListTiming>();
                const uint64_t t0 = os::now_us();
                scenes_->build(f.list);
                // The last listing at once, while storage is read again.
                if (scene_cache_) scenes_->set_scenes(*scene_cache_);
                open_timing_->build_us = os::now_us() - t0;
            }
            break;
        case Page::Settings: {
            // Out of the dialog's event first: a restart or a power off
            // flushes storage and does not come back.
            auto posted = [this](const std::function<void()>& f) -> std::function<void()> {
                if (!f) return nullptr;
                return [this, f, alive = alive_] {
                    deps_.ui.post([f, alive] {
                        if (*alive) f();
                    });
                };
            };
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
                .open_choice = [this](ChoiceSpec spec) { open_choice(std::move(spec)); },
                .restart = posted(deps_.restart),
                .power_off = posted(deps_.power_off),
                .layer = &deps_.layer,
                .submit_io = deps_.submit_io,
            });
            settings_->build(f.list);
            break;
        }
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
        uint64_t hash = 0;                      // icon_hash() of the file
        std::shared_ptr<const SceneIcon> icon;  // null: the file did not decode
    };
    // An icon file not decoded before, or changed since.
    struct Pending {
        std::string id;
        std::shared_ptr<const os::LargeBuffer> png;
        uint64_t hash = 0;
    };

    // Fills each scene's `icon` from what was decoded before, where its
    // file is unchanged (the same icon_hash()), and drops the file bytes;
    // the rest it hands back for decode(), reading with `read` the files
    // the listing named by hash alone. Hashed rather than compared by
    // pointer: a lister may read a file again. Scenes no longer listed, or
    // listed without an icon, are forgotten.
    std::vector<Pending> lookup(
        std::vector<ShellScene>& scenes,
        const std::function<std::shared_ptr<const os::LargeBuffer>(const std::string&)>& read) {
        std::lock_guard<os::Mutex> lock(mu);
        std::map<std::string, Entry> next;
        std::vector<Pending> pending;
        for (ShellScene& sc : scenes) {
            std::shared_ptr<const os::LargeBuffer> png = std::move(sc.icon_png);
            sc.icon = nullptr;
            const uint64_t h = png ? icon_hash(png->span()) : sc.icon_hash;
            if (h == 0) continue;
            auto it = by_id.find(sc.id);
            if (it != by_id.end() && it->second.hash == h) {
                sc.icon = it->second.icon;
                next[sc.id] = it->second;
                continue;
            }
            if (!png && read) png = read(sc.id);
            if (!png) continue;
            pending.push_back({sc.id, png, icon_hash(png->span())});
        }
        by_id = std::move(next);
        return pending;
    }

    // Decodes one icon lookup() handed back and keeps it (a file that does
    // not decode is kept too, as none, so it is not tried again).
    std::shared_ptr<const SceneIcon> decode(const Pending& p, int32_t side) {
        std::shared_ptr<const SceneIcon> icon = decode_scene_icon(*p.png, side);
        if (!icon) WLOG_W(kTag, "%s: icon not shown (not a PNG it can draw)", p.id.c_str());
        std::lock_guard<os::Mutex> lock(mu);
        by_id[p.id] = {p.hash, icon};
        return icon;
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
    // The page's first listing is the one its timing line is about.
    std::shared_ptr<ListTiming> timing = std::move(open_timing_);
    if (timing) timing->asked_us = os::now_us();
    auto job = [this, list = deps_.list_scenes, read_icon = deps_.read_scene_icon,
                recents = deps_.recents, ui = &deps_.ui, queued = listing_, alive = page_alive_,
                icons = icons_, side = SceneList::icon_side(deps_.shape), timing] {
        queued->store(false);
        ShellListing listing = list();
        std::vector<ShellScene> got = std::move(listing.scenes);
        if (timing) {
            timing->cached = listing.cached;
            timing->list_ms = static_cast<uint32_t>((os::now_us() - timing->asked_us) / 1000);
            timing->scenes = got.size();
        }
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
        // The cards first, with the icons decoded before; the new ones
        // follow one by one as they decode.
        const std::vector<IconCache::Pending> pending = icons->lookup(got, read_icon);
        if (!ui->post([this, alive, timing, got = std::move(got)]() mutable {
                if (!*alive || !scenes_) return;
                const uint64_t t0 = os::now_us();
                scene_cache_ = got;
                scenes_->set_scenes(std::move(got));
                if (timing) timing->build_us += os::now_us() - t0;
            })) {
            WLOG_W(kTag, "scene list not shown (UI queue full)");
            return;
        }
        const uint64_t d0 = os::now_us();
        for (const IconCache::Pending& p : pending) {
            std::shared_ptr<const SceneIcon> icon = icons->decode(p, side);
            if (!icon) continue;
            (void)ui->post([this, alive, timing, id = p.id, icon] {
                if (!*alive || !scenes_) return;
                const uint64_t t0 = os::now_us();
                for (ShellScene& sc : *scene_cache_)
                    if (sc.id == id) sc.icon = icon;
                scenes_->set_icon(id, icon);
                if (timing) timing->build_us += os::now_us() - t0;
            });
        }
        if (!timing) return;
        timing->icons_ms = static_cast<uint32_t>((os::now_us() - d0) / 1000);
        timing->decoded = pending.size();
        (void)ui->post([timing] {
            WLOG_I(kTag,
                   "scenes ms_list=%u ms_icons=%u ms_build=%u n=%u icons_decoded=%u cached=%u",
                   static_cast<unsigned>(timing->list_ms), static_cast<unsigned>(timing->icons_ms),
                   static_cast<unsigned>(timing->build_us / 1000),
                   static_cast<unsigned>(timing->scenes), static_cast<unsigned>(timing->decoded),
                   timing->cached ? 1u : 0u);
        });
    };
    if (!deps_.submit_io(std::move(job))) {
        listing_->store(false);
        WLOG_W(kTag, "scene list not read (storage queue full)");
    }
}

std::function<void(const std::string&, std::function<void(bool)>)> Shell::scene_has_data_query() {
    if (!deps_.scene_has_data || !deps_.submit_io) return {};
    return [this](const std::string& id, std::function<void(bool)> answer) {
        auto job = [has = deps_.scene_has_data, ui = &deps_.ui, alive = page_alive_, id,
                    answer = std::move(answer)]() mutable {
            const bool data = has(id);
            (void)ui->post([alive, answer = std::move(answer), data] {
                if (*alive) answer(data);
            });
        };
        if (!deps_.submit_io(std::move(job)))
            WLOG_W(kTag, "scene %s: whether it has data not asked (storage queue full)",
                   id.c_str());
    };
}

void Shell::delete_page() {
    close_choice();
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
            // A choice page goes back to the settings page to the right.
            const std::optional<Page> to =
                choice_ ? (dir == Swipe::Right ? std::optional<Page>(Page::Settings) : std::nullopt)
                        : navigate(page_, dir, press_top_zone_, at_bottom);
            if (!to || !may_drag(scr, press_target_, dir)) {
                track_->reset();
                return;
            }
            begin_drag(*to, dir, press_target_);
            drag_.indev = indev;
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
    lv_obj_t* root = choice_               ? choice_root_
                     : page_ == Page::Home ? (home_ ? home_->root() : nullptr)
                                           : sub_root_;
    if (root == nullptr || lv_obj_get_screen(root) != scr) return false;
    // The scene list's long-press card stays put on the page's screen: a
    // press that opened it goes on to the card, not into a drag.
    if (page_ == Page::Scenes && scenes_ && scenes_->popup() != nullptr) return false;
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
    if (choice_) {
        // The settings page is built already, under the choice page.
        d.from_choice = true;
        d.from_root = choice_root_;
        d.to_root = sub_root_;
    } else if (to == Page::Home) {
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
    if (target != nullptr && target != d.host) {
        lv_obj_remove_state(target, LV_STATE_PRESSED);
        d.target = target;
        lv_obj_add_event_cb(target, &Shell::target_deleted_cb, LV_EVENT_DELETE, this);
    }
    if (page_ == Page::Home && !d.from_choice) home_->drag_started();
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
    lv_anim_set_duration(&a, ui::slide_ms(left, d.extent));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, &Shell::slide_cb);
    lv_anim_set_user_data(&a, this);
    lv_anim_set_completed_cb(&a, &Shell::slide_done_cb);
    lv_anim_start(&a);
}

void Shell::target_deleted_cb(lv_event_t* e) {
    auto* self = static_cast<Shell*>(lv_event_get_user_data(e));
    self->drag_.target = nullptr;
    if (self->drag_.phase != DragPhase::Following) return;
    self->pressing_ = false;
    self->track_->reset();
    self->settle_drag(false);
}

void Shell::check_drag() {
    if (drag_.phase != DragPhase::Following || drag_.indev == nullptr) return;
    if (lv_indev_get_state(drag_.indev) != LV_INDEV_STATE_RELEASED) return;
    pressing_ = false;
    track_->reset();
    settle_drag(false);
}

void Shell::slide_cb(void* var, int32_t v) { static_cast<Shell*>(var)->place_drag(v); }

void Shell::slide_done_cb(lv_anim_t* a) {
    static_cast<Shell*>(lv_anim_get_user_data(a))->finish_drag();
}

void Shell::finish_drag() {
    if (drag_.phase == DragPhase::Idle) return;
    Drag d = std::move(drag_);
    drag_ = Drag{};
    if (d.target != nullptr)
        lv_obj_remove_event_cb_with_user_data(d.target, &Shell::target_deleted_cb, this);
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
        if (d.to != Page::Home && !d.from_choice) delete_page();
        WLOG_D(kTag, "drag to %s snapped back", to_string(d.to));
        return;
    }
    lv_screen_load(own);
    if (d.from_choice) close_choice();
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
    close_choice();
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

void Shell::open_choice(ChoiceSpec spec) {
    if (home_ == nullptr || page_ != Page::Settings || sub_ == nullptr || !settings_) return;
    abort_drag();
    close_choice();
    detail::PageFrame f = detail::make_page(deps_.shape, spec.title);
    lv_obj_add_event_cb(f.screen, &Shell::press_cb, LV_EVENT_ALL, this);
    std::function<void(size_t)> pick = std::move(spec.on_pick);
    // The page goes once the tap's event is over.
    spec.on_pick = [this, pick = std::move(pick), alive = alive_](size_t i) {
        if (pick) pick(i);
        deps_.ui.post([this, alive] {
            if (*alive) close_choice();
        });
    };
    choice_ = std::make_unique<ChoicePage>(std::move(spec));
    choice_screen_ = f.screen;
    choice_root_ = f.root;
    lv_screen_load(choice_screen_);
    choice_->build(f.list);
    pressing_ = false;
    WLOG_I(kTag, "choice open");
}

void Shell::close_choice() {
    if (choice_screen_ == nullptr) return;
    abort_drag();
    lv_obj_t* scr = choice_screen_;
    choice_screen_ = nullptr;
    choice_root_ = nullptr;
    choice_.reset();
    pressing_ = false;
    if (lv_screen_active() == scr) lv_screen_load(sub_ != nullptr ? sub_ : home_screen_);
    lv_obj_delete(scr);
    WLOG_I(kTag, "choice closed");
}

void Shell::rebuild_page() {
    lv_obj_t* old = sub_;
    if (old == nullptr) return;
    lv_obj_t* old_list = sub_root_ != nullptr ? lv_obj_get_child(sub_root_, 0) : nullptr;
    const int32_t y = old_list != nullptr ? lv_obj_get_scroll_y(old_list) : 0;
    const bool front = lv_screen_active() == old;
    sub_ = nullptr;
    drop_pages();
    sub_ = build_page(page_);
    if (front) lv_screen_load(sub_);
    lv_obj_delete(old);
    if (lv_obj_t* list = lv_obj_get_child(sub_root_, 0)) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
    }
    if (page_ == Page::Scenes) list_scenes();
}

void Shell::follow_lang() {
    const ui::Lang now = ui::lang();
    if (now == lang_ || home_ == nullptr || drag_.phase != DragPhase::Idle) return;
    lang_ = now;
    close_choice();
    home_->retext();
    rebuild_page();
    WLOG_I(kTag, "language %s", ui::lang_code(now));
}

}  // namespace wisp::shell
