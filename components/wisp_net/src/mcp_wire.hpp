#pragma once

// McpClient's wire format: the JSON-RPC request bodies it sends and the
// reader that finds the response to one request in what a server answers
// (spec 4.2 of docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "wisp/base/json.hpp"
#include "wisp/net/llm_client.hpp"
#include "wisp/os/large.hpp"

namespace wisp::net::detail {

// {"jsonrpc":"2.0","id":<id>,"method":"initialize","params":{
// "protocolVersion":"2025-06-18","capabilities":{},"clientInfo":
// {"name":"wisp","version":<client_version>}}}
os::LargeString mcp_initialize_body(int64_t id, std::string_view client_version);
// {"jsonrpc":"2.0","method":"notifications/initialized"}
os::LargeString mcp_initialized_body();
// tools/list; the cursor goes in params unless it is empty.
os::LargeString mcp_list_body(int64_t id, std::string_view cursor);

// Appends `s` with every ill-formed UTF-8 sequence replaced by U+FFFD
// (as to_json() does), stopping before the character that would take
// `out` past `max_bytes` in all, so a character is never cut in half.
void append_clean_utf8(os::LargeString& out, std::string_view s, size_t max_bytes);

// Whether `v` is the JSON-RPC response to request `id`: an object whose
// "id" is that integer and that has a "result" or an "error".
bool is_response_to(JsonView v, int64_t id);

// Reads one request's answer as SceneHttp streams it (feed() on
// wisp.shttp; take() on the caller's thread once the request is done --
// its done() callback orders the two). The first non-blank byte decides:
// '{' is a JSON body, collected up to `cap` bytes and parsed by take();
// anything else is a server-sent event stream (one event up to `cap`
// bytes), parsed as it arrives, its
// requests and notifications skipped, until the response to `id`, where
// feed() returns false so the transfer stops there.
class McpReader {
public:
    // An event may be as large as the whole answer may be.
    McpReader(int64_t id, size_t cap) : id_(id), cap_(cap), sse_(cap) {}

    bool feed(std::span<const uint8_t> bytes);
    // The response to `id`; nullopt when there was none (not JSON, not
    // JSON-RPC, another id, a body past the cap, a stream that ended
    // before it).
    std::optional<LargeJson> take();

private:
    enum class Mode : uint8_t { Sniff, Json, Sse };

    void on_event(std::string_view data);

    int64_t id_;
    size_t cap_;
    Mode mode_ = Mode::Sniff;
    bool over_cap_ = false;
    os::LargeString body_;
    SseParser sse_;
    std::optional<LargeJson> found_;
};

}  // namespace wisp::net::detail
