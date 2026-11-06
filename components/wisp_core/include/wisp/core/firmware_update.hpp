#pragma once

// Firmware upload from the admin page (connectivity spec 6.5): the service
// layer between the POST /system/firmware route and the platform's OTA
// writer.
//
// One upload at a time goes through these states:
//
//   Idle --begin()--> Uploading --end()--> Pending --confirm()--> Installing
//     ^                  |                    |                      |
//     +----- abort(), ---+---- decline(), ----+-- activate failed ---+
//            a bad image       tick() after confirm_timeout_ms
//
// Every drop of the image goes through a short Aborting state while the
// platform's abort is queued. begin() refuses a second upload (Busy) until
// the first one has left Pending (or Aborting). write() checks the image as soon as its first
// kFirmwareHeadBytes have arrived and refuses a foreign one before a single
// byte reaches FirmwareUpdater::write(). end() has the platform verify the
// whole image, publishes system.firmware {state:"received", version} and
// asks the device's user to confirm (Deps::request_confirm). confirm()
// makes the new image the boot image, publishes {state:"confirmed"} and
// calls Deps::restart; decline() and the confirmation timeout publish
// {state:"rejected"} and drop the image. A failure after begin() publishes
// {state:"failed", version, error} (spec 5.5). Every outcome frees the slot
// for the next upload, except a confirm() that worked (the device is
// restarting).
//
// Threading: every method may be called from any thread. The state lives
// under one mutex that is never held while the platform writes flash, so
// status()/pending() (read by the UI thread) never wait for an erase. Only
// the caller holding the current upload id calls write()/end(), so the
// platform sees one writer at a time.
//
// Flash (spec 3.6 of docs/superpowers/specs/2026-11-06-flash-thread-design.md):
// with a StorageWorker every FirmwareUpdater call that touches flash runs
// on it, in its Bulk lane. write() gathers the image into one kIoChunk
// PSRAM buffer and hands it over each time it fills, waiting for that
// chunk (chunk_timeout_ms) before taking more, so the uploader is never
// more than one chunk ahead and queued settings or scene saves wait for
// at most one chunk. end() hands over the rest and waits for finish()
// (finish_timeout_ms); confirm() and mark_running_valid() wait for
// activate()/mark_valid() as long as they take. A chunk that does not
// finish in time fails the upload with WriteFailed; it may still run
// later, so it owns its buffer, and the abort queued behind it runs after
// it. abort() reaches the platform as a queued job and never blocks the
// caller. Without a worker the calls go straight to the platform.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/firmware_image.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

// The platform's OTA writer (app::Platform::firmware): the ESP port writes
// the inactive ota_N slot with esp_ota_*; the simulator writes
// <data-dir>/firmware.bin. Calls come one at a time (see above).
class FirmwareUpdater {
public:
    virtual ~FirmwareUpdater() = default;
    // Board name, project and chip the running image was built for; an
    // upload must match them.
    virtual FirmwareTarget target() const = 0;
    // Name of the slot the running image came from ("ota_0").
    virtual std::string running_slot() const = 0;
    // Largest image the slot an upload goes to can hold.
    virtual size_t capacity() const = 0;
    // Starts writing an image of `size` bytes, dropping whatever an
    // earlier, unfinished one left behind.
    virtual Result<void> begin(size_t size) = 0;
    // The next bytes of the image, in order.
    virtual Result<void> write(std::span<const uint8_t> chunk) = 0;
    // All bytes written: verifies the whole image (as the bootloader
    // would, SHA-256 included) and identifies what was actually written.
    virtual Result<FirmwareImageInfo> finish() = 0;
    // Drops the image being written or the finished one that will not be
    // installed. Safe in any state.
    virtual void abort() = 0;
    // Boots the finished image at the next restart (rollback armed: it has
    // to mark itself valid, see mark_valid()).
    virtual Result<void> activate() = 0;
    // The running image started up properly: cancels the rollback a fresh
    // update is booted with. A no-op when none is pending.
    virtual void mark_valid() = 0;
};

// Failures with their own API error code (spec 6.1/6.5).
enum class FirmwareFailure {
    None,
    Unsupported,   // no OTA slots on this board
    Unavailable,   // nothing on the device can confirm an update
    Busy,          // another upload is in progress or awaiting confirmation
    TooLarge,      // larger than the slot
    InvalidImage,  // not an ESP application image, or it failed verification
    WrongBoard,    // an image for another project, board or chip
    WriteFailed,   // the platform could not write or activate it
    NotPending,    // confirm()/decline() with nothing awaiting confirmation
};

// "unsupported", "unavailable", "busy", "payload_too_large",
// "invalid_image", "wrong_board", "write_failed", "not_pending"; "" for None.
const char* to_string(FirmwareFailure failure);

struct FirmwareOutcome {
    FirmwareFailure failure = FirmwareFailure::None;
    std::string message;  // English detail; never names a path
    // end(): the version of the image now awaiting confirmation. Read it
    // here rather than from pending(), which the device may already have
    // answered.
    std::string version;
    bool ok() const { return failure == FirmwareFailure::None; }
};

// GET /system/firmware (spec 6.5).
struct FirmwareStatus {
    bool supported = false;
    std::string version;                 // the running image's
    std::string board;                   // compiled into the running image; "" without OTA
    std::string running;                 // the running slot; "" without OTA
    std::optional<std::string> pending;  // version awaiting confirmation
};

// What the device's confirmation dialog shows.
struct FirmwarePending {
    std::string current;
    std::string next;
    // compare_firmware_versions() says the new one is older.
    bool downgrade = false;
};

class FirmwareUpdateService {
public:
    static constexpr uint32_t kConfirmTimeoutMs = 2 * 60 * 1000;
    static constexpr uint32_t kChunkTimeoutMs = 10 * 1000;
    static constexpr uint32_t kFinishTimeoutMs = 30 * 1000;

    struct Deps {
        // Null: this board has no OTA slots; every upload is Unsupported.
        FirmwareUpdater* updater = nullptr;
        std::string running_version;
        // system.firmware payloads (spec 5.5); the App publishes them.
        std::function<void(Properties)> publish;
        // Shows the confirmation dialog. Empty: nothing on the device can
        // confirm, so uploads are refused as Unavailable.
        std::function<void()> request_confirm;
        // Restarts into the activated image; called after "confirmed".
        std::function<void()> restart;
        std::function<uint64_t()> now_ms;
        uint32_t confirm_timeout_ms = kConfirmTimeoutMs;
        // How long the uploader waits for the flash thread to take one
        // chunk (begin() included), and for the final verification.
        uint32_t chunk_timeout_ms = kChunkTimeoutMs;
        uint32_t finish_timeout_ms = kFinishTimeoutMs;
    };

    explicit FirmwareUpdateService(Deps deps, StorageWorker* worker = nullptr);

    FirmwareUpdateService(const FirmwareUpdateService&) = delete;
    FirmwareUpdateService& operator=(const FirmwareUpdateService&) = delete;

    FirmwareStatus status() const;
    // Set while an image awaits the user's answer.
    std::optional<FirmwarePending> pending() const;

    // --- The upload (POST /system/firmware). ---
    // Claims the single upload slot for an image of `size` bytes; *id
    // names it in the calls below. Unsupported, Unavailable, Busy,
    // TooLarge, InvalidImage (smaller than an image header) or WriteFailed.
    FirmwareOutcome begin(size_t size, uint32_t* id);
    // The next bytes, in order. InvalidImage / WrongBoard / WriteFailed
    // end the upload (published as "failed"); so does writing more than
    // begin()'s size.
    FirmwareOutcome write(uint32_t id, std::span<const uint8_t> chunk);
    // Every byte arrived: verify and wait for the device's answer.
    FirmwareOutcome end(uint32_t id);
    // The connection went away mid-upload: publishes "failed" with error
    // "aborted" and frees the slot. Ignored for a stale id.
    void abort(uint32_t id);

    // --- The device's answer. ---
    // Activates the pending image, publishes "confirmed" and restarts.
    // Without Deps::restart the image boots at the next restart and the
    // slot is free again.
    // Blocks while the platform checks the image again (seconds on the
    // device): never call it on the UI thread.
    FirmwareOutcome confirm();
    // Drops the pending image and publishes "rejected".
    FirmwareOutcome decline();
    // Rejects a pending image nobody answered for confirm_timeout_ms.
    void tick(uint64_t now_ms);

    // The running image booted far enough to be kept (after App::start()
    // has reported the boot).
    void mark_running_valid();

private:
    // Aborting: the partial image is being dropped; like Installing it
    // keeps every other upload and answer out until it is Idle again.
    enum class State { Idle, Uploading, Pending, Installing, Aborting };

    // Under mutex_: back to Idle for the next upload.
    void reset_locked();
    // Called without mutex_, with the state set to Aborting under it:
    // queues FirmwareUpdater::abort() behind every earlier call (waiting
    // for room when the Bulk lane is full), then resets to Idle. Only then
    // can a later upload begin(), so its begin() lands after the abort;
    // the wait for room holds no lock, so status() and pending() never
    // wait on the flash thread.
    void abort_updater_and_reset();
    // Adds `bytes` to chunk_, handing each full chunk to the platform.
    Result<void> gather(std::span<const uint8_t> bytes);
    // Hands the gathered bytes to the platform and waits for them.
    Result<void> flush_chunk();

    // Ends the current upload with `failure`, published as "failed" with
    // `event_error` (default: to_string(failure)). A stale id only returns.
    FirmwareOutcome fail_upload(uint32_t id, FirmwareFailure failure, std::string message,
                                const char* event_error = nullptr);
    void publish(const char* state, const std::string& version, const char* error = nullptr);
    uint64_t now() const;

    struct Chunk;

    Deps deps_;
    StorageWorker* worker_ = nullptr;
    FirmwareTarget target_;

    mutable os::Mutex mutex_;
    State state_ = State::Idle;
    uint32_t next_id_ = 1;
    uint32_t upload_id_ = 0;
    size_t expected_ = 0;
    size_t received_ = 0;
    // The first kFirmwareHeadBytes, held until the image is identified.
    std::string head_;
    bool identified_ = false;
    // Bytes gathered for the next FirmwareUpdater::write(), once the image
    // is identified. Shared with the job writing it, which may outlive a
    // timed-out wait.
    std::shared_ptr<Chunk> chunk_;
    std::string version_;  // of the image being uploaded or awaiting an answer
    uint64_t pending_since_ms_ = 0;
};

}  // namespace wisp::core
