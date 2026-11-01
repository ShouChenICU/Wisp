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

// True if `path` has a ".." path component (lexical only, not resolved
// against the filesystem). Used to stop a caller using "<root>/../other"
// to write outside the area it was handed -- see storage_manager.hpp's
// write_atomic() doc comment.
bool has_dotdot_component(const std::string& path) {
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        std::string component =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (component == "..") {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
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
// doesn't exist). Uses <dirent.h>/<sys/stat.h> only, matching the
// constraint this whole file is built under (no std::filesystem) --
// mirrors tests/support/fake_devices.hpp's FakeStorage::dir_size(), which
// a production StorageManager can't itself depend on.
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
            total += static_cast<uint64_t>(st.st_size);
        }
    }
    closedir(d);
    return total;
}

// Creates `dir` and every missing parent directory above it (like `mkdir
// -p`). Already-existing directories (including `dir` itself) are not an
// error.
Result<void> mkdir_p(const std::string& dir) {
    if (dir.empty() || path_exists(dir)) {
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
    int_mount_ = int_dev_->mount_point();
    WISP_TRY(mkdir_p(int_mount_ + "/system"));
    WISP_TRY(mkdir_p(int_mount_ + "/library"));

    sd_dev_ = registry_.get<hal::Storage>(kSdId);
    if (sd_dev_) {
        sd_mount_ = sd_dev_->mount_point();
        sd_online_ = sd_dev_->online();
        if (sd_online_) {
            WISP_TRY(mkdir_p(sd_mount_ + "/wisp/library"));
            WISP_TRY(mkdir_p(sd_mount_ + "/wisp/backup"));
        }
    } else {
        sd_mount_.clear();
        sd_online_ = false;
    }

    return Result<void>::ok();
}

std::vector<std::string> StorageManager::roots(Area area) const {
    std::lock_guard<os::Mutex> lock(mutex_);
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

Result<void> StorageManager::write_atomic(const std::string& path, std::span<const uint8_t> data) {
    std::lock_guard<os::Mutex> lock(mutex_);

    if (has_dotdot_component(path)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "path must not contain '..' components: " + path);
    }

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
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        return Result<void>::err(ErrorCode::IoError,
                                 "fopen " + tmp + ": " + std::string(strerror(errno)));
    }
    size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
    bool write_failed = written != data.size() || ferror(f) != 0;
    bool flush_failed = !write_failed && fflush(f) != 0;
    bool sync_failed = !write_failed && !flush_failed && fsync(fileno(f)) != 0;
    int saved_errno = errno;
    fclose(f);

    if (write_failed || flush_failed || sync_failed) {
        remove(tmp.c_str());
        return Result<void>::err(ErrorCode::IoError,
                                 "write " + tmp + ": " + std::string(strerror(saved_errno)));
    }

    if (path_exists(path)) {
        if (remove(path.c_str()) != 0) {
            return Result<void>::err(ErrorCode::IoError,
                                     "remove " + path + ": " + std::string(strerror(errno)));
        }
    }

    if (rename(tmp.c_str(), path.c_str()) != 0) {
        return Result<void>::err(ErrorCode::IoError, "rename " + tmp + " -> " + path + ": " +
                                                         std::string(strerror(errno)));
    }

    return Result<void>::ok();
}

Result<std::vector<uint8_t>> StorageManager::read_file(const std::string& path) {
    std::lock_guard<os::Mutex> lock(mutex_);

    if (has_dotdot_component(path)) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::InvalidArgument,
                                                 "path must not contain '..' components: " + path);
    }

    if (!path_exists(path)) {
        std::string tmp = path + ".tmp";
        if (!path_exists(tmp)) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::IoError, "no such file: " + path);
        }
        if (rename(tmp.c_str(), path.c_str()) != 0) {
            return Result<std::vector<uint8_t>>::err(
                ErrorCode::IoError,
                "rename " + tmp + " -> " + path + ": " + std::string(strerror(errno)));
        }
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return Result<std::vector<uint8_t>>::err(
            ErrorCode::IoError, "fopen " + path + ": " + std::string(strerror(errno)));
    }

    std::vector<uint8_t> data(file_size(path));
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
        Result<void> r1 = mkdir_p(sd_mount_ + "/wisp/library");
        Result<void> r2 = mkdir_p(sd_mount_ + "/wisp/backup");
        if (!r1) {
            WLOG_W(kTag, "failed to create %s: %s", (sd_mount_ + "/wisp/library").c_str(),
                   r1.error().message.c_str());
        }
        if (!r2) {
            WLOG_W(kTag, "failed to create %s: %s", (sd_mount_ + "/wisp/backup").c_str(),
                   r2.error().message.c_str());
        }
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
