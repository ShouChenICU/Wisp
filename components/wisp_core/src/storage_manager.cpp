#include "wisp/core/storage_manager.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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
// components, and drops a trailing '/' (so the result never has one,
// matching int_mount_/sd_mount_ -- see strip_trailing_slash() and
// storage_manager.hpp's file header). A ".." component is never resolved
// -- it's rejected outright with ErrorCode::InvalidArgument, since
// resolving one correctly would require knowing the filesystem's current
// state (e.g. what a symlink points at), which this lexical pass
// deliberately doesn't attempt. Used by write_atomic()/read_file() so
// that a non-canonical alias of a path (a doubled slash, a "." segment)
// can't be used to dodge the library quota or otherwise be treated
// differently from its canonical form.
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

// Drops a single trailing '/' from `dir`, if any (but never reduces a
// bare "/" to an empty string). Applied to every device's mount_point()
// in init(), so a driver that reports one with a trailing slash still
// composes correctly with the "/library", "/system" etc. suffixes this
// file appends, and still matches is_under()'s prefix checks against a
// normalize_path()'d caller path (which never has one either).
std::string strip_trailing_slash(std::string dir) {
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    return dir;
}

// True if `name` (a bare filename, no directory component) ends with
// `suffix`.
bool has_suffix(const std::string& name, const char* suffix) {
    size_t len = std::strlen(suffix);
    return name.size() >= len && name.compare(name.size() - len, len, suffix) == 0;
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
uint64_t dir_size_recursive(const std::string& dir) {
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
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            total += dir_size_recursive(full);
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

}  // namespace

StorageManager::StorageManager(DeviceRegistry& registry, EventBus& bus, StorageConfig config)
    : registry_(registry), bus_(bus), config_(config) {}

Result<void> StorageManager::init() {
    std::lock_guard<os::Mutex> lock(mutex_);

    int_dev_ = registry_.get<hal::Storage>(kIntId);
    if (!int_dev_) {
        return Result<void>::err(ErrorCode::StorageUnavailable,
                                 std::string(kIntId) + " not registered");
    }
    int_mount_ = strip_trailing_slash(int_dev_->mount_point());
    WISP_TRY(mkdir_p(int_mount_ + "/system"));
    WISP_TRY(mkdir_p(int_mount_ + "/library"));

    sd_dev_ = registry_.get<hal::Storage>(kSdId);
    if (sd_dev_) {
        sd_mount_ = strip_trailing_slash(sd_dev_->mount_point());
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

    if (sd_dev_ && !sd_mount_.empty() && is_under(path, sd_mount_) && !sd_online_) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "sd card offline: " + path);
    }

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
    if (rename(tmp.c_str(), neu.c_str()) != 0) {
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

    if (rename(neu.c_str(), path.c_str()) != 0) {
        return Result<void>::err(ErrorCode::IoError, "rename " + neu + " -> " + path + ": " +
                                                         std::string(strerror(errno)));
    }

    return Result<void>::ok();
}

Result<std::vector<uint8_t>> StorageManager::read_file(const std::string& raw_path) {
    std::lock_guard<os::Mutex> lock(mutex_);

    if (!ready_) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::StorageUnavailable,
                                                 "StorageManager not initialized");
    }

    Result<std::string> normalized = normalize_path(raw_path);
    if (!normalized) {
        return Result<std::vector<uint8_t>>::err(normalized.error().code,
                                                 normalized.error().message);
    }
    const std::string& path = normalized.value();

    if (!path_exists(path)) {
        // Promote a leftover ".new" (a complete, synced write that was
        // interrupted before the final rename -- see file header). A
        // lone ".tmp" is never promoted -- it can be a torn write from
        // any point in write_synced_file(), including the very first
        // write to this path -- so it's discarded here as garbage
        // (best-effort; its removal failing doesn't change the result).
        std::string neu = path + ".new";
        if (path_exists(neu)) {
            if (rename(neu.c_str(), path.c_str()) != 0) {
                return Result<std::vector<uint8_t>>::err(
                    ErrorCode::IoError,
                    "rename " + neu + " -> " + path + ": " + std::string(strerror(errno)));
            }
        } else {
            remove((path + ".tmp").c_str());
            return Result<std::vector<uint8_t>>::err(ErrorCode::NotFound, "no such file: " + path);
        }
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return Result<std::vector<uint8_t>>::err(
            ErrorCode::IoError, "stat " + path + ": " + std::string(strerror(errno)));
    }
    if (!S_ISREG(st.st_mode)) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::InvalidArgument,
                                                 "not a regular file: " + path);
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return Result<std::vector<uint8_t>>::err(
            ErrorCode::IoError, "fopen " + path + ": " + std::string(strerror(errno)));
    }

    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    size_t read = data.empty() ? 0 : fread(data.data(), 1, data.size(), f);
    bool failed = ferror(f) != 0 || read != data.size();
    int saved_errno = errno;
    fclose(f);
    if (failed) {
        return Result<std::vector<uint8_t>>::err(
            ErrorCode::IoError, "read " + path + ": " + std::string(strerror(saved_errno)));
    }

    return Result<std::vector<uint8_t>>::ok(std::move(data));
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

}  // namespace wisp::core
