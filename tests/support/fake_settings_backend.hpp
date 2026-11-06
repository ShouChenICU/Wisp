#pragma once

// Test-only wisp::core::SettingsBackend standing in for NVS or the JSON
// file under core::CachedSettingsBackend: an in-memory store that records
// how it is used (call counts, every write_batch() it got, whether every
// write ran on the flash thread) and can fail or stall writes on demand.

#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::test {

class FakePlatformBackend : public wisp::core::SettingsBackend {
public:
    wisp::Result<wisp::Value> get(const std::string& ns, const std::string& key) override {
        std::lock_guard<std::mutex> lock(m);
        ++gets;
        auto it = store.find({ns, key});
        if (it == store.end())
            return wisp::Result<wisp::Value>::err(wisp::ErrorCode::NotFound, ns + "/" + key);
        return wisp::Result<wisp::Value>::ok(it->second);
    }
    wisp::Result<void> set(const std::string& ns, const std::string& key,
                           const wisp::Value& value) override {
        std::lock_guard<std::mutex> lock(m);
        store[{ns, key}] = value;
        return wisp::Result<void>::ok();
    }
    wisp::Result<void> erase(const std::string& ns, const std::string& key) override {
        std::lock_guard<std::mutex> lock(m);
        store.erase({ns, key});
        return wisp::Result<void>::ok();
    }
    wisp::Result<void> erase_all() override {
        std::lock_guard<std::mutex> lock(m);
        ++erase_alls;
        if (!wisp::os::on_flash_thread()) off_thread = true;
        store.clear();
        return wisp::Result<void>::ok();
    }
    wisp::Result<std::vector<wisp::core::SettingsEntry>> load_all() override {
        std::lock_guard<std::mutex> lock(m);
        ++loads;
        if (fail_load)
            return wisp::Result<std::vector<wisp::core::SettingsEntry>>::err(
                wisp::ErrorCode::IoError, "injected");
        std::vector<wisp::core::SettingsEntry> out;
        for (const auto& [name, value] : store) out.push_back({name.first, name.second, value});
        return wisp::Result<std::vector<wisp::core::SettingsEntry>>::ok(std::move(out));
    }
    wisp::Result<void> write_batch(std::span<const wisp::core::SettingsChange> changes) override {
        // Outside the lock, so a test can hold the worker inside a write.
        if (on_write) on_write();
        std::lock_guard<std::mutex> lock(m);
        if (!wisp::os::on_flash_thread()) off_thread = true;
        batches.emplace_back(changes.begin(), changes.end());
        for (const wisp::core::SettingsChange& c : changes) {
            if (c.key == refused_key) {
                return wisp::Result<void>::err(wisp::ErrorCode::InvalidArgument,
                                               "refused: " + c.key);
            }
        }
        if (fail_writes > 0) {
            --fail_writes;
            return wisp::Result<void>::err(wisp::ErrorCode::IoError, "injected");
        }
        for (const wisp::core::SettingsChange& c : changes) {
            if (c.value) {
                store[{c.ns, c.key}] = *c.value;
            } else {
                store.erase({c.ns, c.key});
            }
        }
        return wisp::Result<void>::ok();
    }

    wisp::Value stored(const std::string& ns, const std::string& key) {
        std::lock_guard<std::mutex> lock(m);
        auto it = store.find({ns, key});
        return it == store.end() ? wisp::Value() : it->second;
    }
    size_t batch_count() {
        std::lock_guard<std::mutex> lock(m);
        return batches.size();
    }

    std::mutex m;
    std::map<std::pair<std::string, std::string>, wisp::Value> store;
    std::vector<std::vector<wisp::core::SettingsChange>> batches;
    int gets = 0;
    int loads = 0;
    int erase_alls = 0;
    int fail_writes = 0;
    bool fail_load = false;
    // A batch naming this key fails with InvalidArgument, writing nothing.
    std::string refused_key;
    // Runs at the start of every write_batch(), before the lock.
    std::function<void()> on_write;
    bool off_thread = false;
};

}  // namespace wisp::test
