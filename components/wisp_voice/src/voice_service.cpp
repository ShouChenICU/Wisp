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
// How long the idle task sleeps between looks at its flags (quit, a lost
// wake-up, the round backstop).
constexpr uint32_t kIdleWaitMs = 200;

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
    : deps_(std::move(deps)), config_(config), cmds_(config.queue_depth) {}

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
    std::lock_guard<os::Mutex> lock(locks_mu_);
    started_ = true;
    return Result<void>::ok();
}

void VoiceService::stop() {
    if (!task_) return;
    {
        std::lock_guard<os::Mutex> lock(locks_mu_);
        started_ = false;
    }
    quit_.store(true);
    if (uint32_t t = asr_ticket_.load()) watch_->abort(t);
    if (uint32_t t = tts_ticket_.load()) watch_->abort(t);
    deps_.player.stop();
    cmds_.push({.cmd = Cmd::Quit}, 0);  // else the task sees quit_ at its next poll
    task_->join();
    task_.reset();
    capture_busy_.store(false);
    backstop_round_.store(0);
    std::lock_guard<os::Mutex> lock(locks_mu_);
    release_round_locks();
}

void VoiceService::wake() {
    if (wake_pending_.exchange(true)) return;
    if (!cmds_.push({.cmd = Cmd::Wake}, 0)) wake_pending_.store(false);
}

void VoiceService::touch() { last_activity_ms_.store(deps_.now_ms()); }

Result<void> VoiceService::begin_capture(CaptureMode mode) {
    // Bumped before the busy check, so a cancel_capture() racing this
    // call marks the new capture rather than the one before it.
    const uint32_t seq = capture_seq_.fetch_add(1) + 1;
    uint32_t round = 0, previous_round = 0;
    bool had_locks = false;
    {
        std::lock_guard<os::Mutex> lock(locks_mu_);
        if (!started_) return Result<void>::err(ErrorCode::Unsupported, "voice not started");
        bool idle = false;
        if (!capture_busy_.compare_exchange_strong(idle, true)) {
            return Result<void>::err(ErrorCode::Busy, "already capturing");
        }
        previous_round = round_;
        had_locks = lock_screen_.held();
        round = ++round_;
        acquire_round_locks();
    }
    // The BeginCapture wakes the task for the stop too: no wake-up of its
    // own to take the queue's room.
    stop_speech();
    if (!cmds_.push({.cmd = Cmd::BeginCapture, .mode = mode, .capture = seq, .round = round}, 0)) {
        // Nothing reached the task: undo what this call did.
        std::lock_guard<os::Mutex> lock(locks_mu_);
        capture_busy_.store(false);
        if (round_ == round) {
            round_ = previous_round;
            if (!had_locks) release_round_locks();
        }
        WLOG_W(kTag, "capture not started: command queue full");
        return Result<void>::err(ErrorCode::Busy, "voice queue full");
    }
    return Result<void>::ok();
}

void VoiceService::stop_speech() {
    touch();
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
}

void VoiceService::end_capture() {
    ended_seq_.store(capture_seq_.load());
    wake();
}

void VoiceService::cancel_capture() {
    cancelled_seq_.store(capture_seq_.load());
    if (watch_) {
        if (uint32_t t = asr_ticket_.load()) watch_->abort(t);
    }
    wake();
}

bool VoiceService::capture_cancelled(uint32_t seq) const {
    return quit_.load() || cancelled_seq_.load() >= seq;
}

void VoiceService::set_on_transcript(std::function<void(std::string text)> cb) {
    std::lock_guard<os::Mutex> lock(cb_mu_);
    on_transcript_ = std::move(cb);
}

void VoiceService::begin_speech() {
    touch();
    uint32_t round = 0;
    {
        std::lock_guard<os::Mutex> lock(locks_mu_);
        if (!started_) return;
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
    wake();
}

void VoiceService::speak(std::string_view say_delta) {
    touch();
    bool added = false;
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        if (!speech_.open || speech_.ended) return;
        speech_.splitter->feed(say_delta, [&](std::string s) {
            speech_.pending.push_back(std::move(s));
            added = true;
        });
    }
    if (added) wake();
}

void VoiceService::end_speech() {
    touch();
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
    wake();
}

void VoiceService::stop_speaking() {
    stop_speech();
    wake();
}

void VoiceService::set_llm_busy(bool busy) {
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        llm_busy_ = busy;
    }
    wake();
}

bool VoiceService::may_play_tone(core::ToneKind kind) {
    if (kind == core::ToneKind::Alarm) {
        // Under out_mu_ only to wait out a begin_output() in progress:
        // TonePlayer::playing() is already true, so no speech opens the
        // output after this, and what did open it is stopped next.
        {
            std::lock_guard<os::Mutex> lock(out_mu_);
        }
        stop_speaking();
        wait_for([this] { return !deps_.player.active(); }, kAlarmWaitMs);
        return true;
    }
    std::lock_guard<os::Mutex> lock(out_mu_);
    return !deps_.player.active();
}

bool VoiceService::begin_output(uint32_t rate, const std::function<bool()>& stopped) {
    // The last stream closing, outside the lock: the guard must not wait
    // behind it.
    wait_for([this] { return !deps_.player.active(); }, PcmPlayer::kBeginWaitMs);
    const uint64_t deadline = os::now_ms() + kToneWaitMs;
    for (;;) {
        {
            std::lock_guard<os::Mutex> lock(out_mu_);
            // A sound that has been asked for owns the output from play()
            // until it has closed it (TonePlayer::playing()); checking
            // that and opening under out_mu_ is what the guard decides
            // against.
            if (deps_.tones == nullptr || !deps_.tones->playing()) {
                if (auto r = deps_.player.begin(rate, deps_.volume ? deps_.volume() : 100); !r) {
                    WLOG_W(kTag, "no speech output: %s", r.error().message.c_str());
                    return false;
                }
                return true;
            }
        }
        // An alarm keeps the output; past kToneWaitMs this turn's speech
        // gives way.
        if (stopped() || os::now_ms() >= deadline) return false;
        os::sleep_ms(1);
    }
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

void VoiceService::release_round_locks() {
    lock_screen_.release();
    lock_network_.release();
    lock_cpu_.release();
}

void VoiceService::end_round(uint32_t round) {
    std::lock_guard<os::Mutex> lock(locks_mu_);
    // A newer round owns the locks now, or a capture is starting one.
    if (round != round_ || capture_busy_.load()) return;
    release_round_locks();
    uint32_t expected = round;
    backstop_round_.compare_exchange_strong(expected, 0);
}

void VoiceService::check_backstop() {
    const uint32_t round = backstop_round_.load();
    if (round == 0) return;
    {
        std::lock_guard<os::Mutex> lock(locks_mu_);
        if (round != round_) {
            // A newer round took over; this one is gone already.
            backstop_round_.store(0);
            return;
        }
    }
    if (deps_.now_ms() - last_activity_ms_.load() < kVoiceRoundBackstopMs) return;
    WLOG_W(kTag, "round backstop");
    backstop_round_.store(0);
    {
        std::lock_guard<os::Mutex> lock(speech_mu_);
        speech_.open = false;
        speech_.stopped = false;
        speech_.pending.clear();
        speech_gen_.fetch_add(1);
    }
    end_playback(true);
    end_round(round);
    publish(VoiceState::Idle);
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

void VoiceService::publish_level(VoiceState s, uint8_t level) {
    if (state_.load() != s) return;
    if (level == last_level_ || deps_.now_ms() - last_level_ms_ < kLevelIntervalMs) return;
    publish(s, level);
}

void VoiceService::fail(VoiceError e, uint32_t round) {
    publish(VoiceState::Error, 0, to_string(e));
    end_round(round);
    publish(VoiceState::Idle);
}

void VoiceService::run() {
    bool more = false;
    while (!quit_.load()) {
        Command c;
        const uint32_t wait = more ? 0 : (player_begun_ ? kLevelIntervalMs : kIdleWaitMs);
        if (cmds_.pop(c, wait)) {
            if (c.cmd == Cmd::Wake) wake_pending_.store(false);
            if (c.cmd == Cmd::BeginCapture) capture(c);
        }
        if (quit_.load()) break;
        more = pump_speech();
        if (player_begun_) publish_level(VoiceState::Speaking, deps_.player.level());
        check_backstop();
    }
    if (player_begun_ && end_playback(true)) publish(VoiceState::Idle);
}

void VoiceService::capture(const Command& c) {
    // Every ending settles the round before its final idle.
    auto finish = [&](const char* result) {
        capture_busy_.store(false);
        end_round(c.round);
        publish(VoiceState::Idle, 0, nullptr, result);
    };
    auto failed = [&](VoiceError e) {
        capture_busy_.store(false);
        fail(e, c.round);
    };

    if (player_begun_ && end_playback(true)) publish(VoiceState::Idle);
    if (capture_cancelled(c.capture)) return finish(nullptr);
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
    bool cancelled = false, read_failed = false;
    for (;;) {
        // Wake-ups that come now are for speech: pump_speech() reads its
        // state after the capture anyway.
        Command cmd;
        while (cmds_.pop(cmd, 0)) {
            if (cmd.cmd == Cmd::Wake) wake_pending_.store(false);
        }
        if (capture_cancelled(c.capture)) {
            cancelled = true;
            break;
        }
        if (ended_seq_.load() >= c.capture || buf.full()) {
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
        publish_level(VoiceState::Listening, vad.level());
        if (verdict != VadVerdict::Continue) break;
    }
    deps_.mic->close();

    if (cancelled) return finish(nullptr);
    if (read_failed) return failed(VoiceError::MicUnavailable);
    if (verdict != VadVerdict::Done) return finish("not_heard");

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
    if (capture_cancelled(seq)) return finish(nullptr);
    if (!r.error.empty()) {
        if (r.error == "no_profile") return failed(VoiceError::NoAsrProfile);
        const bool offline = deps_.network_up && !deps_.network_up();
        return failed(offline ? VoiceError::Offline : VoiceError::AsrFailed);
    }
    if (blank(r.text)) return finish("not_heard");

    // The round goes on: its speech ends it, or the backstop does.
    touch();
    backstop_round_.store(c.round);
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
    const bool was_speaking = stop_previous && end_playback(true);
    if (stopped_round != 0) end_round(stopped_round);
    if (was_speaking) publish(VoiceState::Idle);

    if (next == Next::Speak) {
        speak_sentence(text, index, gen);
        return true;
    }
    if (next == Next::Finish) {
        bool spoke = false;
        if (player_begun_) {
            if (!draining_) {
                deps_.player.end_of_stream();
                draining_ = true;
            }
            if (deps_.player.active()) return false;
            spoke = end_playback(false);
        }
        finish_speech_turn(gen);
        if (spoke) publish(VoiceState::Idle);
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
        const bool was_speaking = player_begun_ && end_playback(true);
        if (e) return fail(*e, round);
        end_round(round);
        if (was_speaking) publish(VoiceState::Idle);
    };

    const std::optional<uint32_t> rate = deps_.tts.sample_rate();
    if (!rate) return give_up(VoiceError::NoTtsProfile);
    if (deps_.network_up && !deps_.network_up()) return give_up(VoiceError::Offline);

    bool no_output = false;
    auto on_pcm = [&](std::span<const int16_t> pcm) -> bool {
        if (stopped()) return false;
        if (!player_begun_) {
            if (!begin_output(*rate, stopped)) {
                no_output = !stopped();
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
                no_output = !stopped();
                return false;
            }
            publish_level(VoiceState::Speaking, deps_.player.level());
        }
        return true;
    };
    TtsResult r = deps_.tts.synthesize(
        text, on_pcm, stopped, watch_.get(),
        [this, gen](uint32_t ticket) {
            tts_ticket_.store(ticket);
            // stop_speaking() may have looked at the previous ticket.
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

bool VoiceService::end_playback(bool stop_player) {
    if (stop_player) deps_.player.stop();
    player_begun_ = false;
    draining_ = false;
    return state_.load() == VoiceState::Speaking;
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
