#include "mcp_wire.hpp"

#include <string>
#include <utility>

#include "large_json.hpp"
#include "wisp/net/mcp_client.hpp"

namespace wisp::net::detail {

namespace {

void append_id(os::LargeString& out, int64_t id) {
    const std::string digits = std::to_string(id);  // short: stays in the string's own buffer
    out.append(digits.data(), digits.size());
}

}  // namespace

os::LargeString mcp_initialize_body(int64_t id, std::string_view client_version) {
    os::LargeString out = R"({"jsonrpc":"2.0","id":)";
    append_id(out, id);
    out += R"(,"method":"initialize","params":{"protocolVersion":)";
    append_json_string(out, kMcpProtocol);
    out += R"(,"capabilities":{},"clientInfo":{"name":"wisp","version":)";
    append_json_string(out, client_version);
    out += "}}}";
    return out;
}

os::LargeString mcp_initialized_body() {
    return R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
}

os::LargeString mcp_list_body(int64_t id, std::string_view cursor) {
    os::LargeString out = R"({"jsonrpc":"2.0","id":)";
    append_id(out, id);
    out += R"(,"method":"tools/list","params":{)";
    if (!cursor.empty()) {
        out += R"("cursor":)";
        append_json_string(out, cursor);
    }
    out += "}}";
    return out;
}

os::LargeString mcp_call_body(int64_t id, std::string_view tool, std::string_view args_json) {
    os::LargeString out;
    out.reserve(args_json.size() + tool.size() + 96);
    out += R"({"jsonrpc":"2.0","id":)";
    append_id(out, id);
    out += R"(,"method":"tools/call","params":{"name":)";
    append_json_string(out, tool);
    out += R"(,"arguments":)";
    if (args_json.empty()) {
        out += "{}";
    } else {
        out.append(args_json.data(), args_json.size());
    }
    out += "}}";
    return out;
}

void append_clean_utf8(os::LargeString& out, std::string_view s, size_t max_bytes) {
    static constexpr std::string_view kReplacement = "\xef\xbf\xbd";
    size_t i = 0;
    while (i < s.size()) {
        size_t bad = 0;
        // utf8_sequence() is for lead bytes past ASCII only.
        const size_t len =
            static_cast<unsigned char>(s[i]) < 0x80 ? 1 : json_detail::utf8_sequence(s, i, &bad);
        const std::string_view piece = len != 0 ? s.substr(i, len) : kReplacement;
        if (out.size() + piece.size() > max_bytes) return;
        out.append(piece.data(), piece.size());
        i += len != 0 ? len : bad;
    }
}

bool is_response_to(JsonView v, int64_t id) {
    if (v.type() != JsonView::Type::Object) return false;
    const auto got = v.find("id").integer();
    if (!got || *got != id) return false;
    return v.find("result").type() != JsonView::Type::Null ||
           v.find("error").type() != JsonView::Type::Null;
}

bool McpReader::feed(std::span<const uint8_t> bytes) {
    if (found_ || over_cap_) return false;
    if (mode_ == Mode::Sniff) {
        size_t skip = 0;
        while (skip < bytes.size() && (bytes[skip] == ' ' || bytes[skip] == '\t' ||
                                       bytes[skip] == '\r' || bytes[skip] == '\n'))
            ++skip;
        if (skip == bytes.size()) return true;
        mode_ = bytes[skip] == '{' ? Mode::Json : Mode::Sse;
        bytes = bytes.subspan(skip);
    }
    if (mode_ == Mode::Json) {
        if (bytes.size() > cap_ - body_.size()) {
            over_cap_ = true;
            return false;
        }
        body_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }
    sse_.feed(bytes, [this](std::string_view data) {
        if (!found_) on_event(data);
    });
    return !found_ && !sse_.overflowed();
}

void McpReader::on_event(std::string_view data) {
    // Server requests, notifications and anything unparsable are skipped:
    // the client declares no capability that would make them matter.
    auto doc = parse_json_large(data, large_json_alloc());
    if (!doc) return;
    if (!is_response_to(doc.value().root(), id_)) return;
    found_.emplace(std::move(doc.value()));
}

std::optional<LargeJson> McpReader::take() {
    if (mode_ == Mode::Json && !over_cap_) {
        auto doc = parse_json_large(body_, large_json_alloc());
        os::LargeString().swap(body_);
        if (doc && is_response_to(doc.value().root(), id_)) return std::move(doc.value());
        return std::nullopt;
    }
    return std::move(found_);
}

}  // namespace wisp::net::detail
