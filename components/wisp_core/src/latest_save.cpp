#include "wisp/core/latest_save.hpp"

#include <mutex>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {
constexpr const char* kTag = "save";
}  // namespace

LatestSave::LatestSave(SubmitIo submit_io)
    : submit_io_(std::move(submit_io)), shared_(std::make_shared<Shared>()) {}

void LatestSave::set_on_persist_failed(std::function<void()> fn) {
    std::lock_guard<os::Mutex> lock(shared_->mutex);
    shared_->on_persist_failed = std::move(fn);
}

Result<void> LatestSave::save(Write write) {
    if (!submit_io_) {
        if (!write) return Result<void>::ok();
        Result<void> r = write();
        report(*shared_, r);
        return r;
    }

    uint64_t seen = 0;
    bool submit = false;
    {
        std::lock_guard<os::Mutex> lock(shared_->mutex);
        shared_->next = std::move(write);
        seen = ++shared_->generation;
        if (!shared_->queued) {
            shared_->queued = true;
            submit = true;
        }
    }
    if (!submit) return Result<void>::ok();
    const uint64_t mine = seen;
    std::shared_ptr<Shared> shared = shared_;
    for (;;) {
        // Not under the lock: submit_io may run the job at once (on the
        // flash thread itself, or with no worker running), and the job
        // takes it.
        if (submit_io_([shared] { run(shared); })) return Result<void>::ok();
        std::lock_guard<os::Mutex> lock(shared_->mutex);
        if (shared_->generation == seen) {
            shared_->queued = false;
            break;
        }
        // Another save() replaced the write and returned Ok, counting on
        // this job: try again for it.
        seen = shared_->generation;
    }
    // Nothing is queued now. A newer save() that returned Ok meanwhile is
    // then only written with the next one.
    WLOG_W(kTag, "storage queue full; %s",
           seen == mine ? "the change is not saved" : "the newest change waits for the next");
    return Result<void>::err(ErrorCode::Busy, "storage queue full");
}

bool LatestSave::pending() const {
    std::lock_guard<os::Mutex> lock(shared_->mutex);
    return shared_->queued;
}

void LatestSave::report(Shared& shared, const Result<void>& r) {
    std::function<void()> notify;
    {
        std::lock_guard<os::Mutex> lock(shared.mutex);
        if (r) {
            shared.failures = 0;
            return;
        }
        if (++shared.failures == kFailuresToReport) notify = shared.on_persist_failed;
    }
    if (notify) notify();
}

void LatestSave::run(const std::shared_ptr<Shared>& shared) {
    Write write;
    {
        std::lock_guard<os::Mutex> lock(shared->mutex);
        // A save() from here on queues a job of its own: this one has
        // taken what it runs.
        shared->queued = false;
        write = std::move(shared->next);
        shared->next = nullptr;
    }
    if (write) report(*shared, write());
}

}  // namespace wisp::core
