#pragma once

// LatestSave: "last snapshot wins" persistence for a service that keeps its
// whole state in memory and rewrites one file with it (spec 3.5 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md): Wi-Fi,
// pairing, model profiles.
//
// The service serializes its state under its own lock and hands save() a
// write that owns that snapshot. With a `submit_io` (the storage worker in
// production) the write is not run on the caller: at most one job is
// queued per LatestSave, and a save() made before that job has started only
// replaces the write it will run, so a burst of changes costs one flash
// write, of the newest state. Without `submit_io` (tests that have no
// worker) save() runs the write at once and returns its result.
//
// The write runs on whichever thread runs the job, after the caller may
// have gone, so it must own everything it touches: the snapshot by value,
// and only objects that outlive the worker (StorageManager). It never
// points at the service. It logs its own failure; a failed write is not
// retried by itself, the next save() (a full snapshot again) replaces it.
// Failures are counted as CachedSettingsBackend counts them (spec 3.4):
// the third failed write in a row calls `on_persist_failed` once, and a
// successful one starts the count again.
//
// Thread-safe. When submit_io refuses a job while another thread's save()
// replaced the write meanwhile (and returned Ok, seeing a job about to be
// queued), the refused save() submits again for it, and gives up (Busy)
// only once no newer save() came in during its last attempt; that newer
// write then waits for the next save(). The shared state lives
// in a shared_ptr the queued job holds, so a LatestSave may be destroyed
// while its job is still queued; the job then runs the write it last got.

#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/base/result.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

class LatestSave {
public:
    // Queues a job; false when it could not (the job then never runs).
    using SubmitIo = std::function<bool(std::function<void()>)>;
    using Write = std::function<Result<void>()>;

    explicit LatestSave(SubmitIo submit_io = {});

    LatestSave(const LatestSave&) = delete;
    LatestSave& operator=(const LatestSave&) = delete;

    // Without submit_io: runs `write` and returns its result. With it:
    // makes `write` the next one to run, replacing one not yet started, and
    // queues a job unless one is queued already; Ok once queued. Busy when
    // submit_io refused: nothing is queued then, and `write` waits for the
    // next save(), which replaces it with a newer snapshot.
    Result<void> save(Write write);

    // True while a write is queued and has not started.
    bool pending() const;

    // Called, without any lock held, on the thread that ran the third
    // failed write in a row (the worker, or the caller without
    // submit_io). May be empty; set it before the first save().
    void set_on_persist_failed(std::function<void()> fn);

    // Failed writes in a row before on_persist_failed is called.
    static constexpr int kFailuresToReport = 3;

private:
    struct Shared {
        mutable os::Mutex mutex;
        Write next;
        // Bumped by every save(), so a refused submit can tell whether
        // `next` is still its own.
        uint64_t generation = 0;
        bool queued = false;
        int failures = 0;
        std::function<void()> on_persist_failed;
    };

    static void run(const std::shared_ptr<Shared>& shared);
    // Counts `r` and calls on_persist_failed when due.
    static void report(Shared& shared, const Result<void>& r);

    SubmitIo submit_io_;
    std::shared_ptr<Shared> shared_;
};

}  // namespace wisp::core
