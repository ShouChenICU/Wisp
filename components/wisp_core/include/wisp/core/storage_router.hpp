#pragma once

// StorageRouter: picks the storage worker for a job by the medium it
// touches. Card-only work (Medium::Sd) goes to the SD I/O worker
// "wisp.sd.io" from its start until it has been joined; everything else,
// and card work when there is no SD I/O worker, before it starts or once
// it has been joined, goes to the flash worker "wisp.fs", which may touch
// both media (wisp/core/storage_worker.hpp).
//
// The medium comes from the caller: StorageManager::medium_of() for the
// path the job works on. It sends a malformed path to Medium::Internal,
// so such a job runs on wisp.fs, where the StorageManager call rejects
// the path with InvalidArgument before any I/O. A job that touches both
// media (a scene's location record on internal flash and its files on
// the card) is Medium::Internal: only wisp.fs may touch both.
// worker_for() is the one place that decides; submit(), run_sync() and
// call_on() follow it.
//
// Contracts:
//   - Order: per worker, each lane runs in submission order; across lanes
//     and across workers there is no order. A producer that needs two
//     jobs in order on different lanes or media waits for the first
//     (run_sync(), call_on()) before submitting the second. Card work
//     falls back to wisp.fs only once the SD I/O worker has been joined
//     (StorageWorker::joined()), so it never overtakes card jobs queued
//     there: while stop_sd() drains them, new card work is refused
//     (submit() false, run_sync() Busy, which call_on() retries).
//   - Waiting: the SD I/O worker never waits for the flash worker --
//     StorageWorker::run_sync(), drain() and stop() of the flash worker
//     refuse a caller on the SD I/O thread, so a card job cannot block on
//     wisp.fs while wisp.fs waits for a card lock.
//   - Caller obligation, not checked at run time: a wisp.fs job may wait
//     for the SD I/O worker (run_sync(Medium::Sd), call_on()) only while
//     it holds no lock that a card job takes: StorageManager's sd_io_mu_
//     (held only inside one StorageManager call, so never across such a
//     wait) or any caller-side card lock (such as one serializing a
//     scene's card files). The card job would wait for that lock and
//     wisp.fs for the card job, forever.
//   - Full lanes: a full lane is reported (submit() false, run_sync()
//     Busy), never retried on the other worker, which would break the
//     order of the lane it was meant for.
//   - Like StorageWorker's, a job must not call stop_sd() or drain().

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "wisp/base/result.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

class StorageRouter {
public:
    // Both workers are owned by the caller and outlive the router; `sd`
    // is null when the board has no card or no SD I/O worker could start.
    explicit StorageRouter(StorageWorker& flash, StorageWorker* sd = nullptr);

    StorageRouter(const StorageRouter&) = delete;
    StorageRouter& operator=(const StorageRouter&) = delete;

    StorageWorker& flash() const { return flash_; }
    // Null without one.
    StorageWorker* sd() const { return sd_; }

    // The SD I/O worker for Medium::Sd while it is started() and not
    // joined(), else the flash worker.
    StorageWorker& worker_for(Medium m) const;

    // On either worker's thread.
    bool on_worker() const;

    // Queues `job` on worker_for(m). A refusal because the SD I/O worker
    // has been joined meanwhile is retried on the flash worker; a full
    // lane, or an SD I/O worker still draining, is not. False if `job` is
    // empty or it was not queued.
    bool submit(Medium m, std::function<void()> job, IoLane lane = IoLane::Normal);

    // StorageWorker::run_sync() on worker_for(m), with the same errors and
    // timeout rules, except that Medium::Sd never runs inline on the
    // caller because the SD I/O worker stopped: Busy while it drains, and
    // on the flash worker once it has been joined (which itself runs
    // inline before start() / after its own stop()). Called from an SD
    // I/O job, Medium::Sd runs inline, including while stop_sd() drains
    // the jobs queued before it.
    Result<void> run_sync(Medium m, std::function<void()> job, uint32_t timeout_ms,
                          IoLane lane = IoLane::Normal);

    // Waits until both workers are idle at the same check, or the
    // timeout; false on timeout and at once from either worker's job.
    // A job on one worker may submit to the other, so idle means no job
    // queued or running on either, read so that a hand-over in flight in
    // either direction is seen.
    bool drain(uint32_t timeout_ms);

    // Jobs submitted to either worker and not yet finished.
    size_t pending() const;

    // Drains and joins the SD I/O worker (StorageWorker::stop(): every job
    // already queued there runs first). While it drains, new Medium::Sd
    // work is refused (Busy); once it returns, Medium::Sd goes to the
    // flash worker. Idempotent; a no-op without one. Not from a job.
    void stop_sd();

private:
    StorageWorker& flash_;
    StorageWorker* sd_;
};

// fn() (returning Result<T>) on the worker for `m` and its result, waiting
// as long as it takes; with a null router, fn() inline. A full lane is
// retried for up to `busy_retry_ms` before failing with ErrorCode::Busy;
// any other refusal is returned at once. As with call_on_worker(), fn may
// capture references to the caller's stack.
template <typename T, typename Fn>
Result<T> call_on(StorageRouter* router, Medium m, Fn&& fn, IoLane lane = IoLane::Normal,
                  uint32_t busy_retry_ms = 2000) {
    if (router == nullptr) return fn();
    std::optional<Result<T>> out;
    std::function<void()> job = [&out, &fn] { out.emplace(fn()); };
    Result<void> r = router->run_sync(m, job, os::kForever, lane);
    constexpr uint32_t kPollMs = 10;
    for (uint32_t waited = 0; !r && r.error().code == ErrorCode::Busy && waited < busy_retry_ms;
         waited += kPollMs) {
        os::sleep_ms(kPollMs);
        r = router->run_sync(m, job, os::kForever, lane);
    }
    if (!r) return Result<T>::err(r.error().code, r.error().message);
    return std::move(*out);
}

}  // namespace wisp::core
