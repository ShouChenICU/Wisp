#pragma once

// TtsClient (spec 4.4): one POST {base_url}/audio/speech per sentence
// against the active tts profile's OpenAI-compatible endpoint, PCM
// streamed back and handed to `on_pcm` as whole 16-bit samples (an odd
// trailing byte in a chunk carries over to the next one; a final odd byte
// with nothing to pair it with is dropped). A response that never reaches
// 2xx has its body dropped: it never reaches `on_pcm` and is never counted
// in TtsResult::bytes (it may be a JSON error that quotes the key).
//
// Never retried (spec 2): a 429/5xx answer is final, same as any other
// failure. The request's timeout_ms is kTtsTimeoutMs plus
// kTtsMsPerCodePoint per code point of `text` (see limits.hpp for why:
// spec 2's 15 s is a no-new-audio budget, not a whole-sentence one), and
// max_response_bytes is sized from that same timeout at the active
// profile's sample rate, so the cap can never trip before the deadline
// does.
//
// The profile, API key included, comes from
// ModelProfileService::active_for_request(Tts) on every call; the key goes
// into the Authorization header only. The one log line per call ("voice"
// tag) carries only chars/ms/status(/error), plus a leading n=<index> when
// the caller passes one: never the input text (spec 3's privacy rule) and
// never the key.
//
// `watch`, when given, is armed with the request's timeout; `on_armed`,
// when given, is called with the ticket arm() returns, so the caller can
// abort() the request if it is blocked in a read (RequestWatch::abort,
// `cancelled` alone cannot reach a client blocked in a single read).
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
    // HttpClient checks it between pieces of the transfer. `sentence_index`
    // is only for the log line's leading n=<index> (the caller's own
    // numbering, e.g. 1-based per turn); omit it and the log line has no
    // n= field at all.
    TtsResult synthesize(std::string_view text,
                         const std::function<bool(std::span<const int16_t>)>& on_pcm,
                         const std::function<bool()>& cancelled, net::RequestWatch* watch = nullptr,
                         std::function<void(uint32_t)> on_armed = nullptr,
                         std::optional<uint32_t> sentence_index = std::nullopt);

private:
    net::HttpClient& http_;
    core::ModelProfileService& models_;
};

}  // namespace wisp::voice
