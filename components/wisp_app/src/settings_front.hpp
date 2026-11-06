#pragma once

// SettingsFront: what Platform::settings points at once App owns it.
//
// Every Settings user in App (SystemSettings, PowerManager, the self-check
// screen, loopback, ...) holds a reference to Platform::settings for App's
// whole life, but the CachedSettingsBackend that should serve them needs
// the storage worker, which App builds anew on every start() and destroys
// on every stop(). This front keeps that reference stable: between
// start() and stop() it forwards to the cache (load_and_attach()),
// otherwise to the platform backend itself (NVS or the JSON file), as
// before the flash thread existed.
//
// The lock is held across each call forwarded to the cache, so
// detach_and_flush() waits for any call still inside the cache before the
// cache can be destroyed, and no call reaches the platform while the
// cache's last changes are being written. The cache never calls back into
// the front, and a call that reaches flash through it does so only in the
// boot and shutdown phases (see wisp/core/cached_settings.hpp), so holding
// it is cheap.
//
// Without a cache, a call goes to the platform backend, which touches
// flash, so it runs on the worker set with set_worker() (spec 2.1 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md): that is
// every call while the cache could not be loaded or allocated. Before
// the worker starts and after it stops, run_sync() runs it right here.
// Those calls wait for the worker without the lock, so a job on the worker
// that reads a setting cannot deadlock against a caller waiting for it.

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wisp/core/cached_settings.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

class SettingsFront final : public core::SettingsBackend {
public:
    explicit SettingsFront(std::unique_ptr<core::SettingsBackend> platform)
        : platform_(std::move(platform)) {}

    core::SettingsBackend& platform() { return *platform_; }

    // The flash thread direct calls run on; null: on the caller. Set
    // before anything else can call in, cleared before the worker is
    // destroyed.
    void set_worker(core::StorageWorker* worker) {
        std::lock_guard<os::Mutex> lock(mutex_);
        worker_ = worker;
    }

    // Loads `cache` and forwards to it from now on. On a failed load
    // nothing is attached and the load's error is returned: an empty
    // cache would answer every read with the default for the whole run,
    // while the platform backend still has the stored values.
    Result<void> load_and_attach(core::CachedSettingsBackend& cache) {
        WISP_TRY(cache.load());
        std::lock_guard<os::Mutex> lock(mutex_);
        cache_ = &cache;
        return Result<void>::ok();
    }

    // Writes what the attached cache still holds dirty, then forwards to
    // the platform backend again; the cache may be destroyed after this
    // returns. Both happen under the lock, so no call can write the
    // platform directly in between and then be overwritten by an older
    // dirty value. Meant for after the worker has stopped, when the
    // flush runs inline. Ok without a cache.
    Result<void> detach_and_flush() {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (cache_ == nullptr) return Result<void>::ok();
        Result<void> r = cache_->flush(os::kForever);
        cache_ = nullptr;
        return r;
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
        // destroyed by App::stop() after detach_and_flush().
        return cache != nullptr ? cache->flush(timeout_ms) : Result<void>::ok();
    }

    Result<Value> get(const std::string& ns, const std::string& key) override {
        return forward<Value>([&](core::SettingsBackend& b) { return b.get(ns, key); });
    }
    Result<void> set(const std::string& ns, const std::string& key, const Value& value) override {
        return forward<void>([&](core::SettingsBackend& b) { return b.set(ns, key, value); });
    }
    Result<void> erase(const std::string& ns, const std::string& key) override {
        return forward<void>([&](core::SettingsBackend& b) { return b.erase(ns, key); });
    }
    Result<void> erase_all() override {
        return forward<void>([&](core::SettingsBackend& b) { return b.erase_all(); });
    }
    Result<std::vector<core::SettingsEntry>> load_all() override {
        return forward<std::vector<core::SettingsEntry>>(
            [&](core::SettingsBackend& b) { return b.load_all(); });
    }
    Result<void> write_batch(std::span<const core::SettingsChange> changes) override {
        return forward<void>([&](core::SettingsBackend& b) { return b.write_batch(changes); });
    }

private:
    // fn(cache) under the lock, or fn(platform) on the worker without it.
    template <typename T, typename Fn>
    Result<T> forward(Fn&& fn) {
        core::StorageWorker* worker = nullptr;
        {
            std::lock_guard<os::Mutex> lock(mutex_);
            if (cache_ != nullptr) return fn(*cache_);
            worker = worker_;
        }
        return core::call_on_worker<T>(worker, [&] { return fn(*platform_); });
    }

    std::unique_ptr<core::SettingsBackend> platform_;
    os::Mutex mutex_;
    core::CachedSettingsBackend* cache_ = nullptr;
    core::StorageWorker* worker_ = nullptr;
};

}  // namespace wisp::app
