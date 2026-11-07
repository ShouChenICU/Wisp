#include "wisp/voice/voice_service.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/voice/vad.hpp"

namespace wisp::voice {

namespace {

constexpr const char* kTag = "voice";
// How long one push() into the player may wait before the synthesis looks
// at its stop flag again (and publishes a level).
constexpr uint32_t kPushSliceMs = 50;
// How long the idle task sleeps between looks at its quit flag.
constexpr uint32_t kIdleWaitMs = 200;
// How long a command may wait for room in the queue.
constexpr uint32_t kPostWaitMs = 100;

bool blank(std::string_view s) {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

}  // namespace

const char* to_string(VoiceState s) {
    switch (s) {
        case VoiceState::Idle:
            return "idle";
        case VoiceState::Listening:
            return "listening";
        case VoiceState::Transcribing:
            return "transcribing";
        case VoiceState::Speaking:
            return "speaking";
        case VoiceState::Error:
            return "error";
    }
    return "idle";
}

const char* to_string(VoiceError e) {
    switch (e) {
        case VoiceError::NoAsrProfile:
            return "no_asr_profile";
        case VoiceError::NoTtsProfile:
            return "no_tts_profile";
        case VoiceError::Offline:
            return "offline";
        case VoiceError::AsrFailed:
            return "asr_failed";
        case VoiceError::TtsFailed:
            return "tts_failed";
        case VoiceError::MicUnavailable:
            return "mic_unavailable";
    }
    return "asr_failed";
}

VoiceService::VoiceService(VoiceDeps deps, VoiceConfig config)
    : deps_(std::move(deps)), config_(config) {}

VoiceService::~VoiceService() { stop(); }

os::TaskConfig VoiceService::task_config(const VoiceConfig& c) {
    os::TaskConfig t;
    t.name = "wisp.voice";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    t.flash_access = false;
    return t;
}

Result<void> VoiceService::start() {
    if (task_) return Result<void>::err(ErrorCode::Busy, "voice service already started");
    if (!buffer_) {
        auto b = CaptureBuffer::create(config_.max_record_ms);
        if (!b) return Result<void>::err(b.error().code, b.error().message);
        buffer_.emplace(std::move(b.value()));
    }
    if (!watch_) {
        watch_ = net::RequestWatch::create();
        if (!watch_) return Result<void>::err(ErrorCode::NoMemory, "no request watch");
    }
    quit_.store(false);
    task_ = os::Task::spawn(task_config(config_), [this] { run(); });
    if (!task_) return Result<void>::err(ErrorCode::NoMemory, "cannot start wisp.voice");
    started_.store(true);
    return Result<void>::ok();
}

void VoiceService::stop() {
    if (!task_) return;
    started_.store(false);
    quit_.store(true);
    if (uint32_t t = asr_ticket_.load()) watch_->abort(t);
    if (uint32_t t = tts_ticket_.load()) watch_->abort(t);
    deps_.player.stop();
    post({.cmd = Cmd::Quit});
    task_->join();
    task_.reset();
    capture_busy_.store(false);
    std::lock_guard<os::Mutex> lock(locks_mu_);
    lock_screen_.release();
    lock_network_.release();
    lock_cpu_.release();
}

void VoiceService::post(Command c) {
    // The commands only wake the task and mark capture boundaries; the
    // state they act on is in the flags, so one lost to a full queue
    // (16 deep: nothing sends that many at once) is noticed on the next
    // look.
    if (!cmds_.push(c, kPostWaitMs)) WLOG_W(kTag, "command queue full");
}

Result<void> VoiceService::begin_capture(CaptureMode mode) {
    if (!started_.load()) return Result<void>::err(ErrorCode::Unsupported, "voice not started");
    bool idle = false;
    if (!capture_busy_.compare_exchange_strong(idle, true)) {
        return Result<void>::err(ErrorCode::Busy, "already capturing");
    }
    stop_speaking();
    const uint32_t seq = capture_seq_.fetch_add(1) + 1;
    uint32_t round = 0;
    {
        std::lock_guard<os::Mutex> lock(locks_mu_);
        round = ++round_;
        acquire_round_locks();
    }
    post({.cmd = Cmd::BeginCapture, .mode = mode, .capture = seq, .round = round});
    return Result<void>::ok();
}

void VoiceService::end_capture() { post({.cmd = Cmd::EndCapture, .capture = capture_seq_.load()}); }

void VoiceService::cancel_capture() {
    cancelled_seq_.store(capture_seq_.load());
    if (uint32_t t = asr_ticket_.load()) watch_->abort(t);
    post({.cmd = Cmd::CancelCapture});
}

bool VoiceService::capture_cancelled(uint32_t seq) const {
    return quit_.load() || cancelled_seq_.load() >= seq;
}

void VoiceService::set_on_transcript(std::function<void(std::string text)> cb) {
    std::lock_guard<os::Mutex> lock(cb_mu_);
    on_transcript_ = std::move(cb);
}

void VoiceService::begin_speech() {
    uint32_t round = 0;
    {
        std::lock_guard<os::Mutex> lock(locks_mu_);
        // Speech belongs to the round its transcript came from; one
        // without (none is open) gets a round of its own.
        if (deps_.locks != nullptr && !lock_screen_.held()) ++round_;
        acquire_round_locks();
        round = round_;
    }
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        speech_ = Speech{};
        speech_.open = true;
        speech_.round = round;
        speech_.splitter.emplace(kTtsMaxChars);
        speech_gen_.fetch_add(1);
    }
    post({.cmd = Cmd::Wake});
}

void VoiceService::speak(std::string_view say_delta) {
    bool added = false;
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        if (!speech_.open || speech_.ended) return;
        speech_.splitter->feed(say_delta, [&](std::string s) {
            speech_.pending.push_back(std::move(s));
            added = true;
        });
    }
    if (added) post({.cmd = Cmd::Wake});
}

void VoiceService::end_speech() {
    uint32_t round = 0;
    {
        std::lock_guard<os::Mutex> lock(locks_mu_);
        round = round_;
    }
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        if (speech_.open && speech_.ended) return;
        if (speech_.open) {
            speech_.splitter->flush(
                [&](std::string s) { speech_.pending.push_back(std::move(s)); });
        } else {
            // A turn that never spoke (muted, or its speech already
            // stopped): an empty turn, so the round still ends here.
            speech_ = Speech{};
            speech_.open = true;
            speech_.round = round;
            speech_gen_.fetch_add(1);
        }
        speech_.ended = true;
    }
    post({.cmd = Cmd::Wake});
}

void VoiceService::stop_speaking() {
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        if (speech_.open) speech_.stopped = true;
        speech_.open = false;
        speech_.pending.clear();
        speech_gen_.fetch_add(1);
    }
    if (watch_) {
        if (uint32_t t = tts_ticket_.load()) watch_->abort(t);
    }
    deps_.player.stop();
    post({.cmd = Cmd::Wake});
}

void VoiceService::set_llm_busy(bool busy) {
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        llm_busy_ = busy;
    }
    post({.cmd = Cmd::Wake});
}

bool VoiceService::may_play_tone(core::ToneKind kind) {
    if (kind == core::ToneKind::Alarm) {
        stop_speaking();
        wait_for([this] { return !deps_.player.active(); }, kAlarmWaitMs);
        return true;
    }
    return !deps_.player.active();
}

bool VoiceService::wait_for(const std::function<bool()>& done, uint32_t timeout_ms) {
    const uint64_t deadline = os::now_ms() + timeout_ms;
    while (!done()) {
        if (os::now_ms() >= deadline) return done();
        os::sleep_ms(1);
    }
    return true;
}

void VoiceService::acquire_round_locks() {
    if (deps_.locks == nullptr) return;
    auto take = [&](core::WakeLock& l, core::WakeLockKind kind) {
        if (!l.held()) l = core::WakeLock(*deps_.locks, kHolder, kind, kLockReason);
    };
    take(lock_screen_, core::WakeLockKind::Screen);
    take(lock_network_, core::WakeLockKind::Network);
    take(lock_cpu_, core::WakeLockKind::Cpu);
}

void VoiceService::end_round(uint32_t round) {
    std::lock_guard<os::Mutex> lock(locks_mu_);
    // A newer round owns the locks now, or a capture is starting one.
    if (round != round_ || capture_busy_.load()) return;
    lock_screen_.release();
    lock_network_.release();
    lock_cpu_.release();
}

void VoiceService::publish(VoiceState s, uint8_t level, const char* reason, const char* result) {
    const VoiceState before = state_.exchange(s);
    Map payload{{"state", Value(to_string(s))}};
    if (s == VoiceState::Listening || s == VoiceState::Speaking) {
        payload.emplace("level", Value(static_cast<int>(level)));
        last_level_ = level;
        last_level_ms_ = deps_.now_ms();
    }
    if (reason != nullptr) payload.emplace("reason", Value(reason));
    if (result != nullptr) payload.emplace("result", Value(result));
    deps_.bus.publish(core::Event{
        .topic = "voice.state", .source = "voice", .ts_ms = 0, .payload = std::move(payload)});
    if (before == s && reason == nullptr && result == nullptr) return;
    if (reason != nullptr) {
        WLOG_I(kTag, "state %s reason=%s", to_string(s), reason);
    } else if (result != nullptr) {
        WLOG_I(kTag, "state %s result=%s", to_string(s), result);
    } else {
        WLOG_I(kTag, "state %s", to_string(s));
    }
}

void VoiceService::publish_level(VoiceState s, uint8_t level, bool force) {
    if (state_.load() != s) return;
    if (!force && (level == last_level_ || deps_.now_ms() - last_level_ms_ < kLevelIntervalMs)) {
        return;
    }
    publish(s, level);
}

void VoiceService::fail(VoiceError e) {
    publish(VoiceState::Error, 0, to_string(e));
    publish(VoiceState::Idle);
}

void VoiceService::run() {
    bool more = false;
    while (!quit_.load()) {
        Command c;
        const uint32_t wait = more ? 0 : (player_begun_ ? kLevelIntervalMs : kIdleWaitMs);
        if (cmds_.pop(c, wait)) handle(c);
        if (quit_.load()) break;
        more = pump_speech();
        if (player_begun_) publish_level(VoiceState::Speaking, deps_.player.level(), false);
    }
    if (player_begun_) end_playback(true);
}

void VoiceService::handle(const Command& c) {
    // Every other command only wakes the task: pump_speech() reads the
    // speech state, and capture() reads its own commands.
    if (c.cmd == Cmd::BeginCapture) capture(c);
}

void VoiceService::capture(const Command& c) {
    auto finish = [&](VoiceState s, const char* result) {
        capture_busy_.store(false);
        publish(s, 0, nullptr, result);
        end_round(c.round);
    };
    auto failed = [&](VoiceError e) {
        capture_busy_.store(false);
        fail(e);
        end_round(c.round);
    };

    if (player_begun_) end_playback(true);
    if (capture_cancelled(c.capture)) return finish(VoiceState::Idle, nullptr);
    if (!deps_.mic) return failed(VoiceError::MicUnavailable);
    if (deps_.models.get().active_for(core::ModelKind::Asr).empty()) {
        return failed(VoiceError::NoAsrProfile);
    }
    if (deps_.network_up && !deps_.network_up()) return failed(VoiceError::Offline);

    publish(VoiceState::Listening, 0);
    // The start tone must not be skipped for speech that is still closing
    // the output, and the mic must not hear it.
    wait_for([this] { return !deps_.player.active(); }, kToneWaitMs);
    if (deps_.tones != nullptr) {
        deps_.tones->play(core::ToneKind::Tone, deps_.volume ? deps_.volume() : 100);
        wait_for([this] { return !deps_.tones->playing(); }, kToneWaitMs);
    }
    if (auto opened = deps_.mic->open(hal::AudioFormat{kSampleRate, 1}); !opened) {
        WLOG_W(kTag, "mic: %s", opened.error().message.c_str());
        return failed(VoiceError::MicUnavailable);
    }

    CaptureBuffer& buf = *buffer_;
    buf.clear();
    Vad vad(c.mode == CaptureMode::Hold ? VadMode::Hold : VadMode::Auto);
    VadVerdict verdict = VadVerdict::Continue;
    bool ended = false, cancelled = false, read_failed = false;
    for (;;) {
        Command cmd;
        while (cmds_.pop(cmd, 0)) {
            if (cmd.cmd == Cmd::EndCapture && cmd.capture == c.capture) ended = true;
        }
        if (capture_cancelled(c.capture)) {
            cancelled = true;
            break;
        }
        if (ended || buf.full()) {
            verdict = vad.finish();
            break;
        }
        std::span<int16_t> frame = buf.free_space();
        frame = frame.first(std::min(frame.size(), kFrameSamples));
        size_t got = 0;
        while (got < frame.size()) {
            auto r = deps_.mic->read(frame.data() + got, frame.size() - got);
            if (!r || r.value() == 0) {
                read_failed = true;
                break;
            }
            got += r.value();
        }
        if (read_failed) break;
        verdict = vad.push(frame);
        buf.commit(frame.size());
        publish_level(VoiceState::Listening, vad.level(), false);
        if (verdict != VadVerdict::Continue) break;
    }
    deps_.mic->close();

    if (cancelled) return finish(VoiceState::Idle, nullptr);
    if (read_failed) return failed(VoiceError::MicUnavailable);
    if (verdict != VadVerdict::Done) return finish(VoiceState::Idle, "not_heard");

    publish(VoiceState::Transcribing);
    if (deps_.network_up && !deps_.network_up()) return failed(VoiceError::Offline);
    const uint32_t seq = c.capture;
    AsrResult r = deps_.asr.transcribe(
        buf, [this, seq] { return capture_cancelled(seq); }, watch_.get(),
        [this, seq](uint32_t ticket) {
            asr_ticket_.store(ticket);
            // cancel_capture() may have looked at the previous ticket.
            if (capture_cancelled(seq)) watch_->abort(ticket);
        });
    asr_ticket_.store(0);
    if (capture_cancelled(seq)) return finish(VoiceState::Idle, nullptr);
    if (!r.error.empty()) {
        if (r.error == "no_profile") return failed(VoiceError::NoAsrProfile);
        const bool offline = deps_.network_up && !deps_.network_up();
        return failed(offline ? VoiceError::Offline : VoiceError::AsrFailed);
    }
    if (blank(r.text)) return finish(VoiceState::Idle, "not_heard");

    // The round goes on: its speech ends it.
    capture_busy_.store(false);
    publish(VoiceState::Idle);
    std::function<void(std::string)> cb;
    {
        std::lock_guard<os::Mutex> lock(cb_mu_);
        cb = on_transcript_;
    }
    if (cb) cb(std::move(r.text));
}

bool VoiceService::pump_speech() {
    enum class Next : uint8_t { Nothing, Speak, Finish };
    Next next = Next::Nothing;
    std::string text;
    uint32_t index = 0;
    uint32_t stopped_round = 0;
    bool stop_previous = false;
    const uint32_t gen = speech_gen_.load();
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        if (gen != playing_gen_) {
            // A stop, or a new turn: what plays belongs to an older one.
            stop_previous = player_begun_;
            playing_gen_ = gen;
        }
        if (speech_.stopped) {
            speech_.stopped = false;
            stopped_round = speech_.round;
        }
        if (speech_.open && gen == speech_gen_.load()) {
            const bool may =
                speech_.sent_first ? !llm_busy_ : (config_.overlap_first_sentence || !llm_busy_);
            if (!speech_.pending.empty() && may) {
                text = std::move(speech_.pending.front());
                speech_.pending.pop_front();
                index = speech_.next_index++;
                speech_.sent_first = true;
                next = Next::Speak;
            } else if (speech_.ended && speech_.pending.empty()) {
                next = Next::Finish;
            }
        }
    }
    if (stop_previous) end_playback(true);
    if (stopped_round != 0) end_round(stopped_round);

    if (next == Next::Speak) {
        speak_sentence(text, index, gen);
        return true;
    }
    if (next == Next::Finish) {
        if (player_begun_) {
            if (!draining_) {
                deps_.player.end_of_stream();
                draining_ = true;
            }
            if (deps_.player.active()) return false;
            end_playback(false);
        }
        finish_speech_turn(gen);
        return true;
    }
    return false;
}

void VoiceService::speak_sentence(const std::string& text, uint32_t index, uint32_t gen) {
    auto stopped = [this, gen] { return quit_.load() || speech_gen_.load() != gen; };
    auto give_up = [&](std::optional<VoiceError> e) {
        uint32_t round = 0;
        {
            std::lock_guard<os::Mutex> lock(speech_mu_);
            if (speech_gen_.load() != gen) return;
            speech_.open = false;
            speech_.pending.clear();
            round = speech_.round;
        }
        if (player_begun_) end_playback(true);
        if (e) fail(*e);
        end_round(round);
    };

    const std::optional<uint32_t> rate = deps_.tts.sample_rate();
    if (!rate) return give_up(VoiceError::NoTtsProfile);
    if (deps_.network_up && !deps_.network_up()) return give_up(VoiceError::Offline);

    bool no_output = false;
    auto on_pcm = [&](std::span<const int16_t> pcm) -> bool {
        if (stopped()) return false;
        if (!player_begun_) {
            // An alarm keeps the output; this turn's speech gives way.
            if (deps_.tones != nullptr &&
                !wait_for([this] { return !deps_.tones->playing(); }, kToneWaitMs)) {
                no_output = true;
                return false;
            }
            if (auto r = deps_.player.begin(*rate, deps_.volume ? deps_.volume() : 100); !r) {
                WLOG_W(kTag, "no speech output: %s", r.error().message.c_str());
                no_output = true;
                return false;
            }
            player_begun_ = true;
            draining_ = false;
            publish(VoiceState::Speaking, 0);
        }
        size_t off = 0;
        while (off < pcm.size()) {
            if (stopped()) return false;
            off += deps_.player.push(pcm.subspan(off), kPushSliceMs);
            if (!deps_.player.active()) {
                no_output = true;
                return false;
            }
            publish_level(VoiceState::Speaking, deps_.player.level(), false);
        }
        return true;
    };
    TtsResult r = deps_.tts.synthesize(
        text, on_pcm, stopped, watch_.get(),
        [this, gen](uint32_t ticket) {
            tts_ticket_.store(ticket);
            // stop_speaking() may have looked
            // at the previous ticket.
            if (speech_gen_.load() != gen || quit_.load()) watch_->abort(ticket);
        },
        index);
    tts_ticket_.store(0);
    if (stopped()) return;
    if (no_output) return give_up(std::nullopt);
    if (r.error.empty()) return;
    if (r.error == "no_profile") return give_up(VoiceError::NoTtsProfile);
    const bool offline = deps_.network_up && !deps_.network_up();
    give_up(offline ? VoiceError::Offline : VoiceError::TtsFailed);
}

void VoiceService::end_playback(bool stop_player) {
    if (stop_player) deps_.player.stop();
    player_begun_ = false;
    draining_ = false;
    if (state_.load() == VoiceState::Speaking) publish(VoiceState::Idle);
}

void VoiceService::finish_speech_turn(uint32_t gen) {
    uint32_t round = 0;
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        if (speech_gen_.load() != gen || !speech_.open) return;
        speech_.open = false;
        round = speech_.round;
    }
    end_round(round);
}

}  // namespace wisp::voice
