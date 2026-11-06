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
// Jobs run one at a time on the one worker task, from two lanes (spec
// 3.1): Normal (settings, JSON documents, scene data) and Bulk (firmware
// and download chunks, each at most kIoChunk bytes). A queued Normal job
// always runs before any queued Bulk job, so the worst wait for Normal
// work is one Bulk chunk; within a lane, jobs run in submission order.
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

enum class IoLane : uint8_t { Normal, Bulk };

struct StorageWorkerConfig {
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
    size_t stack_bytes = 6144;
    int priority = 3;
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
    // queued in either lane, then joins it. Idempotent; a no-op before
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

private:
    void run();

    StorageWorkerConfig config_;
    // An empty function on the Normal lane is the stop sentinel; submit()
    // refuses empty jobs. The worker acts on it only once the Bulk lane is
    // empty too.
    os::Queue<std::function<void()>> normal_;
    os::Queue<std::function<void()>> bulk_;
    // One count per item pushed to either lane (the sentinel included),
    // given after the push: the worker sleeps on this, not on a lane.
    os::Semaphore work_;
    std::unique_ptr<os::Task> task_;
    // Guards accepting_ together with the push in submit(), so stop()'s
    // sentinel is always the last item queued.
    os::Mutex mutex_;
    bool accepting_ = false;
    bool started_ = false;
    // Set once stop() has joined the worker; from then on run_sync() runs
    // jobs inline.
    bool stopped_ = false;
    // Jobs submitted and not yet finished; counted before the push so
    // drain() never sees a queued job as idle.
    std::atomic<size_t> pending_{0};
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
