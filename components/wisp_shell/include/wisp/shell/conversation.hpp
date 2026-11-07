#pragma once

// ConversationController (MVP spec 1.5, 4, 6.2, 6.3): joins the voice
// chain to the agent and keeps what the screen shows of the current turn.
//
// A voice turn: a capture (tap or hold) ends in a transcript, which goes
// to agent.submit(text, Source::Voice). At the turn's TurnBegin the
// SayParser is reset and, for a voice turn with tts_mute() false, the
// speech opens (voice.begin_speech()); each Delta goes through the
// parser, its say part both into the view and to voice.speak(); every
// State sets voice.set_llm_busy() (thinking or output: the LLM streams).
// At TurnEnd the parser finishes, its fallback sentence (no <say> this
// turn) is spoken, and the speech ends (voice.end_speech()). Turns from
// the web page update the view the same way and are never spoken.
//
// Voice rounds (wisp/voice/voice_service.hpp): a round that delivered a
// transcript stays open, wake locks held, until end_speech() or
// stop_speaking(). Every such round is ended here: at its turn's
// TurnEnd (done, error, stopped; muted or not), at once when submit()
// refuses the text or the hold was cancelled, and by stop_speaking() when
// a tap or a hold interrupts the speech. A round needs no ending only when
// a newer capture has begun since: begin_capture() hands the locks to the
// new round, and an end_speech() then would end that one instead.
//
// Triggers (spec 6.3). tap() is reserved for character interaction (e.g.
// patting the wisp's head) and has no voice effect: it starts no capture,
// ends none, and does not stop speech. hold_begin(): in any state stops
// the speech and the running turn (it ends "stopped") and starts a hold
// capture; a capture already listening is kept as the hold's, and one
// already transcribing is cancelled and the hold capture starts once it
// has ended. hold_end() sends the recording, hold_cancel() drops it.
// While readiness() is not Ready neither hold_begin()/hold_end()/
// hold_cancel() nor voice_text() sends anything.
//
// Threads. The triggers, voice_text(), readiness(), current() and the
// listener calls may come from any thread; the triggers and the
// callbacks below are serialized by one mutex. Nothing here blocks: the
// voice and agent calls made under it only queue work. The transcript
// arrives on wisp.voice (flash_access = false): agent.submit() only
// queues the text for wisp.agent, which writes the conversation through
// the storage worker, so the callback touches no flash.
//
// Listeners are called after every change with a copy of the view as it
// is then, on the thread that made the change: wisp.agent (agent events),
// wisp.voice (a transcript), the event bus's dispatch thread (voice.state:
// errors, "not heard") or the caller of a trigger or voice_text(). The
// last call after a change always shows it. A listener must not block or
// call back into this controller other than current(); the shell's
// screens post to the UI thread from it.

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

enum class Readiness : uint8_t { Ready, NoWifi, NoLlm };
// "ready" "no_wifi" "no_llm"
const char* to_string(Readiness r);

struct TurnView {
    uint32_t turn = 0;  // 0 until the agent takes the text up
    agent::Source source = agent::Source::Web;
    std::string heard;                       // the user's text
    os::LargeString say, body;               // tags removed
    bool running = false;                    // submitted .. TurnEnd
    bool spoken = false;                     // some of it went to the speech
    bool not_heard = false;                  // the last capture heard nothing
    std::optional<voice::VoiceError> error;  // the last voice error
};

struct ConversationDeps {
    core::EventBus& bus;
    agent::AgentService& agent;
    voice::VoiceService& voice;
    std::function<bool()> tts_mute;  // cached voice/tts_mute; empty: false
    std::function<Readiness()>
        readiness;  // Wi-Fi connected and an llm profile active; empty: Ready
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

    void hold_begin();
    void hold_end();
    void hold_cancel();
    // No voice effect (see the file header); kept so the shell UI can
    // route taps here for character interaction, designed later.
    void tap();
    // Between hold_begin() and hold_end()/hold_cancel() (also when
    // hold_begin() did nothing because nothing was ready: then false).
    bool holding() const;

    // A transcript without recording, as Source::Voice (spoken unless
    // muted). Unsupported "no_wifi"/"no_llm" when not ready; Busy "busy"
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

    os::Mutex lmu_;
    std::vector<std::shared_ptr<Listener>> listeners_;
    uint32_t next_listener_ = 1;
};

// The id of the first button whose caps "role" is "talk", if any.
std::optional<std::string> find_talk_button(core::DeviceRegistry& reg);

}  // namespace wisp::shell
