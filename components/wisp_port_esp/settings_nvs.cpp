#include "wisp/port/esp/settings_nvs.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "settings_nvs";

// NVS's own documented cap on a single string value's length, including
// the implicit null terminator nvs_set_str() appends -- see
// settings_nvs.hpp's file comment. The portable limit is derived from it.
constexpr size_t kNvsStringMaxLen = 4000;
static_assert(core::kSettingsMaxValueBytes + 1 <= kNvsStringMaxLen);

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
    WISP_ASSERT_FLASH_THREAD("NvsSettingsBackend::get");
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
    WISP_ASSERT_FLASH_THREAD("NvsSettingsBackend::set");
    WISP_TRY(core::validate_settings_name(ns));
    WISP_TRY(core::validate_settings_name(key));

    if (!ready_) {
        return Result<void>::err(ErrorCode::IoError, "nvs not initialized");
    }

    std::string json = to_json_raw(value);
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
    WISP_ASSERT_FLASH_THREAD("NvsSettingsBackend::erase");
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

Result<void> NvsSettingsBackend::erase_all() {
    WISP_ASSERT_FLASH_THREAD("NvsSettingsBackend::erase_all");
    if (!ready_) {
        return Result<void>::err(ErrorCode::IoError, "nvs not initialized");
    }

    // NVS has no "erase every namespace" call short of nvs_flash_erase(),
    // which would also need a re-init while other components hold handles.
    // Collect the namespace names first: erasing while iterating would
    // invalidate the iterator.
    std::set<std::string> namespaces;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info = {};
        err = nvs_entry_info(it, &info);
        if (err != ESP_OK) {
            break;
        }
        namespaces.insert(info.namespace_name);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return Result<void>::err(ErrorCode::IoError,
                                 std::string("listing nvs namespaces: ") + esp_err_to_name(err));
    }

    for (const auto& ns : namespaces) {
        nvs_handle_t handle = 0;
        err = nvs_open(ns.c_str(), NVS_READWRITE, &handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            return Result<void>::err(ErrorCode::IoError,
                                     std::string("nvs_open: ") + esp_err_to_name(err));
        }
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        if (err != ESP_OK) {
            return Result<void>::err(ErrorCode::IoError,
                                     std::string("nvs_erase_all: ") + esp_err_to_name(err));
        }
    }
    WLOG_I(kTag, "erased %u namespaces", static_cast<unsigned>(namespaces.size()));
    return Result<void>::ok();
}

Result<std::vector<core::SettingsEntry>> NvsSettingsBackend::load_all() {
    WISP_ASSERT_FLASH_THREAD("NvsSettingsBackend::load_all");
    using R = Result<std::vector<core::SettingsEntry>>;
    if (!ready_) {
        return R::err(ErrorCode::IoError, "nvs not initialized");
    }

    // Collect the names first: reading values through a handle while the
    // iterator is open is not something NVS promises to support.
    std::vector<std::pair<std::string, std::string>> names;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_STR, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info = {};
        err = nvs_entry_info(it, &info);
        if (err != ESP_OK) {
            break;
        }
        names.emplace_back(info.namespace_name, info.key);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return R::err(ErrorCode::IoError,
                      std::string("listing nvs entries: ") + esp_err_to_name(err));
    }

    std::vector<core::SettingsEntry> out;
    out.reserve(names.size());
    for (auto& [ns, key] : names) {
        Result<Value> value = get(ns, key);
        if (!value) {
            // Not a Wisp value (ESP-IDF keeps strings of its own here too).
            WLOG_D(kTag, "skipping nvs entry %s/%s: %s", ns.c_str(), key.c_str(),
                   value.error().message.c_str());
            continue;
        }
        out.push_back({std::move(ns), std::move(key), std::move(value.value())});
    }
    WLOG_I(kTag, "loaded %u settings", static_cast<unsigned>(out.size()));
    return R::ok(std::move(out));
}

Result<void> NvsSettingsBackend::write_batch(std::span<const core::SettingsChange> changes) {
    WISP_ASSERT_FLASH_THREAD("NvsSettingsBackend::write_batch");
    // Everything that can be checked without NVS, before any write.
    std::vector<std::string> values(changes.size());
    for (size_t i = 0; i < changes.size(); ++i) {
        const core::SettingsChange& c = changes[i];
        WISP_TRY(core::validate_settings_name(c.ns));
        WISP_TRY(core::validate_settings_name(c.key));
        if (!c.value) {
            continue;
        }
        values[i] = to_json_raw(*c.value);
        if (values[i].size() + 1 > kNvsStringMaxLen) {
            return Result<void>::err(
                ErrorCode::InvalidArgument,
                "settings value too large for NVS (>4000 bytes): " + c.ns + "/" + c.key);
        }
    }
    if (!ready_) {
        return Result<void>::err(ErrorCode::IoError, "nvs not initialized");
    }

    // One handle per namespace, in the order the batch first names it.
    std::vector<std::pair<std::string, nvs_handle_t>> handles;
    auto close_all = [&handles] {
        for (auto& [ns, handle] : handles) {
            nvs_close(handle);
        }
    };
    auto fail = [&close_all](const char* what, esp_err_t err) {
        close_all();
        return Result<void>::err(ErrorCode::IoError, std::string(what) + esp_err_to_name(err));
    };
    for (size_t i = 0; i < changes.size(); ++i) {
        const core::SettingsChange& c = changes[i];
        nvs_handle_t handle = 0;
        bool found = false;
        for (const auto& [ns, h] : handles) {
            if (ns == c.ns) {
                handle = h;
                found = true;
                break;
            }
        }
        if (!found) {
            esp_err_t err = nvs_open(c.ns.c_str(), NVS_READWRITE, &handle);
            if (err != ESP_OK) {
                return fail("nvs_open: ", err);
            }
            handles.emplace_back(c.ns, handle);
        }
        if (c.value) {
            esp_err_t err = nvs_set_str(handle, c.key.c_str(), values[i].c_str());
            if (err != ESP_OK) {
                return fail("nvs_set_str: ", err);
            }
        } else {
            esp_err_t err = nvs_erase_key(handle, c.key.c_str());
            if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                return fail("nvs_erase_key: ", err);
            }
        }
    }
    for (const auto& [ns, handle] : handles) {
        esp_err_t err = nvs_commit(handle);
        if (err != ESP_OK) {
            return fail("nvs_commit: ", err);
        }
    }
    close_all();
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
