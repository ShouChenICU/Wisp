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
// Replacing a package is atomic across power loss (the directory-level
// version of AGENTS.md section 8's two-name scheme):
//   1. the new files are written under <code_root>/.<id>.tmpdir/;
//   2. that directory is renamed to .<id>.newdir -- from here on it is
//      known to be complete;
//   3. <id>/data, if present, moves into .<id>.newdir/data (if both exist,
//      the replace stops with an error rather than delete either);
//   4. <id> is removed;
//   5. .<id>.newdir is renamed to <id>.
// recover() discards every .*.tmpdir and finishes steps 3-5 for every
// .<id>.newdir. The staging names end in "dir" because StorageManager
// reserves names ending in ".tmp"/".new" for its own file writes.
//
// Not thread-safe: every method runs on the wisp.fs worker, which
// serializes them.

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

namespace wisp::core {

struct SceneInfo {
    SceneManifest manifest;
    uint64_t code_bytes = 0;  // the package's own files, data excluded
    uint64_t data_bytes = 0;
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
    // `check` over its .lua files, then replaces <id> keeping its data.
    // Unsupported in dev mode. Nothing is written for a rejected package.
    Result<SceneInfo> install(std::span<const uint8_t> zip, const SourceChecker& check);

    // The package as a zip in Mem::Large; with_data adds data/**.
    Result<os::LargeBuffer> export_zip(std::string_view id, bool with_data);

    // Removes the package. Without with_data, data_dir(id) stays, so a later
    // install of the same id finds it again. NotFound if there is nothing to
    // remove; Unsupported in dev mode.
    Result<void> remove(std::string_view id, bool with_data);

    // <data_root>/<id>/data
    std::string data_dir(std::string_view id) const;

private:
    std::string code_dir(std::string_view id) const;
    // scene.json of <id>, checked to name <id>; NotFound if absent.
    Result<SceneManifest> read_manifest(std::string_view id);
    // Steps 3-5 of the replace in the file header, if .<id>.newdir exists.
    Result<void> finish_replace(const std::string& id);

    StorageManager& storage_;
    SceneRepoConfig config_;
};

}  // namespace wisp::core
