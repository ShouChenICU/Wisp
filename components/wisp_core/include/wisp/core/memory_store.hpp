#pragma once

// MemoryStore: the agent's long-term memory notes (mvp spec 5.1), e.g.
// what to call the user or a standing preference -- never a one-off fact.
// Persisted as one JSON document, <path>'s directory created on demand:
// {"version":1,"next_id":n,"notes":[{"id","text","updated"},...]}, ids in
// ascending order. `next_id` is the id the next add() would use; keeping
// it in the file (rather than deriving it from the highest surviving
// note) means a removed note's id is never handed out again, even across
// a reload.
//
// Capacity (spec 5.1): at most kMaxNotes notes, each at most kMaxChars
// Unicode code points (not bytes, so CJK text is not penalized). add()
// past the limit fails with QuotaExceeded "memory_full"; either call with
// empty text fails with InvalidArgument "empty", and with text over the
// limit, InvalidArgument "too_long". update()/remove() fail with NotFound
// "not_found" for an unknown id.
//
// Threading: every mutating call and load() take one mutex for their
// whole body, including the write_atomic() and the event publish, so two
// threads calling concurrently (the agent's tools and, later, the admin
// page) can never race on next_id or interleave two notes.json writes.
// Per AGENTS.md section 5, only wisp.fs (the flash thread) may actually
// reach flash: callers reach this class already on that thread -- the
// agent's tools declare ToolThread::Fs, so the tool registry dispatches
// them there, and AgentWiring loads this store on the storage worker
// before the agent starts. A write that fails leaves the in-memory notes
// as they were before the call (the attempted change is rolled back) so
// memory and disk never drift apart; a used id is still never reused,
// successful write or not.
//
// list()/count() read the in-memory notes under the same mutex and never
// touch flash, so they are safe to call from wisp.agent (whose stack may
// be in PSRAM) or any other thread without a flash-thread hop.
//
// A damaged notes.json must never be silently replaced by the next write
// -- that would destroy whatever the user actually had saved. load()
// tells the two ways a file can be damaged apart:
//  - It exists but can't even be read (anything other than NotFound,
//    e.g. a read error, or the path turning out to be a directory): load()
//    sets an internal "unavailable" flag and returns the error. While
//    that flag is set, add()/update()/remove() refuse to run (and so
//    never call write_atomic()) with ErrorCode::StorageUnavailable
//    "memory_unavailable"; list()/count() just report zero notes. clear()
//    is the one call that still works: it is the explicit "start over"
//    override, and on success it writes a fresh empty file and clears the
//    flag. A later load() that succeeds also clears it.
//  - It reads fine but the contents are unparsable, or its "version"
//    field isn't 1 (a future, incompatible format): load() renames it
//    aside to "<path>.bad" (replacing any older one) instead of touching
//    it further, logs one warning with no file content, and continues
//    with an empty, writable store -- same as a missing file. If that
//    rename itself fails, load() falls back to the unavailable state
//    above rather than risk the next write clobbering an unreadable but
//    not-yet-quarantined file.
// Either way, every individual note record is also re-validated on a
// successful parse (id 0, a duplicate id, empty text or over kMaxChars is
// dropped; at most the first kMaxNotes surviving notes are kept), so a
// hand-edited file cannot smuggle in anything add()/update() would have
// refused -- a dropped count is logged, never which notes or why.
//
// Privacy: note text never appears in a log line or an event payload.
// Every change publishes memory.changed {"count"} (source "memory") with
// the new note count only.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

struct MemoryNote {
    uint32_t id = 0;
    os::LargeString text;
    int64_t updated = 0;  // wall_s() at the last add() or update()
};

class MemoryStore {
public:
    static constexpr size_t kMaxNotes = 64;
    static constexpr size_t kMaxChars = 200;

    // `bus`: null publishes nothing (tests that do not need the event).
    // `wall_s`: empty stamps every note with 0.
    MemoryStore(StorageManager& storage, std::string path, EventBus* bus,
                std::function<int64_t()> wall_s);

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    // Reads `path`: a missing file is not an error (a fresh, empty
    // store). See the file header for how an unreadable or unparsable
    // file is handled instead. Call once, on the flash thread, before any
    // mutating call.
    Result<void> load();

    // A copy of the notes, ascending by id. Memory only; never touches
    // flash.
    os::LargeVector<MemoryNote> list() const;
    // list().size(), without the copy.
    size_t count() const;

    Result<uint32_t> add(std::string_view text);
    Result<void> update(uint32_t id, std::string_view text);
    Result<void> remove(uint32_t id);
    // Removes every note (e.g. the admin page's "clear all"); ids already
    // handed out are still never reused.
    Result<void> clear();

private:
    os::LargeString serialize_locked() const;
    Result<void> persist_and_publish_locked();
    // The file exists but isn't parsable, or names an unsupported
    // version: moves it aside to "<path_>.bad" and continues empty and
    // writable, or falls back to load_failed_ if even that fails. See
    // the file header.
    Result<void> quarantine_locked();

    StorageManager& storage_;
    std::string path_;
    EventBus* bus_;
    std::function<int64_t()> wall_s_;

    mutable os::Mutex mutex_;
    os::LargeVector<MemoryNote> notes_;  // ascending by id
    uint32_t next_id_ = 1;
    // Set when the file exists but could not even be read; see the file
    // header. Cleared by a successful load() or clear().
    bool load_failed_ = false;
};

}  // namespace wisp::core
