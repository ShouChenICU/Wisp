#pragma once
// LibrarySink: a stream of bytes produced on another task (an HTTP body,
// an upload) into one file of the media library (media spec 3.5). The
// MediaLibrary::Writer behind it lives on the library's storage worker,
// as Writer requires: open(), every append and the commit or abort run as
// library jobs (MediaLibrary::submit(), Bulk lane, so they keep their
// order), and the producer only copies into PSRAM and queues.
//
// Back-pressure: push() waits while more than kMaxQueued bytes wait for
// the worker, so a fast network never piles the body up in memory; a
// failure or cancel() ends the wait. A sink never outlives its library
// (the Writer's own rule); stop() of the library waits for every job a
// sink queued.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/media/library.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::media {

class LibrarySink : public std::enable_shared_from_this<LibrarySink> {
public:
    // Appends go to the worker in pieces of this size.
    static constexpr size_t kChunk = 32 * 1024;
    // How much may wait for the worker before push() waits.
    static constexpr size_t kMaxQueued = 128 * 1024;
    // How long finish() retries a full lane.
    static constexpr uint32_t kFinishWaitMs = 5000;

    using Opened = std::function<void(Result<std::shared_ptr<LibrarySink>>)>;
    using Finished = std::function<void(Result<uint64_t>)>;

    // Begins the write of `path` on the library's worker and calls
    // `opened` there with the sink, or with the error (invalid_path,
    // media_unavailable, busy ...). Not queued (library stopping, lane
    // full): `opened` gets Busy at once on the caller.
    static void open(MediaLibrary& lib, std::string path, Opened opened);

    ~LibrarySink();
    LibrarySink(const LibrarySink&) = delete;
    LibrarySink& operator=(const LibrarySink&) = delete;

    // On the producer: copies `chunk` and queues its append in kChunk
    // pieces. False once a write failed or cancel() was called (the
    // transfer is then to be stopped).
    bool push(std::span<const uint8_t> chunk);

    // Once the transfer is over: queues behind the appends a job that
    // with `ok` writes what is left and commits (`done` gets the file's
    // size), otherwise aborts (`done` gets 0, or the write error that
    // stopped it; after cancel() always 0). `done` runs on the worker. A
    // full lane is retried for up to `wait_ms`, except on the worker
    // itself (a request refused inside open()'s callback), where the lane
    // cannot drain meanwhile and one try is made. False when it was not
    // queued: `done` is then never called, and what was written is left
    // for the library's next listing of the folder to remove. Only the
    // first call does anything (later ones return false).
    bool finish(bool ok, Finished done, uint32_t wait_ms = kFinishWaitMs);

    // Any thread, without touching the card: push() returns false (waking
    // one that waits) and nothing more is written. finish(false, ...)
    // still removes what was written.
    void cancel();

    // The first write error, if any.
    std::optional<Error> error() const;

private:
    LibrarySink(MediaLibrary& lib, std::unique_ptr<MediaLibrary::Writer> writer);

    // Queues the append of `buf`; false when it was refused.
    bool dispatch(os::LargeBuffer buf);
    // On the worker.
    void append(const os::LargeBuffer& buf);
    void run_finish(bool ok, const Finished& done);
    // With mu_.
    bool stopped_locked() const { return error_.has_value() || cancelled_ || finished_; }

    MediaLibrary& lib_;
    // Touched on the worker only (and by the destructor once nothing else
    // holds the sink).
    std::unique_ptr<MediaLibrary::Writer> writer_;
    mutable os::Mutex mu_;
    os::Semaphore wake_{1, 0};
    os::LargeBuffer staging_;
    size_t staged_ = 0;
    size_t queued_ = 0;
    std::optional<Error> error_;
    bool cancelled_ = false;
    bool finished_ = false;
};

}  // namespace wisp::media
