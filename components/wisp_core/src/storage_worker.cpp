#include "wisp/core/storage_worker.hpp"

#include <mutex>
#include <utility>

#include "wisp/os/flash_guard.hpp"

namespace wisp::core {

namespace {

size_t at_least_one(size_t n) { return n == 0 ? 1 : n; }

size_t lane_index(IoLane lane) { return static_cast<size_t>(lane); }

void store_max(std::atomic<uint64_t>& field, uint64_t value) {
    // Only the worker thread writes, so load-then-store cannot lose an update.
    if (value > field.load(std::memory_order_relaxed)) {
        field.store(value, std::memory_order_relaxed);
    }
}

// The worker whose run() is executing on this thread, if any. on_worker()
// reads this rather than os::on_flash_thread(): the guard also answers
// true when no flash thread is registered at all (boot, shutdown), which
// would make run_sync() run inline on any thread then, and it cannot tell
// two workers alive in one test apart.
thread_local const StorageWorker* t_current_worker = nullptr;

}  // namespace

StorageWorker::StorageWorker(StorageWorkerConfig config)
    : config_(std::move(config)),
      lanes_{os::Queue<Item>(at_least_one(config_.interactive_capacity)),
             os::Queue<Item>(at_least_one(config_.queue_capacity)),
             os::Queue<Item>(at_least_one(config_.bulk_capacity))},
      // Room for every lane slot plus stop()'s count, so give() never
      // saturates while an item is waiting for its count.
      work_(static_cast<uint32_t>(at_least_one(config_.interactive_capacity) +
                                  at_least_one(config_.queue_capacity) +
                                  at_least_one(config_.bulk_capacity) + 1),
            0) {}

StorageWorker::~StorageWorker() { stop(); }

os::TaskConfig StorageWorker::task_config(const StorageWorkerConfig& c) {
    os::TaskConfig t;
    t.name = c.name;
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    t.core = -1;
    t.flash_access = c.role == WorkerRole::Flash;
    t.hot_stack = false;
    return t;
}

StorageWorkerConfig StorageWorker::sd_io_config() {
    return StorageWorkerConfig{.name = "wisp.sd.io",
                               .role = WorkerRole::SdIo,
                               .interactive_capacity = 1,
                               .queue_capacity = 16,
                               .bulk_capacity = 4,
                               .stack_bytes = 8192,
                               .priority = 3};
}

namespace {

void register_role(WorkerRole role) {
    if (role == WorkerRole::Flash) {
        os::set_flash_thread();
    } else {
        os::set_sd_io_thread();
    }
}

void clear_role(WorkerRole role) {
    if (role == WorkerRole::Flash) {
        os::clear_flash_thread();
    } else {
        os::clear_sd_io_thread();
    }
}

// A Flash-role worker waited on from the SD I/O thread: refused, since
// wisp.fs may itself be waiting for a card lock that the waiting SD job
// holds (or for the SD worker's queue).
bool sd_thread_waits_for_flash(WorkerRole role) {
    return role == WorkerRole::Flash && os::on_sd_io_thread();
}

}  // namespace

Result<void> StorageWorker::start() {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (started_) {
        return Result<void>::err(ErrorCode::Busy, "storage worker already started");
    }
    task_ = os::Task::spawn(task_config(config_), [this] { run(); });
    if (!task_) {
        return Result<void>::err(ErrorCode::NoMemory, "cannot spawn storage worker");
    }
    // Until the worker has registered, no thread is the flash thread and
    // the guard would let every thread touch flash; run() takes no lock
    // before giving this, so waiting under mutex_ is safe.
    registered_.take(os::kForever);
    started_ = true;
    accepting_ = true;
    return Result<void>::ok();
}

void StorageWorker::stop() {
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (!accepting_) {
            return;
        }
        accepting_ = false;
    }
    // Every accepted job was pushed and counted under the mutex before
    // accepting_ went false, so the worker sees this extra count only
    // after it has taken one per job; it then finds every lane empty and
    // exits.
    stopping_.store(true);
    work_.give();
    task_->join();
    std::lock_guard<os::Mutex> lock(mutex_);
    stopped_ = true;
}

bool StorageWorker::running() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return accepting_;
}

bool StorageWorker::submit(std::function<void()> job, IoLane lane) {
    if (!job) {
        return false;
    }
    Item item{std::move(job), now()};
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!accepting_) {
        return false;
    }
    ++pending_;
    if (!lanes_[lane_index(lane)].push(std::move(item), 0)) {
        --pending_;
        return false;
    }
    work_.give();
    return true;
}

Result<void> StorageWorker::run_sync(std::function<void()> job, uint32_t timeout_ms, IoLane lane) {
    if (!job) {
        return Result<void>::err(ErrorCode::InvalidArgument, "empty storage job");
    }
    if (sd_thread_waits_for_flash(config_.role)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "the sd worker must not wait for the flash worker");
    }
    if (on_worker()) {
        job();
        return Result<void>::ok();
    }
    bool inline_run = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        inline_run = !started_ || stopped_;
    }
    if (inline_run) {
        job();
        return Result<void>::ok();
    }
    auto done = std::make_shared<os::Semaphore>(1, 0);
    // The job's captures are released before done is given, so a caller
    // that waited may free whatever they referenced as soon as it wakes.
    if (!submit(
            [job = std::move(job), done]() mutable {
                job();
                job = nullptr;
                done->give();
            },
            lane)) {
        return Result<void>::err(ErrorCode::Busy, "storage worker queue full or stopping");
    }
    if (!done->take(timeout_ms)) {
        return Result<void>::err(ErrorCode::Timeout, "storage job did not finish in time");
    }
    return Result<void>::ok();
}

bool StorageWorker::on_worker() const { return t_current_worker == this; }

LaneStats StorageWorker::stats(IoLane lane) const {
    const AtomicLaneStats& s = stats_[lane_index(lane)];
    return LaneStats{s.jobs.load(std::memory_order_relaxed),
                     s.max_wait_us.load(std::memory_order_relaxed),
                     s.max_job_us.load(std::memory_order_relaxed)};
}

uint64_t StorageWorker::now() const { return config_.now_us ? config_.now_us() : os::now_us(); }

size_t StorageWorker::pick_lane(const bool (&queued)[kLaneCount]) {
    size_t lane = kLaneCount;
    bool forced = false;
    if (!last_forced_) {
        // The lowest lane due a turn first: it has waited the longest.
        for (size_t l = kLaneCount; l-- > 0;) {
            if (queued[l] && passed_[l] >= kLaneTurnAfter) {
                lane = l;
                forced = true;
                break;
            }
        }
    }
    if (!forced) {
        for (size_t l = 0; l < kLaneCount; ++l) {
            if (queued[l]) {
                lane = l;
                break;
            }
        }
    }
    last_forced_ = forced;
    passed_[lane] = 0;
    for (size_t l = lane + 1; l < kLaneCount; ++l) {
        if (queued[l]) ++passed_[l];
    }
    return lane;
}

bool StorageWorker::drain(uint32_t timeout_ms) {
    if (on_worker() || sd_thread_waits_for_flash(config_.role)) {
        return false;
    }
    uint64_t start = os::now_ms();
    while (pending_.load() != 0) {
        if (timeout_ms != os::kForever && os::now_ms() - start >= timeout_ms) {
            return false;
        }
        os::sleep_ms(1);
    }
    return true;
}

void StorageWorker::run() {
    register_role(config_.role);
    t_current_worker = this;
    registered_.give();
    while (true) {
        work_.take(os::kForever);
        bool queued[kLaneCount];
        bool any = false;
        for (size_t l = 0; l < kLaneCount; ++l) {
            queued[l] = lanes_[l].size() != 0;
            any = any || queued[l];
        }
        if (!any) {
            // Each count is given after its push and each wake-up pops
            // one item, so a wake-up holding a job's count always finds an
            // item: the only empty wake-up is stop()'s, and every accepted
            // job has run by then. The continue is defensive.
            if (stopping_.load()) break;
            continue;
        }
        const size_t lane = pick_lane(queued);
        Item item;
        // size() said the lane holds an item and only this thread pops, so
        // the pop succeeds as soon as the push has given its slot.
        lanes_[lane].pop(item, os::kForever);
        const uint64_t start = now();
        item.fn();
        const uint64_t end = now();
        // Drop the job's captures before reporting it finished, so a
        // drain()ing caller may free what they referenced.
        item.fn = nullptr;
        AtomicLaneStats& s = stats_[lane];
        s.jobs.fetch_add(1, std::memory_order_relaxed);
        store_max(s.max_wait_us, start > item.queued_us ? start - item.queued_us : 0);
        store_max(s.max_job_us, end > start ? end - start : 0);
        // After the statistics, so a drain() that sees this job finished
        // also sees its figures.
        --pending_;
    }
    t_current_worker = nullptr;
    clear_role(config_.role);
}

}  // namespace wisp::core
