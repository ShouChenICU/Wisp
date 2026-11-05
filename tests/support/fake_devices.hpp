#pragma once

// Test-only fake wisp::hal devices, shared across the test suite (not
// built into any production component -- see AGENTS.md's layering rule,
// which does not apply to tests/). Any wisp_test() target can
// `#include "support/fake_devices.hpp"`; tests/CMakeLists.txt adds the
// tests/ root as an include directory for every test target.
//
// These fakes may freely use std::mutex/std::atomic (ordinary host-only
// test code, unlike the portable wisp_* components they stand in for).

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/hal/storage.hpp"
#include "wisp/hal/touch.hpp"

namespace wisp::test {

// Records every flush() call's Area; otherwise behaves like a display
// that's always ready (flush() returns ok immediately, with no simulated
// transfer delay). Caps mirror the simulator's actual panel: 480x480,
// rounded_rect, corner_radius 40, panel "amoled", brightness supported
// (pass `shape` to simulate a different wisp::hal::display_geometry shape,
// e.g. "circle" or "rect").
class FakeDisplay : public wisp::hal::Display {
public:
    explicit FakeDisplay(std::string id, int width, int height, int area_align = 1,
                         std::string shape = "rounded_rect") {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"width", width},           {"height", height},
            {"area_align", area_align}, {"shape", wisp::Value(std::move(shape))},
            {"corner_radius", 40},      {"panel", wisp::Value("amoled")},
            {"brightness", true},
        };
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // Adds or replaces a cap (e.g. "rgb565_be"); only before a UiRuntime
    // reads them in start().
    void set_cap(const std::string& key, wisp::Value value) { info_.caps[key] = std::move(value); }

    wisp::Result<void> flush(const wisp::hal::Area& area, const uint16_t* /*rgb565*/) override {
        assert_owner_thread();
        std::lock_guard<std::mutex> lock(mutex_);
        ++flush_count_;
        areas_.push_back(area);
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> set_brightness(uint8_t level) override {
        assert_owner_thread();
        ++set_brightness_calls_;
        brightness_.store(level);
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> set_power(bool on) override {
        assert_owner_thread();
        ++set_power_calls_;
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

    // Lets PowerManager tests check that enter_standby()/
    // exit_standby() call set_power() exactly once per real transition
    // (idempotency -- a second enter_standby() while already in standby
    // must not call it again).
    int set_power_calls() const { return set_power_calls_.load(); }
    int set_brightness_calls() const { return set_brightness_calls_.load(); }

private:
    wisp::hal::DeviceInfo info_;

    mutable std::mutex mutex_;
    size_t flush_count_ = 0;
    std::vector<wisp::hal::Area> areas_;

    std::atomic<uint8_t> brightness_{0};
    std::atomic<bool> powered_{true};
    std::atomic<int> set_power_calls_{0};
    std::atomic<int> set_brightness_calls_{0};
};

// A touch device test double: read() returns whatever was last set via
// set_points() (empty by default). set_power() records every call
// (set_power_calls()/powered()) like FakeDisplay's, and -- unlike
// FakeDisplay's, which always succeeds -- can be made to simulate a
// touch controller with no sleep mode at all (the common case; see
// wisp::hal::Touch::set_power()'s default) by setting `unsupported`.
class FakeTouch : public wisp::hal::Touch {
public:
    explicit FakeTouch(std::string id, int max_points = 1) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{{"max_points", max_points}};
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<std::vector<wisp::hal::TouchPoint>> read() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return wisp::Result<std::vector<wisp::hal::TouchPoint>>::ok(points_);
    }

    wisp::Result<void> set_power(bool on) override {
        ++set_power_calls_;
        if (unsupported.load()) {
            return wisp::Result<void>::err(wisp::ErrorCode::Unsupported,
                                           "fake touch sleep unsupported");
        }
        powered_.store(on);
        return wisp::Result<void>::ok();
    }

    void set_points(std::vector<wisp::hal::TouchPoint> points) {
        std::lock_guard<std::mutex> lock(mutex_);
        points_ = std::move(points);
    }

    bool powered() const { return powered_.load(); }
    int set_power_calls() const { return set_power_calls_.load(); }

    // When true, set_power() always fails with ErrorCode::Unsupported
    // (default false -- the fake supports sleep).
    std::atomic<bool> unsupported{false};

private:
    wisp::hal::DeviceInfo info_;

    mutable std::mutex mutex_;
    std::vector<wisp::hal::TouchPoint> points_;

    std::atomic<bool> powered_{true};
    std::atomic<int> set_power_calls_{0};
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
        read_count_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex_);
        if (next_error_) {
            return wisp::Result<wisp::hal::ImuSample>::err(*next_error_, next_error_message_);
        }
        // Mirrors wisp::port::esp::EspImu's real contract (IMU power-down
        // stopgap): reading while powered off must fail, not return a
        // stale/frozen sample.
        if (!powered_.load()) {
            return wisp::Result<wisp::hal::ImuSample>::err(wisp::ErrorCode::DeviceFailure,
                                                           "fake imu is powered off");
        }
        return wisp::Result<wisp::hal::ImuSample>::ok(next_sample_);
    }

    wisp::Result<void> set_rate(uint32_t hz) override {
        rate_hz_.store(hz);
        return wisp::Result<void>::ok();
    }

    // IMU power-down stopgap (mirrors FakeTouch::set_power() above):
    // records every call (set_power_calls()/powered()); set `unsupported`
    // to simulate an IMU driver with no power-down support at all.
    wisp::Result<void> set_power(bool on) override {
        ++set_power_calls_;
        if (unsupported.load()) {
            return wisp::Result<void>::err(wisp::ErrorCode::Unsupported,
                                           "fake imu sleep unsupported");
        }
        powered_.store(on);
        return wisp::Result<void>::ok();
    }

    bool powered() const { return powered_.load(); }
    int set_power_calls() const { return set_power_calls_.load(); }

    // Total number of read() calls so far, regardless of outcome -- lets
    // tests prove a sampling loop actually stopped polling (rather than
    // just that its last-seen result stopped updating).
    size_t read_count() const { return read_count_.load(std::memory_order_relaxed); }

    // When true, set_power() always fails with ErrorCode::Unsupported
    // (default false -- the fake supports power-down).
    std::atomic<bool> unsupported{false};

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
    std::atomic<bool> powered_{true};
    std::atomic<int> set_power_calls_{0};
    std::atomic<size_t> read_count_{0};
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
        if (on_capacity) on_capacity();
        wisp::hal::Capacity cap;
        cap.total_bytes = kTotalBytes;
        uint64_t used = dir_size(dir_);
        cap.free_bytes = used >= kTotalBytes ? 0 : kTotalBytes - used;
        return wisp::Result<wisp::hal::Capacity>::ok(cap);
    }

    // Records every
    // set_standby() call, mirroring FakeImu/FakeTouch::set_power()'s
    // set_power_calls()/powered() pattern.
    void set_standby(bool standby) override {
        ++set_standby_calls_;
        standby_.store(standby);
    }
    int set_standby_calls() const { return set_standby_calls_.load(); }
    bool standby() const { return standby_.load(); }

    std::atomic<bool> online_flag{true};
    // Called at the start of every capacity() call, on the caller's
    // thread. Set it before the device is used.
    std::function<void()> on_capacity;

private:
    std::atomic<int> set_standby_calls_{0};
    std::atomic<bool> standby_{false};

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
        if (next_error_) {
            return wisp::Result<wisp::hal::PowerStatus>::err(*next_error_, next_error_message_);
        }
        return wisp::Result<wisp::hal::PowerStatus>::ok(status_);
    }

    wisp::Result<void> power_off() override {
        if (fail_power_off_.load()) {
            return wisp::Result<void>::err(wisp::ErrorCode::DeviceFailure,
                                           "simulated PMIC failure");
        }
        powered_off_.store(true);
        return wisp::Result<void>::ok();
    }

    void set_status(wisp::hal::PowerStatus status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
        next_error_.reset();
    }

    // Test control: makes every subsequent power_off() call fail instead
    // of succeeding -- exercises SelfCheck::do_power_off()'s
    // Platform::request_power_off fallback path.
    void set_fail_power_off(bool fail) { fail_power_off_.store(fail); }

    // Test control: makes the next (and every subsequent, until
    // set_status()/this is called again) status() call fail with `code`/
    // `message` instead -- mirrors FakeImu::set_next_error() (see its doc
    // comment).
    void set_next_error(wisp::ErrorCode code, std::string message = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_error_ = code;
        next_error_message_ = std::move(message);
    }

    bool powered_off() const { return powered_off_.load(); }

private:
    wisp::hal::DeviceInfo info_;

    std::mutex mutex_;
    wisp::hal::PowerStatus status_{};
    std::optional<wisp::ErrorCode> next_error_;
    std::string next_error_message_;
    std::atomic<bool> powered_off_{false};
    std::atomic<bool> fail_power_off_{false};
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

// Fills every read() with silence and reports every frame as delivered
// immediately -- no simulated capture delay, so a caller looping until
// it has read a fixed frame count (e.g. loopback.cpp's record_and_play())
// completes in one call instead of real-time.
class FakeAudioIn : public wisp::hal::AudioIn {
public:
    explicit FakeAudioIn(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<void> open(const wisp::hal::AudioFormat&) override {
        return wisp::Result<void>::ok();
    }

    wisp::Result<size_t> read(int16_t* frames, size_t count) override {
        std::fill(frames, frames + count, 0);
        return wisp::Result<size_t>::ok(count);
    }

    void close() override {}

    wisp::Result<void> set_gain(float) override { return wisp::Result<void>::ok(); }

private:
    wisp::hal::DeviceInfo info_;
};

// Reports every write() as consumed immediately (no simulated playback
// delay) and records every set_volume() call, so a test can assert what
// volume loopback.cpp's record_and_play() applied.
class FakeAudioOut : public wisp::hal::AudioOut {
public:
    explicit FakeAudioOut(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<void> open(const wisp::hal::AudioFormat&) override {
        return wisp::Result<void>::ok();
    }

    wisp::Result<size_t> write(const int16_t*, size_t count) override {
        return wisp::Result<size_t>::ok(count);
    }

    void close() override {}

    wisp::Result<void> set_volume(uint8_t percent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        last_volume_ = percent;
        ++set_volume_calls_;
        return wisp::Result<void>::ok();
    }

    uint8_t last_volume() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_volume_;
    }

    int set_volume_calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_volume_calls_;
    }

private:
    wisp::hal::DeviceInfo info_;

    mutable std::mutex mutex_;
    uint8_t last_volume_ = 0;
    int set_volume_calls_ = 0;
};

}  // namespace wisp::test
