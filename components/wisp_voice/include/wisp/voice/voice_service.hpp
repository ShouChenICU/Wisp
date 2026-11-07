#pragma once

// VoiceService (MVP spec 4): one voice round on the "wisp.voice" task --
// capture with VAD, ASR, then the reply's speech sentence by sentence.
//
// Capture (spec 4.1). begin_capture() starts a round: it takes the three
// wake locks (screen, network, cpu; holder "system.voice", reason
// "voice_turn") and hands the round to the task, which checks for a mic,
// an active asr profile and the network, plays the start tone (and waits
// up to kToneWaitMs for it to end, so the mic does not hear it), opens the
// mic and reads kFrameMs frames into a CaptureBuffer, feeding each to a
// Vad. Hold mode ends at end_capture(); Vad mode ends when the Vad says
// so, or at end_capture() (a second tap). Both end at max_record_ms.
// Too little speech, no speech, or an empty transcript is "not heard"
// (not an error): voice.state goes back to idle with result "not_heard".
// Otherwise the recording goes to AsrClient and the text to the
// on_transcript callback. cancel_capture() drops the recording, or aborts
// the ASR request already sent. The start tone goes through TonePlayer,
// which plays the newest sound only: starting a capture silences an alarm
// that is ringing.
//
// Speech (spec 4.4, 4.5). begin_speech() opens the turn's speech;
// speak() feeds it the say text as it streams (SentenceSplitter, at most
// kTtsMaxChars per turn); end_speech() says no more is coming. The task
// synthesizes one sentence at a time with TtsClient, pushing the PCM into
// the PcmPlayer (blocking while its ring is full), so sentence n+1 is
// being synthesized while sentence n plays. Only the first sentence of a
// turn may be requested while the LLM is still streaming
// (set_llm_busy(true)), and with overlap_first_sentence = false not even
// that one. A TTS failure ends the turn's speech (the text is still
// shown by others). stop_speaking() clears the queue, aborts the request
// in flight and stops playback.
//
// Rounds and wake locks. The round ends -- its locks are released, and
// only then is the final idle published, so a listener that sees idle
// sees the locks gone -- at not heard, an error, a cancel, a
// stop_speaking(), or when the speech queue has played out after
// end_speech(). After a transcript the round stays open for the reply's
// speech. Contract: the caller ends every round that delivered a
// transcript with end_speech() (also when nothing is spoken: muted, the
// agent refused the text, the turn failed) or stop_speaking(). As a
// safety net only, a round that has delivered its transcript and then
// seen no begin_speech()/speak()/end_speech()/stop_speaking()/
// set_llm_busy() for kVoiceRoundBackstopMs ends itself (log "voice: round
// backstop"). set_llm_busy() counts because a turn that runs tools for
// minutes without speaking yet is still alive.
//
// Sharing the output (spec 4.4). TonePlayer and PcmPlayer must never have
// the AudioOut open at once. Install may_play_tone() as the TonePlayer's
// output guard; it and the speech's player.begin() decide under one mutex,
// so the decision and the open cannot interleave. An Alarm always wins
// (speech stops, then the alarm opens the output); a Tone while speech
// holds the output is skipped. The guard captures this service: clear it
// (set_output_guard(nullptr)) before destroying the service.
//
// Events: "voice.state" (source "voice") {"state"}; listening and
// speaking carry "level" (0..100, at most one per kLevelIntervalMs);
// error carries "reason" and is followed at once by idle; idle may carry
// "result":"not_heard". Payloads never carry the transcript or the speech
// text, and neither does the log ("voice: state <s>" per change).
//
// Threads: every public method may be called from any thread. None waits
// for the task: stop_speaking() and cancel_capture() set a flag, abort the
// request through the RequestWatch ticket and wake the task; wake-ups
// never block (they coalesce into one queued command). The exceptions
// that do wait are may_play_tone(Alarm) (up to kAlarmWaitMs, on wisp.tone)
// and stop(). Commands go through an os::Queue (its slots in Mem::Large);
// the CaptureBuffer is one Mem::Large allocation of max_record_ms made at
// start().
//
// Flash: the task touches none (task_config(): flash_access = false, so
// its stack may be in PSRAM): it reads the mic, talks HTTP through the
// clients and reads ModelProfileService's in-memory state. on_transcript
// runs on this task, so it counts too (AGENTS.md section 5): it must not
// touch flash, directly or through Settings, StorageManager or
// AgentService's persistence; it hands the text to another task (post
// it). WISP_ASSERT_FLASH_THREAD (wisp/os/flash_guard.hpp) catches a
// violation at the flash entry points once wisp.fs runs.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/tone_player.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"
#include "wisp/voice/asr_client.hpp"
#include "wisp/voice/capture_buffer.hpp"
#include "wisp/voice/limits.hpp"
#include "wisp/voice/pcm_player.hpp"
#include "wisp/voice/sentences.hpp"
#include "wisp/voice/tts_client.hpp"

namespace wisp::voice {

enum class VoiceState : uint8_t { Idle, Listening, Transcribing, Speaking, Error };
enum class VoiceError : uint8_t {
    NoAsrProfile,
    NoTtsProfile,
    Offline,
    AsrFailed,
    TtsFailed,
    MicUnavailable
};

// "idle" "listening" "transcribing" "speaking" "error"
const char* to_string(VoiceState s);
// "no_asr_profile" "no_tts_profile" "offline" "asr_failed" "tts_failed"
// "mic_unavailable"
const char* to_string(VoiceError e);

enum class CaptureMode : uint8_t { Hold, Vad };

struct VoiceDeps {
    core::EventBus& bus;
    std::shared_ptr<hal::AudioIn> mic;  // null -> mic_unavailable
    PcmPlayer& player;
    AsrClient& asr;
    TtsClient& tts;
    core::ModelProfileService& models;  // is an asr / tts profile active
    core::TonePlayer* tones = nullptr;
    core::WakeLockRegistry* locks = nullptr;
    std::function<bool()> network_up;
    std::function<uint8_t()> volume;  // cached audio/volume
    std::function<uint64_t()> now_ms = os::now_ms;
};

struct VoiceConfig {
    size_t stack_bytes = 8 * 1024;
    int priority = 3;
    uint32_t max_record_ms = kMaxRecordMs;
    bool overlap_first_sentence = true;
    // The command queue: a BeginCapture, one coalesced wake-up and a quit
    // are all it ever holds at once.
    size_t queue_depth = 8;
};

class VoiceService {
public:
    // How long capture waits for the start tone to end before opening the
    // mic, and speech for a tone to end before opening the output.
    static constexpr uint32_t kToneWaitMs = 300;
    // How long the output guard waits for speech to close the output
    // before an alarm.
    static constexpr uint32_t kAlarmWaitMs = 500;
    static constexpr const char* kHolder = "system.voice";
    static constexpr const char* kLockReason = "voice_turn";

    VoiceService(VoiceDeps deps, VoiceConfig config = {});
    ~VoiceService();  // stop()

    VoiceService(const VoiceService&) = delete;
    VoiceService& operator=(const VoiceService&) = delete;

    // "wisp.voice", flash_access = false (see the file header).
    static os::TaskConfig task_config(const VoiceConfig& c);

    // Allocates the capture buffer and the request watch and spawns the
    // task. Busy if started before, NoMemory if any of that fails.
    Result<void> start();
    // Ends whatever is in progress and stops the task. Idempotent.
    void stop();

    // Starts a round (see the file header); stops any speech first. Busy
    // while capturing or transcribing, or when the command cannot be
    // queued (then nothing else changed: no tone, no mic, the locks it
    // took released). Unsupported before start(). Every other failure is
    // reported as a voice.state error, not here.
    Result<void> begin_capture(CaptureMode mode);
    void end_capture();
    void cancel_capture();
    // Runs on wisp.voice; must not touch flash (see the file header).
    void set_on_transcript(std::function<void(std::string text)> cb);

    void begin_speech();
    void speak(std::string_view say_delta);
    void end_speech();
    void stop_speaking();
    void set_llm_busy(bool busy);

    VoiceState state() const { return state_.load(); }
    // From a successful begin_capture() until its recording is dropped,
    // not heard, failed or transcribed (false before on_transcript runs).
    bool capturing() const { return capture_busy_.load(); }
    bool speaking() const { return state_.load() == VoiceState::Speaking; }

    // For TonePlayer::set_output_guard() (see the file header): an Alarm
    // stops the speech and waits up to kAlarmWaitMs for the output to
    // close, then plays; a Tone is skipped while speech has the output.
    // Runs on wisp.tone.
    bool may_play_tone(core::ToneKind kind);

private:
    enum class Cmd : uint8_t { None, BeginCapture, Wake, Quit };
    struct Command {
        Cmd cmd = Cmd::None;
        CaptureMode mode = CaptureMode::Vad;
        uint32_t capture = 0;  // capture_seq_ of a BeginCapture
        uint32_t round = 0;    // round_ of a BeginCapture
    };

    // The turn's speech; guarded by speech_mu_.
    struct Speech {
        bool open = false;   // begin_speech() .. its end
        bool ended = false;  // end_speech() called
        bool sent_first = false;
        bool stopped = false;  // stop_speaking() ended it; its round ends too
        uint32_t round = 0;
        uint32_t next_index = 1;
        std::deque<std::string> pending;
        std::optional<SentenceSplitter> splitter;
    };

    // Wakes the task; never blocks. A wake-up lost to a full queue is
    // made up by the task's idle poll (kIdleWaitMs).
    void wake();
    // stop_speaking() without the wake-up.
    void stop_speech();
    void run();
    void capture(const Command& c);
    bool capture_cancelled(uint32_t seq) const;
    // Plays the speech: true when it did something that may be followed
    // at once by more.
    bool pump_speech();
    void speak_sentence(const std::string& text, uint32_t index, uint32_t gen);
    // Opens the player for the turn's first PCM, deciding under out_mu_.
    bool begin_output(uint32_t rate, const std::function<bool()>& stopped);
    // Ends the speech the task was playing; true if it was speaking (the
    // caller publishes idle once the round is settled).
    bool end_playback(bool stop_player);
    void finish_speech_turn(uint32_t gen);
    void check_backstop();
    void touch();
    void publish(VoiceState s, uint8_t level = 0, const char* reason = nullptr,
                 const char* result = nullptr);
    void publish_level(VoiceState s, uint8_t level);
    // Error, then the round's end, then idle.
    void fail(VoiceError e, uint32_t round);
    void acquire_round_locks();
    void release_round_locks();
    void end_round(uint32_t round);
    bool wait_for(const std::function<bool()>& done, uint32_t timeout_ms);

    VoiceDeps deps_;
    VoiceConfig config_;
    std::unique_ptr<os::Task> task_;
    os::Queue<Command> cmds_;
    std::optional<CaptureBuffer> buffer_;
    os::LargeUnique<net::RequestWatch> watch_;

    std::atomic<VoiceState> state_{VoiceState::Idle};
    std::atomic<bool> quit_{false};
    std::atomic<bool> wake_pending_{false};

    // Capture: begin_capture() bumps the sequence and sets busy;
    // end_capture() and cancel_capture() mark the sequence they saw.
    std::atomic<bool> capture_busy_{false};
    std::atomic<uint32_t> capture_seq_{0};
    std::atomic<uint32_t> ended_seq_{0};
    std::atomic<uint32_t> cancelled_seq_{0};
    std::atomic<uint32_t> asr_ticket_{0};
    std::atomic<uint32_t> tts_ticket_{0};

    // Speech: speech_gen_ moves on with every begin_speech() and
    // stop_speaking(), and a synthesis stops when it is no longer the one
    // it started with.
    mutable os::Mutex speech_mu_;
    Speech speech_;
    bool llm_busy_ = false;  // speech_mu_
    std::atomic<uint32_t> speech_gen_{0};

    // Who opens the AudioOut next: the guard and begin_output() decide
    // under it.
    os::Mutex out_mu_;

    // Task only: the speech it is playing.
    uint32_t playing_gen_ = 0;
    bool player_begun_ = false;
    bool draining_ = false;
    uint64_t last_level_ms_ = 0;
    uint8_t last_level_ = 0;

    // The round that delivered a transcript and waits for its speech
    // (0: none), and when speech activity was last seen.
    std::atomic<uint32_t> backstop_round_{0};
    std::atomic<uint64_t> last_activity_ms_{0};

    // Wake locks of the current round; round_ moves on with every round.
    // started_ is set and cleared under it too, so stop() and a
    // begin_capture() cannot leave locks behind.
    os::Mutex locks_mu_;
    bool started_ = false;
    uint32_t round_ = 0;
    core::WakeLock lock_screen_, lock_network_, lock_cpu_;

    os::Mutex cb_mu_;
    std::function<void(std::string)> on_transcript_;
};

}  // namespace wisp::voice
