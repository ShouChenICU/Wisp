#include <dirent.h>
#include <sys/stat.h>

#include <utility>

#include "sim_devices.hpp"

namespace wisp::port::sim {

namespace {

// Recursively sums the regular-file sizes under `path` (matches
// tests/support/fake_devices.hpp's FakeStorage -- see its comment for
// why <dirent.h>/<sys/stat.h> rather than std::filesystem; this file is
// host-only, so either would work, but the simulator's reported numbers
// stay consistent with the test fakes' convention this way).
uint64_t dir_size(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (d == nullptr) {
        return 0;
    }
    uint64_t total = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        std::string full = path + "/" + name;
        struct stat st{};
        if (lstat(full.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            total += dir_size(full);
        } else if (S_ISREG(st.st_mode)) {
            total += static_cast<uint64_t>(st.st_size);
        }
    }
    closedir(d);
    return total;
}

}  // namespace

SimStorage::SimStorage(std::string id, std::string mount_point, bool removable, std::string fs,
                       uint64_t total_bytes, std::function<bool()> online_fn)
    : mount_point_(std::move(mount_point)),
      total_bytes_(total_bytes),
      online_fn_(std::move(online_fn)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "host-dir";
    info_.caps = wisp::Properties{{"removable", removable}, {"fs", wisp::Value(std::move(fs))}};
}

wisp::Result<wisp::hal::Capacity> SimStorage::capacity() {
    wisp::hal::Capacity cap;
    cap.total_bytes = total_bytes_;
    uint64_t used = dir_size(mount_point_);
    cap.free_bytes = used >= total_bytes_ ? 0 : total_bytes_ - used;
    return wisp::Result<wisp::hal::Capacity>::ok(cap);
}

}  // namespace wisp::port::sim
