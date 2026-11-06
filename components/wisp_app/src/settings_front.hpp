#pragma once

// SettingsFront: what Platform::settings points at once App owns it.
//
// Every Settings user in App (SystemSettings, PowerManager, the self-check
// screen, loopback, ...) holds a reference to Platform::settings for App's
// whole life, but the CachedSettingsBackend that should serve them needs
// the storage worker, which App builds anew on every start() and destroys
// on every stop(). This front keeps that reference stable: between
// start() and stop() it forwards to the cache (attach()), otherwise to the
// platform backend itself (NVS or the JSON file), as before the flash
// thread existed.
//
// The lock is held across each forwarded call, so detach() waits for any
// call still inside the cache before the cache can be destroyed. The cache
// never calls back into the front, and a call that reaches flash through
// it does so only in the boot and shutdown phases (see
// wisp/core/cached_settings.hpp), so holding it is cheap.

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wisp/core/cached_settings.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

class SettingsFront final : public core::SettingsBackend {
public:
    explicit SettingsFront(std::unique_ptr<core::SettingsBackend> platform)
        : platform_(std::move(platform)) {}

    core::SettingsBackend& platform() { return *platform_; }

    // Forwards to `cache` from now on; nullptr goes back to the platform
    // backend, after any call still in the old cache has returned.
    void attach(core::CachedSettingsBackend* cache) {
        std::lock_guard<os::Mutex> lock(mutex_);
        cache_ = cache;
    }

    // CachedSettingsBackend::flush() on the attached cache; Ok without one
    // (the platform backend writes every change at once).
    Result<void> flush(uint32_t timeout_ms) {
        core::CachedSettingsBackend* cache = nullptr;
        {
            std::lock_guard<os::Mutex> lock(mutex_);
            cache = cache_;
        }
        // Not under the lock: the wait may be long, and the cache is only
        // destroyed by App::stop(), which flushes it itself first.
        return cache != nullptr ? cache->flush(timeout_ms) : Result<void>::ok();
    }

    Result<Value> get(const std::string& ns, const std::string& key) override {
        std::lock_guard<os::Mutex> lock(mutex_);
        return current().get(ns, key);
    }
    Result<void> set(const std::string& ns, const std::string& key, const Value& value) override {
        std::lock_guard<os::Mutex> lock(mutex_);
        return current().set(ns, key, value);
    }
    Result<void> erase(const std::string& ns, const std::string& key) override {
        std::lock_guard<os::Mutex> lock(mutex_);
        return current().erase(ns, key);
    }
    Result<void> erase_all() override {
        std::lock_guard<os::Mutex> lock(mutex_);
        return current().erase_all();
    }
    Result<std::vector<core::SettingsEntry>> load_all() override {
        std::lock_guard<os::Mutex> lock(mutex_);
        return current().load_all();
    }
    Result<void> write_batch(std::span<const core::SettingsChange> changes) override {
        std::lock_guard<os::Mutex> lock(mutex_);
        return current().write_batch(changes);
    }

private:
    core::SettingsBackend& current() {
        return cache_ != nullptr ? static_cast<core::SettingsBackend&>(*cache_) : *platform_;
    }

    std::unique_ptr<core::SettingsBackend> platform_;
    os::Mutex mutex_;
    core::CachedSettingsBackend* cache_ = nullptr;
};

}  // namespace wisp::app
