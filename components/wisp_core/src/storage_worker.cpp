#include "wisp/core/storage_worker.hpp"

#include <mutex>
#include <utility>

#include "wisp/os/flash_guard.hpp"

namespace wisp::core {

namespace {

size_t at_least_one(size_t n) { return n == 0 ? 1 : n; }

// The worker whose run() is executing on this thread, if any. A
// per-thread record rather than os::on_flash_thread() alone: that also
// answers true whenever no flash thread is registered, and a test may
// have two workers alive, one of which clears the registration.
thread_local const StorageWorker* t_current_worker = nullptr;

}  // namespace

StorageWorker::StorageWorker(StorageWorkerConfig config)
    : config_(config),
      normal_(at_least_one(config.queue_capacity)),
      bulk_(at_least_one(config.bulk_capacity)),
      // Room for every lane slot plus the stop sentinel, so give() never
      // saturates while an item is waiting for its count.
      work_(static_cast<uint32_t>(at_least_one(config.queue_capacity) +
                                  at_least_one(config.bulk_capacity) + 1),
            0) {}

StorageWorker::~StorageWorker() { stop(); }

os::TaskConfig StorageWorker::task_config(const StorageWorkerConfig& c) {
    os::TaskConfig t;
    t.name = "wisp.fs";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    t.core = -1;
    t.flash_access = true;
    return t;
}

Result<void> StorageWorker::start() {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (started_) {
        return Result<void>::err(ErrorCode::Busy, "storage worker already started");
    }
    task_ = os::Task::spawn(task_config(config_), [this] { run(); });
    if (!task_) {
        return Result<void>::err(ErrorCode::NoMemory, "cannot spawn wisp.fs");
    }
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
    // No submit() can push after accepting_ went false, so the sentinel
    // lands behind every accepted job. Waiting for a slot is bounded: the
    // worker keeps popping until it reaches the sentinel.
    normal_.push(std::function<void()>(), os::kForever);
    work_.give();
    task_->join();
    std::lock_guard<os::Mutex> lock(mutex_);
    stopped_ = true;
}

bool StorageWorker::submit(std::function<void()> job, IoLane lane) {
    if (!job) {
        return false;
    }
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!accepting_) {
        return false;
    }
    ++pending_;
    auto& q = lane == IoLane::Bulk ? bulk_ : normal_;
    if (!q.push(std::move(job), 0)) {
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

bool StorageWorker::on_worker() const { return t_current_worker == this && os::on_flash_thread(); }

bool StorageWorker::drain(uint32_t timeout_ms) {
    if (on_worker()) {
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
    os::set_flash_thread();
    t_current_worker = this;
    auto run_job = [this](std::function<void()>& job) {
        job();
        // Drop the job's captures before reporting it finished, so a
        // drain()ing caller may free what they referenced.
        job = nullptr;
        --pending_;
    };
    while (true) {
        work_.take(os::kForever);
        std::function<void()> job;
        if (normal_.pop(job, 0)) {
            if (!job) {
                // The sentinel is the last item stop() let in, so the
                // Normal lane is empty now; finish what Bulk still holds.
                while (bulk_.pop(job, 0)) run_job(job);
                break;
            }
            run_job(job);
        } else if (bulk_.pop(job, 0)) {
            run_job(job);
        }
    }
    t_current_worker = nullptr;
    os::clear_flash_thread();
}

}  // namespace wisp::core
