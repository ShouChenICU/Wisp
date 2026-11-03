#include "wisp/core/firmware_update.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "firmware";

FirmwareOutcome outcome(FirmwareFailure failure, std::string message) {
    return FirmwareOutcome{failure, std::move(message)};
}

}  // namespace

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

FirmwareUpdateService::FirmwareUpdateService(Deps deps) : deps_(std::move(deps)) {
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
        deps_.updater->abort();
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
        if (auto r = deps_.updater->begin(expected_); !r) {
            return fail_upload(id, FirmwareFailure::WriteFailed, r.error().message);
        }
        if (auto r = deps_.updater->write(head); !r) {
            return fail_upload(id, FirmwareFailure::WriteFailed, r.error().message);
        }
        identified_ = true;
        head_.clear();
        head_.shrink_to_fit();
    }
    if (!chunk.empty()) {
        if (auto r = deps_.updater->write(chunk); !r) {
            return fail_upload(id, FirmwareFailure::WriteFailed, r.error().message);
        }
        received_ += chunk.size();
    }
    return {};
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
    auto written = deps_.updater->finish();
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
    if (auto r = deps_.updater->activate(); !r) {
        {
            std::lock_guard<os::Mutex> lock(mutex_);
            deps_.updater->abort();
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
        deps_.updater->abort();
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
        deps_.updater->abort();
        reset_locked();
    }
    WLOG_I(kTag, "firmware %s not confirmed in time; discarded", version.c_str());
    publish("rejected", version);
}

void FirmwareUpdateService::mark_running_valid() {
    if (deps_.updater != nullptr) deps_.updater->mark_valid();
}

}  // namespace wisp::core
