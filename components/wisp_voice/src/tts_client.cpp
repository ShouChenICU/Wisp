#include "wisp/voice/tts_client.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "text.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::voice {

namespace {

constexpr const char* kTag = "voice";
// Samples are flushed to on_pcm once this many have built up, so a large
// response piece is handled without a per-piece heap allocation.
constexpr size_t kPcmScratchSamples = 256;

const char* transport_error_name(net::TransportError e) {
    switch (e) {
        case net::TransportError::Timeout:
            return "timeout";
        case net::TransportError::Cancelled:
            return "cancelled";
        default:
            return "transport";
    }
}

// Code points, not bytes: `text` may be UTF-8, and kTtsMaxChars and the
// timeout budget below both count in code points. For logging and timeout
// sizing only -- never for anything that copies or echoes the text.
size_t code_points(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        i += std::min(detail::utf8_len(static_cast<unsigned char>(s[i])), s.size() - i);
        ++n;
    }
    return n;
}

// The response size this request's own deadline could admit at `rate`:
// rate * 2 bytes/sample * timeout_ms / 1000, rounded up. Sizing the cap
// from the deadline instead of a fixed figure means it can never trip
// before the timeout does, whatever the rate or the sentence length.
size_t max_pcm_bytes(uint32_t rate, uint32_t timeout_ms) {
    const uint64_t bytes = uint64_t{rate} * 2 * uint64_t{timeout_ms};
    return static_cast<size_t>((bytes + 999) / 1000);
}

void log_result(bool ok, std::optional<uint32_t> sentence_index, size_t chars, unsigned ms,
                int status, std::string_view error) {
    if (sentence_index) {
        const auto index = static_cast<unsigned>(*sentence_index);
        if (ok) {
            WLOG_I(kTag, "tts n=%u chars=%zu ms=%u status=%d", index, chars, ms, status);
        } else {
            WLOG_W(kTag, "tts n=%u chars=%zu ms=%u status=%d error=%.*s", index, chars, ms, status,
                   static_cast<int>(error.size()), error.data());
        }
    } else {
        if (ok) {
            WLOG_I(kTag, "tts chars=%zu ms=%u status=%d", chars, ms, status);
        } else {
            WLOG_W(kTag, "tts chars=%zu ms=%u status=%d error=%.*s", chars, ms, status,
                   static_cast<int>(error.size()), error.data());
        }
    }
}

}  // namespace

TtsClient::TtsClient(net::HttpClient& http, core::ModelProfileService& models)
    : http_(http), models_(models) {}

std::optional<uint32_t> TtsClient::sample_rate() const {
    std::optional<core::ResolvedProfile> profile = models_.active_for_request(core::ModelKind::Tts);
    if (!profile) return std::nullopt;
    return core::profile_tts_sample_rate(profile->options).value_or(kTtsDefaultRate);
}

TtsResult TtsClient::synthesize(std::string_view text,
                                const std::function<bool(std::span<const int16_t>)>& on_pcm,
                                const std::function<bool()>& cancelled, net::RequestWatch* watch,
                                std::function<void(uint32_t)> on_armed,
                                std::optional<uint32_t> sentence_index) {
    TtsResult result;
    const uint64_t start = os::now_ms();
    const size_t chars = code_points(text);

    std::optional<core::ResolvedProfile> profile = models_.active_for_request(core::ModelKind::Tts);
    if (!profile) {
        result.error = "no_profile";
        log_result(false, sentence_index, chars, 0, 0, result.error);
        return result;
    }

    const std::optional<uint32_t> asked_rate = core::profile_tts_sample_rate(profile->options);
    const uint32_t rate = asked_rate.value_or(kTtsDefaultRate);
    const std::string voice = core::profile_tts_voice(profile->options);
    const std::optional<double> speed = core::profile_tts_speed(profile->options);

    Map body_map{
        {"model", Value(profile->model)},
        {"input", Value(std::string(text))},
        {"response_format", Value(std::string("pcm"))},
    };
    // OpenAI's request has no sample rate (its pcm is 24 kHz, the default);
    // servers that follow it strictly, such as speech.cpp, answer 400 to an
    // unknown field. Only a profile that sets one sends it.
    if (asked_rate) body_map["sample_rate"] = Value(static_cast<int64_t>(*asked_rate));
    if (!voice.empty()) body_map["voice"] = Value(voice);
    if (speed) body_map["speed"] = Value(*speed);
    const std::string json_body = to_json(Value(std::move(body_map)));

    net::HttpClientRequest req;
    req.method = net::Method::Post;
    req.url = profile->base_url + "/audio/speech";
    req.headers.emplace_back("Content-Type", "application/json");
    if (!profile->api_key.empty())
        req.headers.emplace_back("Authorization", "Bearer " + profile->api_key);
    // Only the header copy in `req` is needed from here on.
    profile->api_key.clear();
    req.body = json_body;
    // spec 2's 15 s is a no-new-audio budget, not a whole-sentence one
    // (limits.hpp's kTtsMsPerCodePoint), and the response cap is sized
    // from that same deadline so it can never trip first.
    req.timeout_ms = kTtsTimeoutMs + static_cast<uint32_t>(chars) * kTtsMsPerCodePoint;
    req.max_response_bytes = max_pcm_bytes(rate, req.timeout_ms);
    req.cancelled = cancelled;

    int status = 0;
    req.on_status = [&status](int s) { status = s; };

    // Odd-sized chunks never align with a 16-bit sample boundary; the
    // trailing byte of one chunk pairs with the leading byte of the next.
    uint8_t carry = 0;
    bool has_carry = false;
    req.on_body = [&](std::span<const uint8_t> bytes) -> bool {
        // A failed response's body is dropped unread: it may be a JSON
        // error that quotes the key, and it is not audio either way.
        if (status < 200 || status >= 300) return true;
        result.bytes += bytes.size();
        int16_t scratch[kPcmScratchSamples];
        size_t n = 0;
        size_t i = 0;
        if (has_carry && !bytes.empty()) {
            const auto lo = static_cast<uint16_t>(carry);
            const auto hi = static_cast<uint16_t>(bytes[0]);
            scratch[n++] = static_cast<int16_t>(lo | (hi << 8));
            has_carry = false;
            i = 1;
        }
        for (; i + 1 < bytes.size(); i += 2) {
            if (n == kPcmScratchSamples) {
                if (!on_pcm({scratch, n})) return false;
                n = 0;
            }
            const auto lo = static_cast<uint16_t>(bytes[i]);
            const auto hi = static_cast<uint16_t>(bytes[i + 1]);
            scratch[n++] = static_cast<int16_t>(lo | (hi << 8));
        }
        if (i < bytes.size()) {
            carry = bytes[i];
            has_carry = true;
        }
        return n == 0 || on_pcm({scratch, n});
    };

    if (watch != nullptr) {
        const uint32_t ticket = watch->arm(req.timeout_ms);
        req.watch = watch;
        if (on_armed) on_armed(ticket);
    }
    net::HttpClientResponse res = http_.request(req);
    if (watch != nullptr) watch->disarm();
    result.status = status != 0 ? status : res.status;

    if (res.transport != net::TransportError::None) {
        result.error = transport_error_name(res.transport);
    } else if (result.status >= 200 && result.status < 300) {
        // Ok; a trailing unpaired byte (odd total length) is dropped.
    } else if (result.status == 401) {
        result.error = "unauthorized";
    } else if (result.status == 429 || result.status >= 500) {
        result.error = "server";
    } else {
        result.error = "http";
    }

    const auto ms = static_cast<unsigned>(os::now_ms() - start);
    result.ms = ms;
    log_result(result.error.empty(), sentence_index, chars, ms, result.status, result.error);
    return result;
}

}  // namespace wisp::voice
