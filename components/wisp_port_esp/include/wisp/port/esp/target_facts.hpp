#pragma once

#include <cstdint>
#include <string_view>

namespace wisp::port::esp {

// Facts that differ per SoC target and that no Kconfig option carries. This
// table is the one place the ESP port names a chip (AGENTS.md section 3: a
// platform layer may choose by SoC target). Pure data so that host tests can
// cover the lookup; target.hpp binds it to CONFIG_IDF_TARGET.
struct TargetFacts {
    std::string_view idf_target;  // CONFIG_IDF_TARGET
    const char* model;            // net.wifi's DeviceInfo::model
    uint32_t dfs_min_mhz;         // esp_pm_config_t::min_freq_mhz
};

inline constexpr TargetFacts kTargetFacts[] = {{"esp32s3", "ESP32-S3", 80}};

// nullptr when the target has no entry.
constexpr const TargetFacts* find_target_facts(std::string_view idf_target) {
    for (const TargetFacts& f : kTargetFacts) {
        if (f.idf_target == idf_target) return &f;
    }
    return nullptr;
}

}  // namespace wisp::port::esp
