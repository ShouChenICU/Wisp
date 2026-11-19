#pragma once
// The media library on the SD card (media spec 3): the folders
// music/, download/ and document/ under `<sd root>/media`, file operations
// on them, and an index of the tracks under music/ kept in
// `<root>/.index.json`.
//
// Threads. Every card access runs on wisp.sd.io through the StorageRouter
// (AGENTS.md section 5); with no SD I/O worker the router sends it to
// wisp.fs. The asynchronous calls queue a job and call `done` from that
// job, so `done` runs on the storage worker: it keeps to memory work and
// posts anything else onward. A call refused before it is queued (bad
// path, no card, full lane, stopped library) calls `done` at once on the
// caller's thread. Library calls use the Normal lane, scans the Bulk lane
// one directory listing or one file's tag read per job, so a scan holds
// up a playback read on the Interactive lane for at most one tag read.
//
// State. state() is the source of truth for availability and the index's
// revision (AGENTS.md section 3, "State and events"): consumers re-query
// tracks() when `revision` changes and keep no copy of the index.
//
// Paths are relative to the media root and checked by wisp/media/path.hpp:
// reads by normalize(), listings by normalize_dir(), writes, removals and
// folders by normalize_for_write(). Without a mounted card every call fails
// with StorageUnavailable.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::media {

// The most read() and read_now() return per call (media spec 3.4).
inline constexpr size_t kMaxReadBytes = 64 * 1024;
// The largest page tracks() returns (media spec 3.4).
inline constexpr uint32_t kMaxTrackPage = 200;
// Format version of the index file; any other version is rebuilt.
inline constexpr int64_t kIndexVersion = 1;
// The index file in the media root. Hidden, so no path accepted by
// normalize() can name it.
inline constexpr char kIndexFile[] = ".index.json";

struct MediaEntry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;  // bytes for a file, 0 for a folder
    int64_t mtime = 0;  // seconds since the epoch, as the card keeps it
    bool operator==(const MediaEntry&) const = default;
};

struct Track {
    std::string id;  // the path relative to the media root, "music/..."
    std::string title, artist, album;
    uint32_t duration_s = 0;  // 0 = unknown
    uint64_t size = 0;
    int64_t mtime = 0;
    bool operator==(const Track&) const = default;
};

// `artist` and `album` match exactly when non-empty; `q` is an ASCII
// case-insensitive substring of title, artist or album. `limit` is capped
// at kMaxTrackPage.
struct TrackQuery {
    std::string q, artist, album;
    uint32_t offset = 0, limit = 50;
};

struct TrackPage {
    std::vector<Track> items;  // in id order
    uint32_t total = 0;        // matches before paging
};

struct MediaLibraryState {
    bool available = false;  // a card is mounted and the index loaded
    bool scanning = false;
    uint32_t track_count = 0;
    uint32_t revision = 0;  // bumped on every change of the index
    bool operator==(const MediaLibraryState&) const = default;
};

struct MediaLibraryDeps {
    core::StorageManager& storage;
    // Null runs every job inline on the caller's thread (tests).
    core::StorageRouter* router;
    core::EventBus& bus;  // the state cell's
    std::string root;     // "<sd root>/media"
    // Publishes media.library on each state change; may be null.
    core::StateCell<MediaLibraryState>::Mirror mirror = nullptr;
};

class MediaLibrary {
public:
    template <class T>
    using Done = std::function<void(Result<T>)>;

    // A streaming write (uploads, downloads): appends go to a hidden
    // ".<name>.part" beside the target; commit() renames it to
    // ".<name>.done", removes the old file and renames ".done" to the
    // target -- the two-stage rename FAT needs (AGENTS.md section 8). A
    // ".done" left without its target is promoted the next time its folder
    // is listed or scanned; a ".part" nobody writes is removed then.
    // Every method, the destructor included, runs on wisp.sd.io (or
    // wisp.fs without it); a writer never outlives its library. Destroyed
    // without commit(), it aborts.
    class Writer {
    public:
        ~Writer();
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // Fails with StorageUnavailable once the card has been removed or
        // swapped since begin_write(); any failure aborts the writer.
        Result<void> append(std::span<const uint8_t> data);
        // Replaces the target; for a track, updates its index entry.
        Result<void> commit();
        // Removes what was written; the target is left as it was.
        void abort();

        uint64_t written() const { return written_; }

    private:
        friend class MediaLibrary;
        Writer(MediaLibrary& lib, uint32_t gen, std::string rel);
        Result<void> flush();
        void finish();

        MediaLibrary& lib_;
        uint32_t gen_;
        std::string rel_, abs_, part_, done_;
        os::LargeBuffer staging_;
        size_t staged_ = 0;
        uint64_t written_ = 0;
        bool finished_ = false;
    };

    explicit MediaLibrary(MediaLibraryDeps deps);
    ~MediaLibrary();  // stop()

    MediaLibrary(const MediaLibrary&) = delete;
    MediaLibrary& operator=(const MediaLibrary&) = delete;

    // Follows the card (StorageManager::sd_cell()): on mount, creates the
    // three folders, loads the index and scans music/; on removal, clears
    // the index. A no-op without a removable medium.
    void start();
    // Stops following the card, refuses new calls and waits for the jobs
    // already queued: no `done` runs after it returns. Not from a storage
    // job. Idempotent.
    void stop();

    const core::StateCell<MediaLibraryState>& state() const { return state_; }

    // `dir` empty lists the root. Hidden names and write bookkeeping are
    // left out; sorted by name.
    void list(std::string dir, Done<std::vector<MediaEntry>> done);
    void stat(std::string path, Done<MediaEntry> done);
    // At most kMaxReadBytes from `off`; fewer at the end of the file.
    void read(std::string path, uint64_t off, size_t len, Done<os::LargeBuffer> done);
    // Atomic; creates parent folders; updates the index for a track.
    void write(std::string path, os::LargeBuffer data, Done<void> done);
    // A file, or an empty folder (InvalidArgument when it holds anything).
    void remove(std::string path, Done<void> done);
    void mkdir(std::string path, Done<void> done);
    // Memory only: `done` runs before tracks() returns, on the caller.
    void tracks(TrackQuery query, Done<TrackPage> done);
    // Walks music/ again (also mounts a card whose mount was refused by a
    // full lane); completion shows in state().
    void rescan();

    // Queues `job` on the card's worker as one of the library's own jobs,
    // so stop() waits for it: for streams a caller drives through a Writer
    // (wisp/media/library_sink.hpp). False when the library is stopping
    // or the lane is full; `job` is then dropped unrun. Without a router
    // it runs inline.
    bool submit(core::IoLane lane, std::function<void()> job);

    // On wisp.sd.io (or wisp.fs without it) only.
    Result<std::unique_ptr<Writer>> begin_write(std::string path);
    Result<os::LargeBuffer> read_now(const std::string& path, uint64_t off, size_t len);
    Result<MediaEntry> stat_now(const std::string& path);

    // Memory only.
    std::optional<Track> track(const std::string& id) const;

    // Called on the storage worker after a file or folder was removed
    // through the library, with its path (the player skips a removed
    // current track). Keeps to memory work.
    void set_on_removed(std::function<void(const std::string& path)> cb);

    // How many files had their tags read since construction (tests and
    // diagnostics: a rescan of an unchanged card reads none).
    uint64_t tag_reads() const { return tag_reads_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    friend struct Impl;
    friend class Writer;

    core::StorageManager& storage_;
    core::StateCell<MediaLibraryState> state_;
    std::atomic<uint64_t> tag_reads_{0};
    os::LargeUnique<Impl> impl_;
};

}  // namespace wisp::media
