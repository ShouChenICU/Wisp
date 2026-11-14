#include "wisp/core/storage_router.hpp"

#include <memory>
#include <utility>

namespace wisp::core {

StorageRouter::StorageRouter(StorageWorker& flash, StorageWorker* sd) : flash_(flash), sd_(sd) {}

StorageWorker& StorageRouter::worker_for(Medium m) const {
    if (m == Medium::Sd && sd_ != nullptr && sd_->running()) {
        return *sd_;
    }
    return flash_;
}

bool StorageRouter::on_worker() const {
    return flash_.on_worker() || (sd_ != nullptr && sd_->on_worker());
}

bool StorageRouter::submit(Medium m, std::function<void()> job, IoLane lane) {
    if (!job) {
        return false;
    }
    StorageWorker& w = worker_for(m);
    if (&w == &flash_) {
        return flash_.submit(std::move(job), lane);
    }
    // StorageWorker::submit() consumes the job even when it refuses it, so
    // hold it here to have it for the flash worker if the SD I/O worker
    // began stopping since worker_for() looked.
    auto held = std::make_shared<std::function<void()>>(std::move(job));
    if (w.submit([held] { (*held)(); }, lane)) {
        return true;
    }
    if (w.running()) {
        return false;  // full: not moved, or the lane's order would break
    }
    return flash_.submit(std::move(*held), lane);
}

Result<void> StorageRouter::run_sync(Medium m, std::function<void()> job, uint32_t timeout_ms,
                                     IoLane lane) {
    if (!job) {
        return Result<void>::err(ErrorCode::InvalidArgument, "empty storage job");
    }
    StorageWorker& w = worker_for(m);
    if (&w == &flash_ || w.on_worker()) {
        // On the SD I/O worker's own thread its run_sync() runs the job
        // inline, as for any worker called from its own job.
        return w.run_sync(std::move(job), timeout_ms, lane);
    }
    // Not w.run_sync(): once the SD I/O worker has stopped it would run the
    // job inline on this thread, which need not be allowed to touch the
    // card. Same wait as StorageWorker::run_sync(), with the job held so
    // that a refusal because it stopped can go to the flash worker.
    auto held = std::make_shared<std::function<void()>>(std::move(job));
    auto done = std::make_shared<os::Semaphore>(1, 0);
    // The job's captures are released before done is given, so a caller
    // that waited may free whatever they referenced as soon as it wakes.
    if (w.submit(
            [held, done] {
                (*held)();
                *held = nullptr;
                done->give();
            },
            lane)) {
        if (!done->take(timeout_ms)) {
            return Result<void>::err(ErrorCode::Timeout, "storage job did not finish in time");
        }
        return Result<void>::ok();
    }
    if (w.running()) {
        return Result<void>::err(ErrorCode::Busy, "storage worker queue full");
    }
    return flash_.run_sync(std::move(*held), timeout_ms, lane);
}

bool StorageRouter::drain(uint32_t timeout_ms) {
    if (sd_ == nullptr) {
        return flash_.drain(timeout_ms);
    }
    if (on_worker()) {
        return false;
    }
    const uint64_t start = os::now_ms();
    while (true) {
        // A job counts on its new worker before it stops counting on the
        // one that handed it over, so reading SD, flash, SD sees a
        // hand-over in flight in either direction.
        const size_t sd_before = sd_->pending();
        const size_t flash = flash_.pending();
        const size_t sd_after = sd_->pending();
        if (sd_before == 0 && flash == 0 && sd_after == 0) {
            return true;
        }
        if (timeout_ms != os::kForever && os::now_ms() - start >= timeout_ms) {
            return false;
        }
        os::sleep_ms(1);
    }
}

size_t StorageRouter::pending() const {
    return flash_.pending() + (sd_ != nullptr ? sd_->pending() : 0);
}

void StorageRouter::stop_sd() {
    if (sd_ != nullptr) {
        sd_->stop();
    }
}

}  // namespace wisp::core
