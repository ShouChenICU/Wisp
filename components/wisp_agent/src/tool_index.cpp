#include "wisp/agent/tool_index.hpp"

#include <algorithm>
#include <mutex>
#include <string_view>
#include <utility>

namespace wisp::agent {

namespace {

bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// UTF-8 descriptions are searched bytewise, with folding limited to ASCII
// letters. Full Unicode case folding is language-specific and is not part of
// the scene-tool query contract.
bool contains_folded(std::string_view text, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    for (size_t start = 0; start <= text.size() - needle.size(); ++start) {
        size_t offset = 0;
        while (offset < needle.size() &&
               lower_ascii(text[start + offset]) == lower_ascii(needle[offset])) {
            ++offset;
        }
        if (offset == needle.size()) return true;
    }
    return false;
}

bool matches_query(const IndexedTool& tool, std::string_view query) {
    const std::string_view name(tool.full_name.data(), tool.full_name.size());
    const std::string_view description(tool.description.data(), tool.description.size());
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && ascii_space(query[i])) ++i;
        const size_t begin = i;
        while (i < query.size() && !ascii_space(query[i])) ++i;
        if (begin == i) continue;
        const std::string_view term = query.substr(begin, i - begin);
        if (!contains_folded(name, term) && !contains_folded(description, term)) return false;
    }
    return true;
}

bool before(const IndexedTool& a, const IndexedTool& b) {
    if (a.scene_name != b.scene_name) return a.scene_name < b.scene_name;
    const std::string_view a_name(a.full_name.data() + a.scene_id.size() + 1,
                                  a.full_name.size() - a.scene_id.size() - 1);
    const std::string_view b_name(b.full_name.data() + b.scene_id.size() + 1,
                                  b.full_name.size() - b.scene_id.size() - 1);
    if (a_name != b_name) return a_name < b_name;
    return a.full_name < b.full_name;
}

}  // namespace

void ToolIndex::replace_scene(const std::string& scene_id, const std::string& scene_name,
                              const std::vector<core::SceneToolDecl>& declarations) {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::erase_if(tools_, [&scene_id](const IndexedTool& tool) {
        return std::string_view(tool.scene_id.data(), tool.scene_id.size()) == scene_id;
    });
    for (const core::SceneToolDecl& declaration : declarations) {
        IndexedTool indexed;
        indexed.full_name = scene_id;
        indexed.full_name += '.';
        indexed.full_name.append(declaration.name.data(), declaration.name.size());
        indexed.scene_id.assign(scene_id.data(), scene_id.size());
        indexed.scene_name.assign(scene_name.data(), scene_name.size());
        indexed.description.assign(declaration.description.data(), declaration.description.size());
        if (declaration.parameters.empty()) {
            indexed.parameters = R"({"type":"object","properties":{}})";
        } else {
            indexed.parameters.assign(declaration.parameters.data(), declaration.parameters.size());
        }
        indexed.timeout_ms = declaration.timeout_ms;
        tools_.push_back(std::move(indexed));
    }
    std::sort(tools_.begin(), tools_.end(), before);
}

void ToolIndex::remove_scene(const std::string& scene_id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::erase_if(tools_, [&scene_id](const IndexedTool& tool) {
        return std::string_view(tool.scene_id.data(), tool.scene_id.size()) == scene_id;
    });
}

std::optional<IndexedTool> ToolIndex::find(std::string_view full_name) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    const auto found =
        std::find_if(tools_.begin(), tools_.end(), [full_name](const IndexedTool& tool) {
            return std::string_view(tool.full_name.data(), tool.full_name.size()) == full_name;
        });
    if (found == tools_.end()) return std::nullopt;
    return *found;
}

os::LargeVector<IndexedTool> ToolIndex::search(std::string_view query, std::string_view scene,
                                               size_t max) const {
    os::LargeVector<IndexedTool> matches;
    if (max == 0) return matches;
    std::lock_guard<os::Mutex> lock(mutex_);
    for (const IndexedTool& tool : tools_) {
        if (!scene.empty() && std::string_view(tool.scene_id.data(), tool.scene_id.size()) != scene)
            continue;
        if (!matches_query(tool, query)) continue;
        matches.push_back(tool);
        if (matches.size() == max) break;
    }
    return matches;
}

size_t ToolIndex::size() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return tools_.size();
}

}  // namespace wisp::agent
