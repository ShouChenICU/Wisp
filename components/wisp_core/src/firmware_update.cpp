#include "wisp/core/firmware_update.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/os/large.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "firmware";

// A full Bulk lane (downloads share it) drains within a few chunks.
constexpr uint32_t kBusyRetryMs = 2000;
constexpr uint32_t kBusyPollMs = 10;

// Runs `job` on `worker`'s Bulk lane and waits up to `timeout_ms`, or
// right here without a worker.
Result<void> run_on(StorageWorker* worker, const std::function<void()>& job, uint32_t timeout_ms) {
    if (worker == nullptr) {
        job();
        return Result<void>::ok();
    }
    Result<void> r = worker->run_sync(job, timeout_ms, IoLane::Bulk);
    for (uint32_t waited = 0; !r && r.error().code == ErrorCode::Busy && waited < kBusyRetryMs;
         waited += kBusyPollMs) {
        os::sleep_ms(kBusyPollMs);
        r = worker->run_sync(job, timeout_ms, IoLane::Bulk);
    }
    if (!r && r.error().code == ErrorCode::Timeout) {
        return Result<void>::err(ErrorCode::Timeout, "the flash did not keep up with the upload");
    }
    return r;
}

// `fn`'s result, computed by run_on(). `fn` may outlive a timed-out wait,
// so it must own (or point at nothing but the platform's updater) what it
// touches; the slot for its result is shared for the same reason.
template <typename T, typename Fn>
Result<T> call_on(StorageWorker* worker, uint32_t timeout_ms, Fn fn) {
    auto out = std::make_shared<std::optional<Result<T>>>();
    if (auto r = run_on(worker, [out, fn] { out->emplace(fn()); }, timeout_ms); !r) {
        return Result<T>::err(r.error().code, r.error().message);
    }
    return std::move(**out);
}

FirmwareOutcome outcome(FirmwareFailure failure, std::string message) {
    FirmwareOutcome r;
    r.failure = failure;
    r.message = std::move(message);
    return r;
}

}  // namespace

// One chunk of the image on its way to FirmwareUpdater::write().
struct FirmwareUpdateService::Chunk {
    os::LargeBuffer buf;
    size_t used = 0;
};

const char* to_string(FirmwareFailure failure) {
    switch (failure) {
        case FirmwareFailure::None:
            return "";
        case FirmwareFailure::Unsupported:
            return "unsupported";
        case FirmwareFailure::Unavailable:
            return "unavailable";
        case FirmwareFailure::Busy:
            return "busy";
        case FirmwareFailure::TooLarge:
            return "payload_too_large";
        case FirmwareFailure::InvalidImage:
            return "invalid_image";
        case FirmwareFailure::WrongBoard:
            return "wrong_board";
        case FirmwareFailure::WriteFailed:
            return "write_failed";
        case FirmwareFailure::NotPending:
            return "not_pending";
    }
    return "";
}

FirmwareUpdateService::FirmwareUpdateService(Deps deps, StorageWorker* worker)
    : deps_(std::move(deps)), worker_(worker) {
    if (deps_.updater != nullptr) target_ = deps_.updater->target();
}

uint64_t FirmwareUpdateService::now() const { return deps_.now_ms ? deps_.now_ms() : os::now_ms(); }

FirmwareStatus FirmwareUpdateService::status() const {
    FirmwareStatus st;
    st.version = deps_.running_version;
    if (deps_.updater == nullptr) return st;
    st.supported = true;
    st.board = target_.board;
    st.running = deps_.updater->running_slot();
    std::lock_guard<os::Mutex> lock(mutex_);
    if (state_ == State::Pending) st.pending = version_;
    return st;
}

std::optional<FirmwarePending> FirmwareUpdateService::pending() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (state_ != State::Pending) return std::nullopt;
    FirmwarePending p;
    p.current = deps_.running_version;
    p.next = version_;
    auto order = compare_firmware_versions(p.next, p.current);
    p.downgrade = order && *order < 0;
    return p;
}

void FirmwareUpdateService::reset_locked() {
    state_ = State::Idle;
    upload_id_ = 0;
    expected_ = 0;
    received_ = 0;
    head_.clear();
    head_.shrink_to_fit();
    identified_ = false;
    chunk_.reset();
    version_.clear();
    pending_since_ms_ = 0;
}

void FirmwareUpdateService::publish(const char* state, const std::string& version,
                                    const char* error) {
    if (!deps_.publish) return;
    Properties p{{"state", Value(state)}, {"version", Value(version)}};
    if (error != nullptr) p.emplace("error", Value(error));
    deps_.publish(std::move(p));
}

FirmwareOutcome FirmwareUpdateService::begin(size_t size, uint32_t* id) {
    if (deps_.updater == nullptr) {
        return outcome(FirmwareFailure::Unsupported, "this board has no OTA slots");
    }
    if (!deps_.request_confirm) {
        return outcome(FirmwareFailure::Unavailable, "no screen to confirm an update on");
    }
    std::lock_guard<os::Mutex> lock(mutex_);
    if (state_ != State::Idle) {
        return outcome(FirmwareFailure::Busy, "another firmware upload is in progress");
    }
    if (size > deps_.updater->capacity()) {
        return outcome(FirmwareFailure::TooLarge, "image larger than the firmware slot");
    }
    if (size < kFirmwareHeadBytes) {
        return outcome(FirmwareFailure::InvalidImage, "too short for an application image");
    }
    reset_locked();
    state_ = State::Uploading;
    upload_id_ = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    expected_ = size;
    head_.reserve(kFirmwareHeadBytes);
    if (id != nullptr) *id = upload_id_;
    WLOG_I(kTag, "receiving a %u-byte firmware image", static_cast<unsigned>(size));
    return {};
}

FirmwareOutcome FirmwareUpdateService::fail_upload(uint32_t id, FirmwareFailure failure,
                                                   std::string message, const char* event_error) {
    std::string version;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (state_ != State::Uploading || id != upload_id_) {
            return outcome(failure, std::move(message));
        }
        version = version_;
        // Under the lock: once Idle, a new upload may begin() the platform,
        // and this abort() must not land on it.
        abort_updater_locked();
        reset_locked();
    }
    if (event_error == nullptr) event_error = to_string(failure);
    WLOG_W(kTag, "firmware upload failed: %s (%s)", event_error, message.c_str());
    publish("failed", version, event_error);
    return outcome(failure, std::move(message));
}

FirmwareOutcome FirmwareUpdateService::write(uint32_t id, std::span<const uint8_t> chunk) {
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (state_ != State::Uploading || id != upload_id_) {
            return outcome(FirmwareFailure::WriteFailed, "upload is no longer active");
        }
    }
    // The fields below belong to the upload's owner (the one caller with
    // this id); nobody else touches them while the state is Uploading.
    if (chunk.size() > expected_ - received_) {
        return fail_upload(id, FirmwareFailure::InvalidImage, "more bytes than Content-Length");
    }
    if (!identified_) {
        const size_t take = std::min(kFirmwareHeadBytes - head_.size(), chunk.size());
        head_.append(reinterpret_cast<const char*>(chunk.data()), take);
        received_ += take;
        chunk = chunk.subspan(take);
        if (head_.size() < kFirmwareHeadBytes) return {};

        const std::span<const uint8_t> head(reinterpret_cast<const uint8_t*>(head_.data()),
                                            head_.size());
        auto info = parse_firmware_head(head);
        if (!info) return fail_upload(id, FirmwareFailure::InvalidImage, info.error().message);
        {
            std::lock_guard<os::Mutex> lock(mutex_);
            version_ = info.value().version;
        }
        if (!matches_target(info.value(), target_)) {
            return fail_upload(id, FirmwareFailure::WrongBoard,
                               "image is for " + info.value().project + " on board \"" +
                                   info.value().board + "\", not this device");
        }
        FirmwareUpdater* updater = deps_.updater;
        const size_t size = expected_;
        auto begun = call_on<void>(worker_, deps_.chunk_timeout_ms,
                                   [updater, size] { return updater->begin(size); });
        if (!begun) return fail_upload(id, FirmwareFailure::WriteFailed, begun.error().message);
        chunk_ = std::allocate_shared<Chunk>(os::LargeAllocator<Chunk>());
        chunk_->buf = os::LargeBuffer::allocate(kIoChunk);
        if (!chunk_->buf.ok()) {
            return fail_upload(id, FirmwareFailure::WriteFailed, "no memory for the upload buffer");
        }
        identified_ = true;
        if (auto r = gather(head); !r) {
            return fail_upload(id, FirmwareFailure::WriteFailed, r.error().message);
        }
        head_.clear();
        head_.shrink_to_fit();
    }
    if (!chunk.empty()) {
        if (auto r = gather(chunk); !r) {
            return fail_upload(id, FirmwareFailure::WriteFailed, r.error().message);
        }
        received_ += chunk.size();
    }
    return {};
}

Result<void> FirmwareUpdateService::gather(std::span<const uint8_t> bytes) {
    while (!bytes.empty()) {
        Chunk& c = *chunk_;
        const size_t take = std::min(c.buf.size() - c.used, bytes.size());
        std::memcpy(c.buf.data() + c.used, bytes.data(), take);
        c.used += take;
        bytes = bytes.subspan(take);
        if (c.used == c.buf.size()) {
            if (auto r = flush_chunk(); !r) return r;
        }
    }
    return Result<void>::ok();
}

Result<void> FirmwareUpdateService::flush_chunk() {
    if (!chunk_ || chunk_->used == 0) return Result<void>::ok();
    std::shared_ptr<Chunk> c = chunk_;
    FirmwareUpdater* updater = deps_.updater;
    auto r = call_on<void>(worker_, deps_.chunk_timeout_ms, [c, updater] {
        return updater->write(std::span<const uint8_t>(c->buf.data(), c->used));
    });
    // Only once the job has finished with it: a timed-out one still reads
    // it, and the upload ends without touching it again.
    if (r) c->used = 0;
    return r;
}

FirmwareOutcome FirmwareUpdateService::end(uint32_t id) {
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (state_ != State::Uploading || id != upload_id_) {
            return outcome(FirmwareFailure::WriteFailed, "upload is no longer active");
        }
    }
    if (!identified_ || received_ != expected_) {
        return fail_upload(id, FirmwareFailure::InvalidImage, "body shorter than Content-Length");
    }
    if (auto r = flush_chunk(); !r) {
        return fail_upload(id, FirmwareFailure::WriteFailed, r.error().message);
    }
    chunk_.reset();
    FirmwareUpdater* updater = deps_.updater;
    auto slot = std::make_shared<std::optional<Result<FirmwareImageInfo>>>();
    if (auto r = run_on(
            worker_, [updater, slot] { slot->emplace(updater->finish()); },
            deps_.finish_timeout_ms);
        !r) {
        // The device's failure, not the image's.
        return fail_upload(id, FirmwareFailure::WriteFailed, r.error().message);
    }
    Result<FirmwareImageInfo> written = std::move(**slot);
    if (!written) {
        return fail_upload(id, FirmwareFailure::InvalidImage,
                           "image failed verification: " + written.error().message);
    }
    if (!matches_target(written.value(), target_)) {
        return fail_upload(id, FirmwareFailure::WrongBoard, "written image is for another board");
    }
    std::string version;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        state_ = State::Pending;
        pending_since_ms_ = now();
        version = version_;
    }
    WLOG_I(kTag, "firmware %s received; waiting for confirmation on the device", version.c_str());
    publish("received", version);
    deps_.request_confirm();
    FirmwareOutcome done;
    done.version = std::move(version);
    return done;
}

void FirmwareUpdateService::abort(uint32_t id) {
    (void)fail_upload(id, FirmwareFailure::None, "connection closed mid-upload", "aborted");
}

FirmwareOutcome FirmwareUpdateService::confirm() {
    std::string version;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (state_ != State::Pending) {
            return outcome(FirmwareFailure::NotPending, "no firmware awaits confirmation");
        }
        // Installing keeps every other upload and answer out while the
        // platform switches the boot slot, outside the lock.
        state_ = State::Installing;
        version = version_;
    }
    FirmwareUpdater* updater = deps_.updater;
    // As long as it takes: the user has said yes, and a late switch of the
    // boot slot after a reported failure would be worse than the wait.
    if (auto r = call_on<void>(worker_, os::kForever, [updater] { return updater->activate(); });
        !r) {
        {
            std::lock_guard<os::Mutex> lock(mutex_);
            abort_updater_locked();
            reset_locked();
        }
        WLOG_E(kTag, "activating firmware %s failed: %s", version.c_str(),
               r.error().message.c_str());
        publish("failed", version, to_string(FirmwareFailure::WriteFailed));
        return outcome(FirmwareFailure::WriteFailed, r.error().message);
    }
    WLOG_W(kTag, "firmware %s confirmed; restarting into it", version.c_str());
    publish("confirmed", version);
    if (deps_.restart) {
        deps_.restart();
    } else {
        // Installing would refuse every later upload until someone
        // restarts the device by hand.
        std::lock_guard<os::Mutex> lock(mutex_);
        reset_locked();
    }
    return {};
}

FirmwareOutcome FirmwareUpdateService::decline() {
    std::string version;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (state_ != State::Pending) {
            return outcome(FirmwareFailure::NotPending, "no firmware awaits confirmation");
        }
        version = version_;
        abort_updater_locked();
        reset_locked();
    }
    WLOG_I(kTag, "firmware %s declined on the device", version.c_str());
    publish("rejected", version);
    return {};
}

void FirmwareUpdateService::tick(uint64_t now_ms) {
    std::string version;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (state_ != State::Pending || now_ms < pending_since_ms_ ||
            now_ms - pending_since_ms_ < deps_.confirm_timeout_ms) {
            return;
        }
        version = version_;
        abort_updater_locked();
        reset_locked();
    }
    WLOG_I(kTag, "firmware %s not confirmed in time; discarded", version.c_str());
    publish("rejected", version);
}

void FirmwareUpdateService::mark_running_valid() {
    FirmwareUpdater* updater = deps_.updater;
    if (updater == nullptr) return;
    if (auto r = run_on(worker_, [updater] { updater->mark_valid(); }, os::kForever); !r) {
        WLOG_E(kTag, "marking the running image valid failed: %s", r.error().message.c_str());
    }
}

void FirmwareUpdateService::abort_updater_locked() {
    FirmwareUpdater* updater = deps_.updater;
    std::function<void()> job = [updater] { updater->abort(); };
    if (worker_ == nullptr) {
        job();
        return;
    }
    if (worker_->submit(job, IoLane::Bulk)) return;
    // Not running (boot or shutdown: run_sync() runs it right here) or
    // the lane is full (wait for room).
    if (auto r = run_on(worker_, job, os::kForever); !r) {
        WLOG_W(kTag, "could not drop the partial image: %s", r.error().message.c_str());
    }
}

}  // namespace wisp::core
