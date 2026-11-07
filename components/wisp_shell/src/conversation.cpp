#include "wisp/shell/conversation.hpp"

#include <mutex>
#include <utility>

#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/button.hpp"

namespace wisp::shell {

namespace {

constexpr const char* kTag = "shell";

std::optional<voice::VoiceError> parse_error(const std::string& s) {
    using voice::VoiceError;
    for (VoiceError e :
         {VoiceError::NoAsrProfile, VoiceError::NoTtsProfile, VoiceError::Offline,
          VoiceError::AsrFailed, VoiceError::TtsFailed, VoiceError::MicUnavailable}) {
        if (s == voice::to_string(e)) return e;
    }
    return std::nullopt;
}

std::string str(const Properties& p, const char* key) {
    auto it = p.find(key);
    if (it == p.end() || it->second.type() != Value::Type::String) return {};
    return it->second.as_string();
}

}  // namespace

const char* to_string(Readiness r) {
    switch (r) {
        case Readiness::Ready:
            return "ready";
        case Readiness::NoWifi:
            return "no_wifi";
        case Readiness::NoLlm:
            return "no_llm";
    }
    return "ready";
}

ConversationController::ConversationController(ConversationDeps deps)
    : deps_(std::move(deps)),
      // The parser runs under mu_ (on_agent()), so its outputs may touch
      // the view and the turn's flags directly.
      parser_(voice::SayParser::Out{
          .say =
              [this](std::string_view d) {
                  view_.say.append(d.data(), d.size());
                  if (speaking_) {
                      deps_.voice.speak(d);
                      view_.spoken = true;
                  }
              },
          .body = [this](std::string_view d) { view_.body.append(d.data(), d.size()); },
      }) {}

ConversationController::~ConversationController() { stop(); }

void ConversationController::start() {
    if (started_) return;
    started_ = true;
    deps_.voice.set_on_transcript([this](std::string text) { on_transcript(std::move(text)); });
    agent_listener_ = deps_.agent.add_listener([this](const agent::AgentEvent& e) { on_agent(e); });
    voice_sub_ =
        deps_.bus.subscribe("voice.state", [this](const core::Event& e) { on_voice_state(e); });
}

void ConversationController::stop() {
    if (!started_) return;
    started_ = false;
    deps_.voice.set_on_transcript(nullptr);
    deps_.agent.remove_listener(agent_listener_);
    deps_.bus.unsubscribe(voice_sub_);
    agent_listener_ = 0;
    voice_sub_ = 0;
    std::lock_guard<os::Mutex> lock(mu_);
    holding_ = false;
    deferred_hold_ = false;
}

// --- triggers -----------------------------------------------------------

bool ConversationController::ready() const {
    return !deps_.readiness || deps_.readiness() == Readiness::Ready;
}

Readiness ConversationController::readiness() const {
    return deps_.readiness ? deps_.readiness() : Readiness::Ready;
}

bool ConversationController::agent_busy() const {
    if (submitted_ || turn_running_) return true;
    const agent::ConversationView v = deps_.agent.view();
    return v.running_turn.has_value() || v.queued;
}

ConversationController::Phase ConversationController::phase() const {
    if (deps_.voice.capturing()) {
        return deps_.voice.state() == voice::VoiceState::Transcribing ? Phase::Transcribing
                                                                      : Phase::Listening;
    }
    if (deps_.voice.speaking()) return Phase::Speaking;
    if (agent_busy()) return Phase::Busy;
    return Phase::Idle;
}

bool ConversationController::start_capture(voice::CaptureMode mode) {
    view_.not_heard = false;
    view_.error.reset();
    if (auto r = deps_.voice.begin_capture(mode); !r) {
        WLOG_W(kTag, "capture not started: %s", r.error().message.c_str());
        return false;
    }
    ++captures_;
    return true;
}

void ConversationController::tap() {
    // Reserved for character interaction (see conversation.hpp); no voice
    // effect.
}

void ConversationController::hold_begin() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (holding_ || !ready()) return;
        holding_ = true;
        switch (phase()) {
            case Phase::Listening:
                // Already recording: that capture becomes the hold's.
                break;
            case Phase::Transcribing:
                // Busy until the cancelled capture is over; on_voice_state()
                // starts the hold's at its idle.
                deps_.voice.cancel_capture();
                cancelled_capture_ = captures_;
                deferred_hold_ = true;
                break;
            default:
                // Ends the speech's round, or hands its locks to the new
                // capture's.
                deps_.voice.stop_speaking();
                const bool dropped = submitted_;
                if (agent_busy()) {
                    deps_.agent.stop_turn();
                    // A waiting voice text was dropped with the stop.
                    submitted_ = false;
                }
                // A failed capture did not take over the dropped text's
                // round, and no turn will come to end it.
                if (!start_capture(voice::CaptureMode::Hold) && dropped &&
                    captures_ == submitted_capture_ && !deps_.voice.capturing()) {
                    deps_.voice.end_speech();
                }
                break;
        }
    }
    notify();
}

void ConversationController::hold_end() {
    std::lock_guard<os::Mutex> lock(mu_);
    if (!holding_) return;
    holding_ = false;
    if (deferred_hold_) {
        deferred_hold_ = false;
        return;
    }
    if (deps_.voice.capturing()) deps_.voice.end_capture();
}

void ConversationController::hold_cancel() {
    std::lock_guard<os::Mutex> lock(mu_);
    if (!holding_) return;
    holding_ = false;
    deferred_hold_ = false;
    if (deps_.voice.capturing()) {
        deps_.voice.cancel_capture();
        cancelled_capture_ = captures_;
    }
}

bool ConversationController::holding() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return holding_;
}

Result<void> ConversationController::voice_text(std::string text) {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        const Readiness r = readiness();
        if (r != Readiness::Ready) return Result<void>::err(ErrorCode::Unsupported, to_string(r));
        if (phase() != Phase::Idle) return Result<void>::err(ErrorCode::Busy, "busy");
        if (auto sent = deps_.agent.submit(text, agent::Source::Voice); !sent) return sent;
        submitted_ = true;
        submitted_capture_ = captures_;
        begin_view(agent::Source::Voice, text, 0);
    }
    notify();
    return Result<void>::ok();
}

// --- callbacks ----------------------------------------------------------

void ConversationController::begin_view(agent::Source source, std::string_view heard,
                                        uint32_t turn) {
    view_ = TurnView{};
    view_.turn = turn;
    view_.source = source;
    view_.heard.assign(heard);
    view_.running = true;
}

void ConversationController::on_transcript(std::string text) {
    // wisp.voice: queues the text for wisp.agent and ends the round when
    // it cannot go; touches no flash (see the file header).
    bool end_round = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        // A newer capture has taken over this round (and its locks).
        if (deps_.voice.capturing()) return;
        if (cancelled_capture_ == captures_) {
            end_round = true;
        } else if (auto r = deps_.agent.submit(text, agent::Source::Voice); !r) {
            WLOG_W(kTag, "voice text refused: %s", r.error().message.c_str());
            end_round = true;
            begin_view(agent::Source::Voice, text, 0);
            view_.running = false;
        } else {
            submitted_ = true;
            submitted_capture_ = captures_;
            begin_view(agent::Source::Voice, text, 0);
        }
        if (end_round) deps_.voice.end_speech();
    }
    notify();
}

void ConversationController::on_agent(const agent::AgentEvent& e) {
    using Kind = agent::AgentEvent::Kind;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        switch (e.kind) {
            case Kind::TurnBegin: {
                parser_.reset();
                turn_running_ = true;
                voice_turn_ = e.source == agent::Source::Voice;
                if (voice_turn_) {
                    submitted_ = false;
                    turn_capture_ = submitted_capture_;
                }
                begin_view(e.source, e.text, e.turn);
                // Not when a newer capture has begun: its round is not this
                // turn's.
                const bool current = captures_ == turn_capture_ && !deps_.voice.capturing();
                speaking_ = voice_turn_ && current && !(deps_.tts_mute && deps_.tts_mute());
                if (speaking_) deps_.voice.begin_speech();
                break;
            }
            case Kind::Delta:
                if (!turn_running_) return;
                parser_.feed(e.text);
                break;
            case Kind::State:
                deps_.voice.set_llm_busy(e.state == agent::AgentState::Thinking ||
                                         e.state == agent::AgentState::Output);
                return;
            case Kind::TurnEnd: {
                // A stop (from the web page, say) drops every waiting
                // message, the voice text too, without telling us. A text
                // still waiting is queued at this point, so seeing it
                // neither queued nor running means it was dropped; its
                // round ends here unless a newer capture took it over.
                if (submitted_) {
                    const agent::ConversationView v = deps_.agent.view();
                    if (!v.queued && !v.running_turn) {
                        submitted_ = false;
                        if (captures_ == submitted_capture_ && !deps_.voice.capturing()) {
                            deps_.voice.end_speech();
                        }
                    }
                }
                if (!turn_running_) break;
                const std::string fallback = parser_.finish();
                if (speaking_ && !fallback.empty()) {
                    deps_.voice.speak(fallback);
                    view_.spoken = true;
                }
                deps_.voice.set_llm_busy(false);
                const bool end =
                    voice_turn_ && captures_ == turn_capture_ && !deps_.voice.capturing();
                turn_running_ = false;
                voice_turn_ = false;
                speaking_ = false;
                view_.running = false;
                // Ends the round (muted, refused, stopped or spoken alike);
                // with the speech still playing, once it has played.
                if (end) deps_.voice.end_speech();
                break;
            }
            default:
                return;
        }
    }
    notify();
}

void ConversationController::on_voice_state(const core::Event& e) {
    const std::string state = str(e.payload, "state");
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (state == "error") {
            view_.error = parse_error(str(e.payload, "reason"));
        } else if (state == "idle") {
            if (str(e.payload, "result") == "not_heard") view_.not_heard = true;
            if (deferred_hold_ && holding_ && !deps_.voice.capturing()) {
                deferred_hold_ = false;
                start_capture(voice::CaptureMode::Hold);
            }
        } else {
            return;
        }
    }
    notify();
}

// --- view and listeners -------------------------------------------------

TurnView ConversationController::current() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return view_;
}

uint32_t ConversationController::add_listener(std::function<void(const TurnView&)> cb) {
    auto l = std::make_shared<Listener>();
    l->cb = std::move(cb);
    std::lock_guard<os::Mutex> lock(lmu_);
    l->id = next_listener_++;
    listeners_.push_back(std::move(l));
    return listeners_.back()->id;
}

void ConversationController::remove_listener(uint32_t id) {
    std::shared_ptr<Listener> gone;
    {
        std::lock_guard<os::Mutex> lock(lmu_);
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if ((*it)->id == id) {
                gone = *it;
                listeners_.erase(it);
                break;
            }
        }
    }
    if (!gone) return;
    // Waits for a call in progress.
    std::lock_guard<os::Mutex> lock(gone->call);
    gone->removed = true;
}

void ConversationController::notify() {
    std::vector<std::shared_ptr<Listener>> copy;
    {
        std::lock_guard<os::Mutex> lock(lmu_);
        copy = listeners_;
    }
    for (const auto& l : copy) {
        std::lock_guard<os::Mutex> lock(l->call);
        if (l->removed) continue;
        l->cb(current());
    }
}

std::optional<std::string> find_talk_button(core::DeviceRegistry& reg) {
    for (const auto& b : reg.list<hal::Button>()) {
        const Properties& caps = b->info().caps;
        auto it = caps.find("role");
        if (it != caps.end() && it->second.type() == Value::Type::String &&
            it->second.as_string() == "talk") {
            return b->info().id;
        }
    }
    return std::nullopt;
}

}  // namespace wisp::shell
