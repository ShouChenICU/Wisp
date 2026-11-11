#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/core/scene_manifest.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::agent {

struct IndexedTool {
    os::LargeString full_name;
    os::LargeString scene_id;
    os::LargeString scene_name;
    os::LargeString description;
    os::LargeString parameters;
    uint32_t timeout_ms = 30000;
};

class ToolIndex {
public:
    void replace_scene(const std::string& scene_id, const std::string& scene_name,
                       const std::vector<core::SceneToolDecl>& tools);
    void remove_scene(const std::string& scene_id);
    std::optional<IndexedTool> find(std::string_view full_name) const;
    // query is whitespace-split; every word must occur in the name or description.
    os::LargeVector<IndexedTool> search(std::string_view query, std::string_view scene,
                                        size_t max) const;
    size_t size() const;

private:
    mutable os::Mutex mutex_;
    os::LargeVector<IndexedTool> tools_;
};

inline constexpr size_t kFindMax = 20;
inline constexpr size_t kListMax = 200;

}  // namespace wisp::agent
