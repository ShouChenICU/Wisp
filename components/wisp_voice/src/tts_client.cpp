#include "wisp/voice/tts_client.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::voice {

namespace {

constexpr const char* kTag = "voice";

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
                                const std::function<bool()>& cancelled, net::RequestWatch* watch) {
    TtsResult result;
    const uint64_t start = os::now_ms();

    std::optional<core::ResolvedProfile> profile = models_.active_for_request(core::ModelKind::Tts);
    if (!profile) {
        result.error = "no_profile";
        WLOG_W(kTag, "tts n=0 chars=%zu ms=0 status=0 error=no_profile", text.size());
        return result;
    }

    const uint32_t rate = core::profile_tts_sample_rate(profile->options).value_or(kTtsDefaultRate);
    const std::string voice = core::profile_tts_voice(profile->options);
    const std::optional<double> speed = core::profile_tts_speed(profile->options);

    Map body_map{
        {"model", Value(profile->model)},
        {"input", Value(std::string(text))},
        {"response_format", Value(std::string("pcm"))},
        {"sample_rate", Value(static_cast<int64_t>(rate))},
    };
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
    req.timeout_ms = kTtsTimeoutMs;
    req.cancelled = cancelled;

    // Odd-sized chunks never align with a 16-bit sample boundary; the
    // trailing byte of one chunk pairs with the leading byte of the next.
    uint8_t carry = 0;
    bool has_carry = false;
    size_t samples_out = 0;
    req.on_body = [&](std::span<const uint8_t> bytes) -> bool {
        result.bytes += bytes.size();
        std::vector<int16_t> samples;
        size_t i = 0;
        if (has_carry && !bytes.empty()) {
            const auto lo = static_cast<uint16_t>(carry);
            const auto hi = static_cast<uint16_t>(bytes[0]);
            samples.push_back(static_cast<int16_t>(lo | (hi << 8)));
            has_carry = false;
            i = 1;
        }
        for (; i + 1 < bytes.size(); i += 2) {
            const auto lo = static_cast<uint16_t>(bytes[i]);
            const auto hi = static_cast<uint16_t>(bytes[i + 1]);
            samples.push_back(static_cast<int16_t>(lo | (hi << 8)));
        }
        if (i < bytes.size()) {
            carry = bytes[i];
            has_carry = true;
        }
        if (samples.empty()) return true;
        samples_out += samples.size();
        return on_pcm(samples);
    };

    if (watch != nullptr) {
        watch->arm(req.timeout_ms);
        req.watch = watch;
    }
    net::HttpClientResponse res = http_.request(req);
    if (watch != nullptr) watch->disarm();
    result.status = res.status;

    if (res.transport != net::TransportError::None) {
        result.error = transport_error_name(res.transport);
    } else if (res.status >= 200 && res.status < 300) {
        // Ok; a trailing unpaired byte (odd total length) is dropped.
    } else if (res.status == 401) {
        result.error = "unauthorized";
    } else if (res.status == 429 || res.status >= 500) {
        result.error = "server";
    } else {
        result.error = "http";
    }

    const auto ms = static_cast<unsigned>(os::now_ms() - start);
    result.ms = ms;
    if (result.error.empty()) {
        WLOG_I(kTag, "tts n=%zu chars=%zu ms=%u status=%d", samples_out, text.size(), ms,
               result.status);
    } else {
        WLOG_W(kTag, "tts n=%zu chars=%zu ms=%u status=%d error=%s", samples_out, text.size(), ms,
               result.status, result.error.c_str());
    }
    return result;
}

}  // namespace wisp::voice
