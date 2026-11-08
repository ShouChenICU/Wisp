#include "status_shell.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/minute_ticker.hpp"
#include "wisp/ui/clock_text.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_notice.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "status";
constexpr int kDefaultUtcOffsetMin = 480;

// The first UTF-8 character of `s`.
std::string first_char(std::string_view s) {
    if (s.empty()) return {};
    const auto lead = static_cast<unsigned char>(s[0]);
    size_t n = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    return std::string(s.substr(0, std::min(n, s.size())));
}

std::string wifi_text(const std::string& state) {
    if (state == "connected") return ui::tr(ui::StrId::NetConnected);
    if (state == "connecting" || state == "backoff") return ui::tr(ui::StrId::NetConnecting);
    return ui::tr(ui::StrId::NetOff);
}

}  // namespace

struct StatusShell::State : std::enable_shared_from_this<StatusShell::State> {
    explicit State(StatusShellDeps d) : deps(std::move(d)) {}

    StatusShellDeps deps;
    // Set by stop(): a job or a tap that runs after it does nothing.
    std::atomic<bool> stopped{false};
    // A refresh is posted and has not run yet.
    std::atomic<bool> refresh_queued{false};
    // UI thread only: the counts last shown.
    ui::StatusCounts shown{-1, -1};
    // UI thread only: names of scenes read from their manifests ("" for
    // one not installed: it reads as its id), and the reads on their way.
    std::map<std::string, std::string, std::less<>> names;
    std::set<std::string, std::less<>> reading;

    std::vector<scene::BackgroundScene> background() const {
        return deps.scenes != nullptr ? deps.scenes->background_scenes()
                                      : std::vector<scene::BackgroundScene>{};
    }

    // The name of scene `id`: the runtime's while it runs, else its
    // manifest's, read once on the storage worker (a manifest read touches
    // flash, which the UI thread must not) and kept; a refresh follows the
    // read. "" until then, and for a scene that is not installed. UI
    // thread.
    std::string scene_name(const std::string& id) {
        std::string name = deps.scenes != nullptr ? deps.scenes->running_name(id) : std::string();
        if (!name.empty()) {
            names[id] = name;
            return name;
        }
        if (auto it = names.find(id); it != names.end()) return it->second;
        read_name(id);
        return {};
    }

    void read_name(const std::string& id) {
        if (deps.repo == nullptr || !deps.submit_io || reading.count(id) != 0) return;
        reading.insert(id);
        std::shared_ptr<State> self = shared_from_this();
        const bool queued = deps.submit_io([self, id] {
            if (self->stopped.load()) return;
            auto info = self->deps.repo->info(id);
            std::string name = info ? info.value().manifest.name : std::string();
            if (!info && info.error().code != ErrorCode::NotFound) {
                WLOG_W(kTag, "%s: name not read: %s", id.c_str(), info.error().message.c_str());
            }
            const bool posted = self->deps.ui.post([self, id, name = std::move(name)] {
                self->reading.erase(id);
                if (self->stopped.load()) return;
                self->names[id] = name;
                self->refresh();
            });
            if (!posted) WLOG_W(kTag, "%s: name not shown: UI queue full", id.c_str());
        });
        if (!queued) {
            // Read again on the next refresh.
            reading.erase(id);
        }
    }

    // What a system notice's argument reads as: a wake-lock holder by its
    // label (a scene holder by its scene's name, its id while that is
    // unknown); every other code's argument as posted (a scene notice
    // carries the scene's name already).
    std::string notice_arg(std::string_view code, std::string_view arg) {
        if (code != core::notice::kWakeLockLong) return std::string(arg);
        constexpr std::string_view kScene = "scene.";
        std::string name;
        if (arg.substr(0, kScene.size()) == kScene) {
            name = scene_name(std::string(arg.substr(kScene.size())));
        }
        return ui::holder_label(arg, name);
    }

    // `N` is core::Notification (a banner) or core::StoredNotification (the
    // panel's rows, in PSRAM).
    template <class N>
    ui::PanelNote note_of(const N& n) {
        using SV = std::string_view;
        ui::PanelNote p;
        p.source = SV(n.source);
        p.id = SV(n.id);
        if (SV(n.source) == core::kSystemSource) {
            const ui::NoticeText t =
                ui::system_notice_text(SV(n.code), notice_arg(SV(n.code), SV(n.arg)));
            p.title = SV(t.title);
            p.body = SV(t.body);
        } else {
            // A reminder is the system's: the bell, not a scene's letter.
            if (SV(n.source) != core::kReminderSource) {
                p.badge = SV(first_char(n.source_name.empty() ? SV(n.source) : SV(n.source_name)));
            }
            p.title = SV(n.title);
            p.body = SV(n.body);
        }
        p.action_label = SV(action_label(SV(n.action)));
        p.action2_label = SV(action_label(SV(n.action2)));
        return p;
    }

    static const char* action_label(std::string_view action) {
        if (action == core::notice::kActionReleaseLock) return ui::tr(ui::StrId::NoticeRelease);
        if (action == core::notice::kActionTodoDone) return ui::tr(ui::StrId::NoteActionTodoDone);
        if (action == core::notice::kActionTodoSnooze) {
            return ui::tr(ui::StrId::NoteActionTodoSnooze);
        }
        return "";
    }

    ui::BannerInfo banner_of(const core::Notification& n) {
        const ui::PanelNote p = note_of(n);
        ui::BannerInfo b;
        b.source = n.source;
        b.id = n.id;
        b.badge = std::string_view(p.badge);
        if (n.source != core::kSystemSource && n.source != core::kReminderSource)
            b.name = n.source_name.empty() ? n.source : n.source_name;
        b.title = std::string_view(p.title);
        b.body = std::string_view(p.body);
        // Only actions the banner can answer: a wake-lock notice's release
        // stays in the panel, as it always has.
        if (n.source == core::kReminderSource) {
            b.action_label = std::string_view(p.action_label);
            b.action2_label = std::string_view(p.action2_label);
        }
        return b;
    }

    ui::PanelHeader header() const {
        ui::PanelHeader h;
        const int64_t t = deps.wall_s ? deps.wall_s() : 0;
        if (t >= scene::MinuteTicker::kValidSince) {
            const int offset = deps.utc_offset_min ? deps.utc_offset_min() : kDefaultUtcOffsetMin;
            const bool h24 = deps.time_24h ? deps.time_24h() : true;
            h.time = std::string_view(ui::format_local_clock(t, offset, h24, ui::lang()));
        } else {
            h.time = "--:--";
        }
        if (!deps.device_status) return h;
        const Value status = deps.device_status();
        if (const Value* b = status.find("battery"); b != nullptr) {
            if (const Value* p = b->find("percent");
                p != nullptr && p->type() == Value::Type::Int && p->as_int() >= 0) {
                h.battery = static_cast<int>(p->as_int());
            }
            if (const Value* c = b->find("charging");
                c != nullptr && c->type() == Value::Type::Bool) {
                h.charging = c->as_bool();
            }
        }
        if (const Value* w = status.find("wifi");
            w != nullptr && w->type() == Value::Type::String) {
            h.wifi = std::string_view(wifi_text(w->as_string()));
        }
        return h;
    }

    ui::PanelModel model() {
        ui::PanelModel m;
        m.header = header();
        m.now_ms = os::now_ms();
        for (const auto& b : background()) {
            m.scenes.push_back({os::LargeString(std::string_view(b.id)),
                                os::LargeString(std::string_view(b.name)), b.since_ms});
        }
        const auto unread = deps.notes.unread();
        m.notes.reserve(unread.size());
        for (const core::StoredNotification& n : unread) m.notes.push_back(note_of(n));
        return m;
    }

    // UI thread.
    void refresh() {
        const ui::StatusCounts c{static_cast<int>(background().size()),
                                 static_cast<int>(deps.notes.unread_count())};
        if (c.background != shown.background || c.unread != shown.unread) {
            WLOG_I(kTag, "status: background=%d unread=%d", c.background, c.unread);
            shown = c;
        }
        deps.layer.set_status(c);
        if (deps.layer.panel_open()) deps.layer.update_panel(model());
    }
};

namespace {

using StatePtr = std::shared_ptr<StatusShell::State>;

// Runs `fn` on the UI thread unless the shell has stopped by then.
bool post_ui(const StatePtr& s, std::function<void(const StatePtr&)> fn) {
    return s->deps.ui.post([s, fn = std::move(fn)] {
        if (!s->stopped.load()) fn(s);
    });
}

// One refresh at a time: a burst of events coalesces into the one queued.
void request_refresh(const StatePtr& s) {
    if (s->refresh_queued.exchange(true)) return;
    if (!post_ui(s, [](const StatePtr& st) {
            st->refresh_queued.store(false);
            st->refresh();
        })) {
        // The queue is full: the next event tries again.
        s->refresh_queued.store(false);
        WLOG_W(kTag, "status not refreshed: UI queue full");
    }
}

}  // namespace

StatusShell::StatusShell(StatusShellDeps deps)
    : state_(std::allocate_shared<State>(os::LargeAllocator<State>{}, std::move(deps))) {}

StatusShell::~StatusShell() { stop(); }

ui::PanelModel StatusShell::panel_model() const { return state_->model(); }

void StatusShell::start() {
    if (started_) return;
    started_ = true;
    State& st = *state_;
    st.stopped.store(false);
    StatePtr s = state_;
    auto refresh = [s](const core::Event&) { request_refresh(s); };
    subs_[0] = st.deps.bus.subscribe("scene.state", refresh);
    subs_[1] = st.deps.bus.subscribe("notification.", refresh);
    subs_[2] = st.deps.bus.subscribe("power.state", [s](const core::Event& e) {
        auto it = e.payload.find("state");
        const bool off =
            it != e.payload.end() && it->second.type() == Value::Type::String &&
            (it->second.as_string() == "standby" || it->second.as_string() == "deep_standby");
        if (off) {
            post_ui(s, [](const StatePtr& st) { st->deps.layer.close_panel(); });
        } else {
            request_refresh(s);
        }
    });

    // Called on the posting thread (the scene task for wisp.notify), so
    // the text is made on the UI thread, which may ask the runtime for
    // scene names. The banner keeps its tap until the layer goes, which
    // may be after stop(): the tap then does nothing.
    st.deps.router.set_banner([s](const core::Notification& n) {
        if (!post_ui(s, [n](const StatePtr& st) {
                st->deps.layer.show_banner(
                    st->banner_of(n),
                    [st, source = n.source, id = n.id] {
                        if (!st->stopped.load()) st->deps.router.tap(source, id);
                    },
                    [st, source = n.source, id = n.id](int index) {
                        if (!st->stopped.load()) st->deps.router.act(source, id, index);
                    });
            })) {
            WLOG_W(kTag, "%s/%s: no banner: UI queue full", n.source.c_str(), n.id.c_str());
        }
    });

    // The handlers are removed by stop() before the state can go, so they
    // hold it by reference.
    if (!post_ui(s, [](const StatePtr& sp) {
            State& st = *sp;
            ui::PanelHandlers h;
            h.on_open = [&st] {
                WLOG_I(kTag, "panel: open");
                st.deps.layer.update_panel(st.model());
            };
            h.on_close = [] { WLOG_I(kTag, "panel: closed"); };
            if (st.deps.scenes != nullptr) {
                scene::SceneRuntime* rt = st.deps.scenes;
                h.open_scene = [rt](const std::string& id) {
                    if (auto r = rt->launch(id, scene::LaunchReason::User); !r)
                        WLOG_W(kTag, "%s: not opened: %s", id.c_str(), r.error().message.c_str());
                };
                h.exit_scene = [rt](const std::string& id) {
                    if (auto r = rt->exit(id); !r)
                        WLOG_W(kTag, "%s: not exited: %s", id.c_str(), r.error().message.c_str());
                };
            }
            NotificationRouter* router = &st.deps.router;
            core::NotificationService* notes = &st.deps.notes;
            h.tap_note = [router](const std::string& source, const std::string& id) {
                router->tap(source, id);
            };
            h.dismiss_note = [notes](const std::string& source, const std::string& id) {
                notes->dismiss(source, id, core::DismissReason::Dismiss);
            };
            h.note_action = [router](const std::string& source, const std::string& id, int index) {
                router->act(source, id, index);
            };
            h.clear_all = [notes] { notes->clear_all(); };
            st.deps.layer.set_panel_handlers(std::move(h));
            st.refresh();
        })) {
        WLOG_E(kTag, "UI queue full; no status capsule or panel");
    }
}

void StatusShell::stop() {
    if (!started_) return;
    started_ = false;
    State& st = *state_;
    for (core::SubscriptionId& id : subs_) {
        st.deps.bus.unsubscribe(id);
        id = 0;
    }
    st.deps.router.set_banner(nullptr);
    // From here on nothing posted runs and no banner tap reaches the
    // router; a banner call still in flight posts a job that does nothing.
    st.stopped.store(true);
    StatePtr s = state_;
    auto uninstall = [s] {
        s->deps.layer.set_panel_handlers({});
        s->refresh_queued.store(false);
    };
    if (st.deps.ui.on_ui_thread()) {
        uninstall();
    } else {
        // The UI queue runs in order: once this job has run, none posted
        // before it is left.
        st.deps.ui.run_blocking(uninstall);
    }
}

}  // namespace wisp::app
