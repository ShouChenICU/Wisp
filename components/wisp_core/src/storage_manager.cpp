#include "wisp/core/storage_manager.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "storage_manager";

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
// deliberately excludes these suffixes -- see dir_size()),
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

// Directory walks below are iterative, with their work list in
// Mem::Large: they run on the 4 KB wisp.fs stack (StorageWorker), where a
// recursive walk costs one 170-260 B frame per level on the device
// (measured with -fstack-usage) on top of the filesystem driver's own
// frames. Each still stops at kMaxWalkDepth levels below where it
// started; see dir_size()'s and remove_tree_at()'s comments for what that
// bound protects against.
constexpr int kMaxWalkDepth = 16;

struct WalkItem {
    std::string path;
    int depth = 0;
    bool expanded = false;  // remove_tree_at(): children already queued
};

// Sums the size of every regular file under `dir` (0 if `dir` doesn't
// exist), excluding write_atomic()'s own "*.tmp"/"*.new" temp files --
// those are transient write bookkeeping, not library content, so counting
// them would make the quota fluctuate with in-flight writes instead of
// actual stored data. Uses <dirent.h>/<sys/stat.h> only, matching the
// constraint this whole file is built under (no std::filesystem) --
// mirrors tests/support/fake_devices.hpp's FakeStorage::dir_size(), which
// a production StorageManager can't itself depend on.
//
// Descends at most kMaxWalkDepth directory levels: stat() follows
// symlinks, so on the host a link back up the tree would otherwise loop
// forever, and several such links multiply the paths walked per level
// (the device's filesystems have no symlinks).
uint64_t dir_size(const std::string& dir) {
    uint64_t total = 0;
    os::LargeVector<WalkItem> work;
    work.push_back({dir, 0, false});
    while (!work.empty()) {
        WalkItem item = std::move(work.back());
        work.pop_back();
        DIR* d = opendir(item.path.c_str());
        if (!d) {
            continue;
        }
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            std::string full = item.path + "/" + name;
            // stat, not lstat: the device's libc has no lstat, and
            // neither FAT nor LittleFS has symlinks to tell apart anyway.
            struct stat st;
            if (stat(full.c_str(), &st) != 0) {
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (item.depth < kMaxWalkDepth) {
                    work.push_back({std::move(full), item.depth + 1, false});
                }
            } else if (S_ISREG(st.st_mode)) {
                if (has_suffix(name, ".tmp") || has_suffix(name, ".new")) {
                    continue;
                }
                total += static_cast<uint64_t>(st.st_size);
            }
        }
        closedir(d);
    }
    return total;
}

// Creates `dir` and every missing parent directory above it (like `mkdir
// -p`), shallowest first. Already-existing directories (including `dir`
// itself) are not an error, but an existing non-directory (e.g. a plain
// file) at `dir` or at any parent is -- ErrorCode::IoError, since mkdir()
// would otherwise silently treat it as "already there" via the EEXIST
// check below without this upfront type check.
Result<void> mkdir_p(const std::string& dir) {
    if (dir.empty()) {
        return Result<void>::ok();
    }
    size_t pos = 0;
    while (true) {
        pos = dir.find('/', pos + 1);
        std::string prefix = dir.substr(0, pos);
        struct stat st;
        if (stat(prefix.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                return Result<void>::err(ErrorCode::IoError, "not a directory: " + prefix);
            }
        } else if (mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
            return Result<void>::err(ErrorCode::IoError,
                                     "mkdir " + prefix + ": " + std::string(strerror(errno)));
        }
        if (pos == std::string::npos) {
            return Result<void>::ok();
        }
    }
}

static_assert(std::is_trivially_copyable_v<SdStatus>,
              "the cell copies the status under its lock without allocating");

// Reproduces the storage.sd.inserted / storage.sd.removed event refresh()
// published. The topic comes from `after`, not from a flip of `before`.
void sd_mirror(EventBus& bus, const SdStatus&, const SdStatus& after) {
    Event e;
    e.source = kStorageManagerSource;
    e.topic = after.online ? topics::kStorageSdInserted : topics::kStorageSdRemoved;
    bus.publish(std::move(e));
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

// Removes the file or tree at `path` without following symlinks,
// children before their directory. A directory more than kMaxWalkDepth
// levels below `path` fails the call with ErrorCode::IoError: the
// manager never creates one (see check_depth()), so meeting one means
// something else built it, and a bounded walk is what keeps a corrupt or
// hostile tree from exhausting memory.
Result<void> remove_tree_at(const std::string& path) {
    os::LargeVector<WalkItem> work;
    work.push_back({path, 0, false});
    while (!work.empty()) {
        WalkItem item = std::move(work.back());
        work.pop_back();
        if (item.expanded) {
            if (rmdir(item.path.c_str()) != 0) {
                return io_error("rmdir", item.path);
            }
            continue;
        }
        struct stat st;
        if (stat_nofollow(item.path, &st) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            return io_error("stat", item.path);
        }
        if (!S_ISDIR(st.st_mode)) {
            if (remove(item.path.c_str()) != 0 && errno != ENOENT) {
                return io_error("remove", item.path);
            }
            continue;
        }
        if (item.depth >= kMaxWalkDepth) {
            return Result<void>::err(ErrorCode::IoError, "directory tree too deep: " + item.path);
        }
        Result<std::vector<std::string>> names = read_names(item.path);
        if (!names) {
            return Result<void>::err(names.error().code, names.error().message);
        }
        int child_depth = item.depth + 1;
        std::string dir = item.path;
        work.push_back({std::move(item.path), item.depth, true});
        for (const std::string& name : names.value()) {
            work.push_back({dir + "/" + name, child_depth, false});
        }
    }
    return Result<void>::ok();
}

// Appends `data` to `path` and makes it durable (fsync) before closing.
// A raw file descriptor, not stdio: on failure the file is truncated back
// to its size before the call, and a FILE* could still hold buffered
// bytes that fclose() would write after that truncate. Undoing a failed
// write matters because the caller's next append would otherwise land
// after the torn bytes, burying them mid-file where a reader that drops
// only a torn *tail* cannot get rid of them.
Result<void> append_synced(const std::string& path, std::span<const uint8_t> data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return io_error("open", path);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        Result<void> err = io_error("fstat", path);
        close(fd);
        return err;
    }
    const off_t old_size = st.st_size;

    const char* failed_op = nullptr;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n == 0) {
                errno = EIO;
            }
            failed_op = "write";
            break;
        }
        done += static_cast<size_t>(n);
    }
    if (failed_op == nullptr && fsync(fd) != 0) {
        failed_op = "fsync";
    }
    if (failed_op != nullptr) {
        Result<void> err = io_error(failed_op, path);
        if (ftruncate(fd, old_size) != 0 || fsync(fd) != 0) {
            err = Result<void>::err(ErrorCode::IoError,
                                    err.error().message + "; could not undo the partial append: " +
                                        std::string(strerror(errno)));
        }
        close(fd);
        return err;
    }
    if (close(fd) != 0) {
        return io_error("close", path);
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
        return dir_size(path);
    }
    return S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
}

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

StorageManager::StorageManager(DeviceRegistry& registry, EventBus& bus, StorageConfig config)
    : registry_(registry), bus_(bus), config_(config), sd_cell_(bus, SdStatus{}, &sd_mirror) {}

StorageManager::View StorageManager::view() const {
    std::lock_guard<os::Mutex> lock(state_mu_);
    return View{ready_, int_mount_, sd_mount_, sd_online_};
}

Medium StorageManager::medium_in(const View& v, const std::string& path) {
    return v.ready && is_under(path, v.sd_mount) ? Medium::Sd : Medium::Internal;
}

Medium StorageManager::medium_of(const std::string& raw_path) const {
    Result<std::string> normalized = normalize_path(raw_path);
    if (!normalized) {
        return Medium::Internal;
    }
    return medium_in(view(), normalized.value());
}

Result<Medium> StorageManager::medium_for_call(const std::string& raw_path,
                                               uint64_t calls_on_error) const {
    Result<std::string> normalized = normalize_path(raw_path);
    if (!normalized) {
        calls_.fetch_add(calls_on_error, std::memory_order_relaxed);
        return Result<Medium>::err(normalized.error().code, normalized.error().message);
    }
    return Result<Medium>::ok(medium_in(view(), normalized.value()));
}

bool StorageManager::has_sd() const {
    std::lock_guard<os::Mutex> lock(state_mu_);
    return ready_ && sd_dev_ != nullptr;
}

Result<void> StorageManager::init() {
    WISP_ASSERT_FLASH_THREAD("StorageManager::init");
    std::lock_guard<os::Mutex> int_io(int_io_mu_);
    std::lock_guard<os::Mutex> sd_io(sd_io_mu_);

    // Not ready from here on, so that if this call is a *re*-init that
    // fails partway (e.g. storage.int was unplugged since a previous
    // successful init()), the manager is left unusable
    // (StorageUnavailable/empty roots) rather than silently continuing to
    // operate on the previous, now-stale mount state. It only becomes
    // ready again at the very end, once every step below has succeeded.
    {
        std::lock_guard<os::Mutex> state(state_mu_);
        ready_ = false;
    }

    // Everything is worked out in locals first: the directory creation and
    // online() below are I/O, which never runs under state_mu_.
    std::shared_ptr<hal::Storage> int_dev = registry_.by_role<hal::Storage>(hal::role::kInternal);
    if (!int_dev) {
        return Result<void>::err(ErrorCode::StorageUnavailable,
                                 "no storage device with role internal");
    }
    Result<std::string> int_norm = normalize_path(int_dev->mount_point());
    if (!int_norm) {
        return Result<void>::err(int_norm.error().code, int_norm.error().message);
    }
    const std::string& int_mount = int_norm.value();
    WISP_TRY(mkdir_p(int_mount + "/system"));
    WISP_TRY(mkdir_p(int_mount + "/library"));

    std::shared_ptr<hal::Storage> sd_dev = registry_.by_role<hal::Storage>(hal::role::kRemovable);
    if (sd_dev == int_dev) {
        // One medium cannot be both: as a card it would be listed twice,
        // get card events and take the Library root off internal flash.
        WLOG_W(kTag, "'%s' has both roles internal and removable; treating it as internal only",
               int_dev->info().id.c_str());
        sd_dev.reset();
    }
    std::string sd_mount;
    bool sd_online = false;
    if (sd_dev) {
        Result<std::string> sd_norm = normalize_path(sd_dev->mount_point());
        if (!sd_norm) {
            return Result<void>::err(sd_norm.error().code, sd_norm.error().message);
        }
        sd_mount = sd_norm.value();
        sd_online = sd_dev->online();
        if (sd_online) {
            // Best-effort: a write-protected/corrupt SD card must not
            // fail init() for internal storage, which is otherwise fine
            // -- see this method's doc comment.
            ensure_sd_dirs_best_effort(sd_mount);
        }
    }

    // Silent: nothing is published at boot, as before. Under both I/O
    // locks, so no refresh() moves the cell in between.
    sd_cell_.seed({sd_online, sd_cell_.get().changes});

    std::lock_guard<os::Mutex> state(state_mu_);
    int_dev_ = std::move(int_dev);
    sd_dev_ = std::move(sd_dev);
    int_mount_ = int_mount;
    sd_mount_ = std::move(sd_mount);
    sd_online_ = sd_online;
    ready_ = true;
    return Result<void>::ok();
}

std::vector<std::string> StorageManager::roots(Area area) const {
    const View v = view();
    if (!v.ready) {
        return {};
    }
    switch (area) {
        case Area::System:
            return {v.int_mount + "/system"};
        case Area::Library:
            if (v.sd_online) {
                return {v.sd_mount + "/wisp/library", v.int_mount + "/library"};
            }
            return {v.int_mount + "/library"};
        case Area::Backup:
            if (v.sd_online) {
                return {v.sd_mount + "/wisp/backup"};
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
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return Result<void>::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::write_atomic");
    std::lock_guard<os::Mutex> io(io_mu(medium));
    const View v = view();

    // checked_path() also rejects a caller writing directly to one of
    // write_atomic()'s own reserved names, which would be invisible to the
    // quota and clobbered by this very function's "<path>.new"
    // bookkeeping below -- see has_reserved_suffix()'s doc comment.
    Result<std::string> checked = checked_path(v, raw_path, /*mutating=*/true);
    if (!checked) {
        return Result<void>::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();
    WISP_TRY(check_locked_medium(v, path, medium));

    // Recover a "<path>.new" orphaned by a *double*
    // power cut (first write completed and synced, but the final rename
    // to `path` never happened; then this write_atomic() call starts
    // before anything noticed) before this call's own bookkeeping -- in
    // particular before the unconditional `remove(neu)` a few lines
    // below -- would otherwise discard that data without ever exposing
    // it. This also makes the quota calculation right below see the
    // recovered file's size.
    WISP_TRY(promote_pending_new(path));

    if (in_internal_library(v, path)) {
        const std::string library_root = v.int_mount + "/library";
        uint64_t existing_total = dir_size(library_root);
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

Result<std::pair<std::string, uint64_t>> StorageManager::open_for_read(const View& v,
                                                                       const std::string& raw_path,
                                                                       Medium medium) {
    using R = Result<std::pair<std::string, uint64_t>>;

    Result<std::string> checked = checked_path(v, raw_path, /*mutating=*/false);
    if (!checked) {
        return R::err(checked.error().code, checked.error().message);
    }
    std::string path = std::move(checked.value());
    if (Result<void> m = check_locked_medium(v, path, medium); !m) {
        return R::err(m.error().code, m.error().message);
    }

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

Result<std::vector<uint8_t>> StorageManager::read_file(const std::string& raw_path) {
    using R = Result<std::vector<uint8_t>>;
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return R::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::read_file");
    std::lock_guard<os::Mutex> io(io_mu(medium));

    auto opened = open_for_read(view(), raw_path, medium);
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
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return R::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::read_file_large");
    std::lock_guard<os::Mutex> io(io_mu(medium));

    auto opened = open_for_read(view(), raw_path, medium);
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

Result<os::LargeBuffer> StorageManager::read_range(const std::string& raw_path, uint64_t offset,
                                                   size_t len) {
    using R = Result<os::LargeBuffer>;
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return R::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::read_range");
    std::lock_guard<os::Mutex> io(io_mu(medium));

    auto opened = open_for_read(view(), raw_path, medium);
    if (!opened) {
        return R::err(opened.error().code, opened.error().message);
    }
    const auto& [path, size] = opened.value();
    const uint64_t left = offset >= size ? 0 : size - offset;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(left, len));
    os::LargeBuffer buf = os::LargeBuffer::allocate(n);
    if (!buf.ok()) {
        return R::err(ErrorCode::NoMemory, "no memory to read " + path);
    }
    if (n == 0) {
        return R::ok(std::move(buf));
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        Result<void> e = io_error("fopen", path);
        return R::err(e.error().code, e.error().message);
    }
    // errno is taken right after the call that failed: a short read that
    // is only the end of the file sets none.
    int saved_errno = 0;
    size_t got = 0;
    if (fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        saved_errno = errno;
    } else {
        got = fread(buf.data(), 1, n, f);
        if (ferror(f) != 0) saved_errno = errno != 0 ? errno : EIO;
    }
    fclose(f);
    if (saved_errno != 0) {
        return R::err(ErrorCode::IoError,
                      "read " + path + ": " + std::string(strerror(saved_errno)));
    }
    // The file shrank since it was measured: what is there.
    if (got < n) buf.truncate(got);
    return R::ok(std::move(buf));
}

Result<std::string> StorageManager::checked_path(const View& v, const std::string& raw,
                                                 bool mutating) const {
    using R = Result<std::string>;
    calls_.fetch_add(1, std::memory_order_relaxed);
    if (!v.ready) {
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
    if (mutating && is_under(path, v.sd_mount) && !v.sd_online) {
        return R::err(ErrorCode::StorageUnavailable, "sd card offline: " + path);
    }
    Result<void> depth = check_depth(v, path);
    if (!depth) {
        return R::err(depth.error().code, depth.error().message);
    }
    return normalized;
}

Result<void> StorageManager::check_locked_medium(const View& v, const std::string& path,
                                                 Medium locked) {
    // The medium was picked before its lock was taken; only an init()
    // in between (it holds both I/O locks) can move `path` to the other
    // one, and then this call holds the wrong lock for it.
    if (medium_in(v, path) != locked) {
        return Result<void>::err(ErrorCode::StorageUnavailable,
                                 "storage re-initialized during the call: " + path);
    }
    return Result<void>::ok();
}

Result<void> StorageManager::check_depth(const View& v, const std::string& path) {
    for (const std::string* mount : {&v.int_mount, &v.sd_mount}) {
        if (!is_under(path, *mount)) {
            continue;
        }
        size_t levels = static_cast<size_t>(
            std::count(path.begin() + static_cast<std::ptrdiff_t>(mount->size()), path.end(), '/'));
        if (levels > static_cast<size_t>(kMaxWalkDepth)) {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     "path is more than 16 levels below its mount: " + path);
        }
        break;
    }
    return Result<void>::ok();
}

Result<void> StorageManager::check_contained(const View& v, const std::string& path) {
    bool inside = path.size() > v.int_mount.size() && is_under(path, v.int_mount);
    if (!inside && v.sd_online && !v.sd_mount.empty()) {
        inside = path.size() > v.sd_mount.size() && is_under(path, v.sd_mount);
    }
    bool is_root = path == v.int_mount + "/system" || path == v.int_mount + "/library" ||
                   (!v.sd_mount.empty() &&
                    (path == v.sd_mount + "/wisp" || path == v.sd_mount + "/wisp/library" ||
                     path == v.sd_mount + "/wisp/backup"));
    if (path.empty() || path.front() != '/' || !inside || is_root) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "not a removable path inside storage: " + path);
    }
    return Result<void>::ok();
}

bool StorageManager::in_internal_library(const View& v, const std::string& path) {
    return !v.int_mount.empty() && is_under(path, v.int_mount + "/library");
}

Result<void> StorageManager::append(const std::string& raw_path, std::span<const uint8_t> data) {
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return Result<void>::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::append");
    std::lock_guard<os::Mutex> io(io_mu(medium));
    const View v = view();

    Result<std::string> checked = checked_path(v, raw_path, /*mutating=*/true);
    if (!checked) {
        return Result<void>::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();
    WISP_TRY(check_locked_medium(v, path, medium));

    // Appending to a bare path while a complete ".new" sits beside it
    // would strand that data, which a later read_file() would then find
    // shadowed by the appended file.
    WISP_TRY(promote_pending_new(path));

    if (in_internal_library(v, path)) {
        uint64_t projected = dir_size(v.int_mount + "/library") + data.size();
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
    // A malformed path is left to visit_dir(), which rejects (and counts)
    // it before any thread check.
    if (Result<std::string> normalized = normalize_path(raw_path); normalized) {
        WISP_ASSERT_MEDIUM_THREAD(medium_in(view(), normalized.value()) == Medium::Sd,
                                  "StorageManager::list_dir");
    }
    std::vector<DirEntry> out;
    Result<void> r = visit_dir(raw_path, [&out](const DirEntry& e) {
        out.push_back(e);
        return true;
    });
    if (!r) {
        return Result<std::vector<DirEntry>>::err(r.error().code, r.error().message);
    }
    std::sort(out.begin(), out.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return Result<std::vector<DirEntry>>::ok(std::move(out));
}

Result<void> StorageManager::visit_dir(const std::string& raw_path,
                                       const std::function<bool(const DirEntry&)>& visit) {
    using R = Result<void>;
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return R::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::visit_dir");
    std::lock_guard<os::Mutex> io(io_mu(medium));
    const View v = view();

    Result<std::string> checked = checked_path(v, raw_path, /*mutating=*/false);
    if (!checked) {
        return R::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();
    WISP_TRY(check_locked_medium(v, path, medium));

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

    DIR* d = opendir(path.c_str());
    if (!d) {
        return R::err(ErrorCode::IoError, "opendir " + path + ": " + std::string(strerror(errno)));
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        DirEntry e;
        e.name = entry->d_name;
        if (e.name == "." || e.name == "..") {
            continue;
        }
        struct stat es;
        if (stat_nofollow(path + "/" + e.name, &es) == 0) {
            e.mtime = static_cast<int64_t>(es.st_mtime);
            if (S_ISREG(es.st_mode)) {
                e.kind = EntryKind::File;
                e.size = static_cast<uint64_t>(es.st_size);
            } else if (S_ISDIR(es.st_mode)) {
                e.kind = EntryKind::Dir;
            }
        }
        if (!visit(e)) {
            break;
        }
    }
    closedir(d);
    return R::ok();
}

Result<void> StorageManager::remove_tree(const std::string& raw_path) {
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return Result<void>::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::remove_tree");
    std::lock_guard<os::Mutex> io(io_mu(medium));
    const View v = view();

    Result<std::string> checked = checked_path(v, raw_path, /*mutating=*/true);
    if (!checked) {
        return Result<void>::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();
    WISP_TRY(check_locked_medium(v, path, medium));
    WISP_TRY(check_contained(v, path));

    WISP_TRY(remove_tree_at(path));
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

Result<void> StorageManager::make_dirs(const std::string& raw_path) {
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return Result<void>::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::make_dirs");
    std::lock_guard<os::Mutex> io(io_mu(medium));
    const View v = view();

    Result<std::string> checked = checked_path(v, raw_path, /*mutating=*/true);
    if (!checked) {
        return Result<void>::err(checked.error().code, checked.error().message);
    }
    const std::string& path = checked.value();
    WISP_TRY(check_locked_medium(v, path, medium));
    WISP_TRY(check_contained(v, path));
    return mkdir_p(path);
}

Result<void> StorageManager::rename(const std::string& raw_from, const std::string& raw_to) {
    Result<Medium> picked = medium_for_call(raw_from);
    if (!picked) {
        return Result<void>::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    // A malformed target counts both paths, as a rename that reaches it
    // always has.
    if (Result<Medium> to_picked = medium_for_call(raw_to, /*calls_on_error=*/2); !to_picked) {
        return Result<void>::err(to_picked.error().code, to_picked.error().message);
    }
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::rename");
    std::lock_guard<os::Mutex> io(io_mu(medium));
    const View v = view();

    Result<std::string> from_r = checked_path(v, raw_from, /*mutating=*/true);
    if (!from_r) {
        return Result<void>::err(from_r.error().code, from_r.error().message);
    }
    Result<std::string> to_r = checked_path(v, raw_to, /*mutating=*/true);
    if (!to_r) {
        return Result<void>::err(to_r.error().code, to_r.error().message);
    }
    const std::string& from = from_r.value();
    const std::string& to = to_r.value();
    WISP_TRY(check_locked_medium(v, from, medium));
    // Only `from`'s medium is locked (and checked against this thread), so
    // `to` must be on it too; the filesystems cannot rename across devices
    // anyway.
    if (medium_in(v, to) != medium) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "cannot rename across storage devices: " + from + " -> " + to);
    }
    WISP_TRY(check_contained(v, from));
    WISP_TRY(check_contained(v, to));

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

    if (in_internal_library(v, to) && !in_internal_library(v, from)) {
        uint64_t projected = dir_size(v.int_mount + "/library") + path_size(from);
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
    Result<Medium> picked = medium_for_call(raw_path);
    if (!picked) {
        return Result<uint64_t>::err(picked.error().code, picked.error().message);
    }
    const Medium medium = picked.value();
    WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "StorageManager::tree_size");
    std::lock_guard<os::Mutex> io(io_mu(medium));
    const View v = view();

    Result<std::string> checked = checked_path(v, raw_path, /*mutating=*/false);
    if (!checked) {
        return Result<uint64_t>::err(checked.error().code, checked.error().message);
    }
    if (Result<void> m = check_locked_medium(v, checked.value(), medium); !m) {
        return Result<uint64_t>::err(m.error().code, m.error().message);
    }
    return Result<uint64_t>::ok(path_size(checked.value()));
}

Result<std::string> StorageManager::internal_root() const {
    std::lock_guard<os::Mutex> lock(state_mu_);
    if (!ready_) {
        return Result<std::string>::err(ErrorCode::StorageUnavailable,
                                        "StorageManager not initialized");
    }
    return Result<std::string>::ok(int_mount_);
}

void StorageManager::refresh() {
    WISP_ASSERT_SD_IO_THREAD("StorageManager::refresh");
    // online() may deinit a removed card (EspSdStorage), which is only
    // safe with no card I/O in flight: holding sd_io_mu_ waits out the
    // running card operation, and the one after this sees the new state.
    std::lock_guard<os::Mutex> io(sd_io_mu_);

    std::shared_ptr<hal::Storage> sd_dev;
    bool was_online = false;
    std::string sd_mount;
    {
        std::lock_guard<os::Mutex> state(state_mu_);
        sd_dev = sd_dev_;
        was_online = sd_online_;
        sd_mount = sd_mount_;
    }
    if (!sd_dev) {
        return;
    }

    bool now_online = sd_dev->online();
    if (now_online == was_online) {
        return;
    }
    {
        std::lock_guard<os::Mutex> state(state_mu_);
        sd_online_ = now_online;
    }

    if (now_online) {
        // Best-effort, same as init(): a write-protected/corrupt card
        // stays marked online rather than flipping refresh() back to
        // "removed" just because its directories couldn't be (re)made.
        ensure_sd_dirs_best_effort(sd_mount);
    }
    // Always a new value: `changes` moves, so no event is lost to equal-value
    // skipping, and the mirror runs after the mount above, as the publish did.
    // Set under sd_io_mu_, so two refresh() calls move the cell (and the
    // mirror publishes) in the order they observed the card; the mirror's
    // publish() never blocks on (or calls back into) a handler -- see
    // event_bus.hpp.
    sd_cell_.update([&](SdStatus& s) {
        s.online = now_online;
        ++s.changes;
    });
}

StorageStatus StorageManager::status() const {
    WISP_ASSERT_FLASH_THREAD("StorageManager::status");
    std::lock_guard<os::Mutex> int_io(int_io_mu_);
    std::lock_guard<os::Mutex> sd_io(sd_io_mu_);

    std::shared_ptr<hal::Storage> int_dev;
    std::shared_ptr<hal::Storage> sd_dev;
    {
        std::lock_guard<os::Mutex> state(state_mu_);
        int_dev = int_dev_;
        sd_dev = sd_dev_;
    }

    StorageStatus result;
    for (const auto& dev : {int_dev, sd_dev}) {
        if (!dev) {
            continue;
        }
        MediaStatus m;
        m.id = dev->info().id;
        m.removable = dev == sd_dev;
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
    // Deliberately takes no lock. PowerManager calls this from the UI
    // thread while holding its own transition_ mutex, and sd_io_mu_ can be
    // held for a whole card operation (write_atomic()'s fsync + rename)
    // -- taking it here too would make a standby transition wait out
    // whatever card write happens to be in flight, eating into the
    // wake-latency budget for no reason. Safe without it: sd_dev_ is
    // assigned inside init() only (see sd_dev_'s own comment), and
    // hal::Storage::set_standby() (EspSdStorage::set_standby(), in
    // practice) is itself just an atomic store plus a semaphore give(),
    // already safe to call concurrently with anything else touching the
    // same device.
    if (sd_dev_) {
        sd_dev_->set_standby(standby);
    }
}

}  // namespace wisp::core
