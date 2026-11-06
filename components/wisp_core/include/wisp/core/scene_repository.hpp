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
// the card is in; with the card out it logs that the card's copy, if any,
// stays (nothing removes it later on its own). (SceneFiles keeps its own usage table: whoever
// removes a scene's data tells it with SceneFiles::remove_all().)
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
// The previous version (agent-core spec 4.1): an install with
// InstallOptions::keep_prev renames the old package directory (its data/
// and files/ already moved out in step 3) to <code_root>/.prev/<id>
// instead of removing it in step 4, replacing whatever was there. Only
// one is kept; remove() deletes it with the package. A power cut between
// that rename and step 5 is finished by recover() as any other replace.
// list() and recover() skip .prev, like every directory starting with
// '.' (the agent's drafts, <data_root>/.drafts, live beside it on the
// device).
//
// Threads: the file work runs on the flash thread (SceneRepoConfig::
// worker; spec 2.1 of docs/superpowers/specs/2026-11-06-flash-thread-
// design.md). A method called anywhere else queues it there and waits as
// long as it takes; on the worker, or without one, it runs right away.
// install() validates the package and runs its checker and before_replace
// on the caller's thread, since a checker may wait seconds for the scene
// task; it then stages the files in jobs of at most kIoChunk each, and
// only the final swap holds the repository's lock. export_zip() likewise
// reads in jobs of at most kIoChunk and fails with Busy if a replace or
// remove lands in between. Every other method holds the lock for its one
// job. A SceneStore's own writes under data/ do not take this lock.
// list() walking every package is still one job.

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
#include "wisp/core/storage_worker.hpp"
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

struct InstallOptions {
    // Keep the package being replaced as the previous version (file
    // header).
    bool keep_prev = false;
};

// Whether `path` may be a file of a scene package: scene.json, a top-level
// *.lua file (the entry), lib/**/*.lua or assets/**. Relative, '/'
// separated, without empty, "." or ".." segments, backslashes or a
// segment ending in ".tmp"/".new" (StorageManager's bookkeeping names).
// install() additionally requires a top-level .lua file to be the
// manifest's entry.
bool valid_package_path(std::string_view path);

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
    // The flash thread the file work runs on. Null: on the caller (boot,
    // tests without one).
    StorageWorker* worker = nullptr;
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

    // Only scene.json, checked to name <id>: cheaper than info(), which
    // also sizes the package. NotFound if it is absent; InvalidArgument for
    // a malformed id or manifest.
    Result<SceneManifest> manifest(std::string_view id);

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
    // With opt.keep_prev the replaced package becomes the previous
    // version (file header).
    Result<SceneInfo> install(
        std::span<const uint8_t> zip, const SourceChecker& check,
        const std::function<Result<void>(const SceneManifest&)>& before_replace = {},
        InstallOptions opt = {});

    // Whether <code_root>/.prev/<id>/scene.json exists.
    bool has_prev(std::string_view id);
    // The previous version as a zip in Mem::Large (no data), read like
    // export_zip(). NotFound without one.
    Result<os::LargeBuffer> prev_zip(std::string_view id);

    // The package as a zip in Mem::Large; with_data adds data/**.
    Result<os::LargeBuffer> export_zip(std::string_view id, bool with_data);

    // Removes the package and its previous version. Without with_data, data_dir(id) and
    // files_dir(id) stay, so a later install of the same id finds them again; with it, the SD
    // card's files go too. NotFound if there is nothing to remove; Unsupported in dev mode.
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
    // The same for the previous version, prev_dir(id).
    Result<SceneManifest> read_prev_manifest(std::string_view id);
    Result<SceneManifest> read_manifest_in(const std::string& dir, std::string_view id);
    // Steps 3-5 of the replace in the file header, if .<id>.newdir exists.
    // With keep_prev, step 4 renames <id> to prev_dir(id) instead.
    Result<void> finish_replace(const std::string& id, bool keep_prev = false);
    // info() with the lock already held.
    Result<SceneInfo> info_locked(std::string_view id);
    // The public methods' file work, on the flash thread.
    Result<void> recover_here();
    Result<std::vector<SceneInfo>> list_here();
    Result<SceneInfo> info_here(std::string_view id);
    Result<SceneManifest> manifest_here(std::string_view id);
    Result<ScenePackage> load_here(std::string_view id);
    // install()'s last step: .<id>.tmpdir becomes the package (steps 2-5).
    Result<SceneInfo> swap_here(const SceneManifest& manifest, bool keep_prev);
    // <code_root>/.prev/<id>
    std::string prev_dir(std::string_view id) const;
    // export_zip() of the package in `dir` (the installed one or the
    // previous version).
    Result<os::LargeBuffer> export_dir(std::string_view id, bool prev, bool with_data);
    Result<void> remove_here(std::string_view id, bool with_data);
    Result<SceneRemoval> removal_here(std::string_view id, bool with_data);

    StorageManager& storage_;
    SceneRepoConfig config_;
    os::Mutex mutex_;
    // Under mutex_: bumped by every replace and remove, so an export read
    // across several flash-thread jobs notices one that landed in between.
    uint64_t generation_ = 0;
    // Guards installing_ and removing_ only, so installing() and
    // removing() never wait behind file work.
    mutable os::Mutex marks_mutex_;
    std::vector<std::string> installing_;
    std::vector<std::string> removing_;  // one entry per begin_removing()
};

}  // namespace wisp::core
