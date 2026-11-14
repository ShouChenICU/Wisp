#pragma once

// SceneFiles: each scene's private, flat directory of files behind the Lua
// wisp.files API and wisp.http.download (scene-device spec 7.1, 7.2).
//
// Location: the first write (write, append or a download) picks where the
// scene's files live from the manifest's "files" declaration
// (SceneFilesConfig::declared): "auto" takes the SD card when one is
// online and internal storage otherwise, "sd" only the card (no card:
// "unavailable", never internal storage), "int" always internal storage.
// The choice is recorded in <data_root>/<id>/data/files_loc.json
// ({"location":"sd"|"int"}, write_atomic) and never changes after that: a
// record that disagrees with a later declaration wins (one warning when
// it is loaded) and nothing is moved.
//   sd:  <sd>/wisp/library/scenes/<id>/files   quota sd_quota (64 MB)
//   int: <data_root>/<id>/files                quota int_quota (512 KB)
// (on the device <data_root> is <int>/scenes). A scene whose files are on
// the card, or that declares "sd" and has none yet, gets "unavailable"
// from every call while the card is out.
//
// Nothing here deletes a scene's files on its own: remove() and
// remove_all() are asked for. Files on the card with no installed scene
// (the scene was deleted while the card was out) are only logged, by
// scan() and on_sd_changed().
//
// The agent's own files (agent-tools spec 4.2): the id kAgentFilesId
// ("@agent", which valid_scene_id() never accepts, so no scene can take
// it) is the only one outside the scene id rules. It is chosen and counted
// like a scene that declares "auto", with the same quotas, but lives in
// the Library instead:
//   sd:  <sd>/wisp/library/agent/files
//   int: <int>/library/agent/files, record <int>/library/agent/files_loc.json
//
// Names: valid_file_name(), one level only. Names starting with '.' are
// this class's own bookkeeping and never listed; names ending in ".tmp" or
// ".new" are StorageManager's (write_atomic()) and refused too.
//
// Writes: write() replaces the file through write_atomic(); append()
// extends it through StorageManager::append() (a failed append leaves it
// as it was). Both refuse more than max_write bytes ("too_large"), read()
// more than max_read ("too_large").
//
// Usage: counted per scene the first time it is needed (scan() does every
// scene at boot) and then kept up to date by each write; a write that
// would take the scene past its quota fails with "quota_exceeded" and
// writes nothing. Downloads in flight reserve what they have received so
// far, so two of them cannot both fit in the room for one.
//
// Downloads use the two-phase naming of AGENTS.md section 8 at the file
// level: the body goes to ".<name>.dl" (possibly half written, so garbage
// after a power cut), and a finished one is renamed ".<name>.done" (known
// complete), then the old <name> is removed and ".<name>.done" renamed to
// <name>. Counting a directory (scan(), on_sd_changed(), first use)
// removes every ".dl" that no download in flight owns and finishes every
// ".done" (removing a <name> still beside it, since the ".done" is the
// newer one -- unless that <name> is a directory, which is left alone
// together with the ".done", and logged).
//
// FAT (the SD card) compares names without case, so on the card a name
// that differs from an existing one only in case is that file: every call
// (write, append, read, remove, a download and its in-flight check, the
// directory count) acts on the spelling already on disk. The ".tmp" and
// ".new" endings are refused in any case for the same reason.
//
// Adoption: a scene with no location record whose declaration allows the
// card ("auto" or "sd") takes over <sd>/.../scenes/<id>/files when it
// exists and the card is in -- files a delete with the card out left
// behind -- on its first call of any kind, read-only ones included.
//
// Errors carry these messages, which scene_files_code() hands to scripts:
// InvalidArgument "bad_name", InvalidArgument "too_large", QuotaExceeded
// "quota_exceeded", StorageUnavailable "unavailable", NotFound
// "not_found". Any other storage failure keeps its code and message.
//
// Threads: every SceneFiles method does file work, so it runs on a
// storage worker or on a task that may access flash (an httpd thread),
// never on the scene task or the UI thread. Work that touches only the
// card may run on the SD I/O worker "wisp.sd.io" (StorageWorker,
// WorkerRole::SdIo), which never touches internal flash: there a method
// works only on a scene whose record is loaded with location Sd, and
// fails Busy ("busy") for any other, without reading the location record
// or asking `declared` (both are internal flash). Everything else --
// loading or choosing a location, adoption, scan(), remove_all(), the
// orphan log -- runs on wisp.fs, which may touch the card too.
//
// Routing (submit()): a scene's job goes to Medium::Internal (wisp.fs)
// while the scene has a job routed Internal that has not finished, or its
// record is not loaded, or its location is not Sd; otherwise to
// Medium::Sd. The location never moves from Sd back to Internal at run
// time (only scan() at boot and remove_all() unload it), so one scene's
// jobs keep their order across the two workers: a later job can only
// move from wisp.fs to wisp.sd.io once every earlier one has finished.
// A card job that runs after its scene's record was unloaded (the scene
// removed meanwhile) fails Busy; one that runs while the card is out
// fails "unavailable" without card I/O.
//
// Locks, outer first: io_int_mu_ -> io_sd_mu_ -> (declared: the
// repository's lock) -> StorageManager's int_io_mu_ -> sd_io_mu_. On
// wisp.sd.io a method takes io_sd_mu_ only. Elsewhere it takes io_int_mu_,
// plus io_sd_mu_ unless the scene's record is loaded with location
// Internal; scan(), remove_all(), sd_changed_begin() and sd_changed_end()
// take both. A record's `loaded` and `loc` change only with both held, so
// either lock keeps them stable for a method's duration. So card work of
// one scene never waits for an internal scene's work, and the SD I/O
// thread never waits for anything wisp.fs holds except io_sd_mu_, which
// wisp.fs holds only across card I/O and never while waiting for
// wisp.sd.io. A third lock (mu_), never held across I/O, guards the usage
// table, so a FileDownload::push() on the HTTP task never waits for flash.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/flash_guard.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class FilesLocation : uint8_t { None, Sd, Internal };

// "none", "sd", "int".
const char* to_string(FilesLocation l);

struct SceneFileEntry {
    // In PSRAM: a list holds up to max_list of them, and a std::string
    // past its inline 15 bytes would put each one in internal RAM.
    os::LargeString name;
    uint64_t size = 0;
    int64_t mtime = 0;  // DirEntry::mtime
};

// list(): the first max_list names in order; `truncated` when there were
// more.
struct SceneFileList {
    os::LargeVector<SceneFileEntry> entries;
    bool truncated = false;
};

struct SceneFilesInfo {
    // None until the scene first writes; quota is then what it would get now.
    FilesLocation location = FilesLocation::None;
    FilesWant want = FilesWant::Auto;  // the manifest's declaration
    uint64_t quota = 0;
    uint64_t used = 0;
};

// The longest single write() or append() (spec 7.1).
inline constexpr size_t kMaxFileWrite = 256 * 1024;

struct SceneFilesConfig {
    std::string data_root;  // SceneRepoConfig::data_root
    uint64_t sd_quota = 64ull * 1024 * 1024;
    uint64_t int_quota = 512 * 1024;
    size_t max_write = kMaxFileWrite;  // at most kMaxFileWrite
    size_t max_read = 64 * 1024;
    // list() returns at most this many names (the first ones in order).
    size_t max_list = 256;
    // The installed scene's "files" declaration (SceneManifest::files);
    // NotFound when no scene of that id is installed. Called on wisp.fs
    // with this class's locks held, so it must not call back into it.
    // Empty: every scene declares Auto and none is reported as missing.
    std::function<Result<FilesWant>(std::string_view scene)> declared;
    // Queues a job on the worker for the medium it touches
    // (StorageRouter::submit); false: not queued. Used by submit() and a
    // download's appends. Empty: jobs run on the caller's thread, which
    // must then be allowed the scene's medium.
    std::function<bool(Medium, IoLane, std::function<void()>)> submit_io;
    // Test seam: called with the scene's I/O locks held, before a public
    // method's file work. Empty in production.
    std::function<void(std::string_view scene)> on_io;
};

// The agent's files (file header).
inline constexpr const char* kAgentFilesId = "@agent";

// [A-Za-z0-9._-]{1,64}, not starting with '.', not ending in ".tmp" or
// ".new".
bool valid_file_name(std::string_view name);

// The snake_case code a script sees for a SceneFiles error: one of the
// messages in the file header, else wisp::to_string(e.code).
const char* scene_files_code(const Error& e);

// <sd>/wisp/library/scenes/<id> while the SD card is online, else "".
std::string sd_scene_dir(const StorageManager& storage, std::string_view id);

class FileDownload;

class SceneFiles {
public:
    SceneFiles(StorageManager& storage, SceneFilesConfig config);

    SceneFiles(const SceneFiles&) = delete;
    SceneFiles& operator=(const SceneFiles&) = delete;

    // Counts every scene that has a location record (recovering downloads
    // as the file header says). A scene whose card is out is counted when
    // it comes back. Logs files on the card that belong to no installed
    // scene. Fails only if <data_root> cannot be listed. On wisp.fs.
    Result<void> scan();
    // The card came or went: the scenes on it are counted again (now, if
    // it is in), and its files without a scene are logged. On wisp.fs; the
    // composition of the three calls below, which let the counting run on
    // wisp.sd.io instead.
    void on_sd_changed();
    // Marks every scene on the card as not counted and returns their ids;
    // empty while the card is out. On wisp.fs.
    std::vector<std::string> sd_changed_begin();
    // Counts one scene on the card again (card I/O only, io_sd_mu_); a
    // scene not on the card is a no-op.
    Result<void> recount(std::string_view scene);
    // Logs the card's files that belong to no installed scene. On wisp.fs.
    void sd_changed_end();

    // Queues `job` for `scene` on the worker the routing rule in the file
    // header picks, through submit_io (without one, runs it at once).
    // False if it was not queued; the job is then dropped unrun.
    bool submit(std::string_view scene, std::function<void()> job, IoLane lane = IoLane::Normal);
    // The medium submit() would pick now, without queuing anything.
    Medium medium_of(std::string_view scene) const;

    Result<void> write(std::string_view scene, std::string_view name,
                       std::span<const uint8_t> data);
    Result<void> append(std::string_view scene, std::string_view name,
                        std::span<const uint8_t> data);
    // At most `len` bytes from `offset`; fewer at the end of the file.
    Result<os::LargeBuffer> read(std::string_view scene, std::string_view name, uint64_t offset,
                                 size_t len);
    // Sorted by name, at most max_list of them (`truncated` when there were
    // more); bookkeeping names hidden. Empty before the first write.
    Result<SceneFileList> list(std::string_view scene);
    Result<void> remove(std::string_view scene, std::string_view name);
    // The entry of file `name` (on the card, its spelling on disk):
    // NotFound "not_found" when there is none, as for read().
    Result<SceneFileEntry> stat(std::string_view scene, std::string_view name);
    Result<SceneFilesInfo> info(std::string_view scene);
    // Removes the files in both locations (the card's only while it is in)
    // and the location record. Nothing there is success.
    Result<void> remove_all(std::string_view scene);
    // Picks the location as a write does and creates an empty
    // ".<name>.dl". Busy if a download of that name is already in flight.
    Result<std::shared_ptr<FileDownload>> begin_download(std::string_view scene,
                                                         std::string_view name);
    // Whether an SD card is in and usable: what "auto" and "sd" go by.
    // Touches no flash.
    bool sd_online() const;

private:
    friend class FileDownload;

    struct Record {
        bool loaded = false;  // `loc` read from the record file
        FilesLocation loc = FilesLocation::None;
        // The declaration, cached when the record loads (and refreshed by
        // calls off wisp.sd.io), so card calls never ask `declared`.
        FilesWant want = FilesWant::Auto;
        bool counted = false;  // `used` counted from the directory
        uint64_t used = 0;
        uint64_t pending = 0;  // reserved by downloads in flight
        std::vector<std::string> downloading;
        // Jobs routed Medium::Internal and not finished (submit()); bounded
        // by the flash worker's queue. While non-zero the record is never
        // erased.
        uint32_t int_in_flight = 0;
    };

    // The I/O locks a method holds (file header).
    struct IoLocks {
        std::unique_lock<os::Mutex> in;
        std::unique_lock<os::Mutex> sd;
    };
    // Takes the I/O locks for a method on `scene` (file header), plus
    // io_sd_mu_ in any case with `card`; then calls on_io.
    IoLocks lock_io(std::string_view scene, bool card = false);
    // Takes both I/O locks.
    IoLocks lock_both();
    // The routing rule (file header). With mu_.
    Medium route_locked(std::string_view scene) const;
    // submit() with the medium fixed by `pinned` when it has a value.
    bool submit_on(std::string_view scene, std::optional<Medium> pinned, std::function<void()> job,
                   IoLane lane);
    // One Internal job of `scene` has finished.
    void end_internal(const std::string& scene);
    // Ends an Internal job's count once: when the job has run, or when it
    // is destroyed unrun (a refused submission destroys it).
    struct InFlight {
        InFlight(SceneFiles& f, std::string id) : files(f), scene(std::move(id)) {}
        ~InFlight() { end(); }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        void end() {
            if (!ended.exchange(true)) files.end_internal(scene);
        }
        SceneFiles& files;
        const std::string scene;
        std::atomic<bool> ended{false};
    };

    // The scene's record, loaded and counted; with `choose` a scene with no
    // location gets one. With lock_io(). On wisp.sd.io only a record that
    // is loaded with location Sd: Busy for any other.
    Result<Record*> ensure(std::string_view scene, bool choose);
    // Counts `r` from its directory unless it is counted. With lock_io().
    Result<Record*> count(std::string_view scene, Record* r);
    // The directory of `loc` for `scene`; "" when it is the card and the
    // card is out.
    std::string dir_of(std::string_view scene, FilesLocation loc) const;
    std::string record_path(std::string_view scene) const;
    uint64_t quota_of(FilesLocation loc) const;
    // config_.declared, Auto when it has no answer; `installed` false when
    // it says NotFound.
    FilesWant want_of(std::string_view scene, bool* installed = nullptr) const;
    // Whether `scene` names the files: a scene id, or kAgentFilesId.
    static bool valid_owner(std::string_view scene);
    // <int>/library/agent ("" without a Library root): the agent's
    // internal files and its location record.
    std::string agent_int_root() const;
    // `dir` is on the card (FAT: names compare without case).
    bool on_card(const std::string& dir) const;
    // `name` as a file in `dir` is spelled on disk (on the card it may
    // differ in case); `name` itself when there is none. With lock_io().
    std::string name_on_disk(const std::string& dir, std::string_view name);
    // <sd>/wisp/library/scenes/<scene>/files exists (card in). With lock_io().
    bool sd_files_exist(std::string_view scene) const;
    // Logs each <sd>/wisp/library/scenes/<id>/files whose scene is not
    // installed. With lock_io().
    void log_orphans();
    // Sums the visible files of `dir`, removing stray ".dl" files and
    // finishing ".done" ones. With lock_io().
    Result<uint64_t> count_dir(const std::string& dir, const std::vector<std::string>& skip);
    // The entry `name` of `dir`, or NotFound; on the card a name differing
    // only in case matches when there is no exact one. With lock_io().
    Result<DirEntry> entry_of(const std::string& dir, std::string_view name);

    // For FileDownload, under mu_ only (no flash). reserve() is called with
    // the download's own lock held: that lock comes first, never after mu_.
    bool reserve(const std::string& scene, uint64_t n);
    void release(const std::string& scene, const std::string& name, uint64_t reserved);
    // On the download's worker: FileDownload::finish().
    Result<uint64_t> finish_download(FileDownload& dl, bool ok);

    StorageManager& storage_;
    SceneFilesConfig config_;
    // Held across a public method's file work (file header, "Locks").
    os::Mutex io_int_mu_;
    os::Mutex io_sd_mu_;
    // Guards records_ itself and every field of a Record; loaded and loc
    // change with both I/O locks held too, counted with the scene's I/O
    // locks.
    mutable os::Mutex mu_;
    std::map<std::string, Record, std::less<>> records_;
};

// One download into a scene's files, made by begin_download() on a
// storage worker and fed from the HTTP task. Its appends and its finish
// go to the worker begin_download() ran on (wisp.sd.io: Medium::Sd, else
// Medium::Internal, counted like any Internal job of the scene) on the
// Bulk lane, so they keep their order for the whole download even if the
// scene's other calls change worker meanwhile.
class FileDownload : public std::enable_shared_from_this<FileDownload> {
public:
    // How much may wait for the worker before push() waits.
    static constexpr size_t kMaxQueued = 128 * 1024;
    // Appends go to the worker in pieces of this size (fewer fsyncs than
    // the HTTP client's 512-byte reads).
    static constexpr size_t kChunk = 32 * 1024;

    FileDownload(SceneFiles& files, std::string scene, std::string name, std::string dir);
    ~FileDownload();

    FileDownload(const FileDownload&) = delete;
    FileDownload& operator=(const FileDownload&) = delete;

    // Reserves `chunk` against the quota, copies it into PSRAM and queues
    // its append on submit_io in kChunk pieces; waits while more than
    // kMaxQueued bytes are queued, until they drain or cancel() is called.
    // Never touches flash itself when submit_io is set. False once the
    // quota would be exceeded, a write failed or cancel() was called (the
    // transfer is then to be stopped).
    bool push(std::span<const uint8_t> chunk);
    // On its worker (submit()), once the transfer is over: with `ok` (a 2xx answer, all
    // of it pushed) and no error, writes what is left and renames .dl ->
    // .done -> name, returning the file's size; otherwise removes the .dl.
    // Only the first call does anything.
    Result<uint64_t> finish(bool ok);
    // Stops it from any thread without touching flash: push() returns
    // false (waking one that waits) and nothing more is written. The
    // caller still runs finish(false) through submit(), which removes the
    // .dl.
    void cancel();
    // When finish() cannot be queued: lets go of the reservation without
    // touching flash. The ".dl" left behind goes at the next count.
    void abandon();
    // "" or "quota_exceeded", "unavailable", "no_memory", "busy", or a
    // storage error's snake_case code.
    std::string error() const;
    const std::string& scene() const { return scene_; }
    // Queues `job` behind this download's appends, on the same worker and
    // lane (for finish()). A full lane is retried for up to 5 s, like
    // push()'s appends, so the caller may block that long; false if it was
    // not queued.
    bool submit(std::function<void()> job);

private:
    friend class SceneFiles;

    // Queues (or, without submit_io, runs) the append of `buf`.
    bool dispatch(os::LargeBuffer buf);
    // On its worker: appends `buf` unless the download is over.
    void append(const os::LargeBuffer& buf);
    // With mu_.
    void fail_locked(const char* code);
    std::string dl_path() const;

    SceneFiles& files_;
    const std::string scene_;
    const std::string name_;
    const std::string dir_;
    // `dir_` is on the card.
    const bool card_;
    // Where begin_download() ran; set before the download is handed out.
    Medium medium_ = Medium::Internal;
    mutable os::Mutex mu_;
    os::Semaphore wake_{1, 0};
    os::LargeBuffer staging_;
    size_t staged_ = 0;
    size_t queued_ = 0;
    uint64_t reserved_ = 0;
    uint64_t written_ = 0;
    std::string error_;
    bool cancelled_ = false;
    bool finished_ = false;
};

// fn(files) (returning Result<T>) as a job of `scene` through
// SceneFiles::submit(), waiting up to `timeout_ms` for its result. On a
// storage worker it runs inline instead: a job waiting for its own worker
// would wait for itself, and wisp.sd.io never waits for wisp.fs (a call
// there for a scene not on the card fails Busy in SceneFiles itself).
// Busy if the job was not queued, Timeout if it did not finish in time:
// it may still run later, so unless the wait is os::kForever, fn must own
// its captures.
template <typename T, typename Fn>
Result<T> run_files(SceneFiles& files, std::string_view scene, Fn fn,
                    uint32_t timeout_ms = os::kForever, IoLane lane = IoLane::Normal) {
    if (os::on_flash_thread() || os::on_sd_io_thread()) return fn(files);
    struct Done {
        os::Semaphore sem{1, 0};
        std::optional<Result<T>> out;
    };
    auto done = std::make_shared<Done>();
    const bool queued = files.submit(
        scene,
        [done, &files, fn = std::move(fn)]() mutable {
            done->out.emplace(fn(files));
            done->sem.give();
        },
        lane);
    if (!queued) return Result<T>::err(ErrorCode::Busy, "busy");
    if (!done->sem.take(timeout_ms)) return Result<T>::err(ErrorCode::Timeout, "timeout");
    return std::move(*done->out);
}

}  // namespace wisp::core
