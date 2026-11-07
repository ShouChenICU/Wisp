#pragma once

// TtsClient (spec 4.4): one POST {base_url}/audio/speech per sentence
// against the active tts profile's OpenAI-compatible endpoint, PCM
// streamed back and handed to `on_pcm` as whole 16-bit samples (an odd
// trailing byte in a chunk carries over to the next one; a final odd byte
// with nothing to pair it with is dropped).
//
// Never retried (spec 2's M22): a 429/5xx answer is final, same as any
// other failure. The profile, API key included, comes from
// ModelProfileService::active_for_request(Tts) on every call; the key goes
// into the Authorization header only. The one log line per call ("voice"
// tag) carries only n/chars/ms/status(/error): never the input text
// (spec 3's privacy rule) and never the key.
//
// synthesize() blocks; one call at a time per client.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "wisp/core/model_profiles.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/request_watch.hpp"

namespace wisp::voice {

struct TtsResult {
    // "" ok; "no_profile" "unauthorized" "http" "server" "transport"
    // "timeout" "cancelled"
    std::string error;
    int status = 0;
    size_t bytes = 0;  // PCM bytes received
    uint32_t ms = 0;
};

class TtsClient {
public:
    TtsClient(net::HttpClient& http, core::ModelProfileService& models);

    // The active tts profile's sample rate (its "sample_rate" option, or
    // kTtsDefaultRate without one); nullopt when there is no active tts
    // profile.
    std::optional<uint32_t> sample_rate() const;

    // `on_pcm` is called with each group of whole samples as they arrive,
    // in order; returning false stops the request (TtsResult::error
    // becomes "cancelled"). `cancelled` is checked the same way an
    // HttpClient checks it between pieces of the transfer.
    TtsResult synthesize(std::string_view text,
                         const std::function<bool(std::span<const int16_t>)>& on_pcm,
                         const std::function<bool()>& cancelled,
                         net::RequestWatch* watch = nullptr);

private:
    net::HttpClient& http_;
    core::ModelProfileService& models_;
};

}  // namespace wisp::voice
