#pragma once

// Test-only stand-in for the storage worker behind a service's submit_io:
// jobs are only collected, and run_all() runs them later, in order, on a
// thread of its own registered as the flash thread -- so a test can see
// what a service wrote on the calling thread (nothing) and what it left
// for the worker.

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "wisp/os/flash_guard.hpp"

namespace wisp::test {

class FakeIoQueue {
public:
    std::function<bool(std::function<void()>)> submit_io() {
        return [this](std::function<void()> job) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (refuse) return false;
            jobs_.push_back(std::move(job));
            return true;
        };
    }

    size_t queued() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.size();
    }

    // Runs every job queued so far, and any those jobs queue, on another
    // thread registered as the flash thread; returns how many ran.
    size_t run_all() {
        size_t ran = 0;
        std::thread worker([&] {
            wisp::os::set_flash_thread();
            for (;;) {
                std::vector<std::function<void()>> batch;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    batch.swap(jobs_);
                }
                if (batch.empty()) break;
                for (auto& job : batch) {
                    job();
                    ++ran;
                }
            }
            wisp::os::clear_flash_thread();
        });
        worker.join();
        return ran;
    }

    // submit_io() refuses every job while set (a full queue).
    std::atomic<bool> refuse{false};

private:
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> jobs_;
};

}  // namespace wisp::test
