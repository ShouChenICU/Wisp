#include "wisp/port/esp/esp_font_partition.hpp"

#include "esp_err.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "wisp/core/log.hpp"

namespace wisp::port::esp {

std::span<const uint8_t> map_font_partition() {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fonts");
    if (part == nullptr) {
        WLOG_W("fonts", "no fonts partition");
        return {};
    }
    const void* ptr = nullptr;
    // Never unmapped: wisp_ui keeps reading glyphs from the mapping until
    // the device powers off (spec 2.3).
    esp_partition_mmap_handle_t handle = 0;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err =
        esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK) {
        WLOG_W("fonts", "mapping the fonts partition failed: %s", esp_err_to_name(err));
        return {};
    }
    WLOG_I("fonts", "mapped %u bytes in %lld us", static_cast<unsigned>(part->size),
           static_cast<long long>(esp_timer_get_time() - t0));
    return {static_cast<const uint8_t*>(ptr), static_cast<size_t>(part->size)};
}

}  // namespace wisp::port::esp
