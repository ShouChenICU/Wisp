#include "wisp/scene/scene_data.hpp"

namespace wisp::scene {

std::string scene_data(std::string_view scene_id, std::string_view content) {
    static constexpr std::string_view kClose = "</data>";
    std::string out = "<data source=\"scene:";
    out.append(scene_id);
    out.append("\">");
    out.reserve(out.size() + content.size() + kClose.size());
    size_t i = 0;
    while (i < content.size()) {
        bool closing = content.size() - i >= kClose.size();
        for (size_t k = 0; closing && k < kClose.size(); ++k) {
            char c = content[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            closing = c == kClose[k];
        }
        if (closing) {
            out.append("&lt;");
            out.append(content.substr(i + 1, kClose.size() - 1));
            i += kClose.size();
        } else {
            out.push_back(content[i]);
            ++i;
        }
    }
    out.append(kClose);
    return out;
}

}  // namespace wisp::scene
