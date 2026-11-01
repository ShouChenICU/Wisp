#pragma once

// Test-only fake wisp::hal devices, shared across the test suite (not
// built into any production component -- see AGENTS.md's layering rule,
// which does not apply to tests/). Any wisp_test() target can
// `#include "support/fake_devices.hpp"`; tests/CMakeLists.txt adds the
// tests/ root as an include directory for every test target.
//
// These fakes may freely use std::mutex/std::atomic (ordinary host-only
// test code, unlike the portable wisp_* components they stand in for).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/hal/storage.hpp"

namespace wisp::test {

// Records every flush() call's Area; otherwise behaves like a display
// that's always ready (flush() returns ok immediately, with no simulated
// transfer delay). Caps mirror the simulator's actual panel: 480x480,
// rounded_square, corner_radius 40, panel "amoled", brightness supported.
class FakeDisplay : public wisp::hal::Display {
public:
    explicit FakeDisplay(std::string id, int width, int height, int area_align = 1) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"width", width},
            {"height", height},
            {"area_align", area_align},
            {"shape", wisp::Value("rounded_square")},
            {"corner_radius", 40},
            {"panel", wisp::Value("amoled")},
            {"brightness", true},
        };
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<void> flush(const wisp::hal::Area& area, const uint16_t* /*rgb565*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++flush_count_;
        areas_.push_back(area);
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> set_brightness(uint8_t level) override {
        brightness_.store(level);
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> set_power(bool on) override {
        powered_.store(on);
        return wisp::Result<void>::ok();
    }

    bool powered() const override { return powered_.load(); }

    size_t flush_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return flush_count_;
    }

    std::vector<wisp::hal::Area> areas() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return areas_;
    }

    uint8_t brightness() const { return brightness_.load(); }

private:
    wisp::hal::DeviceInfo info_;

    mutable std::mutex mutex_;
    size_t flush_count_ = 0;
    std::vector<wisp::hal::Area> areas_;

    std::atomic<uint8_t> brightness_{0};
    std::atomic<bool> powered_{true};
};

// read() returns whatever was last set via set_next_sample()/set_next_error()
// (defaulting to an all-zero sample), repeatedly, until changed again.
class FakeImu : public wisp::hal::Imu {
public:
    explicit FakeImu(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"accel", true},
            {"gyro", true},
            {"magnetometer", false},
            {"max_rate_hz", 1000},
        };
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<wisp::hal::ImuSample> read() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_error_) {
            return wisp::Result<wisp::hal::ImuSample>::err(*next_error_, next_error_message_);
        }
        return wisp::Result<wisp::hal::ImuSample>::ok(next_sample_);
    }

    wisp::Result<void> set_rate(uint32_t hz) override {
        rate_hz_.store(hz);
        return wisp::Result<void>::ok();
    }

    // Test control: makes the next (and every subsequent, until changed
    // again) read() return `sample`.
    void set_next_sample(wisp::hal::ImuSample sample) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_sample_ = sample;
        next_error_.reset();
    }

    // Test control: makes the next (and every subsequent, until changed
    // again) read() fail with `code`/`message`.
    void set_next_error(wisp::ErrorCode code, std::string message = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_error_ = code;
        next_error_message_ = std::move(message);
    }

    uint32_t rate_hz() const { return rate_hz_.load(); }

private:
    wisp::hal::DeviceInfo info_;

    std::mutex mutex_;
    wisp::hal::ImuSample next_sample_{};
    std::optional<wisp::ErrorCode> next_error_;
    std::string next_error_message_;

    std::atomic<uint32_t> rate_hz_{0};
};

class FakeButton : public wisp::hal::Button {
public:
    explicit FakeButton(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    bool pressed() override { return down.load(); }

    std::atomic<bool> down{false};

private:
    wisp::hal::DeviceInfo info_;
};

// mount_point() is the `dir` passed to the constructor. capacity() reports
// a fixed total (1 GiB) and a free_bytes derived from how many bytes are
// actually used under `dir` right now -- walked with <dirent.h>/<sys/stat.h>
// (not std::filesystem), matching the constraint the production
// StorageManager is built under.
class FakeStorage : public wisp::hal::Storage {
public:
    FakeStorage(std::string id, std::string dir, bool removable) : dir_(std::move(dir)) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"removable", removable},
            {"fs", wisp::Value("fake")},
        };
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    std::string mount_point() const override { return dir_; }
    bool online() override { return online_flag.load(); }

    wisp::Result<wisp::hal::Capacity> capacity() override {
        wisp::hal::Capacity cap;
        cap.total_bytes = kTotalBytes;
        uint64_t used = dir_size(dir_);
        cap.free_bytes = used >= kTotalBytes ? 0 : kTotalBytes - used;
        return wisp::Result<wisp::hal::Capacity>::ok(cap);
    }

    std::atomic<bool> online_flag{true};

private:
    static constexpr uint64_t kTotalBytes = 1ull * 1024 * 1024 * 1024;  // 1 GiB

    static uint64_t dir_size(const std::string& path) {
        DIR* d = opendir(path.c_str());
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
            std::string full = path + "/" + name;
            struct stat st;
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

    std::string dir_;
    wisp::hal::DeviceInfo info_;
};

class FakePower : public wisp::hal::Power {
public:
    explicit FakePower(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"battery", true},
            {"charging_status", true},
        };
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<wisp::hal::PowerStatus> status() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return wisp::Result<wisp::hal::PowerStatus>::ok(status_);
    }

    wisp::Result<void> power_off() override {
        powered_off_.store(true);
        return wisp::Result<void>::ok();
    }

    void set_status(wisp::hal::PowerStatus status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
    }

    bool powered_off() const { return powered_off_.load(); }

private:
    wisp::hal::DeviceInfo info_;

    std::mutex mutex_;
    wisp::hal::PowerStatus status_{};
    std::atomic<bool> powered_off_{false};
};

class FakeClock : public wisp::hal::Clock {
public:
    explicit FakeClock(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<int64_t> get_unix() override {
        return wisp::Result<int64_t>::ok(unix_seconds_.load());
    }

    wisp::Result<void> set_unix(int64_t seconds) override {
        unix_seconds_.store(seconds);
        return wisp::Result<void>::ok();
    }

private:
    wisp::hal::DeviceInfo info_;
    std::atomic<int64_t> unix_seconds_{0};
};

}  // namespace wisp::test
