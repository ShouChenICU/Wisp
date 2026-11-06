#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "wisp/net/llm_client.hpp"

namespace wisp::net {

namespace {

constexpr std::string_view kDataField = "data:";

}  // namespace

size_t SseParser::pending_data_bytes() const {
    const std::string_view line(line_.data(), line_.size());
    size_t value = line.size();
    if (line.substr(0, kDataField.size()) == kDataField) {
        value -= kDataField.size();
        if (line.size() > kDataField.size() && line[kDataField.size()] == ' ') --value;
    }
    return data_.size() + (has_data_ ? 1 : 0) + value;
}

void SseParser::end_line(const std::function<void(std::string_view)>& on_data) {
    const std::string_view line(line_.data(), line_.size());
    if (line.empty()) {
        if (has_data_) on_data({data_.data(), data_.size()});
        data_.clear();
        has_data_ = false;
    } else if (line.front() != ':') {
        const size_t colon = line.find(':');
        const std::string_view field = line.substr(0, colon);
        std::string_view value;
        if (colon != std::string_view::npos) {
            value = line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        }
        // event, id and retry mean nothing to a chat-completions stream.
        if (field == "data") {
            if (has_data_) data_.push_back('\n');
            data_.append(value.data(), value.size());
            has_data_ = true;
        }
    }
    line_.clear();
}

void SseParser::feed(std::span<const uint8_t> bytes,
                     const std::function<void(std::string_view)>& on_data) {
    const char* p = reinterpret_cast<const char*>(bytes.data());
    const size_t n = bytes.size();
    size_t i = 0;
    while (i < n && !overflowed_) {
        if (after_cr_) {
            // The LF of a CRLF split across two pieces.
            after_cr_ = false;
            if (p[i] == '\n') {
                ++i;
                continue;
            }
        }
        size_t j = i;
        while (j < n && p[j] != '\n' && p[j] != '\r') ++j;
        line_.append(p + i, j - i);
        if (pending_data_bytes() > kMaxEventBytes) {
            overflowed_ = true;
            line_.clear();
            data_.clear();
            has_data_ = false;
            return;
        }
        if (j == n) return;
        if (p[j] == '\r') {
            if (j + 1 < n) {
                if (p[j + 1] == '\n') ++j;
            } else {
                after_cr_ = true;
            }
        }
        end_line(on_data);
        i = j + 1;
    }
}

}  // namespace wisp::net
