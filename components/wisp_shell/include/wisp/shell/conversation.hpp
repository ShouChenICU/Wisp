#pragma once

// ConversationController (MVP spec 1.5, 4, 6.2, 6.3): joins the voice
// chain to the agent and keeps what the screen shows of the current turn.
//
// A voice turn: a capture (tap or hold) ends in a transcript, which goes
// to agent.submit(text, Source::Voice). At the turn's TurnBegin the
// SayParser is reset and, for a voice turn with tts_mute() false, the
// speech opens (voice.begin_speech()); each Delta goes through the
// parser, its say part both into the view and to voice.speak(), and the
// say's "</say>" to voice.say_closed() (so the rest of the say is
// synthesized while the model writes the body); every State sets
// voice.set_llm_busy() (thinking or output: the LLM streams). A turn not
// spoken (muted, or its speech stopped by a hold) reports no closed say.
// At TurnEnd the parser finishes, its fallback sentence (no <say> this
// turn) is spoken, and the speech ends (voice.end_speech()). Turns from
// the web page update the view the same way and are never spoken.
//
// Voice rounds (wisp/voice/voice_service.hpp): a round that delivered a
// transcript stays open, wake locks held, until end_speech() or
// stop_speaking(). Every such round is ended here: at its turn's
// TurnEnd (done, error, stopped; muted or not), at once when submit()
// refuses the text or the hold was cancelled, and by stop_speaking() when
// a hold or stop_turn() interrupts the speech. A round needs no ending
// only when a newer capture has begun since: begin_capture() hands the
// locks to the new round, and an end_speech() then would end that one
// instead.
//
// Presses. A hold is only told from a tap kHoldStartMs after the press,
// so the press itself primes the mic (voice.prime(), see
// wisp/voice/voice_service.hpp): press_begin() at touch-down or key-down
// primes when ready and no capture runs. press_end() comes once the press
// can no longer become a hold: it ended early, moved away, was taken by a
// drag, the screen went dark, the input stopped -- or hold_begin() was
// just called for it. Unless that hold_begin() started a hold (whose
// capture has taken the primed mic), it unprimes, so the mic closes at once
// instead of at kPrimeMaxMs. Every press_begin() is followed by one
// press_end().
//
// Triggers (spec 6.3). tap() is reserved for character interaction (e.g.
// patting the wisp's head) and has no voice effect: it starts no capture,
// ends none, and does not stop speech. hold_begin(): stops the speech and
// starts a hold capture; a capture already listening is kept as the
// hold's, and one already transcribing is cancelled and the hold capture
// starts once it has ended. A running turn (a scene build from the web
// page, say) runs on, unspoken, while the hold records. hold_end() sends
// the recording, hold_cancel() drops it. The hold's transcript replaces
// the turn: a turn running or a message waiting is stopped (it ends
// "stopped") just before the text is submitted, and until the text's own
// TurnBegin the view is the text's (running, no outcome), whatever the
// stopped turn still reports. A transcript that comes in the instant
// after the agent took a message up and before it marked the turn
// running sees nothing to stop, and waits behind that turn instead. A
// cancelled hold, or one that heard nothing, stops nothing. stop_turn()
// (the screen's Stop) stops the speech, the running turn and a waiting
// voice text. Unless readiness().can_talk() neither press_begin() nor
// hold_begin()/hold_end()/hold_cancel() records or sends anything, and
// voice_text() also needs the network up.
//
// Threads. The triggers, voice_text(), readiness(), current() and the
// listener calls may come from any thread; the triggers and the
// callbacks below are serialized by one mutex. Nothing here blocks: the
// voice and agent calls made under it only queue work. The transcript
// arrives on wisp.voice (flash_access = false): agent.submit() only
// queues the text for wisp.agent, which writes the conversation through
// the storage worker, so the callback touches no flash.
//
// The agent's steps (TurnView::steps, mvp spec 6.2). ToolPrep starts a
// running step for a call the model is still writing, or updates it
// (activity_prep_label(): its size so far); a ToolPrep for another tool,
// or with fewer bytes than the last, is the reply's next call. ToolStart
// turns the oldest such step into the real call (activity_label(), with
// its target), or starts a new one; ToolEnd marks it Done or Failed. A
// Thinking state once the turn has had a step sets `thinking` until the
// next step or output. Only the last kTurnSteps steps are kept.
//
// Listeners are called after every change with a copy of the view as it
// is then, on the thread that made the change: wisp.agent (agent events),
// wisp.voice (a transcript), the event bus's dispatch thread (voice.state:
// errors, "not heard") or the caller of a trigger or voice_text(). The
// last call after a change always shows it. A listener must not block or
// call back into this controller other than current(); the shell's
// screens post to the UI thread from it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/voice/say_parser.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::shell {

// What talking needs (mvp spec 6.6). Net: Up online; Connecting offline
// while a connection is being made or will be
// (net::WifiService::connection_expected(): a hold records and its
// recognition waits, see
// wisp/voice/voice_service.hpp); Down offline otherwise. llm / asr / tts:
// a profile of that kind is active (tts also when the speech is muted).
// A hold needs can_talk(); the home screen lists what complete() lacks.
struct Readiness {
    enum class Net : uint8_t { Up, Connecting, Down };
    Net net = Net::Up;
    bool llm = true, asr = true, tts = true;
    bool can_talk() const { return net != Net::Down && llm && asr; }
    bool complete() const { return net == Net::Up && llm && asr && tts; }
    bool operator==(const Readiness&) const = default;
};

// One step of the agent's turn as the screen shows it (activity.hpp).
struct TurnStep {
    enum class State : uint8_t { Running, Done, Failed };
    os::LargeString label;
    State st = State::Running;
};

// The steps a TurnView keeps: the last ones.
inline constexpr size_t kTurnSteps = 4;

struct TurnView {
    uint32_t turn = 0;  // 0 until the agent takes the text up
    agent::Source source = agent::Source::Web;
    std::string heard;                       // the user's text
    os::LargeString say, body;               // tags removed
    bool running = false;                    // submitted .. TurnEnd
    bool spoken = false;                     // some of it went to the speech
    bool not_heard = false;                  // the last capture heard nothing
    std::optional<voice::VoiceError> error;  // the last voice error
    // The agent's steps: the last kTurnSteps (oldest first, step_count of
    // them), how many the turn has had, and when it began and ended
    // (ConversationDeps::now_ms; ended_ms 0 while it runs). `thinking`:
    // the model works on its next step after a tool (shown as a passing
    // "思考中" step, never kept).
    std::array<TurnStep, kTurnSteps> steps{};
    uint8_t step_count = 0;
    uint16_t steps_total = 0;
    uint64_t started_ms = 0;
    uint64_t ended_ms = 0;
    bool thinking = false;
    // Set at TurnEnd: AgentEvent::reason and AgentEvent::note ("" for a
    // turn that went well). Home and the talk overlay show only the short
    // per-reason words for end_reason (outcome_text.hpp): the full note
    // does not fit the small round screens. end_note keeps the agent's
    // full note for a screen with room for it (the conversation has it
    // too).
    std::string end_reason;
    os::LargeString end_note;

    // The step under way, if the last one still runs.
    const TurnStep* running_step() const {
        if (step_count == 0 || steps[step_count - 1].st != TurnStep::State::Running) return nullptr;
        return &steps[step_count - 1];
    }
};

struct ConversationDeps {
    core::EventBus& bus;
    agent::AgentService& agent;
    voice::VoiceService& voice;
    std::function<bool()> tts_mute;        // cached voice/tts_mute; empty: false
    std::function<Readiness()> readiness;  // empty: complete
    // TurnView's started_ms/ended_ms; empty: os::now_ms (the screens'
    // clock).
    std::function<uint64_t()> now_ms;
};

class ConversationController {
public:
    explicit ConversationController(ConversationDeps deps);
    ~ConversationController();  // stop()

    ConversationController(const ConversationController&) = delete;
    ConversationController& operator=(const ConversationController&) = delete;

    // Takes the voice's transcripts and listens to the agent and to
    // voice.state. Call once both services exist; stop() before either
    // goes. Idempotent.
    void start();
    void stop();

    // A press (talk key or wisp) went down / ended without a hold (see
    // the file header).
    void press_begin();
    void press_end();
    void hold_begin();
    void hold_end();
    void hold_cancel();
    // No voice effect (see the file header); kept so the shell UI can
    // route taps here for character interaction, designed later.
    void tap();
    // Stops the speech (not a capture's round), the agent's running turn
    // and a waiting voice text; the view ends "stopped" with the turn's
    // TurnEnd. Nothing to stop: nothing happens.
    void stop_turn();
    // Between hold_begin() and hold_end()/hold_cancel() (also when
    // hold_begin() did nothing because nothing was ready: then false).
    bool holding() const;

    // A transcript without recording, as Source::Voice (spoken unless
    // muted). Unsupported "no_wifi" unless the network is up, "no_llm"
    // without an llm profile; Busy "busy"
    // while capturing, speaking or a turn runs; submit()'s own errors.
    Result<void> voice_text(std::string text);

    Readiness readiness() const;
    TurnView current() const;

    uint32_t add_listener(std::function<void(const TurnView&)> cb);
    // Once it returns, `id`'s callback is not running and never runs
    // again (not from inside a listener).
    void remove_listener(uint32_t id);

private:
    enum class Phase : uint8_t { Idle, Listening, Transcribing, Speaking, Busy };

    struct Listener {
        uint32_t id = 0;
        std::function<void(const TurnView&)> cb;
        os::Mutex call;        // held while cb runs
        bool removed = false;  // under call
    };

    // mu_ held.
    Phase phase() const;
    bool agent_busy() const;
    bool ready() const;
    // False when the voice refused the capture.
    bool start_capture(voice::CaptureMode mode);
    void begin_view(agent::Source source, std::string_view heard, uint32_t turn);
    uint64_t now() const;
    // Step `n` (from 1, of the whole turn), if it is still kept.
    TurnStep* step_no(uint32_t n);
    // Appends a running step (dropping the oldest when full); its number.
    uint32_t push_step(os::LargeString label);
    void on_tool_prep(const agent::AgentEvent& e);
    void on_tool_start(const agent::AgentEvent& e);
    // Drops the steps of calls written whose ToolStart never came (the
    // agent skipped them or the reply failed) and resets the mapping;
    // whether there were any.
    bool drop_unstarted();

    void on_agent(const agent::AgentEvent& e);
    void on_transcript(std::string text);
    void on_voice_state(const core::Event& e);
    void notify();

    ConversationDeps deps_;
    bool started_ = false;  // start()/stop() only
    uint32_t agent_listener_ = 0;
    core::SubscriptionId voice_sub_ = 0;

    mutable os::Mutex mu_;
    TurnView view_;
    voice::SayParser parser_;
    bool holding_ = false;
    // hold_begin() started a hold since the last press_begin() whose
    // capture took (or, deferred, will take) the primed mic; not when the
    // voice refused the capture.
    bool press_held_ = false;
    // A hold waits for the capture it cancelled to end before its own.
    bool deferred_hold_ = false;
    // Successful begin_capture() calls; the transcript always belongs to
    // the latest one.
    uint32_t captures_ = 0;
    // The capture whose recording was dropped (hold_cancel(), or a hold
    // over a transcription).
    uint32_t cancelled_capture_ = 0;
    // A voice text was submitted and its TurnBegin has not come yet.
    bool submitted_ = false;
    uint32_t submitted_capture_ = 0;
    // TurnBegin .. TurnEnd: any turn; the voice turn's capture; whether
    // its reply is spoken.
    bool turn_running_ = false;
    bool voice_turn_ = false;
    uint32_t turn_capture_ = 0;
    bool speaking_ = false;
    // The turn's steps: the first and last calls being written whose
    // ToolStart has not come yet (0: none), the last one's tool and
    // bytes, and the step whose tool runs (0: none).
    uint32_t prep_first_ = 0;
    uint32_t prep_last_ = 0;
    os::LargeString prep_tool_;
    uint32_t prep_bytes_ = 0;
    uint32_t tool_step_ = 0;

    os::Mutex lmu_;
    std::vector<std::shared_ptr<Listener>> listeners_;
    uint32_t next_listener_ = 1;
};

// The id of the button with role talk (wisp/hal/roles.hpp), if any.
std::optional<std::string> find_talk_button(core::DeviceRegistry& reg);

}  // namespace wisp::shell
