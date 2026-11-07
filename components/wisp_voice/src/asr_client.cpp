#include "wisp/voice/asr_client.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::voice {

namespace {

constexpr const char* kTag = "voice";

// "timeout"/"cancelled"/"transport" for anything else: AsrResult's error
// set has no separate "rate_limited" (429 and 5xx share "server"), unlike
// LlmClient's.
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

// Not cryptographic: the boundary only has to be unlikely to appear inside
// the PCM it delimits. Seeded from the clock and mixed with a per-client
// counter so back-to-back calls (same microsecond, e.g. under test) still
// differ.
std::string random_boundary(uint64_t seed) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(24, '0');
    for (size_t i = 0; i < 12; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        const auto byte = static_cast<uint8_t>(seed);
        out[2 * i] = kHex[byte >> 4];
        out[2 * i + 1] = kHex[byte & 0x0F];
    }
    return out;
}

}  // namespace

AsrClient::AsrClient(net::HttpClient& http, core::ModelProfileService& models,
                     std::function<void(uint32_t)> sleep_ms)
    : http_(http), models_(models), sleep_ms_(std::move(sleep_ms)) {}

AsrResult AsrClient::transcribe(CaptureBuffer& audio, const std::function<bool()>& cancelled,
                                net::RequestWatch* watch, std::function<void(uint32_t)> on_armed) {
    AsrResult result;
    const uint64_t start = os::now_ms();

    std::optional<core::ResolvedProfile> profile = models_.active_for_request(core::ModelKind::Asr);
    if (!profile) {
        result.error = "no_profile";
        WLOG_W(kTag, "asr ms=0 bytes=0 status=0 error=no_profile");
        return result;
    }

    const std::string boundary =
        random_boundary(os::now_us() ^ (static_cast<uint64_t>(start) << 32) ^
                        (static_cast<uint64_t>(++boundary_seq_) << 20));
    const std::string language = core::profile_asr_language(profile->options);
    std::span<const uint8_t> body = audio.finish_multipart(boundary, profile->model, language);
    result.bytes = body.size();

    net::HttpClientRequest req;
    req.method = net::Method::Post;
    req.url = profile->base_url + "/audio/transcriptions";
    req.headers.emplace_back("Content-Type", "multipart/form-data; boundary=" + boundary);
    if (!profile->api_key.empty())
        req.headers.emplace_back("Authorization", "Bearer " + profile->api_key);
    // Only the header copy in `req` is needed from here on.
    profile->api_key.clear();
    req.body_ref = body;
    req.timeout_ms = kAsrTimeoutMs;
    req.cancelled = cancelled;

    const auto stopped = [&cancelled] { return cancelled && cancelled(); };

    for (;;) {
        if (stopped()) {
            result.error = "cancelled";
            break;
        }
        if (watch != nullptr) {
            const uint32_t ticket = watch->arm(req.timeout_ms);
            req.watch = watch;
            if (on_armed) on_armed(ticket);
        }
        ++result.attempts;
        net::HttpClientResponse res = http_.request(req);
        if (watch != nullptr) watch->disarm();
        result.status = res.status;

        if (res.transport != net::TransportError::None) {
            result.error = transport_error_name(res.transport);
            break;
        }
        if (res.status >= 200 && res.status < 300) {
            auto parsed = from_json(res.body);
            const Value* text = parsed && parsed.value().type() == Value::Type::Map
                                    ? parsed.value().find("text")
                                    : nullptr;
            if (text != nullptr && text->type() == Value::Type::String) {
                result.text = text->as_string();
            } else {
                result.error = "bad_response";
            }
            break;
        }
        if (res.status == 401) {
            result.error = "unauthorized";
            break;
        }
        const bool retryable = res.status == 429 || res.status >= 500;
        if (!retryable) {
            result.error = "http";
            break;
        }
        if (result.attempts > kAsrRetries) {
            result.error = "server";
            break;
        }
        for (uint32_t wait = kRetryDelayMs; wait > 0 && !stopped();) {
            const uint32_t slice = std::min(wait, kWaitSliceMs);
            sleep_ms_(slice);
            wait -= slice;
        }
    }

    const auto ms = static_cast<unsigned>(os::now_ms() - start);
    result.ms = ms;
    if (result.error.empty()) {
        WLOG_I(kTag, "asr ms=%u bytes=%zu status=%d", ms, result.bytes, result.status);
    } else {
        WLOG_W(kTag, "asr ms=%u bytes=%zu status=%d error=%s", ms, result.bytes, result.status,
               result.error.c_str());
    }
    return result;
}

}  // namespace wisp::voice
