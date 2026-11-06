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

Result<void> LatestSave::save(Write write) {
    if (!submit_io_) return write ? write() : Result<void>::ok();

    bool submit = false;
    {
        std::lock_guard<os::Mutex> lock(shared_->mutex);
        shared_->next = std::move(write);
        if (!shared_->queued) {
            shared_->queued = true;
            submit = true;
        }
    }
    if (!submit) return Result<void>::ok();
    // Not under the lock: submit_io may run the job at once (on the flash
    // thread itself, or with no worker running), and the job takes it.
    std::shared_ptr<Shared> shared = shared_;
    if (submit_io_([shared] { run(shared); })) return Result<void>::ok();
    {
        std::lock_guard<os::Mutex> lock(shared_->mutex);
        shared_->queued = false;
    }
    WLOG_W(kTag, "storage queue full; the change is saved with the next one");
    return Result<void>::err(ErrorCode::Busy, "storage queue full");
}

bool LatestSave::pending() const {
    std::lock_guard<os::Mutex> lock(shared_->mutex);
    return shared_->queued;
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
    if (write) (void)write();
}

}  // namespace wisp::core
