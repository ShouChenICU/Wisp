#include "wisp/net/llm_probe.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "llm_probe";
constexpr std::string_view kBearer = "Bearer ";

// data[].id from an OpenAI-style /models body; false when the body is not
// shaped like one. Ids containing `key` are dropped (see the header).
bool parse_models(const std::string& body, std::string_view key, std::vector<std::string>& out) {
    auto parsed = from_json(body);
    if (!parsed) return false;
    const Value* data = parsed.value().find("data");
    if (data == nullptr || data->type() != Value::Type::List) return false;
    for (const Value& entry : data->as_list()) {
        const Value* id = entry.find("id");
        if (id == nullptr || id->type() != Value::Type::String) continue;
        const std::string& s = id->as_string();
        if (!key.empty() && s.find(key) != std::string::npos) continue;
        out.push_back(s);
    }
    return true;
}

}  // namespace

LlmProbe probe_llm(const core::LlmConfigService& llm, HttpClient& client,
                   std::function<uint64_t()> now_ms) {
    LlmProbe probe;
    const core::LlmConfig config = llm.get();
    if (config.base_url.empty()) {
        probe.transport = TransportError::Other;
        return probe;
    }

    HttpClientRequest req;
    req.method = Method::Get;
    // LlmConfigService strips trailing '/' from base_url.
    req.url = config.base_url + "/models";
    req.timeout_ms = kLlmProbeTimeoutMs;
    req.headers.emplace_back("Accept", "application/json");
    const std::string auth = llm.authorization_for_request();
    if (!auth.empty()) req.headers.emplace_back("Authorization", auth);

    const uint64_t start = now_ms();
    HttpClientResponse res = client.request(req);
    const uint64_t end = now_ms();
    probe.latency_ms = static_cast<uint32_t>(end >= start ? end - start : 0);
    probe.transport = res.transport;
    if (res.transport != TransportError::None) {
        WLOG_I(kTag, "probe failed: transport error %s", to_string(res.transport));
        return probe;
    }
    probe.http_status = res.status;
    if (res.status != 200) {
        WLOG_I(kTag, "probe failed: HTTP %d", res.status);
        return probe;
    }

    std::string_view key = auth;
    if (key.substr(0, kBearer.size()) == kBearer) key.remove_prefix(kBearer.size());
    std::vector<std::string> models;
    if (!parse_models(res.body, key, models)) {
        WLOG_I(kTag, "probe failed: HTTP 200 without a model list");
        return probe;
    }
    probe.ok = true;
    probe.models = std::move(models);
    WLOG_I(kTag, "probe ok: %u ms, %u models", static_cast<unsigned>(probe.latency_ms),
           static_cast<unsigned>(probe.models.size()));
    return probe;
}

}  // namespace wisp::net
