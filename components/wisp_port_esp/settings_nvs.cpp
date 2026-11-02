#include "wisp/port/esp/settings_nvs.hpp"

#include <string>
#include <utility>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "settings_nvs";

// NVS's own documented cap on a single string value's length, including
// the implicit null terminator nvs_set_str() appends -- see
// settings_nvs.hpp's file comment.
constexpr size_t kNvsStringMaxLen = 4000;

}  // namespace

std::unique_ptr<NvsSettingsBackend> NvsSettingsBackend::create() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        WLOG_W(kTag, "nvs_flash_init: %s, erasing and retrying once", esp_err_to_name(err));
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            WLOG_E(kTag, "nvs_flash_erase failed: %s", esp_err_to_name(erase_err));
            return std::unique_ptr<NvsSettingsBackend>(new NvsSettingsBackend(false));
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        WLOG_E(kTag, "nvs_flash_init failed: %s; settings will not persist", esp_err_to_name(err));
        return std::unique_ptr<NvsSettingsBackend>(new NvsSettingsBackend(false));
    }
    return std::unique_ptr<NvsSettingsBackend>(new NvsSettingsBackend(true));
}

Result<Value> NvsSettingsBackend::get(const std::string& ns, const std::string& key) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    if (!ready_) {
        return Result<Value>::err(ErrorCode::IoError, "nvs not initialized");
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ns.c_str(), NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace was never written to.
        return Result<Value>::err(ErrorCode::NotFound, ns + "/" + key);
    }
    if (err != ESP_OK) {
        return Result<Value>::err(ErrorCode::IoError,
                                  std::string("nvs_open: ") + esp_err_to_name(err));
    }

    size_t len = 0;
    err = nvs_get_str(handle, key.c_str(), nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return Result<Value>::err(ErrorCode::NotFound, ns + "/" + key);
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return Result<Value>::err(ErrorCode::IoError,
                                  std::string("nvs_get_str (size): ") + esp_err_to_name(err));
    }

    // `len` includes the null terminator nvs_get_str() counts in its
    // size-query mode.
    std::string buf(len, '\0');
    err = nvs_get_str(handle, key.c_str(), buf.data(), &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return Result<Value>::err(ErrorCode::IoError,
                                  std::string("nvs_get_str: ") + esp_err_to_name(err));
    }
    if (!buf.empty()) {
        buf.resize(buf.size() - 1);  // drop the null terminator nvs_get_str() wrote
    }

    Result<Value> parsed = from_json(buf);
    if (!parsed) {
        return Result<Value>::err(ErrorCode::IoError, "corrupt settings value at " + ns + "/" +
                                                          key + ": " + parsed.error().message);
    }
    return Result<Value>::ok(std::move(parsed.value()));
}

Result<void> NvsSettingsBackend::set(const std::string& ns, const std::string& key,
                                     const Value& value) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    if (!ready_) {
        return Result<void>::err(ErrorCode::IoError, "nvs not initialized");
    }

    std::string json = to_json(value);
    if (json.size() + 1 > kNvsStringMaxLen) {
        return Result<void>::err(
            ErrorCode::InvalidArgument,
            "settings value too large for NVS (>4000 bytes): " + ns + "/" + key);
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ns.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("nvs_open: ") + esp_err_to_name(err));
    }
    err = nvs_set_str(handle, key.c_str(), json.c_str());
    if (err != ESP_OK) {
        nvs_close(handle);
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("nvs_set_str: ") + esp_err_to_name(err));
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("nvs_commit: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

Result<void> NvsSettingsBackend::erase(const std::string& ns, const std::string& key) {
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    if (!ready_) {
        return Result<void>::err(ErrorCode::IoError, "nvs not initialized");
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ns.c_str(), NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return Result<void>::ok();  // namespace never existed: nothing to erase
    }
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("nvs_open: ") + esp_err_to_name(err));
    }
    err = nvs_erase_key(handle, key.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return Result<void>::ok();  // key already absent
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("nvs_erase_key: ") + esp_err_to_name(err));
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("nvs_commit: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
