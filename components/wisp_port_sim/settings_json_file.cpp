#include "wisp/port/sim/settings_json_file.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::port::sim {

namespace {
constexpr const char* kTag = "settings";
}  // namespace

JsonFileSettingsBackend::JsonFileSettingsBackend(std::string file_path)
    : file_path_(std::move(file_path)) {
    errno = 0;
    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) {
        if (errno == ENOENT) {
            WLOG_W(kTag, "settings file missing, starting empty: %s", file_path_.c_str());
        } else {
            WLOG_W(kTag, "settings file open failed (%s), starting empty: %s", strerror(errno),
                   file_path_.c_str());
        }
        return;
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    Result<Value> parsed = from_json(buf.str());
    if (!parsed || parsed.value().type() != Value::Type::Map) {
        WLOG_W(kTag, "settings file corrupt, starting empty: %s", file_path_.c_str());
        return;
    }

    for (const auto& [ns, ns_value] : parsed.value().as_map()) {
        if (ns_value.type() != Value::Type::Map) {
            continue;
        }
        for (const auto& [key, value] : ns_value.as_map()) {
            store_[ns][key] = value;
        }
    }
}

Result<Value> JsonFileSettingsBackend::get(const std::string& ns, const std::string& key) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    std::lock_guard<os::Mutex> lock(mutex_);
    auto ns_it = store_.find(ns);
    if (ns_it == store_.end()) {
        return Result<Value>::err(ErrorCode::NotFound, ns + "/" + key);
    }
    auto key_it = ns_it->second.find(key);
    if (key_it == ns_it->second.end()) {
        return Result<Value>::err(ErrorCode::NotFound, ns + "/" + key);
    }
    return Result<Value>::ok(key_it->second);
}

Result<void> JsonFileSettingsBackend::set(const std::string& ns, const std::string& key,
                                          const Value& value) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));
    WISP_TRY(core::validate_settings_value(ns, key, value));

    std::lock_guard<os::Mutex> lock(mutex_);

    // Snapshot whatever was there before (or its absence) so a write
    // failure below can be rolled back: callers must be able to rely on
    // an IoError from set() meaning nothing changed, in memory or on
    // disk.
    auto ns_it = store_.find(ns);
    const bool had_ns = ns_it != store_.end();
    bool had_key = false;
    Value previous_value;
    if (had_ns) {
        auto key_it = ns_it->second.find(key);
        had_key = key_it != ns_it->second.end();
        if (had_key) {
            previous_value = key_it->second;
        }
    }

    store_[ns][key] = value;
    Result<void> result = write_through();
    if (!result) {
        if (had_key) {
            store_[ns][key] = previous_value;
        } else {
            auto it = store_.find(ns);
            if (it != store_.end()) {
                it->second.erase(key);
                if (!had_ns) {
                    store_.erase(it);
                }
            }
        }
    }
    return result;
}

Result<void> JsonFileSettingsBackend::erase(const std::string& ns, const std::string& key) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    std::lock_guard<os::Mutex> lock(mutex_);

    auto ns_it = store_.find(ns);
    if (ns_it == store_.end()) {
        return Result<void>::ok();  // nothing to erase, nothing to persist
    }
    auto key_it = ns_it->second.find(key);
    if (key_it == ns_it->second.end()) {
        return Result<void>::ok();  // key already absent
    }

    Value previous_value = key_it->second;
    ns_it->second.erase(key_it);
    const bool ns_emptied = ns_it->second.empty();
    if (ns_emptied) {
        store_.erase(ns_it);
    }

    Result<void> result = write_through();
    if (!result) {
        // Roll back: put the erased key (and its ns entry, if removed)
        // back so an IoError from erase() means nothing changed.
        store_[ns][key] = previous_value;
    }
    return result;
}

Result<void> JsonFileSettingsBackend::erase_all() {
    std::lock_guard<os::Mutex> lock(mutex_);
    auto previous = std::move(store_);
    store_.clear();
    Result<void> result = write_through();
    if (!result) {
        store_ = std::move(previous);
    }
    return result;
}

Result<std::vector<core::SettingsEntry>> JsonFileSettingsBackend::load_all() {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<core::SettingsEntry> out;
    for (const auto& [ns, keys] : store_) {
        for (const auto& [key, value] : keys) out.push_back({ns, key, value});
    }
    return Result<std::vector<core::SettingsEntry>>::ok(std::move(out));
}

Result<void> JsonFileSettingsBackend::write_batch(std::span<const core::SettingsChange> changes) {
    for (const core::SettingsChange& c : changes) {
        WISP_TRY(core::validate_settings_name(c.ns));
        WISP_TRY(core::validate_settings_name(c.key));
        if (c.value) WISP_TRY(core::validate_settings_value(c.ns, c.key, *c.value));
    }
    std::lock_guard<os::Mutex> lock(mutex_);
    auto previous = store_;
    for (const core::SettingsChange& c : changes) {
        if (c.value) {
            store_[c.ns][c.key] = *c.value;
            continue;
        }
        auto ns_it = store_.find(c.ns);
        if (ns_it == store_.end()) continue;
        ns_it->second.erase(c.key);
        if (ns_it->second.empty()) store_.erase(ns_it);
    }
    Result<void> result = write_through();
    if (!result) store_ = std::move(previous);
    return result;
}

Result<void> JsonFileSettingsBackend::write_through() {
    Map root;
    for (const auto& [ns, keys] : store_) {
        Map ns_map;
        for (const auto& [key, value] : keys) {
            ns_map[key] = value;
        }
        root[ns] = Value(std::move(ns_map));
    }
    std::string json = to_json_raw(Value(std::move(root)));

    std::string tmp_path = file_path_ + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return Result<void>::err(ErrorCode::IoError, "failed to open " + tmp_path);
        }
        out << json;
        out.flush();
        if (!out) {
            return Result<void>::err(ErrorCode::IoError, "failed to write " + tmp_path);
        }
    }
    if (std::rename(tmp_path.c_str(), file_path_.c_str()) != 0) {
        std::remove(tmp_path.c_str());  // best-effort cleanup; ignore any failure
        return Result<void>::err(ErrorCode::IoError,
                                 "failed to rename " + tmp_path + " to " + file_path_);
    }
    ++file_writes_;
    return Result<void>::ok();
}

}  // namespace wisp::port::sim
