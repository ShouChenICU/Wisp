#pragma once

// LLM endpoint connectivity test (spec 5.3). Lives in wisp_net rather than
// on core::LlmConfigService because it needs HttpClient, and wisp_core
// cannot depend on wisp_net.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wisp/core/llm_config.hpp"
#include "wisp/net/http.hpp"

namespace wisp::net {

struct LlmProbe {
    bool ok = false;
    TransportError transport = TransportError::None;
    int http_status = 0;  // 0 when no HTTP response arrived
    uint32_t latency_ms = 0;
    std::vector<std::string> models;  // data[].id, only when ok
};

// Timeout of the probe request.
constexpr uint32_t kLlmProbeTimeoutMs = 10000;

// Sends GET {base_url}/models with "Authorization: Bearer <key>" (no
// header when no key is stored). `ok` means HTTP 200 and a body with a
// "data" list whose entries carry string "id"s. Without a base_url no
// request is made and the result is {ok=false, transport=Other}.
// `now_ms` is read once before and once after the request.
//
// The key goes into the Authorization header and nowhere else: not the
// URL, not a log line, not the result. The upstream body is not returned
// either (error bodies may quote the key back), and model ids that
// contain the key are dropped.
LlmProbe probe_llm(const core::LlmConfigService& llm, HttpClient& client,
                   std::function<uint64_t()> now_ms);

}  // namespace wisp::net
