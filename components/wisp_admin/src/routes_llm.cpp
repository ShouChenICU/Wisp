// LLM endpoint configuration and connectivity test (spec 5.3, 6.1).
//
// The API key is write-only: responses report only has_key, and the test
// result carries the status code and an error category, never the
// upstream body (an error body may quote the key back).

#include <optional>
#include <string>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/net/llm_probe.hpp"

namespace wisp::admin::detail {

namespace {

Value llm_value(const core::LlmConfig& c) {
    return Value(Map{
        {"base_url", Value(c.base_url)},
        {"model", Value(c.model)},
        {"has_key", Value(c.has_key)},
    });
}

net::HttpResponse get_llm(AdminDeps& deps, const RequestContext&) {
    return net::HttpResponse::json(200, llm_value(deps.llm.get()));
}

// Any subset of {base_url, model, api_key}, all strings; api_key "" clears
// the key. Unknown fields are rejected so a typo does not silently do
// nothing.
net::HttpResponse put_llm(AdminDeps& deps, const RequestContext& ctx) {
    if (ctx.body.type() != Value::Type::Map) {
        return bad_request("body must be a JSON object");
    }
    core::LlmConfigUpdate update;
    for (const auto& [key, value] : ctx.body.as_map()) {
        std::optional<std::string>* slot = nullptr;
        if (key == "base_url") {
            slot = &update.base_url;
        } else if (key == "model") {
            slot = &update.model;
        } else if (key == "api_key") {
            slot = &update.api_key;
        } else {
            return bad_request("unknown field; expected base_url, model, api_key");
        }
        if (value.type() != Value::Type::String) {
            return bad_request("base_url, model and api_key must be strings");
        }
        *slot = value.as_string();
    }
    auto r = deps.llm.set(update);
    if (!r) return service_error(r.error());
    return net::HttpResponse::json(200, llm_value(deps.llm.get()));
}

// Blocks the HTTP thread for up to the probe's 10 s timeout. "error" is
// the transport category, "http" for any HTTP-level failure (non-200, or
// a 200 that is not a model list), or null on success.
net::HttpResponse test_llm(AdminDeps& deps, const RequestContext&) {
    if (deps.http_client == nullptr) {
        return net::HttpResponse::error(503, "unavailable", "no HTTP client on this platform");
    }
    net::LlmProbe p = net::probe_llm(deps.llm, *deps.http_client, deps.now_ms);
    Value error;
    if (p.transport != net::TransportError::None) {
        error = Value(net::to_string(p.transport));
    } else if (!p.ok) {
        error = Value("http");
    }
    List models;
    for (std::string& m : p.models) models.push_back(Value(std::move(m)));
    return net::HttpResponse::json(200, Value(Map{
                                            {"ok", Value(p.ok)},
                                            {"error", std::move(error)},
                                            {"http_status", Value(p.http_status)},
                                            {"latency_ms", Value(int64_t{p.latency_ms})},
                                            {"models", Value(std::move(models))},
                                        }));
}

}  // namespace

void add_llm_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/llm", false, get_llm});
    routes.push_back({Method::Put, "/llm", false, put_llm});
    routes.push_back({Method::Post, "/llm/test", false, test_llm});
}

}  // namespace wisp::admin::detail
