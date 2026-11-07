#pragma once

// Scripted audio devices for the voice tests.
//
// FakeAudioIn plays a script of segments (silence, noise, a sine) as fast
// as it is read, advancing an optional virtual clock by the audio's
// duration; once the script is used up it reads silence at real-time pace
// (one 20 ms frame per 20 ms), as a microphone would, so a capture that
// waits for a key release does not run away to its cap.
//
// FakeAudioOut records the formats it was opened with and every sample
// written; writes can be made to take time (write_ms) or be held at a
// gate. Both log their open()/close() calls into an optional shared
// OrderLog, and FakeAudioOut counts an open() while it is already open
// (two players sharing one output must never do that).
//
// Not the FakeAudioIn/FakeAudioOut of fake_devices.hpp (those just
// accept everything): these live in wisp::test::audio.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "wisp/hal/audio.hpp"
#include "wisp/os/os.hpp"

namespace wisp::test::audio {

class OrderLog {
public:
    void add(std::string what) {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.push_back(std::move(what));
    }
    std::vector<std::string> entries() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> entries_;
};

struct Segment {
    enum class Kind : uint8_t { Silence, Noise, Sine };
    Kind kind = Kind::Silence;
    uint32_t ms = 0;
    int16_t amplitude = 8000;
};

inline Segment silence(uint32_t ms) { return {Segment::Kind::Silence, ms, 0}; }
inline Segment noise(uint32_t ms, int16_t amplitude = 30) {
    return {Segment::Kind::Noise, ms, amplitude};
}
inline Segment sine(uint32_t ms, int16_t amplitude = 8000) {
    return {Segment::Kind::Sine, ms, amplitude};
}

class FakeAudioIn : public hal::AudioIn {
public:
    static constexpr uint32_t kRate = 16000;

    explicit FakeAudioIn(std::vector<Segment> script = {}, std::shared_ptr<OrderLog> order = {},
                         std::atomic<uint64_t>* virtual_ms = nullptr)
        : order_(std::move(order)), virtual_ms_(virtual_ms) {
        info_.id = "audio_in.main";
        info_.cls = kClass;
        info_.model = "FAKE";
        set_script(std::move(script));
    }

    const hal::DeviceInfo& info() const override { return info_; }

    // Replaces the script; the next open() starts it from the beginning.
    void set_script(std::vector<Segment> script) {
        std::lock_guard<std::mutex> lock(mu_);
        script_ = std::move(script);
        total_ = 0;
        for (const Segment& s : script_) total_ += static_cast<uint64_t>(s.ms) * kRate / 1000;
        pos_ = 0;
    }

    void set_fail_open(bool fail) { fail_open_ = fail; }

    Result<void> open(const hal::AudioFormat& format) override {
        if (order_) order_->add("in.open");
        std::lock_guard<std::mutex> lock(mu_);
        ++opens_;
        if (fail_open_) return Result<void>::err(ErrorCode::DeviceFailure, "no mic");
        format_ = format;
        open_ = true;
        pos_ = 0;
        return Result<void>::ok();
    }

    Result<size_t> read(int16_t* frames, size_t count) override {
        size_t scripted = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            while (scripted < count && pos_ < total_) {
                frames[scripted++] = sample_at(pos_++);
            }
            read_frames_ += scripted;
        }
        if (scripted > 0) {
            if (virtual_ms_ != nullptr) virtual_ms_->fetch_add(scripted * 1000 / kRate);
            return Result<size_t>::ok(scripted);
        }
        // Past the script: a live microphone, real time.
        os::sleep_ms(static_cast<uint32_t>(count * 1000 / kRate));
        std::fill(frames, frames + count, int16_t{0});
        std::lock_guard<std::mutex> lock(mu_);
        read_frames_ += count;
        return Result<size_t>::ok(count);
    }

    void close() override {
        if (order_) order_->add("in.close");
        std::lock_guard<std::mutex> lock(mu_);
        open_ = false;
        ++closes_;
    }

    Result<void> set_gain(float) override { return Result<void>::ok(); }

    // The whole script has been read since the last open().
    bool script_done() const {
        std::lock_guard<std::mutex> lock(mu_);
        return opens_ > 0 && pos_ >= total_;
    }
    bool is_open() const {
        std::lock_guard<std::mutex> lock(mu_);
        return open_;
    }
    int opens() const {
        std::lock_guard<std::mutex> lock(mu_);
        return opens_;
    }
    int closes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closes_;
    }
    hal::AudioFormat format() const {
        std::lock_guard<std::mutex> lock(mu_);
        return format_;
    }

private:
    // With mu_ held.
    int16_t sample_at(uint64_t pos) const {
        uint64_t start = 0;
        for (const Segment& s : script_) {
            const uint64_t len = static_cast<uint64_t>(s.ms) * kRate / 1000;
            if (pos < start + len) {
                const uint64_t i = pos - start;
                switch (s.kind) {
                    case Segment::Kind::Silence:
                        return 0;
                    case Segment::Kind::Noise: {
                        // A fixed LCG: the same script always reads the same.
                        const uint32_t x = static_cast<uint32_t>(i * 1103515245u + 12345u);
                        const int r = static_cast<int>((x >> 16) % 2001) - 1000;
                        return static_cast<int16_t>(r * s.amplitude / 1000);
                    }
                    case Segment::Kind::Sine: {
                        const double phase = 6.283185307179586 * 440.0 * static_cast<double>(i) /
                                             static_cast<double>(kRate);
                        return static_cast<int16_t>(std::lround(s.amplitude * std::sin(phase)));
                    }
                }
            }
            start += len;
        }
        return 0;
    }

    hal::DeviceInfo info_;
    std::shared_ptr<OrderLog> order_;
    std::atomic<uint64_t>* virtual_ms_;
    std::atomic<bool> fail_open_{false};
    mutable std::mutex mu_;
    std::vector<Segment> script_;
    uint64_t total_ = 0;
    uint64_t pos_ = 0;
    uint64_t read_frames_ = 0;
    hal::AudioFormat format_{};
    bool open_ = false;
    int opens_ = 0;
    int closes_ = 0;
};

class FakeAudioOut : public hal::AudioOut {
public:
    explicit FakeAudioOut(std::shared_ptr<OrderLog> order = {}) : order_(std::move(order)) {
        info_.id = "audio_out.main";
        info_.cls = kClass;
        info_.model = "FAKE";
    }

    const hal::DeviceInfo& info() const override { return info_; }

    // How long every write() takes.
    void set_write_ms(uint32_t ms) { write_ms_ = ms; }
    // While closed, write() waits at the gate (polling, so close of the
    // gate never strands a writer forever: open_gate() lets it through).
    void close_gate() { gate_open_ = false; }
    void open_gate() { gate_open_ = true; }
    void set_fail_open(bool fail) { fail_open_ = fail; }

    Result<void> open(const hal::AudioFormat& format) override {
        if (order_) order_->add("out.open");
        std::lock_guard<std::mutex> lock(mu_);
        ++opens_;
        if (open_) ++overlapping_opens_;
        if (fail_open_) return Result<void>::err(ErrorCode::DeviceFailure, "no codec");
        formats_.push_back(format);
        open_ = true;
        return Result<void>::ok();
    }

    Result<size_t> write(const int16_t* frames, size_t count) override {
        ++writers_waiting_;
        while (!gate_open_.load()) os::sleep_ms(1);
        --writers_waiting_;
        if (write_ms_ > 0) os::sleep_ms(write_ms_);
        std::lock_guard<std::mutex> lock(mu_);
        samples_.insert(samples_.end(), frames, frames + count);
        ++writes_;
        return Result<size_t>::ok(count);
    }

    void close() override {
        if (order_) order_->add("out.close");
        std::lock_guard<std::mutex> lock(mu_);
        open_ = false;
        ++closes_;
    }

    Result<void> set_volume(uint8_t) override {
        std::lock_guard<std::mutex> lock(mu_);
        ++set_volume_calls_;
        return Result<void>::ok();
    }

    std::vector<int16_t> samples() const {
        std::lock_guard<std::mutex> lock(mu_);
        return samples_;
    }
    size_t frames() const {
        std::lock_guard<std::mutex> lock(mu_);
        return samples_.size();
    }
    std::vector<hal::AudioFormat> formats() const {
        std::lock_guard<std::mutex> lock(mu_);
        return formats_;
    }
    bool is_open() const {
        std::lock_guard<std::mutex> lock(mu_);
        return open_;
    }
    int opens() const {
        std::lock_guard<std::mutex> lock(mu_);
        return opens_;
    }
    int closes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closes_;
    }
    int overlapping_opens() const {
        std::lock_guard<std::mutex> lock(mu_);
        return overlapping_opens_;
    }
    int set_volume_calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return set_volume_calls_;
    }
    // A write() is waiting at the closed gate.
    bool writer_waiting() const { return writers_waiting_.load() > 0; }

private:
    hal::DeviceInfo info_;
    std::shared_ptr<OrderLog> order_;
    std::atomic<uint32_t> write_ms_{0};
    std::atomic<bool> gate_open_{true};
    std::atomic<bool> fail_open_{false};
    std::atomic<int> writers_waiting_{0};
    mutable std::mutex mu_;
    std::vector<hal::AudioFormat> formats_;
    std::vector<int16_t> samples_;
    bool open_ = false;
    int opens_ = 0;
    int closes_ = 0;
    int overlapping_opens_ = 0;
    int writes_ = 0;
    int set_volume_calls_ = 0;
};

}  // namespace wisp::test::audio
