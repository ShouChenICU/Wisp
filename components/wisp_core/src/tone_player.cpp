#include "wisp/core/tone_player.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "tone";
constexpr const char* kHolder = "system.tone";
// About -6 dBFS at full volume: loud on a small speaker, short of
// clipping in the codec.
constexpr float kPeak = 16000.0f;
constexpr uint32_t kFadeMs = 5;
constexpr float kToneHz = 1000.0f;
constexpr float kAlarmHz = 880.0f;
// The alarm's two beeps inside its segment, in ms.
constexpr uint32_t kBeeps[2][2] = {{0, 200}, {300, 500}};
constexpr float kTwoPi = 6.28318530717958647692f;

uint32_t frames_of(uint32_t ms, uint32_t rate) {
    return static_cast<uint32_t>(static_cast<uint64_t>(rate) * ms / 1000);
}

// Frame `i` of a beep of `len` frames at `hz`, faded in and out so it
// starts and ends at zero (no click).
int16_t beep(uint32_t i, uint32_t len, float hz, uint32_t rate) {
    const uint32_t fade = std::max<uint32_t>(frames_of(kFadeMs, rate), 1);
    const uint32_t to_end = len - 1 - i;
    const float env = std::min({1.0f, static_cast<float>(i) / static_cast<float>(fade),
                                static_cast<float>(to_end) / static_cast<float>(fade)});
    const float phase = kTwoPi * hz * static_cast<float>(i) / static_cast<float>(rate);
    return static_cast<int16_t>(std::lround(kPeak * env * std::sin(phase)));
}

int16_t alarm_frame(uint32_t pos, uint32_t rate) {
    for (const auto& b : kBeeps) {
        const uint32_t start = frames_of(b[0], rate);
        const uint32_t end = frames_of(b[1], rate);
        if (pos >= start && pos < end) return beep(pos - start, end - start, kAlarmHz, rate);
    }
    return 0;
}

}  // namespace

size_t synth_tone(ToneKind kind, uint32_t sample_rate, uint32_t offset, std::span<int16_t> out) {
    if (sample_rate == 0) return 0;
    if (kind == ToneKind::Tone) {
        const uint32_t len = frames_of(TonePlayer::kToneMs, sample_rate);
        if (offset >= len) return 0;
        const size_t n = std::min<size_t>(out.size(), len - offset);
        for (size_t i = 0; i < n; ++i)
            out[i] = beep(offset + static_cast<uint32_t>(i), len, kToneHz, sample_rate);
        return n;
    }
    const uint32_t segment = frames_of(TonePlayer::kAlarmSegmentMs, sample_rate);
    uint32_t pos = offset % segment;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = alarm_frame(pos, sample_rate);
        if (++pos == segment) pos = 0;
    }
    return out.size();
}

TonePlayer::TonePlayer(std::shared_ptr<hal::AudioOut> out, WakeLockRegistry* locks,
                       TonePlayerConfig config)
    : out_(std::move(out)), locks_(locks), config_(config) {}

TonePlayer::~TonePlayer() { stop(); }

os::TaskConfig TonePlayer::task_config(const TonePlayerConfig& c) {
    os::TaskConfig t;
    t.name = "wisp.tone";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    t.flash_access = false;
    return t;
}

Result<void> TonePlayer::start() {
    if (task_) return Result<void>::err(ErrorCode::Busy, "tone player already started");
    if (!out_) return Result<void>::ok();
    quit_.store(false);
    task_ = os::Task::spawn(task_config(config_), [this] { run(); });
    if (!task_) return Result<void>::err(ErrorCode::NoMemory, "cannot start wisp.tone");
    return Result<void>::ok();
}

void TonePlayer::stop() {
    if (!task_) return;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        quit_.store(true);
        pending_.reset();
        generation_.fetch_add(1);
    }
    wake_.give();
    task_->join();
    task_.reset();
    playing_.store(false);
}

void TonePlayer::play(ToneKind kind, uint8_t volume_percent) {
    if (volume_percent == 0) return silence();
    if (!task_) return;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        pending_ = Request{kind, std::min<uint8_t>(volume_percent, 100)};
        generation_.fetch_add(1);
        playing_.store(true);
    }
    wake_.give();
}

void TonePlayer::silence() {
    // Called on every touch read while a finger is down: nothing to take
    // when nothing plays.
    if (!playing_.load()) return;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        pending_.reset();
        generation_.fetch_add(1);
    }
    wake_.give();
}

bool TonePlayer::playing() const { return playing_.load(); }

void TonePlayer::run() {
    for (;;) {
        wake_.take(os::kForever);
        for (;;) {
            Request r{};
            uint32_t gen = 0;
            {
                std::lock_guard<os::Mutex> lock(mu_);
                if (quit_.load()) return;
                if (!pending_) {
                    playing_.store(false);
                    break;
                }
                r = *pending_;
                pending_.reset();
                gen = generation_.load();
            }
            play_one(r, gen);
        }
    }
}

void TonePlayer::play_one(const Request& r, uint32_t gen) {
    const bool alarm = r.kind == ToneKind::Alarm;
    WakeLock cpu;
    if (locks_ != nullptr)
        cpu = WakeLock(*locks_, kHolder, WakeLockKind::Cpu, alarm ? "alarm" : "tone");
    if (auto opened = out_->open(hal::AudioFormat{config_.sample_rate, 1}); !opened) {
        // Every notification would say it again: once until a sound plays.
        if (!open_warned_) WLOG_W(kTag, "no sound: %s", opened.error().message.c_str());
        open_warned_ = true;
        return;
    }
    open_warned_ = false;
    const uint64_t limit =
        alarm ? static_cast<uint64_t>(frames_of(kAlarmMaxMs, config_.sample_rate)) : UINT64_MAX;
    uint64_t offset = 0;
    while (generation_.load() == gen && !quit_.load() && offset < limit) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(kChunkFrames, limit - offset));
        const size_t n = synth_tone(r.kind, config_.sample_rate, static_cast<uint32_t>(offset),
                                    std::span<int16_t>(buf_.data(), want));
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) buf_[i] = static_cast<int16_t>(buf_[i] * r.volume / 100);
        if (!write_all(n)) break;
        offset += n;
    }
    out_->close();
}

bool TonePlayer::write_all(size_t n) {
    size_t done = 0;
    while (done < n) {
        auto w = out_->write(buf_.data() + done, n - done);
        if (!w || w.value() == 0) {
            WLOG_W(kTag, "sound cut short: %s",
                   w ? "the output took nothing" : w.error().message.c_str());
            return false;
        }
        done += w.value();
    }
    return true;
}

}  // namespace wisp::core
