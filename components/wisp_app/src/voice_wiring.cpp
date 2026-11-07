#include "voice_wiring.hpp"

#include <algorithm>
#include <utility>

#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "voice";

// What a model request gets when the platform has no HTTP client.
class NoHttpClient final : public net::HttpClient {
public:
    net::HttpClientResponse request(const net::HttpClientRequest&) override {
        net::HttpClientResponse res;
        res.transport = net::TransportError::Other;
        return res;
    }
};

uint8_t clamp_volume(int64_t v) { return static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 100)); }

}  // namespace

VoiceWiring::VoiceWiring(VoiceWiringDeps deps) : deps_(std::move(deps)) {
    // Subscribed before the seed is read, so a change stored in between
    // is not lost.
    settings_sub_ = deps_.bus.subscribe("system.settings", [this](const core::Event& e) {
        auto it = e.payload.find("tts_mute");
        if (it != e.payload.end() && it->second.type() == Value::Type::Bool) {
            tts_mute_.store(it->second.as_bool());
        }
        it = e.payload.find("volume");
        if (it != e.payload.end() && it->second.type() == Value::Type::Int) {
            volume_.store(clamp_volume(it->second.as_int()));
        }
    });
    const core::SystemSettingsValues now = deps_.settings.get();
    tts_mute_.store(now.tts_mute);
    volume_.store(clamp_volume(now.volume));

    net::HttpClient* http = deps_.http;
    if (http == nullptr) {
        no_http_ = std::make_unique<NoHttpClient>();
        http = no_http_.get();
    }
    asr_ = std::make_unique<voice::AsrClient>(*http, deps_.models);
    tts_ = std::make_unique<voice::TtsClient>(*http, deps_.models);
    player_ =
        std::make_unique<voice::PcmPlayer>(deps_.registry.get<hal::AudioOut>("audio_out.main"));
    voice_ = std::make_unique<voice::VoiceService>(voice::VoiceDeps{
        .bus = deps_.bus,
        .mic = deps_.registry.get<hal::AudioIn>("audio_in.main"),
        .player = *player_,
        .asr = *asr_,
        .tts = *tts_,
        .models = deps_.models,
        .tones = deps_.tones,
        .locks = deps_.locks,
        .network_up = deps_.network_up,
        .volume = [this] { return volume_.load(); },
    });
    core::ModelProfileService* models = &deps_.models;
    std::function<bool()> network_up = deps_.network_up;
    conv_ = std::make_unique<shell::ConversationController>(shell::ConversationDeps{
        .bus = deps_.bus,
        .agent = deps_.agent,
        .voice = *voice_,
        .tts_mute = [this] { return tts_mute_.load(); },
        // In-memory reads only: the triggers come from any thread.
        .readiness =
            [models, network_up] {
                if (network_up && !network_up()) return shell::Readiness::NoWifi;
                if (models->get().active_for(core::ModelKind::Llm).empty()) {
                    return shell::Readiness::NoLlm;
                }
                return shell::Readiness::Ready;
            },
        .now_ms = os::now_ms,
    });
    talk_button_ = shell::find_talk_button(deps_.registry);
    if (talk_button_) {
        talk_key_ =
            std::make_unique<shell::TalkKey>(deps_.bus, *talk_button_, *conv_, deps_.screen_on);
    }
}

VoiceWiring::~VoiceWiring() {
    stop();
    deps_.bus.unsubscribe(settings_sub_);
}

Result<void> VoiceWiring::start() {
    if (started_) return Result<void>::err(ErrorCode::Busy, "already started");
    if (auto r = player_->start(); !r) {
        WLOG_E(kTag, "speech player not started: %s", r.error().message.c_str());
        return r;
    }
    if (auto r = voice_->start(); !r) {
        WLOG_E(kTag, "voice not started: %s", r.error().message.c_str());
        player_->stop_task();
        return r;
    }
    started_ = true;
    if (deps_.tones != nullptr) {
        deps_.tones->set_output_guard(
            [v = voice_.get()](core::ToneKind kind) { return v->may_play_tone(kind); });
    }
    conv_->start();
    if (talk_key_) talk_key_->start();
    WLOG_I(kTag, "voice on, talk key %s", talk_button_ ? talk_button_->c_str() : "none");
    return Result<void>::ok();
}

void VoiceWiring::stop() {
    if (!started_) return;
    started_ = false;
    if (talk_key_) talk_key_->stop();
    conv_->stop();
    // The guard calls into the voice: gone before it is.
    if (deps_.tones != nullptr) deps_.tones->set_output_guard(nullptr);
    voice_->stop();
    player_->stop_task();
}

}  // namespace wisp::app
