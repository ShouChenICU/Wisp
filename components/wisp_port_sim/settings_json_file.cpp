#include "wisp/port/sim/settings_json_file.hpp"

#include <cstdio>
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
    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) {
        WLOG_W(kTag, "settings file missing, starting empty: %s", file_path_.c_str());
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

    std::lock_guard<os::Mutex> lock(mutex_);
    store_[ns][key] = value;
    return write_through();
}

Result<void> JsonFileSettingsBackend::erase(const std::string& ns, const std::string& key) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    std::lock_guard<os::Mutex> lock(mutex_);
    auto ns_it = store_.find(ns);
    if (ns_it != store_.end()) {
        ns_it->second.erase(key);
        if (ns_it->second.empty()) {
            store_.erase(ns_it);
        }
    }
    return write_through();
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
    std::string json = to_json(Value(std::move(root)));

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
        return Result<void>::err(ErrorCode::IoError,
                                 "failed to rename " + tmp_path + " to " + file_path_);
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::sim
