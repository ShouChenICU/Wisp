#include "wisp/voice/pcm_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "wisp/core/log.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::voice {

namespace {

constexpr const char* kTag = "voice";
// How long the task waits for samples before looking at the phase again;
// every change of phase also gives data_, so this is only a backstop.
constexpr uint32_t kIdleWaitMs = 100;

// The same mapping as Vad::level(): RMS in dBFS, -60..0 onto 0..100.
uint8_t level_of(const int16_t* s, size_t n) {
    if (n == 0) return 0;
    double e = 0;
    for (size_t i = 0; i < n; ++i) e += static_cast<double>(s[i]) * s[i];
    e /= static_cast<double>(n);
    if (e <= 0) return 0;
    const double db = 10.0 * std::log10(e / (32768.0 * 32768.0));
    const double pct = std::clamp((db + 60.0) / 60.0, 0.0, 1.0) * 100.0;
    return static_cast<uint8_t>(std::lround(pct));
}

}  // namespace

PcmPlayer::PcmPlayer(std::shared_ptr<hal::AudioOut> out, Config c)
    : out_(std::move(out)), config_(c) {}

PcmPlayer::~PcmPlayer() { stop_task(); }

os::TaskConfig PcmPlayer::task_config(const Config& c) {
    os::TaskConfig t;
    t.name = "wisp.play";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    t.flash_access = false;
    return t;
}

Result<void> PcmPlayer::start() {
    if (task_) return Result<void>::err(ErrorCode::Busy, "pcm player already started");
    if (!out_) return Result<void>::ok();
    {
        std::lock_guard<os::Mutex> lock(mu_);
        quit_ = false;
    }
    task_ = os::Task::spawn(task_config(config_), [this] { run(); });
    if (!task_) return Result<void>::err(ErrorCode::NoMemory, "cannot start wisp.play");
    return Result<void>::ok();
}

void PcmPlayer::stop_task() {
    if (!task_) return;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        quit_ = true;
        size_ = 0;
        if (phase_ == Phase::Open) phase_ = Phase::Closing;
    }
    data_.give();
    space_.give();
    task_->join();
    task_.reset();
}

Result<void> PcmPlayer::begin(uint32_t sample_rate, uint8_t volume) {
    if (!out_ || !task_) return Result<void>::err(ErrorCode::Unsupported, "no audio output");
    if (sample_rate == 0) return Result<void>::err(ErrorCode::InvalidArgument, "sample rate 0");
    const uint64_t deadline = os::now_ms() + kBeginWaitMs;
    while (active()) {
        if (os::now_ms() >= deadline) {
            return Result<void>::err(ErrorCode::Busy, "audio output still playing");
        }
        os::sleep_ms(1);
    }
    const size_t want = static_cast<size_t>(static_cast<uint64_t>(sample_rate) * kRingMs / 1000);
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (want > capacity_) {
            os::LargeBuffer grown =
                os::LargeBuffer::allocate((want + kChunkFrames) * sizeof(int16_t));
            if (!grown.ok()) return Result<void>::err(ErrorCode::NoMemory, "no room for the ring");
            storage_ = std::move(grown);
            ring_ = reinterpret_cast<int16_t*>(storage_.data());
            chunk_ = ring_ + want;
            capacity_ = want;
        }
    }
    if (auto opened = out_->open(hal::AudioFormat{sample_rate, 1}); !opened) return opened;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        limit_ = want;
        head_ = 0;
        size_ = 0;
        eos_ = false;
        volume_ = std::min<uint8_t>(volume, 100);
        phase_ = Phase::Open;
    }
    // A give left over from the last stream must not let push() through.
    space_.take(0);
    data_.give();
    return Result<void>::ok();
}

size_t PcmPlayer::push(std::span<const int16_t> pcm, uint32_t timeout_ms) {
    const uint64_t deadline = timeout_ms == os::kForever ? UINT64_MAX : os::now_ms() + timeout_ms;
    size_t done = 0;
    while (done < pcm.size()) {
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (phase_ != Phase::Open || eos_ || quit_) break;
            const size_t room = limit_ - size_;
            size_t n = std::min(room, pcm.size() - done);
            size_t tail = (head_ + size_) % limit_;
            while (n > 0) {
                const size_t run = std::min(n, limit_ - tail);
                std::memcpy(ring_ + tail, pcm.data() + done, run * sizeof(int16_t));
                size_ += run;
                done += run;
                n -= run;
                tail = (tail + run) % limit_;
            }
        }
        data_.give();
        if (done == pcm.size()) break;
        const uint64_t now = os::now_ms();
        if (now >= deadline) break;
        const uint64_t left = deadline - now;
        if (!space_.take(left >= os::kForever ? os::kForever - 1 : static_cast<uint32_t>(left))) {
            break;
        }
    }
    return done;
}

void PcmPlayer::end_of_stream() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (phase_ != Phase::Open) return;
        eos_ = true;
    }
    data_.give();
}

void PcmPlayer::stop() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (phase_ != Phase::Open) return;
        size_ = 0;
        phase_ = Phase::Closing;
    }
    data_.give();
    space_.give();
}

bool PcmPlayer::active() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return phase_ != Phase::Closed;
}

void PcmPlayer::run() {
    for (;;) {
        size_t n = 0;
        uint8_t volume = 100;
        bool close = false;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (phase_ == Phase::Closing || (phase_ == Phase::Open && eos_ && size_ == 0)) {
                close = true;
            } else if (phase_ == Phase::Open && size_ > 0) {
                n = std::min(size_, kChunkFrames);
                const size_t first = std::min(n, limit_ - head_);
                std::memcpy(chunk_, ring_ + head_, first * sizeof(int16_t));
                std::memcpy(chunk_ + first, ring_, (n - first) * sizeof(int16_t));
                head_ = (head_ + n) % limit_;
                size_ -= n;
                volume = volume_;
            } else if (quit_) {
                return;
            }
        }
        if (close) {
            out_->close();
            level_.store(0);
            bool quit = false;
            {
                std::lock_guard<os::Mutex> lock(mu_);
                phase_ = Phase::Closed;
                size_ = 0;
                eos_ = false;
                quit = quit_;
            }
            if (quit) return;
            continue;
        }
        if (n == 0) {
            data_.take(kIdleWaitMs);
            continue;
        }
        space_.give();
        if (volume < 100) {
            for (size_t i = 0; i < n; ++i) {
                chunk_[i] = static_cast<int16_t>(chunk_[i] * volume / 100);
            }
        }
        level_.store(level_of(chunk_, n));
        if (!write_all(chunk_, n)) {
            // The stream cannot go on: drop it, as stop() would.
            std::lock_guard<os::Mutex> lock(mu_);
            size_ = 0;
            if (phase_ == Phase::Open) phase_ = Phase::Closing;
        }
    }
}

bool PcmPlayer::write_all(const int16_t* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        auto w = out_->write(data + done, n - done);
        if (!w || w.value() == 0) {
            WLOG_W(kTag, "playback cut short: %s",
                   w ? "the output took nothing" : w.error().message.c_str());
            return false;
        }
        done += w.value();
    }
    return true;
}

}  // namespace wisp::voice
