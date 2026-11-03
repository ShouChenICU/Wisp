#include "wisp/net/model_probe.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "model_probe";

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

core::ModelProbe probe_models(const core::ResolvedProfile& target, HttpClient& client,
                              const std::function<uint64_t()>& now_ms) {
    core::ModelProbe probe;
    if (target.base_url.empty()) {
        probe.error = to_string(TransportError::Other);
        return probe;
    }

    HttpClientRequest req;
    req.method = Method::Get;
    // ModelProfileService strips trailing '/' from base_url.
    req.url = target.base_url + "/models";
    req.timeout_ms = kModelProbeTimeoutMs;
    req.headers.emplace_back("Accept", "application/json");
    if (!target.api_key.empty())
        req.headers.emplace_back("Authorization", "Bearer " + target.api_key);

    const uint64_t start = now_ms();
    HttpClientResponse res = client.request(req);
    const uint64_t end = now_ms();
    probe.latency_ms = static_cast<uint32_t>(end >= start ? end - start : 0);
    if (res.transport != TransportError::None) {
        probe.error = to_string(res.transport);
        WLOG_I(kTag, "probe of %s failed: transport error %s", target.id.c_str(),
               probe.error.c_str());
        return probe;
    }
    probe.http_status = res.status;
    if (res.status != 200) {
        probe.error = res.status == 404 ? "no_model_list" : "http";
        WLOG_I(kTag, "probe of %s failed: HTTP %d", target.id.c_str(), res.status);
        return probe;
    }

    std::vector<std::string> models;
    if (!parse_models(res.body, target.api_key, models)) {
        probe.error = "http";
        WLOG_I(kTag, "probe of %s failed: HTTP 200 without a model list", target.id.c_str());
        return probe;
    }
    probe.ok = true;
    probe.models = std::move(models);
    WLOG_I(kTag, "probe of %s ok: %u ms, %u models", target.id.c_str(),
           static_cast<unsigned>(probe.latency_ms), static_cast<unsigned>(probe.models.size()));
    return probe;
}

core::ModelProber make_model_prober(HttpClient& client, std::function<uint64_t()> now_ms) {
    return [&client, now_ms = std::move(now_ms)](const core::ResolvedProfile& target) {
        return probe_models(target, client, now_ms);
    };
}

}  // namespace wisp::net
