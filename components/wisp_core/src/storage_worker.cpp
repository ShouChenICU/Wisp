#include "wisp/core/storage_worker.hpp"

#include <mutex>
#include <utility>

namespace wisp::core {

StorageWorker::StorageWorker(StorageWorkerConfig config)
    : config_(config), queue_(config.queue_capacity == 0 ? 1 : config.queue_capacity) {}

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
    queue_.push(std::function<void()>(), os::kForever);
    task_->join();
}

bool StorageWorker::submit(std::function<void()> job) {
    if (!job) {
        return false;
    }
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!accepting_) {
        return false;
    }
    ++pending_;
    if (!queue_.push(std::move(job), 0)) {
        --pending_;
        return false;
    }
    return true;
}

bool StorageWorker::on_worker() const { return task_ && task_->is_current(); }

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
    while (true) {
        std::function<void()> job;
        queue_.pop(job, os::kForever);
        if (!job) {
            return;
        }
        job();
        // Drop the job's captures before reporting it finished, so a
        // drain()ing caller may free what they referenced.
        job = nullptr;
        --pending_;
    }
}

}  // namespace wisp::core
