#pragma once

// Model profile connectivity test (spec 5.3): the HTTP half of
// core::ModelProfileService::test(). It lives in wisp_net because it needs
// HttpClient, which wisp_core cannot depend on; App hands the service a
// make_model_prober() at boot.

#include <cstdint>
#include <functional>

#include "wisp/core/model_profiles.hpp"
#include "wisp/net/http.hpp"

namespace wisp::net {

// Timeout of the probe request.
constexpr uint32_t kModelProbeTimeoutMs = 10000;

// Sends GET {base_url}/models with "Authorization: Bearer <key>" (no
// header without a key). `ok` means HTTP 200 and a body with a "data"
// list whose entries carry string "id"s. HTTP 404 is reported as
// "no_model_list" (the platform has no model list), every other failure
// as described on core::ModelProbe. Without a base_url no request is made
// and the error is "other". `now_ms` is read once before and once after
// the request.
//
// The key goes into the Authorization header and nowhere else: not the
// URL, not a log line, not the result. The upstream body is not returned
// either (error bodies may quote the key back), and model ids that
// contain the key are dropped.
core::ModelProbe probe_models(const core::ResolvedProfile& target, HttpClient& client,
                              const std::function<uint64_t()>& now_ms);

// A core::ModelProber over probe_models(). `client` must outlive it.
core::ModelProber make_model_prober(HttpClient& client, std::function<uint64_t()> now_ms);

}  // namespace wisp::net
