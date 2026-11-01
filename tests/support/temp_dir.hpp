#pragma once

// Test-only helper for creating scratch directories on disk (not built
// into any production component -- see fake_devices.hpp's header comment
// for why that's fine here). Ordinary std::filesystem is used throughout
// (allowed in test code, unlike the portable wisp_* components); since
// wisp_test() targets link against -fno-exceptions libraries, every call
// below uses the std::error_code overloads instead of the throwing ones.

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace wisp::test {

namespace detail {

// Tracks every directory handed out by temp_dir() in this process and
// removes them all (recursively, best-effort) when the process exits.
class TempDirRegistry {
public:
    static TempDirRegistry& instance() {
        static TempDirRegistry registry;
        return registry;
    }

    void track(std::filesystem::path dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        dirs_.push_back(std::move(dir));
    }

    ~TempDirRegistry() {
        std::error_code ec;
        for (const auto& dir : dirs_) {
            std::filesystem::remove_all(dir, ec);
        }
    }

private:
    TempDirRegistry() = default;

    std::mutex mutex_;
    std::vector<std::filesystem::path> dirs_;
};

}  // namespace detail

// Creates and returns a new, empty directory under the system temp
// directory; every call returns a different directory. Every directory
// returned by temp_dir() in this process is recursively removed when the
// process exits.
inline std::filesystem::path temp_dir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        base = "/tmp";
    }

    static std::atomic<uint64_t> counter{0};
    uint64_t n = counter.fetch_add(1);

    std::filesystem::path dir = base / ("wisp_test_" + std::to_string(static_cast<long>(getpid())) +
                                        "_" + std::to_string(n));
    std::filesystem::create_directories(dir, ec);

    detail::TempDirRegistry::instance().track(dir);
    return dir;
}

}  // namespace wisp::test
