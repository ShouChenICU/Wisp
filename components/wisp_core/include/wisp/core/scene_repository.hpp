#pragma once

// Installed scene packages on disk (spec 6.2, 6.3).
//
// Layout: <code_root>/<id>/ holds scene.json, the entry file, lib/**/*.lua
// and assets/**; the scene's data lives in data_dir(id) =
// <data_root>/<id>/data. On the device both roots are <int>/scenes, so the
// data directory sits inside the package directory and a replace has to
// carry it over; in the simulator's dev mode the code comes straight from a
// working directory and the data lives elsewhere. Packages never contain
// data/, so <code_root>/<id>/data is always treated as the scene's data,
// without comparing the two roots (which may be spelled differently).
//
// files/ (SceneFiles, scene-device spec 7.2) is treated exactly like data/:
// <data_root>/<id>/files on internal storage, never part of a package, not
// read by load(), not exported, not counted in code_bytes, carried over by
// a replace and kept by remove() without data. remove() with data also
// deletes the copy on the SD card, <sd>/wisp/library/scenes/<id>, when
// the card is in. (SceneFiles keeps its own usage table: whoever removes a
// scene's data tells it with SceneFiles::remove_all().)
//
// Replacing a package is atomic across power loss (the directory-level
// version of AGENTS.md section 8's two-name scheme):
//   1. the new files are written under <code_root>/.<id>.tmpdir/;
//   2. that directory is renamed to .<id>.newdir -- from here on it is
//      known to be complete;
//   3. <id>/data and <id>/files, if present, move into .<id>.newdir (if
//      one exists on both sides, the replace stops with an error rather
//      than delete either);
//   4. <id> is removed;
//   5. .<id>.newdir is renamed to <id>.
// recover() discards every .*.tmpdir and finishes steps 3-5 for every
// .<id>.newdir. The staging names end in "dir" because StorageManager
// reserves names ending in ".tmp"/".new" for its own file writes.
//
// Thread-safe: each method holds the repository's lock while it touches
// the files, so the wisp.fs worker (opening, deleting) and HTTP server
// threads (installing, exporting) never see each other's half-done work.
// install() takes it only after its checker has returned, since a checker
// may wait seconds for the scene task. A SceneStore's own writes under
// data/ do not take this lock. A long hold (an install's writes, list()
// walking every package) delays an open queued on the worker, and every
// worker job behind it, until it ends.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

struct SceneInfo {
    SceneManifest manifest;
    uint64_t code_bytes = 0;  // the package's own files, data excluded
    uint64_t data_bytes = 0;
};

// What deleting a scene would remove (SceneRepository::removal()).
struct SceneRemoval {
    // The manifest's name, or the id when there is no valid manifest.
    std::string name;
    bool package = false;  // scene.json is there, valid or not
    bool data = false;     // the scene has a data or files directory
};

// One Lua source handed to a SourceChecker. Both views point into the
// package being installed and are valid only during the call.
struct SourceFile {
    std::string_view path;
    std::string_view text;
};

// Compiles (without running) every .lua file of a package. Its error is
// returned to the installer unchanged, so it should read like
// "lib/util.lua:3: ...".
// The span is backed by Mem::Large: its length is up to the uploader.
using SourceChecker = std::function<Result<void>(std::span<const SourceFile>)>;

// A package's files (everything except data/) loaded into Mem::Large.
// Paths are relative to the package root, e.g. "lib/util.lua".
struct ScenePackage {
    SceneManifest manifest;
    os::LargeVector<std::pair<std::string, os::LargeBuffer>> files;
};

struct SceneRepoConfig {
    std::string code_root;
    std::string data_root;
    bool dev = false;  // the code root is a working directory: never written
    size_t max_code_bytes = 512 * 1024;
    // Test seam: runs during a replace between moving data/ aside (step 3)
    // and removing the old package (step 4), where anything still writing
    // into the scene's data would be lost. Tests use it to widen that
    // window; nothing sets it in production.
    std::function<void()> on_data_moved;
};

class SceneRepository {
public:
    SceneRepository(StorageManager& storage, SceneRepoConfig config);

    // Finishes or discards interrupted installs (see the file header). Run
    // at boot before anything lists or opens a scene. A no-op in dev mode.
    Result<void> recover();

    // Installed scenes sorted by id. Directories starting with '.' and
    // packages whose scene.json is missing, invalid or names another id are
    // skipped. A missing code root is an empty list.
    Result<std::vector<SceneInfo>> list();

    // NotFound if `id` has no scene.json; InvalidArgument for a malformed id
    // or manifest.
    Result<SceneInfo> info(std::string_view id);

    // Reads scene.json, the entry file, lib/**/*.lua and assets/** into
    // Mem::Large. Anything else in the directory (data/, or stray files in
    // a dev working directory) is not read. QuotaExceeded past
    // max_code_bytes; InvalidArgument if the entry or icon file is missing.
    Result<ScenePackage> load(std::string_view id);

    // Validates the zip (manifest, allowed files, max_code_bytes), runs
    // `check` over its .lua files, then `before_replace` with the new
    // manifest (both without the lock held, so either may wait), then
    // replaces <id> keeping its data. An error from either stops the
    // install. Unsupported in dev mode; Busy while another install of the
    // same id runs. Nothing is written for a rejected package.
    Result<SceneInfo> install(
        std::span<const uint8_t> zip, const SourceChecker& check,
        const std::function<Result<void>(const SceneManifest&)>& before_replace = {});

    // The package as a zip in Mem::Large; with_data adds data/**.
    Result<os::LargeBuffer> export_zip(std::string_view id, bool with_data);

    // Removes the package. Without with_data, data_dir(id) and files_dir(id)
    // stay, so a later install of the same id finds them again; with it,
    // the SD card's files go too. NotFound if there is nothing to
    // remove; Unsupported in dev mode.
    Result<void> remove(std::string_view id, bool with_data);

    // Whether an install() of `id` is under way: from the moment its
    // manifest has been read until it returns, whatever the outcome. The
    // scene runtime refuses to open such a scene, so nothing runs on, or
    // writes into, a package that is being replaced. Takes only a short
    // lock of its own, never the repository's: safe on the UI thread and
    // the scene task.
    bool installing(std::string_view id) const;

    // Marks `id` as being deleted, from the user's yes until its files are
    // gone (the caller ends it with end_removing(), whatever the outcome).
    // The scene runtime refuses to open such a scene, so nothing comes up
    // on, or writes into, files that are being removed. Marks nest;
    // remove() itself neither sets nor needs one. Short lock of its own,
    // as installing().
    void begin_removing(std::string_view id);
    void end_removing(std::string_view id);
    bool removing(std::string_view id) const;

    // What remove(id, with_data) would find: NotFound exactly when remove()
    // would answer NotFound (no package, and no data or !with_data), so
    // data left behind by a package that is gone can still be deleted.
    // InvalidArgument for a malformed id.
    Result<SceneRemoval> removal(std::string_view id, bool with_data);

    // <data_root>/<id>/data
    std::string data_dir(std::string_view id) const;
    // <data_root>/<id>/files
    std::string files_dir(std::string_view id) const;

    // The code root is a working directory (SceneRepoConfig::dev): install()
    // and remove() are Unsupported.
    bool dev() const { return config_.dev; }

private:
    std::string code_dir(std::string_view id) const;
    // scene.json of <id>, checked to name <id>; NotFound if absent.
    Result<SceneManifest> read_manifest(std::string_view id);
    // Steps 3-5 of the replace in the file header, if .<id>.newdir exists.
    Result<void> finish_replace(const std::string& id);
    // info() with the lock already held.
    Result<SceneInfo> info_locked(std::string_view id);

    StorageManager& storage_;
    SceneRepoConfig config_;
    os::Mutex mutex_;
    // Guards installing_ and removing_ only, so installing() and
    // removing() never wait behind file work.
    mutable os::Mutex marks_mutex_;
    std::vector<std::string> installing_;
    std::vector<std::string> removing_;  // one entry per begin_removing()
};

}  // namespace wisp::core
