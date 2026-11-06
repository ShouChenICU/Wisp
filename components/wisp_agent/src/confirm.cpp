#include "wisp/agent/confirm.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kTag = "confirm";
// How long a close is retried while the UI queue is full.
constexpr int kCloseTries = 50;

// One question, shared by ask() and the jobs it posts to the UI thread
// (which may run after ask() has returned).
struct Dialog {
    std::atomic<int> answer{-1};  // -1 none yet, 0 no, 1 yes
    std::atomic<bool> open_queued{false};
    std::atomic<bool> shown{false};
    // Set before ask() returns; a job still queued then opens nothing.
    std::atomic<bool> over{false};
    // UI thread only.
    lv_obj_t* box = nullptr;
    bool woke = false;
};

}  // namespace

const char* to_string(Answer a) {
    switch (a) {
        case Answer::Allowed:
            return "allowed";
        case Answer::Denied:
            return "denied";
        case Answer::TimedOut:
            return "timed_out";
        case Answer::Cancelled:
            return "cancelled";
    }
    return "denied";
}

ConfirmService::ConfirmService(ConfirmDeps deps) : deps_(std::move(deps)) {}

std::optional<std::string> ConfirmService::pending() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return pending_;
}

void ConfirmService::set_listener(std::function<void(const std::optional<std::string>&)> cb) {
    std::lock_guard<os::Mutex> lock(mu_);
    listener_ = std::move(cb);
}

void ConfirmService::announce(const std::optional<std::string>& text) {
    std::function<void(const std::optional<std::string>&)> cb;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        pending_ = text;
        if (!text) busy_ = false;
        cb = listener_;
    }
    if (cb) cb(text);
}

Answer ConfirmService::ask(const std::string& text, const std::function<bool()>& cancelled) {
    auto is_cancelled = [&] { return cancelled && cancelled(); };
    auto now = [this] { return deps_.now_ms ? deps_.now_ms() : os::now_ms(); };

    // One request at a time: wait for the slot.
    for (;;) {
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (!busy_) {
                busy_ = true;
                break;
            }
        }
        if (is_cancelled()) return Answer::Cancelled;
        os::sleep_ms(kPollMs);
    }
    announce(text);

    auto d = std::make_shared<Dialog>();
    ui::SystemLayer* layer = &deps_.layer;
    // Each attempt runs on the UI thread: wake the screen the first time,
    // then open the dialog unless another one holds the screen.
    auto open = [d, layer, wake = deps_.wake, text] {
        if (!d->over.load()) {
            if (!d->woke) {
                d->woke = true;
                if (wake) wake();
            }
            if (!layer->confirm_open()) {
                d->box = layer->open_confirm(
                    ui::StrId::AgentConfirmTitle, text,
                    [d](bool yes) {
                        // The layer has closed the box already.
                        d->box = nullptr;
                        d->answer = yes ? 1 : 0;
                    },
                    ui::ButtonRole::Danger, ui::StrId::AgentAllow, ui::StrId::AgentDeny);
                if (d->box != nullptr) d->shown = true;
            }
        }
        d->open_queued = false;
    };

    const uint64_t start = now();
    Answer result = Answer::TimedOut;
    for (;;) {
        const int a = d->answer.load();
        if (a >= 0) {
            result = a == 1 ? Answer::Allowed : Answer::Denied;
            break;
        }
        if (is_cancelled()) {
            result = Answer::Cancelled;
            break;
        }
        if (now() - start >= deps_.timeout_ms) break;
        if (!d->shown.load() && !d->open_queued.load()) {
            d->open_queued = true;
            if (!deps_.ui.post(open)) d->open_queued = false;
        }
        os::sleep_ms(kPollMs);
    }
    d->over = true;

    if (result == Answer::TimedOut || result == Answer::Cancelled) {
        // Queued after any open attempt, so it sees the box that attempt
        // made. An answer that lands first has cleared `box`.
        auto close = [d, layer] {
            if (d->box != nullptr) layer->close_confirm(d->box);
            d->box = nullptr;
        };
        int tries = 0;
        while (!deps_.ui.post(close) && ++tries < kCloseTries) os::sleep_ms(kPollMs);
        if (tries == kCloseTries) WLOG_W(kTag, "could not close the dialog: UI queue full");
    }
    WLOG_I(kTag, "%s", to_string(result));
    announce(std::nullopt);
    return result;
}

}  // namespace wisp::agent
