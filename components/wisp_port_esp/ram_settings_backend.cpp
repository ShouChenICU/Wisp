#include "wisp/port/esp/ram_settings_backend.hpp"

#include <mutex>

namespace wisp::port::esp {

Result<Value> RamSettingsBackend::get(const std::string& ns, const std::string& key) {
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

Result<void> RamSettingsBackend::set(const std::string& ns, const std::string& key,
                                     const Value& value) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    std::lock_guard<os::Mutex> lock(mutex_);
    store_[ns][key] = value;
    return Result<void>::ok();
}

Result<void> RamSettingsBackend::erase(const std::string& ns, const std::string& key) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    std::lock_guard<os::Mutex> lock(mutex_);
    auto ns_it = store_.find(ns);
    if (ns_it == store_.end()) {
        return Result<void>::ok();
    }
    ns_it->second.erase(key);
    if (ns_it->second.empty()) {
        store_.erase(ns_it);
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
