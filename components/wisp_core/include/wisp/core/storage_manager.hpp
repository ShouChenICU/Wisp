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
// while LittleFS's doesn't care either way. write_atomic() therefore
// always does: write "<path>.tmp" -> fflush+fsync -> remove(path) if it
// exists -> rename(tmp, path). If power is lost between the remove() and
// the rename(), the target is gone but "<path>.tmp" still holds the new
// data; read_file() recovers from exactly that state by renaming the
// leftover "<path>.tmp" back to "<path>" before reading. See
// AGENTS.md's "FAT 上的原子写" trap and global-constraints.md Review
// Focus #1.
//
// Quota: writes under "<int>/library/" are accounted against
// StorageConfig::internal_library_quota (recursively summing that
// directory's current on-disk size, minus the target file's current size
// if it's being overwritten, plus the new data's size); exceeding the
// quota fails with ErrorCode::QuotaExceeded and performs no write. The SD
// card's library tier is not quota-limited (bounded only by the card's own
// capacity).
//
// Hot-plug: refresh() re-probes storage.sd's online() state. A
// offline->online transition (re)creates <sd>/wisp/library and
// <sd>/wisp/backup and publishes "storage.sd.inserted"; an
// online->offline transition publishes "storage.sd.removed". Neither
// event is published by init() itself, only by a later refresh() call
// that observes a change -- callers (e.g. a 2s poll timer, not added by
// this task) are expected to call refresh() periodically.
//
// Only <cstdio>, <dirent.h>, <sys/stat.h>, <unistd.h> are used for all
// filesystem access (no std::filesystem) -- those are available in both
// the host simulator's libc and ESP-IDF's newlib, so the exact same code
// runs on both ports; see global-constraints.md's portable-layer rule.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/storage.hpp"
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

class StorageManager {
public:
    StorageManager(DeviceRegistry& registry, EventBus& bus, StorageConfig config = {});

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    // Looks up "storage.int" (required) and "storage.sd" (optional) in the
    // registry, and creates the area roots that should exist right now:
    // <int>/system, <int>/library always; <sd>/wisp/library and
    // <sd>/wisp/backup only if storage.sd is present and currently
    // online(). Fails with ErrorCode::StorageUnavailable (without creating
    // anything) if "storage.int" isn't registered -- every board must
    // have one, so its absence means storage is unusable, not merely
    // degraded. A directory-creation failure (e.g. a read-only internal
    // filesystem) fails with ErrorCode::IoError.
    Result<void> init();

    // Returns the current root directories for `area`, most-preferred
    // first, per the rules in the file header. Never fails; an empty
    // vector is possible only for Area::Backup with no SD card online.
    std::vector<std::string> roots(Area area) const;

    // Returns roots(area).front(), i.e. the single root new writes for
    // `area` should target. Fails with ErrorCode::StorageUnavailable if
    // roots(area) is empty.
    Result<std::string> write_root(Area area) const;

    // Atomically (see file header) writes `data` to `path`. `path` must
    // not contain a ".." component (checked lexically, not resolved against
    // the filesystem) -- fails with ErrorCode::InvalidArgument otherwise,
    // so a caller can't use one to escape its intended area root (e.g. to
    // dodge the library quota below). Fails with
    // ErrorCode::StorageUnavailable if `path` falls under the SD card's
    // mount point while the card is offline. If `path` falls under
    // "<int>/library/", the write is additionally accounted against
    // StorageConfig::internal_library_quota (see file header); exceeding
    // it fails with ErrorCode::QuotaExceeded and performs no write.
    // Any other I/O failure fails with ErrorCode::IoError, with
    // strerror(errno) folded into the message.
    Result<void> write_atomic(const std::string& path, std::span<const uint8_t> data);

    // Reads the full contents of `path`. If `path` doesn't exist but
    // "<path>.tmp" does (a write_atomic() that was interrupted after
    // deleting the target but before the final rename -- see file
    // header), the leftover tmp file is first renamed back to `path`,
    // then read. Fails with ErrorCode::InvalidArgument for a ".."
    // component, or ErrorCode::IoError for any other failure (including
    // neither `path` nor its ".tmp" existing).
    Result<std::vector<uint8_t>> read_file(const std::string& path);

    // Re-probes storage.sd's online() state and, on a change since the
    // last init()/refresh(), (re)creates its area roots (on an
    // offline->online transition) and publishes "storage.sd.inserted" or
    // "storage.sd.removed" (source "storage.sd") accordingly. A no-op if
    // "storage.sd" isn't registered, or if its online() state hasn't
    // changed. Intended to be called periodically (e.g. by a 2s timer) by
    // a caller outside this class; see file header.
    void refresh();

    // A snapshot of every registered storage device's current id/online/
    // capacity, for diagnostics (e.g. a self-check screen).
    StorageStatus status() const;

private:
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
    std::shared_ptr<hal::Storage> sd_dev_;
    std::string int_mount_;
    std::string sd_mount_;
    bool sd_online_ = false;
};

}  // namespace wisp::core
