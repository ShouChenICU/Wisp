#pragma once

// AsrClient (spec 4.2): one POST {base_url}/audio/transcriptions against
// the active asr profile's OpenAI-compatible endpoint, multipart/form-data
// built zero-copy by CaptureBuffer::finish_multipart().
//
// Retries: a 429 or 5xx is retried once, kRetryDelayMs later in
// kWaitSliceMs slices so `cancelled` ends the wait early; a 2xx is parsed
// for "text" and never retried; everything else (401, another 4xx, a
// transport error) is final on the first attempt. An empty "text" is not
// an error (spec 4.6's "not_heard": the caller decides what that means).
//
// The profile, API key included, comes from
// ModelProfileService::active_for_request(Asr) on every call; the key goes
// into the Authorization header only, never into the result or a log
// line. The one log line per call ("voice" tag) carries only
// ms/bytes/status(/error): never the transcript (spec 3's privacy rule)
// and never the key.
//
// `watch`, when given, is armed for every attempt (the retry included)
// with the request's timeout; `on_armed`, when given, is called with the
// ticket each arm() returns, so the caller can abort() the attempt that is
// actually blocked rather than guessing its ticket (RequestWatch::abort,
// `cancelled` alone cannot reach a client blocked in a single read).
//
// transcribe() blocks; one call at a time per client.

#include <cstdint>
#include <functional>
#include <string>

#include "wisp/core/model_profiles.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/os/os.hpp"
#include "wisp/voice/capture_buffer.hpp"

namespace wisp::voice {

struct AsrResult {
    // "" ok; "no_profile" "unauthorized" "http" "server" "transport"
    // "timeout" "cancelled" "bad_response"
    std::string error;
    int status = 0;  // last attempt; 0 without a response
    uint32_t attempts = 0;
    uint32_t ms = 0;
    size_t bytes = 0;  // the request body's size
    std::string text;  // "" is not an error
};

class AsrClient {
public:
    static constexpr uint32_t kRetryDelayMs = 1000;
    // How long a retry wait sleeps before asking `cancelled` again.
    static constexpr uint32_t kWaitSliceMs = 100;

    AsrClient(net::HttpClient& http, core::ModelProfileService& models,
              std::function<void(uint32_t)> sleep_ms = os::sleep_ms);

    AsrResult transcribe(CaptureBuffer& audio, const std::function<bool()>& cancelled,
                         net::RequestWatch* watch = nullptr,
                         std::function<void(uint32_t)> on_armed = nullptr);

private:
    net::HttpClient& http_;
    core::ModelProfileService& models_;
    std::function<void(uint32_t)> sleep_ms_;
    // Mixed into the multipart boundary so two calls in the same
    // microsecond (e.g. under test) still differ; not a security property.
    uint32_t boundary_seq_ = 0;
};

}  // namespace wisp::voice
