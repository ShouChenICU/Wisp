#include "wisp/core/storage_router.hpp"

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
    if (w.try_submit(job, lane)) {
        return true;
    }
    // Refused by the SD I/O worker because it began stopping since
    // worker_for() looked: the job is still here, so the flash worker
    // takes it. A full lane is not moved, or the lane's order would break.
    if (&w == &flash_ || w.running()) {
        return false;
    }
    return flash_.try_submit(job, lane);
}

Result<void> StorageRouter::run_sync(Medium m, std::function<void()> job, uint32_t timeout_ms,
                                     IoLane lane) {
    if (!job) {
        return Result<void>::err(ErrorCode::InvalidArgument, "empty storage job");
    }
    if (m == Medium::Sd && sd_ != nullptr && sd_->on_worker()) {
        // Inline, as for any worker called from its own job -- also while
        // stop_sd() drains the jobs queued before it, when worker_for()
        // already answers the flash worker, which would refuse this thread.
        return sd_->run_sync(std::move(job), timeout_ms, lane);
    }
    StorageWorker& w = worker_for(m);
    if (&w == &flash_) {
        return flash_.run_sync(std::move(job), timeout_ms, lane);
    }
    // Not w.run_sync(): once the SD I/O worker has stopped it would run the
    // job inline on this thread, which need not be allowed to touch the
    // card. run_queued() never runs inline and leaves a refused job here.
    Result<void> r = w.run_queued(job, timeout_ms, lane);
    if (r || r.error().code != ErrorCode::Busy || w.running()) {
        return r;
    }
    return flash_.run_sync(std::move(job), timeout_ms, lane);
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
