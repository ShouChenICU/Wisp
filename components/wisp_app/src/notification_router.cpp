#include "notification_router.hpp"

#include <charconv>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "notify";
constexpr uint8_t kDefaultVolume = 60;

core::ToneKind tone_kind(core::NotifySound s) {
    return s == core::NotifySound::Alarm ? core::ToneKind::Alarm : core::ToneKind::Tone;
}

}  // namespace

NotificationRouter::NotificationRouter(NotificationRouterDeps deps) : deps_(std::move(deps)) {}

NotificationRouter::~NotificationRouter() { stop(); }

void NotificationRouter::start() {
    if (started_) return;
    started_ = true;
    deps_.notes.set_listener([this](const core::NotificationChange& c) { on_change(c); });
}

void NotificationRouter::stop() {
    if (!started_) return;
    started_ = false;
    deps_.notes.set_listener(nullptr);
    if (deps_.ui == nullptr || deps_.ui->on_ui_thread()) return;
    // The UI queue runs in order: once this job has run, none this router
    // posted is left to run after it is gone.
    auto done = std::make_shared<os::Semaphore>(1, 0);
    if (deps_.ui->post([done] { done->give(); })) done->take(os::kForever);
}

void NotificationRouter::set_banner(std::function<void(const core::Notification&)> show) {
    auto shared = show ? std::make_shared<const std::function<void(const core::Notification&)>>(
                             std::move(show))
                       : nullptr;
    std::lock_guard<os::Mutex> lock(mu_);
    banner_ = std::move(shared);
}

uint8_t NotificationRouter::volume() const {
    return deps_.notify_volume ? deps_.notify_volume() : kDefaultVolume;
}

void NotificationRouter::on_change(const core::NotificationChange& c) {
    if (c.kind == core::NotificationChange::Kind::Posted) return posted(c.n);
    bool sounding = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        sounding = c.n.source == sounding_source_ && c.n.id == sounding_id_;
    }
    if (sounding && deps_.tones != nullptr) deps_.tones->silence();
}

void NotificationRouter::posted(const core::Notification& n) {
    const bool off = deps_.power != nullptr && deps_.power->standby();
    // An alarm rings until the user touches the device (spec 5.1), which
    // they can only do with the screen on, so it wakes it as wake = true
    // would.
    const bool wake = off && (n.wake || n.sound == core::NotifySound::Alarm);
    // Audio is off with the screen (hal/audio.hpp): no sound for a
    // notification that leaves it off (a tone without wake).
    const bool sound =
        n.sound != core::NotifySound::None && deps_.tones != nullptr && !(off && !wake);
    if (sound) {
        std::lock_guard<os::Mutex> lock(mu_);
        sounding_source_ = n.source;
        sounding_id_ = n.id;
    }
    const core::ToneKind kind = tone_kind(n.sound);
    const uint8_t vol = sound ? volume() : 0;
    if (wake) {
        // The sound follows the wake on the same task, so it starts once
        // the output is powered again.
        PowerManager* power = deps_.power;
        core::TonePlayer* tones = sound ? deps_.tones : nullptr;
        auto job = [power, tones, kind, vol] {
            power->exit_standby();
            if (tones != nullptr) tones->play(kind, vol);
        };
        if (deps_.ui == nullptr) {
            job();
        } else if (!deps_.ui->post(std::move(job))) {
            WLOG_W(kTag, "%s/%s: screen not woken: UI queue full", n.source.c_str(), n.id.c_str());
        }
    } else if (sound) {
        deps_.tones->play(kind, vol);
    }
    std::shared_ptr<const std::function<void(const core::Notification&)>> banner;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        banner = banner_;
    }
    if (banner) (*banner)(n);
}

void NotificationRouter::tap(const std::string& source, const std::string& id) {
    // Read before the dismiss removes it.
    bool agent_done = false;
    if (source == core::kSystemSource) {
        auto n = deps_.notes.find(source, id);
        agent_done = n && n->code == core::notice::kAgentDone;
    }
    deps_.notes.dismiss(source, id, core::DismissReason::Tap);
    if (deps_.tones != nullptr) deps_.tones->silence();
    if (agent_done && deps_.open_home) deps_.open_home();
    if (source == core::kSystemSource || source == core::kReminderSource ||
        deps_.scenes == nullptr) {
        return;
    }
    if (auto r = deps_.scenes->deliver_notification(source, id); !r)
        WLOG_W(kTag, "%s/%s: not delivered: %s", source.c_str(), id.c_str(),
               r.error().message.c_str());
}

void NotificationRouter::act(const std::string& source, const std::string& id, int index) {
    auto n = deps_.notes.find(source, id);
    if (!n) return;
    if (source == core::kReminderSource) return act_reminder(*n, index);
    if (index != 0 || n->action != core::notice::kActionReleaseLock) return;
    uint32_t lock = 0;
    const char* first = n->action_arg.data();
    const char* last = first + n->action_arg.size();
    auto [end, ec] = std::from_chars(first, last, lock);
    if (ec != std::errc() || end != last || lock == 0) {
        WLOG_W(kTag, "%s/%s: bad lock id \"%s\"", source.c_str(), id.c_str(),
               n->action_arg.c_str());
    } else if (deps_.locks != nullptr && !deps_.locks->release_if(lock, n->arg)) {
        // Released meanwhile, and the id possibly reused by another holder:
        // the notice is out of date either way.
        WLOG_I(kTag, "%s/%s: lock %u no longer held by %s", source.c_str(), id.c_str(),
               static_cast<unsigned>(lock), n->arg.c_str());
    }
    deps_.notes.dismiss(source, id, core::DismissReason::Dismiss);
}

void NotificationRouter::act_reminder(const core::Notification& n, int index) {
    const std::string& action = index == 0 ? n.action : n.action2;
    const std::string& arg = index == 0 ? n.action_arg : n.action2_arg;
    const bool is_done = action == core::notice::kActionTodoDone;
    if (!is_done && action != core::notice::kActionTodoSnooze) return;
    uint32_t todo = 0;
    const char* first = arg.data();
    const char* last = first + arg.size();
    auto [end, ec] = std::from_chars(first, last, todo);
    if (deps_.tones != nullptr) deps_.tones->silence();
    deps_.notes.dismiss(n.source, n.id, core::DismissReason::Dismiss);
    if (ec != std::errc() || end != last || todo == 0) {
        WLOG_W(kTag, "%s/%s: bad todo id", n.source.c_str(), n.id.c_str());
        return;
    }
    core::ReminderScheduler* reminders = deps_.reminders;
    if (reminders == nullptr || !deps_.submit_io) return;
    const bool queued = deps_.submit_io([reminders, todo, is_done] {
        Result<void> r = is_done ? reminders->done(todo) : reminders->snooze(todo);
        if (!r) {
            WLOG_W(kTag, "todo %u: %s failed: %s", static_cast<unsigned>(todo),
                   is_done ? "done" : "snooze", r.error().message.c_str());
        }
    });
    if (!queued) WLOG_W(kTag, "todo %u: storage queue full", static_cast<unsigned>(todo));
}

}  // namespace wisp::app
