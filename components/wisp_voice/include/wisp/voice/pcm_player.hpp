#pragma once

// PcmPlayer (MVP spec 4.4, 4.5): plays one stream of 16-bit mono PCM at a
// time through a hal::AudioOut, from a ring buffer of kRingMs, on a task
// of its own ("wisp.play").
//
// One stream: begin() opens the output at the stream's sample rate;
// push() appends samples, blocking while the ring is full (the producer,
// TtsClient's on_pcm on wisp.voice, is paced by playback that way);
// end_of_stream() lets the ring drain and then closes the output; stop()
// drops what is buffered and closes it within one chunk. active() is true
// from begin() until the output is closed again.
//
// The volume is applied by scaling the samples, as TonePlayer does, so
// the device's own volume setting is left alone and both players sound
// alike.
//
// Memory: the ring and the chunk the task writes from are one Mem::Large
// allocation (PSRAM), made at the first begin() and grown only when a
// later stream's rate needs more; the output's DMA buffers are the
// driver's. The task never touches flash (it only copies samples and
// writes to the output), so task_config() declares flash_access = false
// and the port may put its stack in PSRAM. Its priority is above the UI
// task's, so drawing never starves playback.
//
// Threads: push(), begin() and end_of_stream() come from one producer
// thread; stop(), active() and level() may be called from any thread and
// never wait. start()/stop_task() are not concurrent with the rest.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "wisp/base/result.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::voice {

class PcmPlayer {
public:
    struct Config {
        size_t stack_bytes = 4096;
        int priority = 5;
    };

    // Frames copied out of the ring and written at a time: 20 ms at
    // 24 kHz. A stop() takes effect between two of them.
    static constexpr size_t kChunkFrames = 480;
    // How long begin() waits for the previous stream's output to close.
    static constexpr uint32_t kBeginWaitMs = 500;

    // `out` may be null: begin() then fails and nothing plays.
    PcmPlayer(std::shared_ptr<hal::AudioOut> out, Config c);
    explicit PcmPlayer(std::shared_ptr<hal::AudioOut> out) : PcmPlayer(std::move(out), Config{}) {}
    ~PcmPlayer();  // stop_task()

    PcmPlayer(const PcmPlayer&) = delete;
    PcmPlayer& operator=(const PcmPlayer&) = delete;

    // "wisp.play", flash_access = false (see the file header).
    static os::TaskConfig task_config(const Config& c);

    // Spawns the task; without an output there is no task (Ok). Busy if
    // started before, NoMemory if the task cannot be made.
    Result<void> start();
    // Stops the stream and the task. Idempotent.
    void stop_task();

    // Opens the output at `sample_rate`, mono, and starts a stream played
    // at `volume` percent. Unsupported without an output or a task, Busy
    // if the last stream's output did not close within kBeginWaitMs,
    // NoMemory if the ring cannot be allocated, the output's own error if
    // open() fails.
    Result<void> begin(uint32_t sample_rate, uint8_t volume);
    // Appends what fits, waiting up to `timeout_ms` in all for room;
    // returns how many samples were taken. 0 when no stream is open, after
    // end_of_stream() or once stop() was called.
    size_t push(std::span<const int16_t> pcm, uint32_t timeout_ms);
    // Plays what is buffered, then closes the output.
    void end_of_stream();
    // Drops what is buffered and closes the output after the chunk being
    // written. Never waits; a no-op without a stream.
    void stop();

    bool active() const;
    // The RMS of the last chunk written, -60..0 dBFS mapped onto 0..100; 0
    // when nothing plays.
    uint8_t level() const { return level_.load(); }

private:
    enum class Phase : uint8_t { Closed, Open, Closing };

    void run();
    bool write_all(const int16_t* data, size_t n);

    std::shared_ptr<hal::AudioOut> out_;
    Config config_;
    std::unique_ptr<os::Task> task_;

    // Guards everything below up to level_.
    mutable os::Mutex mu_;
    os::LargeBuffer storage_;  // [ring][chunk]
    int16_t* ring_ = nullptr;
    int16_t* chunk_ = nullptr;
    size_t capacity_ = 0;  // ring samples allocated
    size_t limit_ = 0;     // ring samples used by this stream
    size_t head_ = 0;      // next sample to read
    size_t size_ = 0;      // samples buffered
    Phase phase_ = Phase::Closed;
    bool eos_ = false;
    bool quit_ = false;
    uint8_t volume_ = 100;

    std::atomic<uint8_t> level_{0};
    // The task waits on data_ for samples (or a phase change); a blocked
    // push() waits on space_.
    os::Semaphore data_{1, 0};
    os::Semaphore space_{1, 0};
};

}  // namespace wisp::voice
