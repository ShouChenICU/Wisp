#pragma once

// Portable storage manager: tiered System/Library/Backup logical areas on
// top of the two fixed storage devices every board registers --
// "storage.int" (always-present internal flash, LittleFS on the ESP port)
// and, optionally, "storage.sd" (a removable SD card, FAT on the ESP
// port). See AGENTS.md's storage rule: every persistent write in Wisp goes
// through write_atomic() here, never a raw fopen/fwrite elsewhere.
//
// Area root rules (spec 7.2):
//   System  = [<int>/system]
//   Library = [<sd>/wisp/library, <int>/library] when the SD card is
//             online, else [<int>/library]
//   Backup  = [<sd>/wisp/backup] when the SD card is online, else []
// (<int>/<sd> are storage.int/storage.sd's mount_point()). roots(Area)
// returns this list; write_root(Area) returns its first entry, or
// ErrorCode::StorageUnavailable if the list is empty (only possible for
// Area::Backup with no SD card online).
//
// Atomic writes, and why: on the device, <int> is LittleFS and <sd> is FAT
// (FatFS) -- the *same* write_atomic() code must behave correctly on both,
// which rules out the usual "write a temp file then rename() over the
// target" trick: FatFS's rename() fails if the target already exists,
// while LittleFS's doesn't care either way. write_atomic() therefore uses
// *two* temp names: write "<path>.tmp" -> fflush+fsync -> rename(tmp,
// "<path>.new") (the ".new" name is never already occupied at this point,
// so this rename works on FAT too) -> remove(path) if it exists ->
// rename("<path>.new", path). ".new" only ever holds a complete, synced
// write -- it's the name that's safe to resurrect. ".tmp" can be a half
// -written file at any time (e.g. power lost mid-fwrite on a *first*
// write to a path, before any ".new"/target ever existed) and is never
// treated as valid data: both write_atomic() and read_file() recover a
// missing target by promoting a leftover "<path>.new" if present (at the
// very start of write_atomic(), before anything else: otherwise a
// *second* power cut, between one write's completed-and-synced ".new"
// and its final rename, followed by a fresh write_atomic() call to the
// same path, would have that fresh call's own `remove("<path>.new")`
// bookkeeping silently discard the still-good orphaned data instead of
// recovering it), and otherwise discard any leftover "<path>.tmp" as
// garbage (best-effort, errors ignored) and report the file as not
// found (ErrorCode::NotFound). See AGENTS.md's "FAT 上的原子写" trap;
// two temp names (rather than just ".tmp") are what let this recovery
// distinguish a complete-but-unrenamed write from a torn first write --
// a single-".tmp" scheme can't tell those apart.
//
// Because ".tmp" and ".new" are reserved for this bookkeeping, a caller
// may not write_atomic()/read_file() a path whose last component itself
// ends in ".tmp" or ".new": such a file would silently escape the
// library quota (dir_size() excludes those
// suffixes), get clobbered by another write to the same base name's own
// "<path>.new"/".tmp" juggling, or be promoted/discarded as if it were
// write_atomic()'s own bookkeeping. Rejected with
// ErrorCode::InvalidArgument before any other check.
//
// Quota: writes under "<int>/library/" are accounted against
// StorageConfig::internal_library_quota (recursively summing that
// directory's current on-disk size -- excluding any "*.tmp"/"*.new" temp
// files, which are transient bookkeeping, not library content -- minus
// the target file's current size if it's being overwritten, plus the new
// data's size); exceeding the quota fails with ErrorCode::QuotaExceeded
// and performs no write. The SD card's library tier is not quota-limited
// (bounded only by the card's own capacity). Every path given to
// write_atomic()/read_file() is lexically normalized first (repeated '/'
// collapsed, '.' components dropped, trailing '/' dropped; mount points
// are normalized the same way in init()) before any prefix check or I/O,
// so a non-canonical alias of a quota-checked path (e.g. a doubled slash,
// or a "." component) can't be used to dodge the quota -- a ".."
// component is rejected outright (ErrorCode::InvalidArgument) rather than
// resolved, since resolving it would require knowing the filesystem's
// current state.
//
// Append-only logs: append() is the one exception to "every persistent
// write goes through write_atomic()", and the only way to extend a file in
// place. Each call opens the file in append mode, writes, then fflush()es
// and fsync()s before returning, so once append() succeeds its bytes are
// durable. A power cut during a call can leave any prefix of that call's
// data at the end of the file -- typically half a record. append() does no
// framing: the caller writes self-delimiting records (e.g. one line each)
// and must drop a trailing incomplete one when reading the file back.
// Appending is not atomic the way write_atomic() is, so never append to a
// file whose readers expect it to be all-or-nothing.
//
// If append() fails it truncates the file back to its size before the
// call, so after a failed append the file is unchanged (unless that
// truncate fails too, which the error message then says). A power cut is
// different: it can leave a torn tail that nothing undoes. A caller that
// finds one when reading must rewrite the file without it (write_atomic())
// before appending again; appending after it would bury the torn bytes
// mid-file.
//
// The other primitives below (list_dir(), remove_tree(), rename(),
// tree_size(), read_file_large()) share write_atomic()'s path rules:
// lexical normalization, ".." rejected, and a last component ending in
// ".tmp"/".new" rejected with ErrorCode::InvalidArgument. Mutating calls
// on a path under an offline SD card fail with StorageUnavailable.
//
// Depth: no path more than 16 levels below a storage mount point is
// accepted by write_atomic(), append(), rename() or the other primitives
// (ErrorCode::InvalidArgument), so the manager never builds a tree its
// own bounded walks (remove_tree(), the quota walk) would refuse. Those
// walks are iterative because they run on the 4 KB wisp.fs stack.
//
// Containment: remove_tree() and both ends of rename() are destructive
// enough that they check for themselves what they may touch: an absolute
// path strictly inside storage.int's mount or an online storage.sd's
// mount, and not a mount point, an area root (<int>/system,
// <int>/library, <sd>/wisp/library, <sd>/wisp/backup) or <sd>/wisp.
// Anything else fails with ErrorCode::InvalidArgument. Directories inside
// a mount but outside the areas (e.g. <int>/scenes) are allowed.
//
// Symlinks: list_dir() and remove_tree() never follow one (a link is
// listed as EntryKind::Other and removed as itself). On the device neither
// LittleFS nor FAT has symlinks and the libc has no lstat(), so there they
// use stat(); see stat_nofollow() in the .cpp.
//
// Hot-plug: refresh() re-probes storage.sd's online() state. A
// offline->online transition (re)creates <sd>/wisp/library and
// <sd>/wisp/backup (logging a warning and leaving the card marked online
// if that fails, e.g. a write-protected card -- the same degrade-not-fail
// policy init() uses) and publishes "storage.sd.inserted"; an
// online->offline transition publishes "storage.sd.removed". Neither
// event is published by init() itself, only by a later refresh() call
// that observes a change -- callers (e.g. a 2s poll timer, not added by
// this task) are expected to call refresh() periodically. Every public
// method here serializes on one mutex, so refresh() and a concurrent
// write_atomic()/read_file() never interleave -- but this also means a
// card that's inserted and removed again entirely between two refresh()
// polls is never observed at all: no event is published for it, the same
// way a button press shorter than ButtonService's poll period produces
// no events. refresh() must be called often enough for a caller's
// purposes to tolerate that.
//
// Only <cstdio>, <dirent.h>, <sys/stat.h>, <unistd.h> are used for all
// filesystem access (no std::filesystem) -- those are available in both
// the host simulator's libc and ESP-IDF's newlib (via its VFS), so the
// exact same code runs on both ports -- see docs/architecture.md's
// dependency rules on why POSIX file APIs are allowed here.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/storage.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class Area { System, Library, Backup };

struct StorageConfig {
    uint64_t internal_library_quota = 3ull * 1024 * 1024;  // 3 MiB; see AGENTS.md/spec 7.2
};

struct MediaStatus {
    std::string id;
    bool online = false;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

struct StorageStatus {
    std::vector<MediaStatus> media;
};

enum class EntryKind { File, Dir, Other };

struct DirEntry {
    std::string name;
    EntryKind kind = EntryKind::Other;
    uint64_t size = 0;  // bytes for a File, 0 otherwise
    // Last modification, seconds since the epoch, as the filesystem keeps
    // it (0 when it cannot be read). Only as good as the clock was when the
    // file was written: before a time sync the device's clock starts at 0.
    int64_t mtime = 0;
};

class StorageManager {
public:
    StorageManager(DeviceRegistry& registry, EventBus& bus, StorageConfig config = {});

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    // Looks up "storage.int" (required) and "storage.sd" (optional) in the
    // registry, and creates the area roots that should exist right now:
    // <int>/system, <int>/library always; <sd>/wisp/library and
    // <sd>/wisp/backup if storage.sd is present and currently online().
    // Fails with ErrorCode::StorageUnavailable (without creating anything,
    // and leaving every other method returning StorageUnavailable/empty
    // roots -- see roots()/write_atomic()/read_file() below) if
    // "storage.int" isn't registered -- every board must have one, so its
    // absence means storage is unusable, not merely degraded. A failure
    // to create <int>'s directories (e.g. a read-only internal
    // filesystem, or a non-directory file already sitting at that path --
    // see mkdir_p() in the .cpp) fails init() itself with
    // ErrorCode::IoError. A failure to create the *SD* card's directories
    // only logs a warning and does not fail init() (storage.sd is left
    // marked online; a write into the directory that failed to create
    // will surface its own clear IoError) -- internal storage being fine
    // shouldn't be reported as "storage unavailable" just because a write
    // -protected or corrupt SD card couldn't be prepared, and this matches
    // refresh()'s same-situation handling below. Calling init() again
    // (e.g. to pick up a storage.int that was re-registered) first marks
    // this manager not-ready, so if the re-init itself fails partway, it
    // is left unusable rather than silently still operating on the
    // previous call's now-stale mount points.
    Result<void> init();

    // Returns the current root directories for `area`, most-preferred
    // first, per the rules in the file header. Never fails. Returns an
    // empty vector for every area before init() has succeeded (so a
    // caller can't be handed a bogus root like "/system" built from an
    // empty mount point), and for Area::Backup whenever no SD card is
    // online.
    std::vector<std::string> roots(Area area) const;

    // Returns roots(area).front(), i.e. the single root new writes for
    // `area` should target. Fails with ErrorCode::StorageUnavailable if
    // roots(area) is empty (including before init() has succeeded).
    Result<std::string> write_root(Area area) const;

    // Atomically (see file header) writes `data` to `path`. Fails with
    // ErrorCode::StorageUnavailable if init() hasn't succeeded yet, or if
    // `path` falls under the SD card's mount point while the card is
    // offline. `path` is lexically normalized first (see file header);
    // if it has a ".." component (checked before normalization, since
    // resolving one isn't attempted) this fails with
    // ErrorCode::InvalidArgument, so a caller can't use one to escape its
    // intended area root (e.g. to dodge the library quota below) --
    // likewise if its last component ends in the reserved ".tmp" or
    // ".new" suffix (see file header). If `path` doesn't exist but
    // "<path>.new" does (an earlier write_atomic() call that completed
    // and synced but whose final rename was interrupted -- see file
    // header), that leftover is promoted to `path` first, before
    // anything else. If the normalized path falls under
    // "<int>/library/", the write is additionally accounted against
    // StorageConfig::internal_library_quota (see file header); exceeding
    // it fails with ErrorCode::QuotaExceeded and performs no write.
    // Missing parent directories of `path` are created (like `mkdir -p`;
    // a non-directory in the way fails with ErrorCode::IoError).
    // Any other I/O failure fails with ErrorCode::IoError, with
    // strerror(errno) folded into the message.
    Result<void> write_atomic(const std::string& path, std::span<const uint8_t> data);

    // Reads the full contents of `path` (normalized the same way as
    // write_atomic(); a ".." component, or a last component ending in
    // the reserved ".tmp"/".new" suffix, fails with
    // ErrorCode::InvalidArgument). Fails with ErrorCode::StorageUnavailable
    // if init() hasn't succeeded yet. If `path` doesn't exist but
    // "<path>.new" does (a write_atomic() that was interrupted after
    // deleting the target but before the final rename -- see file
    // header), the leftover ".new" file is promoted (renamed to `path`)
    // first, then read; if neither `path` nor "<path>.new" exists, any
    // leftover "<path>.tmp" is discarded as garbage (best-effort) and
    // this fails with ErrorCode::NotFound -- a ".tmp" alone is never
    // promoted. Fails with ErrorCode::InvalidArgument if `path` names a
    // directory rather than a regular file. Any other failure fails with
    // ErrorCode::IoError, with strerror(errno) folded into the message.
    Result<std::vector<uint8_t>> read_file(const std::string& path);

    // Same as read_file(), but the contents land in an os::LargeBuffer
    // (Mem::Large, PSRAM on the device) instead of a std::vector, which
    // would land in internal RAM below the PSRAM malloc threshold. Fails
    // with ErrorCode::NoMemory if that buffer cannot be allocated.
    Result<os::LargeBuffer> read_file_large(const std::string& path);

    // At most `len` bytes of `path` from `offset` (fewer at the end of the
    // file, none past it), in Mem::Large. The same path rules, ".new"
    // recovery and errors as read_file_large(); NotFound if missing.
    Result<os::LargeBuffer> read_range(const std::string& path, uint64_t offset, size_t len);

    // Appends `data` to `path`, creating it (and missing parent
    // directories) if needed, then fsync()s. A failed append leaves the
    // file as it was -- see the file header, also for power loss. A leftover "<path>.new" is
    // promoted first, as in write_atomic(). Under "<int>/library/" the
    // appended bytes count against the quota; exceeding it fails with
    // ErrorCode::QuotaExceeded and leaves the file unchanged.
    Result<void> append(const std::string& path, std::span<const uint8_t> data);

    // The entries of directory `path`, sorted by name, without "." and
    // "..". Fails with ErrorCode::NotFound if `path` does not exist and
    // ErrorCode::InvalidArgument if it is not a directory. Every entry is
    // listed as it is on disk, including any "*.tmp"/"*.new" write
    // bookkeeping; callers that list data files skip those names.
    Result<std::vector<DirEntry>> list_dir(const std::string& path);

    // list_dir() one entry at a time, in directory order (unsorted),
    // without holding the whole listing: `visit` returns false to stop.
    // It runs with the manager's lock held, so it must not call back into
    // this class.
    Result<void> visit_dir(const std::string& path,
                           const std::function<bool(const DirEntry&)>& visit);

    // Removes the file or directory tree at `path` without following
    // symlinks. Removing a file also removes its "<path>.tmp"/"<path>.new"
    // bookkeeping, so read_file() cannot resurrect it afterwards. A
    // missing `path` is success. `path` must pass the containment rule
    // in the file header (else ErrorCode::InvalidArgument). A tree deeper
    // than 16 levels below `path` fails with ErrorCode::IoError, leaving
    // whatever was not yet removed.
    Result<void> remove_tree(const std::string& path);

    // Renames the file or directory `from` to `to`, creating `to`'s
    // missing parent directories. Fails with ErrorCode::NotFound if
    // `from` does not exist and ErrorCode::AlreadyExists if `to` does --
    // checked here, so the result is the same on FAT (whose rename()
    // refuses an existing target) and LittleFS (whose rename() replaces
    // it). Moving into "<int>/library/" from outside it counts the moved
    // tree's size against the quota. Both paths must pass the
    // containment rule in the file header (else ErrorCode::InvalidArgument)
    // and be on the same storage device.
    Result<void> rename(const std::string& from, const std::string& to);

    // The total size of the regular files at or under `path`, excluding
    // "*.tmp"/"*.new" bookkeeping; 0 if `path` does not exist.
    Result<uint64_t> tree_size(const std::string& path);

    // storage.int's normalized mount point ("<int>"). Fails with
    // ErrorCode::StorageUnavailable before init() has succeeded.
    Result<std::string> internal_root() const;

    // Re-probes storage.sd's online() state and, on a change since the
    // last init()/refresh(), (re)creates its area roots (on an
    // offline->online transition; a creation failure only logs a warning,
    // same as init() -- see its doc comment) and publishes
    // "storage.sd.inserted" or "storage.sd.removed" (source "storage.sd")
    // accordingly. A no-op if "storage.sd" isn't registered, or if its
    // online() state hasn't changed since the last call -- see the file
    // header's note on what that means for a card inserted and removed
    // between two polls. Intended to be called periodically (e.g. by a 2s
    // timer) by a caller outside this class; see file header.
    void refresh();

    // A snapshot of every registered storage device's current id/online/
    // capacity, for diagnostics (e.g. a self-check screen). Slow, and not
    // for the UI thread: it takes mutex_, the same lock write_atomic()
    // holds across its fsync()+rename() calls (see file header), and
    // measuring capacity reaches the medium -- on LittleFS it walks every
    // block in use, so it takes longer the more is stored. The self-check
    // screen reads it on the storage worker
    // (SceneWiring::storage_status_async() in wisp_app).
    StorageStatus status() const;

    // Forwards to storage.sd's hal::Storage::set_standby() (a no-op if "storage.sd"
    // isn't registered, or if its implementation doesn't override the
    // hal::Storage default) -- see that method's doc comment. A no-op
    // for "storage.int": internal flash has no background polling to
    // pause in the first place. Called by PowerManager on enter_standby()
    // (standby=true) and exit_standby() (standby=false); does not change
    // sd_online_ or publish any event -- refresh() keeps running exactly
    // as before (spec: "StorageManager 的 2 s 状态刷新保持不变"), it just
    // observes whatever online() returns while the underlying device's
    // own detection is paused.
    //
    // Unlike every other method here, this one does NOT take mutex_ --
    // see its own doc comment in storage_manager.cpp for why that's
    // still safe.
    void set_standby(bool standby);

private:
    // Shared argument handling for the primitives above, called with
    // mutex_ held: the ready_ check, normalization, and the ".."/reserved
    // suffix rules; `mutating` adds the offline-SD check.
    Result<std::string> checked_path(const std::string& raw, bool mutating) const;

    // read_file()'s checks and ".new" recovery; returns the normalized
    // path and the regular file's size. Called with mutex_ held.
    Result<std::pair<std::string, uint64_t>> open_for_read(const std::string& raw);

    // Fails with ErrorCode::InvalidArgument if normalized `path` lies
    // more than 16 levels below a storage mount point. Called with mutex_
    // held.
    Result<void> check_depth(const std::string& path) const;

    // Fails with ErrorCode::InvalidArgument unless normalized `path` is
    // strictly inside storage.int's mount or an online storage.sd's mount
    // and is not a mount point, an area root or "<sd>/wisp". Called with
    // mutex_ held.
    Result<void> check_contained(const std::string& path) const;

    // Whether `path` is under the quota-checked "<int>/library".
    bool in_internal_library(const std::string& path) const;

    DeviceRegistry& registry_;
    EventBus& bus_;
    StorageConfig config_;

    // Guards every member below against concurrent calls -- refresh() may
    // be driven by a periodic timer task while writes come from service
    // tasks (see AGENTS.md's thread-safety expectations for this class).
    // Held across EventBus::publish() calls in refresh(), which is safe
    // because publish() never blocks on (or calls back into) a handler --
    // see event_bus.hpp.
    mutable os::Mutex mutex_;

    std::shared_ptr<hal::Storage> int_dev_;
    // Assigned exactly once, inside init() (re-init()ing re-assigns it,
    // but no caller does that concurrently with set_standby() in
    // practice -- App::start() calls init() once, before anything can
    // call set_standby()); set_standby() above relies on that to read it
    // without mutex_.
    std::shared_ptr<hal::Storage> sd_dev_;
    std::string int_mount_;  // never has a trailing '/'; see init()
    std::string sd_mount_;   // ditto; empty if storage.sd isn't registered
    bool sd_online_ = false;

    // True once init() has found "storage.int" and created its
    // directories; roots()/write_atomic()/read_file() all refuse to do
    // anything (StorageUnavailable) while this is false, rather than
    // operating on empty int_mount_/sd_mount_ strings.
    bool ready_ = false;
};

}  // namespace wisp::core
