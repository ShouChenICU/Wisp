#include "wisp/port/esp/esp_font_partition.hpp"

#include "esp_err.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "wisp/core/log.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::port::esp {

namespace {

// Maps the whole data partition `name` read-only for good; empty, with a
// warning, when it is missing or cannot be mapped.
std::span<const uint8_t> map_partition(const char* name) {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, name);
    if (part == nullptr) {
        WLOG_W("fonts", "no %s partition", name);
        return {};
    }
    const void* ptr = nullptr;
    // Never unmapped: wisp_ui keeps reading glyphs from the mapping until
    // the device powers off (vector-font spec 2.3).
    esp_partition_mmap_handle_t handle = 0;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err =
        esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK) {
        WLOG_W("fonts", "mapping the %s partition failed: %s", name, esp_err_to_name(err));
        return {};
    }
    WLOG_I("fonts", "mapped %u bytes of %s in %lld us", static_cast<unsigned>(part->size), name,
           static_cast<long long>(esp_timer_get_time() - t0));
    return {static_cast<const uint8_t*>(ptr), static_cast<size_t>(part->size)};
}

}  // namespace

std::span<const uint8_t> map_font_partition() {
    WISP_ASSERT_FLASH_THREAD("map_font_partition");
    return map_partition("fonts");
}

std::span<const uint8_t> map_glyph_partition() {
    WISP_ASSERT_FLASH_THREAD("map_glyph_partition");
    return map_partition("glyphs");
}

}  // namespace wisp::port::esp
