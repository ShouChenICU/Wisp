#pragma once

// Model profile connectivity test (spec 5.3): the HTTP half of
// core::ModelProfileService::test(). It lives in wisp_net because it needs
// HttpClient, which wisp_core cannot depend on; App hands the service a
// make_model_prober() at boot.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include "wisp/core/model_profiles.hpp"
#include "wisp/net/http.hpp"

namespace wisp::net {

// Timeout of the probe request.
constexpr uint32_t kModelProbeTimeoutMs = 10000;
// The largest /models body read (received into Mem::Large); a longer one
// fails the test as "other".
constexpr size_t kModelProbeMaxBytes = 256 * 1024;

// Bounds on what one list yields: entries past kMaxListedModels are
// ignored, and so are voices past either voice limit and languages past
// kMaxModelLanguages. Ids, voice names, languages and genders longer than
// their limit are dropped (a cut one would name something else);
// descriptions are cut on a character boundary and end in "...".
constexpr size_t kMaxListedModels = 256;
constexpr size_t kMaxModelIdBytes = core::ModelProfileService::kMaxModelBytes;
constexpr size_t kMaxModelLanguages = 48;
constexpr size_t kMaxModelVoices = 64;
constexpr size_t kMaxListedVoices = 512;  // over all models
constexpr size_t kMaxVoiceNameBytes = core::ModelProfileService::kMaxOptionValueBytes;
constexpr size_t kMaxLanguageBytes = 16;
constexpr size_t kMaxGenderBytes = 16;
constexpr size_t kMaxVoiceDescriptionBytes = 120;

// Reads an OpenAI-style /models body into probe.models (data[].id) and
// probe.details. False when it is not one: not JSON, no "data" list, or
// nested deeper than 64 levels. Strings containing `key` (when not empty)
// are dropped: a model with such an id, a voice with such a field, such a
// language.
bool parse_model_list(std::string_view body, std::string_view key, core::ModelProbe& probe);

// Sends GET {base_url}/models with "Authorization: Bearer <key>" (no
// header without a key). `ok` means HTTP 200 and a body that
// parse_model_list() accepts. HTTP 404 is reported as
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
