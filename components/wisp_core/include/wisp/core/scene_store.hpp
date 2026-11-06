#pragma once

// SceneStore: one scene's persistent data -- a key/value space and up to
// 16 collections of JSON records -- behind the Lua `wisp.store` API
// (spec 5.4, 6.1).
//
// Reads and writes work on an in-memory index and return at once; the
// index is persisted asynchronously as append-only logs under
// `data_dir`: "kv.log" for the key/value space and "<collection>.log"
// for each collection. Every line is one JSON object,
// {"op":"put","id":<id>,"v":<value>} or {"op":"del","id":<id>}, where a
// key/value id is the key string and a collection id an integer. Every
// line ends in '\n', so on load the bytes after the last '\n' are a tail
// torn by a power cut: they are dropped with a warning and the file is
// rewritten (write_atomic) before anything is appended to it again, or
// the next append would glue a new record onto the torn bytes.
//
// Persistence: a write changes the index, then appends its line to that
// log's pending buffer (PSRAM). The first pending write arms a one-shot
// `schedule(coalesce_ms)`; when it fires, a flush job goes to
// `submit_io` (the wisp.fs worker in production). The job takes every
// pending buffer and appends each to its file. A power cut can leave any
// prefix of an append behind, so a flush touching more than one file, or
// holding a committed batch of more than one line, first writes all of
// its lines to "journal.json" (write_atomic) and a batch survives whole:
// load() replays a journal it finds (put/del of whole records is
// idempotent), appends the replayed lines and deletes the journal. A
// failed flush is retried after coalesce_ms, the delay doubling up to
// 5 s, until one succeeds or the store is closed. At most one timer or
// retry is in flight at a time.
// Instead of appending, a job rewrites a file with only its live records
// (write_atomic) when its dead bytes exceed half its live bytes, or when
// more than compact_append_bytes would have been appended since the last
// rewrite. A rewritten collection keeps a {"op":"del"} line for its
// highest id when that record is gone, and a batch that hands out an id
// and deletes it again writes that delete too, so ids stay monotonic
// across a reload.
//
// The flush job and the timer hold a shared_ptr to the store, so a flush
// already scheduled or queued completes after the owner lets go. The
// scene host's exit path calls flush(), then close(), then drops the
// store: close() stops the timers and retries, so a store whose final
// flush failed cannot later append its older lines behind a newer store
// opened on the same directory.
//
// Threads: the scene task reads and writes, the wisp.fs worker flushes.
// One mutex guards the index and the pending buffers; it is never held
// across file I/O. Flush jobs are serialized among themselves (and with
// load()) by a second mutex that is held across their I/O. Apart from
// load(), no method does file I/O on the caller's thread.
//
// Batches: between begin_batch() and commit_batch() writes go to an
// overlay that reads already see; commit_batch() moves the whole overlay
// into the index and the pending buffers under one lock, so a flush sees
// either none or all of it, and abort_batch() drops it. Batches nest and
// all levels share one overlay: an aborted inner batch fails the whole
// batch (see commit_batch()).
//
// Data version (scene-device spec 7.3): "meta.json" holds
// {"schema_version":n}, the structure version of the data. Data without
// it (written before versions existed) is version 1; a store with no data
// file at all is new (0) until set_initial_version(). A version change is
// written by the next flush, before any log line of that flush, and a
// migration's batch and its new version go through one journal (its
// entry {"f":"meta.json","r":{"schema_version":n}}), so a power cut keeps
// both or neither.
//
// Limits (spec 5.6): used_bytes() -- what the logs will total once every
// pending line is written -- may not exceed quota_bytes. A write that
// would pass it is still accepted if the logs rewritten with only their
// live records would fit and no more than quota_bytes are pending; the
// next flush then rewrites every file that has dead lines. Otherwise it
// fails with QuotaExceeded
// "quota_exceeded" and changes nothing. A record (the encoded value) over
// record_bytes fails with InvalidArgument "record_too_large", a new
// collection past max_collections with QuotaExceeded
// "too_many_collections". Values must be single-line JSON (no raw CR or
// LF, which JSON encoders escape anyway); a collection record must be a
// JSON object, since find() filters and sorts on its top-level fields.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

struct SceneStoreLimits {
    uint64_t quota_bytes = 1024 * 1024;
    size_t record_bytes = 16 * 1024;
    size_t max_collections = 16;
    uint32_t coalesce_ms = 300;
    uint64_t compact_append_bytes = 64 * 1024;
    size_t key_bytes = 64;
};

struct SceneStoreDeps {
    StorageManager& storage;
    // Runs a job on the storage worker; false if it could not be queued.
    // Production: StorageWorker::submit.
    std::function<bool(std::function<void()>)> submit_io;
    // Calls `cb` once after `delay_ms`. Production: a one-shot os::Timer.
    std::function<void(uint32_t delay_ms, std::function<void()> cb)> schedule;
};

// Top-level field equality filter, sort and paging for find(). A number
// equals a number of the same value (3 == 3.0); lists and maps compare
// as Values. `id` keeps only the record with that id. `sort` orders
// numbers by value and strings by bytes; records whose sort field is
// missing (or another type) come last whatever `desc` says, and equal
// values keep ascending id order. Without `sort` records come in id
// order, newest first with `desc`.
struct FindQuery {
    std::optional<int64_t> id;
    std::vector<std::pair<std::string, Value>> equals;
    std::string sort;
    bool desc = false;
    size_t limit = SIZE_MAX;
    size_t offset = 0;
};

struct StoreRecord {
    int64_t id = 0;
    os::LargeString json;
};

class SceneStore : public std::enable_shared_from_this<SceneStore> {
    struct Private {};

public:
    static std::shared_ptr<SceneStore> create(SceneStoreDeps deps, std::string data_dir,
                                              SceneStoreLimits limits = {});

    SceneStore(Private, SceneStoreDeps deps, std::string data_dir, SceneStoreLimits limits);
    ~SceneStore();

    SceneStore(const SceneStore&) = delete;
    SceneStore& operator=(const SceneStore&) = delete;

    // Reads every log into the index, rewrites any file with a torn tail
    // or unreadable lines, then replays and deletes a leftover journal.
    // Blocks; call it once, on the storage worker, before any write.
    // Writes before a successful load() fail with StorageUnavailable.
    Result<void> load();

    std::optional<os::LargeString> kv_get(std::string_view key) const;
    // `json` is the encoded value. Keys are 1..key_bytes bytes.
    Result<void> kv_set(std::string_view key, std::string_view json);
    // Deleting a missing key succeeds and writes nothing.
    Result<void> kv_del(std::string_view key);

    // Collection names match ^[a-z0-9_-]{1,32}$ and are not "kv". insert()
    // creates the collection; the other calls fail with NotFound (update,
    // remove) or return nothing (find) for one that does not exist.
    Result<int64_t> insert(std::string_view coll, std::string_view json);
    // Replaces the whole record; NotFound if `id` does not exist.
    Result<void> update(std::string_view coll, int64_t id, std::string_view json);
    Result<void> remove(std::string_view coll, int64_t id);
    Result<os::LargeVector<StoreRecord>> find(std::string_view coll, const FindQuery& q) const;

    // Batches nest: only the outermost commit_batch() applies the
    // overlay. An inner abort_batch() closes its level and marks the batch
    // aborted; writes until the outermost commit still go to the overlay
    // (so none of them reaches the index), and that commit drops the
    // overlay and fails with InvalidArgument "batch_aborted". The Lua
    // binding re-raises its error after an inner abort, so the outer
    // batch normally aborts too; "batch_aborted" surfaces only when scene
    // code catches the inner error with pcall. An outermost
    // abort_batch() drops the overlay at once. commit_batch() without an
    // open batch fails with InvalidArgument "no_batch"; abort_batch()
    // without one does nothing.
    void begin_batch();
    Result<void> commit_batch();
    void abort_batch();

    // The data version load() found (see above), or the one set since.
    int data_version() const;
    // A new store (data_version() == 0) records `v` as its version; any
    // other store keeps its own.
    void set_initial_version(int v);
    // Commits the open batch, which must be the outermost one, together
    // with `new_version` (one journal, see above). InvalidArgument
    // "no_batch" without an open batch, and with an inner one still open
    // the whole batch is dropped and it fails with InvalidArgument
    // "batch_open"; otherwise it fails as commit_batch() would, and the
    // version stays.
    Result<void> commit_migration(int new_version);

    // Queues a flush job now (scene close, screen off). `done` runs on the
    // worker after the job, or at once with ErrorCode::Busy if the job
    // could not be queued.
    void flush(std::function<void(Result<void>)> done = {});

    // Stops the store for good: a timer or retry that fires afterwards
    // does nothing, and writes and flush() fail with StorageUnavailable
    // "closed". A flush job already queued still runs, so what was written
    // before close() and taken by that job still reaches the disk; a job
    // the coalescing timer or a retry queued does nothing. The
    // scene host calls it after the final flush() when a scene exits; a
    // store reopened on the same directory then owns it alone. Anything
    // that final flush could not write is lost, and its `done` says so.
    void close();

    // The total size of the logs once every pending line is written.
    uint64_t used_bytes() const;

    const SceneStoreLimits& limits() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // commit_batch(), and with a version commit_migration().
    Result<void> commit(std::optional<int> new_version);
    void arm_timer();
    void arm_timer(uint32_t delay_ms);
    void schedule_retry();
    void on_timer();
    // `from_timer`: queued by on_timer(); such a job does nothing once the
    // store is closed, while a flush() job still runs.
    void flush_job(const std::function<void(Result<void>)>& done, bool from_timer);
};

}  // namespace wisp::core
