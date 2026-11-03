#pragma once

// ESP-IDF wisp::core::SettingsBackend backed by NVS: each Wisp namespace
// maps 1:1 to an NVS namespace, and each key's Value is serialized with
// wisp::to_json_raw() and stored as a single NVS string entry (nvs_set_str),
// decoded back with wisp::from_json() on get(). Both `ns` and `key` are
// validated with core::validate_settings_name() before touching NVS,
// same as every other SettingsBackend -- this class's host/PC equivalent
// is wisp::port::sim::JsonFileSettingsBackend -- see wisp/core/
// settings.hpp.
//
// NVS_KEY_NAME_MAX_SIZE is 16 (15 usable characters including the null
// terminator), which is exactly validate_settings_name()'s 15-character
// limit, so no extra length check is needed beyond that shared call.
//
// NVS string values are capped at ~4000 bytes (ESP-IDF's documented NVS
// limit); set() rejects a serialized value that would exceed that with
// ErrorCode::InvalidArgument rather than letting nvs_set_str() fail
// opaquely.
//
// This is the only SettingsBackend wisp::port::esp provides; there is no
// RAM-only placeholder to fall back to.

#include <cstdint>
#include <memory>
#include <string>

#include "wisp/base/value.hpp"
#include "wisp/core/settings.hpp"

namespace wisp::port::esp {

class NvsSettingsBackend : public core::SettingsBackend {
public:
    // Calls nvs_flash_init(); if that returns ESP_ERR_NVS_NO_FREE_PAGES or
    // ESP_ERR_NVS_NEW_VERSION_FOUND, erases the NVS partition
    // (nvs_flash_erase()) and retries nvs_flash_init() once.
    // If NVS still cannot be initialized after that retry, logs WLOG_E and
    // returns a backend that is still fully usable as an object (never
    // null -- Platform::settings must never be null, see
    // firmware/main/main.cpp) but whose get()/set()/erase() all
    // immediately fail with ErrorCode::IoError without touching NVS
    // again: AGENTS.md's "缺失或初始化失败时只降级运行，不得阻止启动" --
    // a dead NVS partition degrades Settings to defaults-only (see
    // core::Settings::get_*()'s "any failure falls back to the caller's
    // default" contract) rather than blocking boot.
    static std::unique_ptr<NvsSettingsBackend> create();

    Result<Value> get(const std::string& ns, const std::string& key) override;
    Result<void> set(const std::string& ns, const std::string& key, const Value& value) override;
    Result<void> erase(const std::string& ns, const std::string& key) override;
    // Erases every namespace in the default NVS partition (nvs_erase_all +
    // nvs_commit on each), including ones owned by ESP-IDF itself such as
    // the Wi-Fi driver's: a factory reset must not leave old credentials
    // behind. Not atomic across namespaces -- a failure partway leaves the
    // namespaces already erased empty; the caller reboots either way.
    Result<void> erase_all() override;

private:
    explicit NvsSettingsBackend(bool ready) : ready_(ready) {}

    bool ready_;
};

}  // namespace wisp::port::esp
