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
// that differs from an existing one only in case is that file.
//
// Errors carry these messages, which scene_files_code() hands to scripts:
// InvalidArgument "bad_name", InvalidArgument "too_large", QuotaExceeded
// "quota_exceeded", StorageUnavailable "unavailable", NotFound
// "not_found". Any other storage failure keeps its code and message.
//
// Threads: every SceneFiles method touches flash (the location record,
// the directory), so it runs on the wisp.fs worker or on a task that may
// access flash (an httpd thread), never on the scene task or the UI
// thread. One lock serializes the methods' file work and a second one,
// never held across I/O, guards the usage table, so a FileDownload::push()
// on the HTTP task never waits for flash.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class FilesLocation : uint8_t { None, Sd, Internal };

// "none", "sd", "int".
const char* to_string(FilesLocation l);

struct SceneFileEntry {
    std::string name;
    uint64_t size = 0;
    int64_t mtime = 0;  // DirEntry::mtime
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
    // How a download's appends reach the wisp.fs worker
    // (StorageWorker::submit); false: not queued. Empty: FileDownload::push()
    // appends on the caller's thread, which must then be allowed flash.
    std::function<bool(std::function<void()>)> submit_io;
};

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
    // scene. Fails only if <data_root> cannot be listed.
    Result<void> scan();
    // The card came or went: the scenes on it are counted again (now, if
    // it is in), and its files without a scene are logged.
    void on_sd_changed();

    Result<void> write(std::string_view scene, std::string_view name,
                       std::span<const uint8_t> data);
    Result<void> append(std::string_view scene, std::string_view name,
                        std::span<const uint8_t> data);
    // At most `len` bytes from `offset`; fewer at the end of the file.
    Result<os::LargeBuffer> read(std::string_view scene, std::string_view name, uint64_t offset,
                                 size_t len);
    // Sorted by name, at most max_list of them; bookkeeping names hidden.
    // Empty before the first write.
    Result<std::vector<SceneFileEntry>> list(std::string_view scene);
    Result<void> remove(std::string_view scene, std::string_view name);
    Result<SceneFilesInfo> info(std::string_view scene);
    // Removes the files in both locations (the card's only while it is in)
    // and the location record. Nothing there is success.
    Result<void> remove_all(std::string_view scene);
    // Picks the location as a write does and creates an empty
    // ".<name>.dl". Busy if a download of that name is already in flight.
    Result<std::shared_ptr<FileDownload>> begin_download(std::string_view scene,
                                                         std::string_view name);

private:
    friend class FileDownload;

    struct Record {
        bool loaded = false;  // `loc` read from the record file
        FilesLocation loc = FilesLocation::None;
        bool counted = false;  // `used` counted from the directory
        uint64_t used = 0;
        uint64_t pending = 0;  // reserved by downloads in flight
        std::vector<std::string> downloading;
    };

    // The scene's record, loaded and counted; with `choose` a scene with no
    // location gets one. With io_mu_.
    Result<Record*> ensure(std::string_view scene, bool choose);
    // The directory of `loc` for `scene`; "" when it is the card and the
    // card is out.
    std::string dir_of(std::string_view scene, FilesLocation loc) const;
    std::string record_path(std::string_view scene) const;
    uint64_t quota_of(FilesLocation loc) const;
    bool sd_online() const;
    // config_.declared, Auto when it has no answer.
    FilesWant want_of(std::string_view scene) const;
    // Logs each <sd>/wisp/library/scenes/<id>/files whose scene is not
    // installed. With io_mu_.
    void log_orphans();
    // Sums the visible files of `dir`, removing stray ".dl" files and
    // finishing ".done" ones. With io_mu_.
    Result<uint64_t> count_dir(const std::string& dir, const std::vector<std::string>& skip);
    // The entry `name` of `dir`, or NotFound; on the card a name differing
    // only in case matches when there is no exact one. With io_mu_.
    Result<DirEntry> entry_of(const std::string& dir, std::string_view name);
    // entry_of()'s size; NotFound unless it is a regular file.
    Result<uint64_t> size_of(const std::string& dir, std::string_view name);

    // For FileDownload, under mu_ only (no flash). reserve() is called with
    // the download's own lock held: that lock comes first, never after mu_.
    bool reserve(const std::string& scene, uint64_t n);
    void release(const std::string& scene, const std::string& name, uint64_t reserved);
    // On wisp.fs: FileDownload::finish().
    Result<uint64_t> finish_download(FileDownload& dl, bool ok);

    StorageManager& storage_;
    SceneFilesConfig config_;
    // Held across each public method's file work.
    os::Mutex io_mu_;
    // Guards records_' usage (used, pending, downloading). loaded, loc and
    // counted change with both locks held.
    mutable os::Mutex mu_;
    std::map<std::string, Record, std::less<>> records_;
};

// One download into a scene's files, made by begin_download() on wisp.fs
// and fed from the HTTP task.
class FileDownload : public std::enable_shared_from_this<FileDownload> {
public:
    // How much may wait for the wisp.fs worker before push() waits.
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
    // On wisp.fs, once the transfer is over: with `ok` (a 2xx answer, all
    // of it pushed) and no error, writes what is left and renames .dl ->
    // .done -> name, returning the file's size; otherwise removes the .dl.
    // Only the first call does anything.
    Result<uint64_t> finish(bool ok);
    // Stops it from any thread without touching flash: push() returns
    // false (waking one that waits) and nothing more is written. The
    // caller still runs finish(false) on wisp.fs, which removes the .dl.
    void cancel();
    // When finish() cannot be queued: lets go of the reservation without
    // touching flash. The ".dl" left behind goes at the next count.
    void abandon();
    // "" or "quota_exceeded", "unavailable", "no_memory", "busy", or a
    // storage error's snake_case code.
    std::string error() const;

private:
    friend class SceneFiles;

    // Queues (or, without submit_io, runs) the append of `buf`.
    bool dispatch(os::LargeBuffer buf);
    // On wisp.fs: appends `buf` unless the download is over.
    void append(const os::LargeBuffer& buf);
    // With mu_.
    void fail_locked(const char* code);
    std::string dl_path() const;

    SceneFiles& files_;
    const std::string scene_;
    const std::string name_;
    const std::string dir_;
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

}  // namespace wisp::core
