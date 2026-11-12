#pragma once

#include <charconv>
#include <string_view>

#include "wisp/base/json.hpp"
#include "wisp/os/large.hpp"

namespace wisp::agent::detail {

inline void append_number(os::LargeString& out, uint64_t value) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), value).ptr;
    out.append(text, static_cast<size_t>(end - text));
}

// Keep the caller's Value as a view: strings and nested objects may be
// hundreds of KiB, and even their intermediate copies must use PSRAM.
inline void append_value(os::LargeString& out, const Value& value) {
    switch (value.type()) {
        case Value::Type::String:
            append_json_string(out, value.as_string());
            break;
        case Value::Type::Map: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, item] : value.as_map()) {
                if (!first) out.push_back(',');
                first = false;
                append_json_string(out, key);
                out.push_back(':');
                append_value(out, item);
            }
            out.push_back('}');
            break;
        }
        case Value::Type::List: {
            out.push_back('[');
            bool first = true;
            for (const Value& item : value.as_list()) {
                if (!first) out.push_back(',');
                first = false;
                append_value(out, item);
            }
            out.push_back(']');
            break;
        }
        default:
            // Scalar serialization is bounded regardless of the payload.
            out += to_json(value);
            break;
    }
}

inline os::LargeString external_data_large(std::string_view source, std::string_view content) {
    constexpr std::string_view close = "</data>";
    os::LargeString out;
    out.reserve(source.size() + content.size() + 48);
    out.append("<data source=\"");
    out.append(source.data(), source.size());
    out.append("\">");
    size_t i = 0;
    while (i < content.size()) {
        bool closing = content.size() - i >= close.size();
        for (size_t k = 0; closing && k < close.size(); ++k) {
            char c = content[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            closing = c == close[k];
        }
        if (closing) {
            out.append("&lt;");
            out.append(content.data() + i + 1, close.size() - 1);
            i += close.size();
        } else {
            out.push_back(content[i++]);
        }
    }
    out.append(close.data(), close.size());
    return out;
}

inline void append_external(os::LargeString& out, std::string_view source,
                            std::string_view content) {
    const auto wrapped = external_data_large(source, content);
    append_json_string(out, std::string_view(wrapped.data(), wrapped.size()));
}

}  // namespace wisp::agent::detail
