#pragma once

// StorageWorker: the "wisp.fs" task that runs file work on behalf of tasks
// whose stacks live in PSRAM (the scene task above all). Any call that
// reaches internal flash -- StorageManager on LittleFS included -- may
// stop the flash cache, and on the ESP port that asserts the calling
// task's stack is in internal RAM (AGENTS.md section 5). Such tasks
// therefore never call StorageManager themselves: they submit() a job
// here, and this task, whose stack is internal, runs it.
//
// wisp.fs is also the flash thread (spec 2.1 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md): its first
// act is os::set_flash_thread() and its last os::clear_flash_thread().
//
// Jobs run one at a time on the one worker task, from three lanes in
// priority order (spec 3.1): Interactive (short jobs a user or the agent
// waits for: settings saves, conversation appends, memory and todos),
// Normal (JSON documents, scene data, listings) and Bulk (firmware and
// download chunks, each at most kIoChunk bytes). Within a lane jobs run in
// submission order; across lanes there is no order, so a producer that
// needs two jobs on different lanes in order waits for the first.
// The worker takes the highest non-empty lane, except that a lower lane
// that has been passed over by kLaneTurnAfter higher jobs while it waited
// gets one forced turn, and two forced turns never run back to back. So a
// steady stream of higher work cannot starve Normal or Bulk, and an
// Interactive job waits at most for the job already running (one Bulk
// chunk at worst) plus one forced turn of a lower lane.
// submit() never blocks: a full lane is reported to the caller, which
// decides whether to retry, merge or drop. A job must not block on
// another job submitted after it (the worker would wait on itself) and
// must not call stop() or drain().

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "wisp/base/result.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"

namespace wisp::core {

// The most one Bulk job writes, so one Bulk job holds up queued Normal
// work for at most one flash erase-and-write of this size.
inline constexpr size_t kIoChunk = 32 * 1024;

// In priority order; the value indexes per-lane state.
enum class IoLane : uint8_t { Interactive, Normal, Bulk };
inline constexpr size_t kLaneCount = 3;

// How many higher-lane jobs may run while a lower lane waits before that
// lane gets a forced turn: bounds starvation without letting a lower lane
// interleave with every higher job.
inline constexpr uint32_t kLaneTurnAfter = 8;

// Per-lane figures since start(), for the mem command. Wait is the time a
// job spent queued (its start minus its enqueue time); job is how long it
// ran. Jobs run inline by run_sync() are not counted.
struct LaneStats {
    uint64_t jobs = 0;
    uint64_t max_wait_us = 0;
    uint64_t max_job_us = 0;
};

struct StorageWorkerConfig {
    // Interactive lane capacity.
    size_t interactive_capacity = 16;
    // Normal lane capacity.
    size_t queue_capacity = 32;
    // Bulk lane capacity: a producer of large data (an OTA upload) can be
    // at most this many chunks ahead of the flash.
    size_t bulk_capacity = 4;
    // Internal RAM (the worker touches flash). Listing installed scenes
    // parses every manifest on this stack: with the seven example scenes a
    // boot plus install-and-open peaked at about 4.5 KB, so 4 KB overflowed
    // into a boot loop (docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md).
    // The parse recurses once per JSON level, and parse_scene_manifest()
    // caps a manifest at 4 levels (the examples use 3), so no installed
    // package can need much more: on the host the deepest accepted
    // manifest takes about 9 % more stack than the examples, and a deeper
    // one is refused before it costs more. Not shrunk until the device's
    // high-water mark has been measured with a 4-level manifest.
    // Since every flash access runs here, the jobs of the services that
    // used to touch flash on their own stacks (settings batches, Wi-Fi
    // and AP switches, scene install/export/download, the OTA chunks and
    // their final check, the factory-reset erase) all share this stack:
    // at 6 KB a scene install, export and download plus an AP on/off left
    // only 740 B, under the quarter the mem rule keeps free, so 8 KB.
    size_t stack_bytes = 8192;
    int priority = 3;
    // The clock behind the lane statistics; empty means os::now_us. Tests
    // inject one so the figures are exact.
    std::function<uint64_t()> now_us;
};

class StorageWorker {
public:
    explicit StorageWorker(StorageWorkerConfig config = {});
    ~StorageWorker();  // stop()

    StorageWorker(const StorageWorker&) = delete;
    StorageWorker& operator=(const StorageWorker&) = delete;

    // The task this worker spawns: "wisp.fs", unpinned, declaring flash
    // access (flash_access = true) because every job touches flash, so its
    // stack stays in internal RAM.
    static os::TaskConfig task_config(const StorageWorkerConfig& c);

    // Spawns the worker task. Fails with ErrorCode::Busy if it is already
    // running or has been stopped (a worker is started at most once), and
    // ErrorCode::NoMemory if the task cannot be created.
    Result<void> start();

    // Stops accepting jobs, lets the worker finish every job already
    // queued in every lane, then joins it. Idempotent; a no-op before
    // start().
    void stop();

    // Queues `job` on `lane`. Returns false without queuing it if that
    // lane is full, the worker is not running, or `job` is empty.
    bool submit(std::function<void()> job, IoLane lane = IoLane::Normal);

    // Runs `job` on the worker and waits up to `timeout_ms` (or
    // os::kForever) for it to finish. Runs it inline, on the caller, when
    // called from a job, or before start() / after stop() (the
    // single-threaded boot and shutdown phases). Fails with
    // ErrorCode::Busy if the lane is full or the worker is stopping (the
    // job then never runs), and ErrorCode::Timeout if it did not finish
    // in time: it may still run later, so unless the wait is kForever the
    // job must own what it touches (capture shared_ptrs, not references
    // to the caller's stack). An empty job is InvalidArgument.
    Result<void> run_sync(std::function<void()> job, uint32_t timeout_ms,
                          IoLane lane = IoLane::Normal);

    // True when called from inside one of this worker's jobs (on its
    // thread, between start and stop).
    bool on_worker() const;

    // Waits up to `timeout_ms` (or os::kForever) until no job is queued or
    // running. Returns false on timeout, and at once when called from a
    // job (the running job itself would never finish).
    bool drain(uint32_t timeout_ms);

    // Jobs submitted and not yet finished (queued plus the one running, if
    // any). Lets a caller wait for a job it just submitted to actually be
    // queued, rather than guessing how long that takes.
    size_t pending() const { return pending_.load(); }

    // `lane`'s statistics (see LaneStats). Safe from any thread while the
    // worker runs; the three fields are read one by one, so a job finishing
    // meanwhile may show in some of them only.
    LaneStats stats(IoLane lane) const;

private:
    struct Item {
        std::function<void()> fn;
        uint64_t queued_us = 0;
    };

    // Written only by the worker thread; relaxed atomics are enough since
    // each field stands alone, and they cost the hot path no lock.
    struct AtomicLaneStats {
        std::atomic<uint64_t> jobs{0};
        std::atomic<uint64_t> max_wait_us{0};
        std::atomic<uint64_t> max_job_us{0};
    };

    void run();
    uint64_t now() const;
    // The lane the next job comes from (the turn rule in the file header),
    // given which lanes hold a job; updates passed_ and last_forced_.
    size_t pick_lane(const bool (&queued)[kLaneCount]);

    StorageWorkerConfig config_;
    // Indexed by IoLane.
    os::Queue<Item> lanes_[kLaneCount];
    // One count per item pushed to any lane, given after the push, plus
    // one from stop(): the worker sleeps on this, not on a lane, and exits
    // on a wake-up that finds every lane empty once stopping_ is set.
    os::Semaphore work_;
    std::unique_ptr<os::Task> task_;
    // Guards accepting_ together with the push and give in submit(), so
    // every accepted job is queued and counted before stop() gives work_.
    os::Mutex mutex_;
    bool accepting_ = false;
    bool started_ = false;
    // Set once stop() has joined the worker; from then on run_sync() runs
    // jobs inline.
    bool stopped_ = false;
    // Jobs submitted and not yet finished; counted before the push so
    // drain() never sees a queued job as idle.
    std::atomic<size_t> pending_{0};
    std::atomic<bool> stopping_{false};
    // Worker-thread state of the turn rule: passed_[L] counts the
    // higher-lane jobs that ran while lane L held a job, since L last ran.
    uint32_t passed_[kLaneCount] = {};
    bool last_forced_ = false;
    AtomicLaneStats stats_[kLaneCount];
};

// fn() (returning Result<T>) on `worker` and its result, waiting as long as
// it takes: inline without a worker, on the worker, or outside its run (see
// run_sync()). A full lane is retried for up to `busy_retry_ms` before
// failing with ErrorCode::Busy. Since the wait has no timeout, fn may
// capture references to the caller's stack: once queued it has finished
// before this returns, and a refused job never runs.
template <typename T, typename Fn>
Result<T> call_on_worker(StorageWorker* worker, Fn&& fn, IoLane lane = IoLane::Normal,
                         uint32_t busy_retry_ms = 2000) {
    if (worker == nullptr) return fn();
    std::optional<Result<T>> out;
    std::function<void()> job = [&out, &fn] { out.emplace(fn()); };
    Result<void> r = worker->run_sync(job, os::kForever, lane);
    constexpr uint32_t kPollMs = 10;
    for (uint32_t waited = 0; !r && r.error().code == ErrorCode::Busy && waited < busy_retry_ms;
         waited += kPollMs) {
        os::sleep_ms(kPollMs);
        r = worker->run_sync(job, os::kForever, lane);
    }
    if (!r) return Result<T>::err(r.error().code, r.error().message);
    return std::move(*out);
}

}  // namespace wisp::core
