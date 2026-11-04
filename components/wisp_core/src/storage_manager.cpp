#include "wisp/core/storage_manager.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "storage_manager";
constexpr const char* kIntId = "storage.int";
constexpr const char* kSdId = "storage.sd";

// Lexically normalizes `path`: collapses repeated '/', drops '.'
// components, and drops a trailing '/' (so the result never has one).
// Applied both to every caller-supplied path in write_atomic()/
// read_file(), and to both devices' mount_point() values in init(): not
// just a trailing-slash strip, since a driver's mount point itself could
// have a doubled slash or a '.' component -- so int_mount_/sd_mount_
// and every normalized caller path compose and prefix-compare
// consistently. A ".." component is never resolved -- it's
// rejected outright with ErrorCode::InvalidArgument, since resolving one
// correctly would require knowing the filesystem's current state (e.g.
// what a symlink points at), which this lexical pass deliberately doesn't
// attempt. Used by write_atomic()/read_file() so that a non-canonical
// alias of a path (a doubled slash, a "." segment) can't be used to dodge
// the library quota or otherwise be treated differently from its
// canonical form.
Result<std::string> normalize_path(const std::string& path) {
    bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        std::string component =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (component == "..") {
            return Result<std::string>::err(ErrorCode::InvalidArgument,
                                            "path must not contain '..' components: " + path);
        }
        if (!component.empty() && component != ".") {
            parts.push_back(std::move(component));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::string out;
    if (absolute) {
        out += "/";
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += "/";
        }
        out += parts[i];
    }
    return Result<std::string>::ok(std::move(out));
}

// True if `s` ends with `suffix`. Used both on bare filenames (quota
// accounting's "*.tmp"/"*.new" exclusion) and on full paths (the
// reserved-suffix rejection in write_atomic()/read_file()) -- a suffix
// that doesn't itself contain '/' can't straddle a path separator, so
// both uses are equivalent to checking the last path component.
bool has_suffix(const std::string& s, const char* suffix) {
    size_t len = std::strlen(suffix);
    return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
}

// True if `path`'s final component ends with ".tmp" or ".new" --
// write_atomic()'s own reserved suffixes (see file header). Rejected by
// write_atomic()/read_file() with ErrorCode::InvalidArgument so a caller
// can never create/read a file under one of these names itself: doing so
// would let it (a) sit in "<int>/library/" uncounted by the quota (which
// deliberately excludes these suffixes -- see dir_size_recursive()),
// (b) have write_atomic()'s own bookkeeping silently delete it (the
// unconditional `remove(neu)` before `rename(tmp, neu)`, or the
// leftover-".tmp" cleanup in read_file()), or (c) have its size
// mis-subtracted from the quota's "old_target_size" (file_size() assumes
// `path` -- never one of these suffixes -- is the thing being
// overwritten).
bool has_reserved_suffix(const std::string& path) {
    return has_suffix(path, ".tmp") || has_suffix(path, ".new");
}

// True if `path` is exactly `dir` or is nested under it (i.e. `path` ==
// `dir` or starts with `dir + "/"`). `dir` must not have a trailing slash.
bool is_under(const std::string& path, const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    if (path == dir) {
        return true;
    }
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
           path[dir.size()] == '/';
}

bool path_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Returns the file's size, or 0 if it doesn't exist (or isn't a regular
// file) -- callers that need to distinguish "doesn't exist" from "is
// empty" use path_exists() separately.
uint64_t file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_size);
}

// Recursively sums the size of every regular file under `dir` (0 if `dir`
// doesn't exist), excluding write_atomic()'s own "*.tmp"/"*.new" temp
// files -- those are transient write bookkeeping, not library content,
// so counting them would make the quota fluctuate with in-flight writes
// instead of actual stored data. Uses <dirent.h>/<sys/stat.h> only,
// matching the constraint this whole file is built under (no
// std::filesystem) -- mirrors tests/support/fake_devices.hpp's
// FakeStorage::dir_size(), which a production StorageManager can't itself
// depend on.
//
// Descends at most kMaxQuotaWalkDepth directory levels: stat() follows
// symlinks, so on the host a link back up the tree would otherwise recurse
// until the stack overflows, and several such links multiply the paths
// walked per level (the device's filesystems have no symlinks).
constexpr int kMaxQuotaWalkDepth = 16;

uint64_t dir_size_recursive(const std::string& dir, int depth = 0) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return 0;
    }
    uint64_t total = 0;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        std::string full = dir + "/" + name;
        // stat, not lstat: ESP-IDF's newlib has no lstat, and neither FAT
        // nor LittleFS has symlinks to tell apart anyway.
        struct stat st;
        if (stat(full.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth < kMaxQuotaWalkDepth) total += dir_size_recursive(full, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            if (has_suffix(name, ".tmp") || has_suffix(name, ".new")) {
                continue;
            }
            total += static_cast<uint64_t>(st.st_size);
        }
    }
    closedir(d);
    return total;
}

// Creates `dir` and every missing parent directory above it (like `mkdir
// -p`). Already-existing directories (including `dir` itself) are not an
// error, but an existing non-directory (e.g. a plain file) at `dir`
// itself is -- ErrorCode::IoError, since mkdir() would otherwise silently
// treat it as "already there" via the EEXIST check below without this
// upfront type check.
Result<void> mkdir_p(const std::string& dir) {
    if (dir.empty()) {
        return Result<void>::ok();
    }
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            return Result<void>::err(ErrorCode::IoError, "not a directory: " + dir);
        }
        return Result<void>::ok();
    }
    size_t pos = dir.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        WISP_TRY(mkdir_p(dir.substr(0, pos)));
    }
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        return Result<void>::err(ErrorCode::IoError,
                                 "mkdir " + dir + ": " + std::string(strerror(errno)));
    }
    return Result<void>::ok();
}

// Creates <sd_mount>/wisp/library and <sd_mount>/wisp/backup, logging a
// warning (but not failing) for either that can't be created -- used by
// both init() and refresh() so a write-protected/corrupt SD card never
// fails internal storage's own init(), nor flips the card back to
// "offline" in refresh(); see both methods' doc comments.
void ensure_sd_dirs_best_effort(const std::string& sd_mount) {
    Result<void> r1 = mkdir_p(sd_mount + "/wisp/library");
    Result<void> r2 = mkdir_p(sd_mount + "/wisp/backup");
    if (!r1) {
        WLOG_W(kTag, "failed to create %s: %s", (sd_mount + "/wisp/library").c_str(),
               r1.error().message.c_str());
    }
    if (!r2) {
        WLOG_W(kTag, "failed to create %s: %s", (sd_mount + "/wisp/backup").c_str(),
               r2.error().message.c_str());
    }
}

// Writes `data` to `tmp` (truncating/creating it), fflush()es and
// fsync()s it, and fclose()s it -- checking every one of those calls'
// return values, not just fwrite()'s, since any of them failing silently
// would mean a ".tmp" that looks complete but isn't actually durable on
// disk yet. Best-effort removes `tmp` before returning an error (so a
// half/un-synced attempt doesn't linger), but that cleanup's own
// success/failure doesn't change the error returned.
Result<void> write_synced_file(const std::string& tmp, std::span<const uint8_t> data) {
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        return Result<void>::err(ErrorCode::IoError,
                                 "fopen " + tmp + ": " + std::string(strerror(errno)));
    }

    size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
    if (written != data.size() || ferror(f) != 0) {
        int saved_errno = errno;
        fclose(f);
        remove(tmp.c_str());
        return Result<void>::err(ErrorCode::IoError,
                                 "fwrite " + tmp + ": " + std::string(strerror(saved_errno)));
    }

    if (fflush(f) != 0) {
        int saved_errno = errno;
        fclose(f);
        remove(tmp.c_str());
        return Result<void>::err(ErrorCode::IoError,
                                 "fflush " + tmp + ": " + std::string(strerror(saved_errno)));
    }

    if (fsync(fileno(f)) != 0) {
        int saved_errno = errno;
        fclose(f);
        remove(tmp.c_str());
        return Result<void>::err(ErrorCode::IoError,
                                 "fsync " + tmp + ": " + std::string(strerror(saved_errno)));
    }

    if (fclose(f) != 0) {
        int saved_errno = errno;
        remove(tmp.c_str());
        return Result<void>::err(ErrorCode::IoError,
                                 "fclose " + tmp + ": " + std::string(strerror(saved_errno)));
    }

    return Result<void>::ok();
}

// If `path` doesn't exist but `path + ".new"` does, promotes it (renames
// it to `path`) -- recovers a write whose data was fully flushed/synced
// but whose final rename to `path` was interrupted by power loss (see
// file header). A no-op (not an error) if `path` already exists or
// neither it nor its ".new" does; callers that care about the
// "truly missing" case (read_file()) check path_exists() themselves
// after calling this. Called at the start of both write_atomic() and
// read_file() so that a *second* write to a path whose previous write
// left an orphaned ".new" recovers that data into `path` first, instead
// of write_atomic()'s own best-effort `remove(path + ".new")` (see
// write_atomic()) silently discarding it.
Result<void> promote_pending_new(const std::string& path) {
    if (path_exists(path)) {
        return Result<void>::ok();
    }
    std::string neu = path + ".new";
    if (!path_exists(neu)) {
        return Result<void>::ok();
    }
    if (rename(neu.c_str(), path.c_str()) != 0) {
        return Result<void>::err(ErrorCode::IoError, "rename " + neu + " -> " + path + ": " +
                                                         std::string(strerror(errno)));
    }
    return Result<void>::ok();
}

// stat() that does not follow a final symlink. Hosts (the simulator's
// temp dirs) have symlinks and lstat(); the device's libc has no lstat()
// to link against, and neither LittleFS nor FAT can hold a symlink, so
// plain stat() is already non-following there.
int stat_nofollow(const std::string& path, struct stat* st) {
#if defined(__linux__) || defined(__APPLE__)
    return lstat(path.c_str(), st);
#else
    return stat(path.c_str(), st);
#endif
}

Result<void> io_error(const char* op, const std::string& path) {
    return Result<void>::err(ErrorCode::IoError,
                             std::string(op) + " " + path + ": " + std::string(strerror(errno)));
}

// The names in `dir` other than "." and "..". Collected and the handle
// closed before the caller acts on them, so a recursive walk keeps one
// directory open at a time and never removes entries under a live readdir().
Result<std::vector<std::string>> read_names(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return Result<std::vector<std::string>>::err(
            ErrorCode::IoError, "opendir " + dir + ": " + std::string(strerror(errno)));
    }
    std::vector<std::string> names;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(std::move(name));
        }
    }
    closedir(d);
    return Result<std::vector<std::string>>::ok(std::move(names));
}

// The wisp.fs worker that runs these calls has a 4 KB stack, so the walk
// is bounded rather than trusting the tree's depth.
constexpr int kMaxRemoveDepth = 16;

Result<void> remove_tree_at(const std::string& path, int depth) {
    struct stat st;
    if (stat_nofollow(path, &st) != 0) {
        return errno == ENOENT ? Result<void>::ok() : io_error("stat", path);
    }
    if (S_ISDIR(st.st_mode)) {
        if (depth >= kMaxRemoveDepth) {
            return Result<void>::err(ErrorCode::IoError, "directory tree too deep: " + path);
        }
        Result<std::vector<std::string>> names = read_names(path);
        if (!names) {
            return Result<void>::err(names.error().code, names.error().message);
        }
        for (const std::string& name : names.value()) {
            WISP_TRY(remove_tree_at(path + "/" + name, depth + 1));
        }
        if (rmdir(path.c_str()) != 0) {
            return io_error("rmdir", path);
        }
        return Result<void>::ok();
    }
    if (remove(path.c_str()) != 0 && errno != ENOENT) {
        return io_error("remove", path);
    }
    return Result<void>::ok();
}

// Opens `path` for appending, writes `data`, and makes it durable
// (fflush + fsync) before closing. Every call's return value is checked:
// an unchecked failure would report bytes as stored that are not on disk.
Result<void> append_synced(const std::string& path, std::span<const uint8_t> data) {
    FILE* f = fopen(path.c_str(), "ab");
    if (!f) {
        return io_error("fopen", path);
    }
    size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
    if (written != data.size() || ferror(f) != 0) {
        Result<void> err = io_error("fwrite", path);
        fclose(f);
        return err;
    }
    if (fflush(f) != 0) {
        Result<void> err = io_error("fflush", path);
        fclose(f);
        return err;
    }
    if (fsync(fileno(f)) != 0) {
        Result<void> err = io_error("fsync", path);
        fclose(f);
        return err;
    }
    if (fclose(f) != 0) {
        return io_error("fclose", path);
    }
    return Result<void>::ok();
}

// Size of the regular file at `path`, or of every regular file under it
// if it is a directory, excluding "*.tmp"/"*.new"; 0 if it is missing.
uint64_t path_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        return dir_size_recursive(path);
    }
    return S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
}

}  // namespace

StorageManager::StorageManager(DeviceRegistry& registry, EventBus& bus, StorageConfig config)
    : registry_(registry), bus_(bus), config_(config) {}

Result<void> StorageManager::init() {
    std::lock_guard<os::Mutex> lock(mutex_);

    // Set this false up front, before anything else
    // -- so that if this call is a *re*-init that fails partway (e.g.
    // storage.int was unplugged since a previous successful init()), the
    // manager is left unusable (StorageUnavailable/empty roots) rather
    // than silently continuing to operate on the previous, now-stale
    // int_mount_/sd_mount_/sd_online_ state. It's only set back to true
    // at the very end, once every step below has actually succeeded.
    ready_ = false;

    int_dev_ = registry_.get<hal::Storage>(kIntId);
    if (!int_dev_) {
        return Result<void>::err(ErrorCode::StorageUnavailable,
                                 std::string(kIntId) + " not registered");
    }
    Result<std::string> int_norm = normalize_path(int_dev_->mount_point());
    if (!int_norm) {
        return Result<void>::err(int_norm.error().code, int_norm.error().message);
    }
    int_mount_ = int_norm.value();
    WISP_TRY(mkdir_p(int_mount_ + "/system"));
    WISP_TRY(mkdir_p(int_mount_ + "/library"));

    sd_dev_ = registry_.get<hal::Storage>(kSdId);
    if (sd_dev_) {
        Result<std::string> sd_norm = normalize_path(sd_dev_->mount_point());
        if (!sd_norm) {
            return Result<void>::err(sd_norm.error().code, sd_norm.error().message);
        }
        sd_mount_ = sd_norm.value();
        sd_online_ = sd_dev_->online();
        if (sd_online_) {
            // Best-effort: a write-protected/corrupt SD card must not
            // fail init() for internal storage, which is otherwise fine
            // -- see this method's doc comment.
            ensure_sd_dirs_best_effort(sd_mount_);
        }
    } else {
        sd_mount_.clear();
        sd_online_ = false;
    }

    ready_ = true;
    return Result<void>::ok();
}

std::vector<std::string> StorageManager::roots(Area area) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!ready_) {
        return {};
    }
    switch (area) {
        case Area::System:
            return {int_mount_ + "/system"};
        case Area::Library:
            if (sd_online_) {
                return {sd_mount_ + "/wisp/library", int_mount_ + "/library"};
            }
            return {int_mount_ + "/library"};
        case Area::Backup:
            if (sd_online_) {
                return {sd_mount_ + "/wisp/backup"};
            }
            return {};
    }
    return {};
}

Result<std::string> StorageManager::write_root(Area area) const {
    std::vector<std::string> r = roots(area);
    if (r.empty()) {
        return Result<std::string>::err(ErrorCode::StorageUnavailable,
                                        "no writable root for this area");
    }
    return Result<std::string>::ok(r.front());
}

Result<void> StorageManager::write_atomic(const std::string& raw_path,
                                          std::span<const uint8_t> data) {
    std::lock_guard<os::Mutex> lock(mutex_);

    if (!ready_) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "StorageManager not initialized");
    }

    Result<std::string> normalized = normalize_path(raw_path);
    if (!normalized) {
        return Result<void>::err(normalized.error().code, normalized.error().message);
    }
    const std::string& path = normalized.value();

    // A caller writing directly to one of
    // write_atomic()'s own reserved names would be invisible to the
    // quota, and would be clobbered by this very function's "<path>.new"
    // bookkeeping below -- see has_reserved_suffix()'s doc comment.
    if (has_reserved_suffix(path)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "path must not end in '.tmp' or '.new': " + path);
    }

    if (sd_dev_ && !sd_mount_.empty() && is_under(path, sd_mount_) && !sd_online_) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "sd card offline: " + path);
    }

    // Recover a "<path>.new" orphaned by a *double*
    // power cut (first write completed and synced, but the final rename
    // to `path` never happened; then this write_atomic() call starts
    // before anything noticed) before this call's own bookkeeping -- in
    // particular before the unconditional `remove(neu)` a few lines
    // below -- would otherwise discard that data without ever exposing
    // it. This also makes the quota calculation right below see the
    // recovered file's size.
    WISP_TRY(promote_pending_new(path));

    std::string library_root = int_mount_ + "/library";
    if (!int_mount_.empty() && is_under(path, library_root)) {
        uint64_t existing_total = dir_size_recursive(library_root);
        uint64_t old_target_size = file_size(path);
        uint64_t projected = existing_total - old_target_size + data.size();
        if (projected > config_.internal_library_quota) {
            return Result<void>::err(ErrorCode::QuotaExceeded,
                                     "internal library quota exceeded: " + path);
        }
    }

    // Services own subdirectories of an area root (net/, secrets/, ...)
    // that init() doesn't know about, so create the parent on demand.
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        WISP_TRY(mkdir_p(path.substr(0, slash)));
    }

    std::string tmp = path + ".tmp";
    std::string neu = path + ".new";

    WISP_TRY(write_synced_file(tmp, data));

    // `tmp` is now a complete, synced write. Move it to the ".new" name
    // -- never already occupied by a complete write of our own, but a
    // previous write_atomic() on this path could have been interrupted
    // after creating its own ".new" and before removing it, so discard
    // any stray one first (best-effort; about to be replaced either way)
    // so this rename can't fail with EEXIST-like behavior on FAT.
    remove(neu.c_str());
    if (::rename(tmp.c_str(), neu.c_str()) != 0) {
        int saved_errno = errno;
        remove(tmp.c_str());
        return Result<void>::err(ErrorCode::IoError, "rename " + tmp + " -> " + neu + ": " +
                                                         std::string(strerror(saved_errno)));
    }

    if (path_exists(path)) {
        if (remove(path.c_str()) != 0) {
            return Result<void>::err(ErrorCode::IoError,
                                     "remove " + path + ": " + std::string(strerror(errno)));
        }
    }

    if (::rename(neu.c_str(), path.c_str()) != 0) {
        return Result<void>::err(ErrorCode::IoError, "rename " + neu + " -> " + path + ": " +
                                                         std::string(strerror(errno)));
    }

    return Result<void>::ok();
}

Result<std::pair<std::string, uint64_t>> StorageManager::open_for_read(
    const std::string& raw_path) {
    using R = Result<std::pair<std::string, uint64_t>>;

    Result<std::string> checked = checked_path(raw_path, /*mutating=*/false);
    if (!checked) {
        return R::err(checked.error().code, checked.error().message);
    }
    std::string path = std::move(checked.value());

    // Promote a leftover ".new" (a complete, synced write that was
    // interrupted before the final rename -- see file header and
    // promote_pending_new()'s doc comment).
    Result<void> promoted = promote_pending_new(path);
    if (!promoted) {
        return R::err(promoted.error().code, promoted.error().message);
    }

    if (!path_exists(path)) {
        // Neither `path` nor a promotable ".new" existed. A lone ".tmp"
        // is never promoted -- it can be a torn write from any point in
        // write_synced_file(), including the very first write to this
        // path -- so it's discarded here as garbage (best-effort; its
        // removal failing doesn't change the result).
        remove((path + ".tmp").c_str());
        return R::err(ErrorCode::NotFound, "no such file: " + path);
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return R::err(ErrorCode::IoError, "stat " + path + ": " + std::string(strerror(errno)));
    }
    if (!S_ISREG(st.st_mode)) {
        return R::err(ErrorCode::InvalidArgument, "not a regular file: " + path);
    }
    return R::ok({std::move(path), static_cast<uint64_t>(st.st_size)});
}

namespace {

// Reads exactly `size` bytes of `path` into `dst`.
Result<void> read_exact(const std::string& path, uint8_t* dst, size_t size) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return io_error("fopen", path);
    }
    size_t read = size == 0 ? 0 : fread(dst, 1, size, f);
    bool failed = ferror(f) != 0 || read != size;
    int saved_errno = errno;
    fclose(f);
    if (failed) {
        return Result<void>::err(ErrorCode::IoError,
                                 "read " + path + ": " + std::string(strerror(saved_errno)));
    }
    return Result<void>::ok();
}

}  // namespace

Result<std::vector<uint8_t>> StorageManager::read_file(const std::string& raw_path) {
    using R = Result<std::vector<uint8_t>>;
    std::lock_guard<os::Mutex> lock(mutex_);

    auto opened = open_for_read(raw_path);
    if (!opened) {
        return R::err(opened.error().code, opened.error().message);
    }
    const auto& [path, size] = opened.value();
    std::vector<uint8_t> data(static_cast<size_t>(size));
    Result<void> r = read_exact(path, data.data(), data.size());
    if (!r) {
        return R::err(r.error().code, r.error().message);
    }
    return R::ok(std::move(data));
}

Result<os::LargeBuffer> StorageManager::read_file_large(const std::string& raw_path) {
    using R = Result<os::LargeBuffer>;
    std::lock_guard<os::Mutex> lock(mutex_);

    auto opened = open_for_read(raw_path);
    if (!opened) {
        return R::err(opened.error().code, opened.error().message);
    }
    const auto& [path, size] = opened.value();
    os::LargeBuffer buf = os::LargeBuffer::allocate(static_cast<size_t>(size));
    if (!buf.ok()) {
        return R::err(ErrorCode::NoMemory, "no memory to read " + path);
    }
    Result<void> r = read_exact(path, buf.data(), buf.size());
    if (!r) {
        return R::err(r.error().code, r.error().message);
    }
    return R::ok(std::move(buf));
}

Result<std::string> StorageManager::checked_path(const std::string& raw, bool mutating) const {
    using R = Result<std::string>;
    if (!ready_) {
        return R::err(ErrorCode::StorageUnavailable, "StorageManager not initialized");
    }
    Result<std::string> normalized = normalize_path(raw);
    if (!normalized) {
        return normalized;
    }
    const std::string& path = normalized.value();
    // See has_reserved_suffix()'s doc comment.
    if (has_reserved_suffix(path)) {
        return R::err(ErrorCode::InvalidArgument, "path must not end in '.tmp' or '.new': " + path);
    }
    if (mutating && sd_dev_ && is_under(path, sd_mount_) && !sd_online_) {
        return R::err(ErrorCode::StorageUnavailable, "sd card offline: " + path);
    }
    return normalized;
}

bool StorageManager::in_internal_library(const std::string& path) const {
    return !int_mount_.empty() && is_under(path, int_mount_ + "/library");
}

Result<void> StorageManager::append(const std::string& raw_path, std::span<const uint8_t> data) {
    std::lock_guard<os::Mutex> lock(mutex_);

    Result<std::string> checked = checked_path(raw_path, /*mutating=*/true);
    if (!checked) {
        return Result<void>::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();

    // Appending to a bare path while a complete ".new" sits beside it
    // would strand that data, which a later read_file() would then find
    // shadowed by the appended file.
    WISP_TRY(promote_pending_new(path));

    if (in_internal_library(path)) {
        uint64_t projected = dir_size_recursive(int_mount_ + "/library") + data.size();
        if (projected > config_.internal_library_quota) {
            return Result<void>::err(ErrorCode::QuotaExceeded,
                                     "internal library quota exceeded: " + path);
        }
    }

    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        WISP_TRY(mkdir_p(path.substr(0, slash)));
    }
    return append_synced(path, data);
}

Result<std::vector<DirEntry>> StorageManager::list_dir(const std::string& raw_path) {
    using R = Result<std::vector<DirEntry>>;
    std::lock_guard<os::Mutex> lock(mutex_);

    Result<std::string> checked = checked_path(raw_path, /*mutating=*/false);
    if (!checked) {
        return R::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return R::err(ErrorCode::NotFound, "no such directory: " + path);
        }
        return R::err(ErrorCode::IoError, "stat " + path + ": " + std::string(strerror(errno)));
    }
    if (!S_ISDIR(st.st_mode)) {
        return R::err(ErrorCode::InvalidArgument, "not a directory: " + path);
    }

    Result<std::vector<std::string>> names = read_names(path);
    if (!names) {
        return R::err(names.error().code, names.error().message);
    }
    std::vector<DirEntry> out;
    out.reserve(names.value().size());
    for (std::string& name : names.value()) {
        DirEntry e;
        struct stat es;
        if (stat_nofollow(path + "/" + name, &es) == 0) {
            if (S_ISREG(es.st_mode)) {
                e.kind = EntryKind::File;
                e.size = static_cast<uint64_t>(es.st_size);
            } else if (S_ISDIR(es.st_mode)) {
                e.kind = EntryKind::Dir;
            }
        }
        e.name = std::move(name);
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return R::ok(std::move(out));
}

Result<void> StorageManager::remove_tree(const std::string& raw_path) {
    std::lock_guard<os::Mutex> lock(mutex_);

    Result<std::string> checked = checked_path(raw_path, /*mutating=*/true);
    if (!checked) {
        return Result<void>::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();
    if (path.empty() || path == "/" || path == int_mount_ ||
        (!sd_mount_.empty() && path == sd_mount_)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "refusing to remove a storage root: " + path);
    }

    WISP_TRY(remove_tree_at(path, 0));
    // Bookkeeping beside a removed file; a directory never has any, so
    // these are no-ops then.
    for (const char* suffix : {".new", ".tmp"}) {
        std::string extra = path + suffix;
        if (remove(extra.c_str()) != 0 && errno != ENOENT) {
            return io_error("remove", extra);
        }
    }
    return Result<void>::ok();
}

Result<void> StorageManager::rename(const std::string& raw_from, const std::string& raw_to) {
    std::lock_guard<os::Mutex> lock(mutex_);

    Result<std::string> from_r = checked_path(raw_from, /*mutating=*/true);
    if (!from_r) {
        return Result<void>::err(from_r.error().code, from_r.error().message);
    }
    Result<std::string> to_r = checked_path(raw_to, /*mutating=*/true);
    if (!to_r) {
        return Result<void>::err(to_r.error().code, to_r.error().message);
    }
    const std::string& from = from_r.value();
    const std::string& to = to_r.value();

    WISP_TRY(promote_pending_new(from));
    WISP_TRY(promote_pending_new(to));

    struct stat st;
    if (stat_nofollow(from, &st) != 0) {
        return Result<void>::err(ErrorCode::NotFound, "no such file or directory: " + from);
    }
    if (stat_nofollow(to, &st) == 0) {
        return Result<void>::err(ErrorCode::AlreadyExists, "already exists: " + to);
    }
    if (is_under(to, from)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "cannot move " + from + " into itself: " + to);
    }

    if (in_internal_library(to) && !in_internal_library(from)) {
        uint64_t projected = dir_size_recursive(int_mount_ + "/library") + path_size(from);
        if (projected > config_.internal_library_quota) {
            return Result<void>::err(ErrorCode::QuotaExceeded,
                                     "internal library quota exceeded: " + to);
        }
    }

    size_t slash = to.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        WISP_TRY(mkdir_p(to.substr(0, slash)));
    }

    // A stale ".new" left at the old name would otherwise be promoted by
    // a later read_file(from), resurrecting a file that was moved away.
    remove((from + ".new").c_str());
    remove((from + ".tmp").c_str());

    if (::rename(from.c_str(), to.c_str()) != 0) {
        return Result<void>::err(ErrorCode::IoError, "rename " + from + " -> " + to + ": " +
                                                         std::string(strerror(errno)));
    }
    return Result<void>::ok();
}

Result<uint64_t> StorageManager::tree_size(const std::string& raw_path) {
    std::lock_guard<os::Mutex> lock(mutex_);

    Result<std::string> checked = checked_path(raw_path, /*mutating=*/false);
    if (!checked) {
        return Result<uint64_t>::err(checked.error().code, checked.error().message);
    }
    return Result<uint64_t>::ok(path_size(checked.value()));
}

Result<std::string> StorageManager::internal_root() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!ready_) {
        return Result<std::string>::err(ErrorCode::StorageUnavailable,
                                        "StorageManager not initialized");
    }
    return Result<std::string>::ok(int_mount_);
}

void StorageManager::refresh() {
    std::lock_guard<os::Mutex> lock(mutex_);

    if (!sd_dev_) {
        return;
    }

    bool now_online = sd_dev_->online();
    if (now_online == sd_online_) {
        return;
    }
    sd_online_ = now_online;

    Event e;
    e.source = kSdId;
    if (sd_online_) {
        // Best-effort, same as init(): a write-protected/corrupt card
        // stays marked online rather than flipping refresh() back to
        // "removed" just because its directories couldn't be (re)made.
        ensure_sd_dirs_best_effort(sd_mount_);
        e.topic = "storage.sd.inserted";
    } else {
        e.topic = "storage.sd.removed";
    }
    bus_.publish(std::move(e));
}

StorageStatus StorageManager::status() const {
    std::lock_guard<os::Mutex> lock(mutex_);

    StorageStatus result;
    for (const auto& dev : {int_dev_, sd_dev_}) {
        if (!dev) {
            continue;
        }
        MediaStatus m;
        m.id = dev->info().id;
        m.online = dev->online();
        Result<hal::Capacity> cap = dev->capacity();
        if (cap) {
            m.total_bytes = cap.value().total_bytes;
            m.free_bytes = cap.value().free_bytes;
        }
        result.media.push_back(std::move(m));
    }
    return result;
}

void StorageManager::set_standby(bool standby) {
    // Deliberately not taking mutex_ here. PowerManager
    // calls this from the UI thread while holding its own transition_
    // mutex_, and mutex_ can be held elsewhere (write_atomic()'s fsync +
    // rename) for up to the duration of an SD write -- taking it here too
    // would make a standby transition wait out whatever write happens to
    // be in flight, eating into the wake-latency budget for no reason.
    // Safe without it: sd_dev_ is assigned exactly once, inside init()
    // (never reassigned afterwards -- see sd_dev_'s own comment), and
    // hal::Storage::set_standby() (EspSdStorage::set_standby(), in
    // practice) is itself just an atomic store plus a semaphore give(),
    // already safe to call concurrently with anything else touching the
    // same device.
    if (sd_dev_) {
        sd_dev_->set_standby(standby);
    }
}

}  // namespace wisp::core
