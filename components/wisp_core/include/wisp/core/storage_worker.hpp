#pragma once

// StorageWorker: the "wisp.fs" task that runs file work on behalf of tasks
// whose stacks live in PSRAM (the scene task above all). Any call that
// reaches internal flash -- StorageManager on LittleFS included -- may
// stop the flash cache, and on the ESP port that asserts the calling
// task's stack is in internal RAM (AGENTS.md section 5). Such tasks
// therefore never call StorageManager themselves: they submit() a job
// here, and this task, whose stack is internal, runs it.
//
// Jobs run one at a time in submission order (FIFO) on the one worker
// task. submit() never blocks: a full queue is reported to the caller,
// which decides whether to retry, merge or drop. A job must not block on
// another job submitted after it (the worker would wait on itself) and
// must not call stop() or drain().

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/base/result.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"

namespace wisp::core {

struct StorageWorkerConfig {
    size_t queue_capacity = 32;
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
    // queued, then joins it. Idempotent; a no-op before start().
    void stop();

    // Queues `job`. Returns false without queuing it if the queue is full,
    // the worker is not running, or `job` is empty.
    bool submit(std::function<void()> job);

    // True when called from inside a job.
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
    // An empty function is the stop sentinel; submit() refuses empty jobs.
    os::Queue<std::function<void()>> queue_;
    std::unique_ptr<os::Task> task_;
    // Guards accepting_ together with the push in submit(), so stop()'s
    // sentinel is always the last item queued.
    os::Mutex mutex_;
    bool accepting_ = false;
    bool started_ = false;
    // Jobs submitted and not yet finished; counted before the push so
    // drain() never sees a queued job as idle.
    std::atomic<size_t> pending_{0};
};

}  // namespace wisp::core
