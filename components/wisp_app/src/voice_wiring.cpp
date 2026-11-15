#include "voice_wiring.hpp"

#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/roles.hpp"
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

// A memory read of the settings cell, safe from any thread.
bool tts_muted(const core::SystemSettings& settings) {
    return settings.cell().read([](const core::SettingsSnapshot& v) { return v.tts_mute; });
}

}  // namespace

VoiceWiring::VoiceWiring(VoiceWiringDeps deps) : deps_(std::move(deps)) {
    net::HttpClient* http = deps_.http;
    if (http == nullptr) {
        no_http_ = std::make_unique<NoHttpClient>();
        http = no_http_.get();
    }
    asr_ = std::make_unique<voice::AsrClient>(*http, deps_.models);
    tts_ = std::make_unique<voice::TtsClient>(*http, deps_.models);
    player_ =
        std::make_unique<voice::PcmPlayer>(deps_.registry.by_role<hal::AudioOut>(hal::role::kMain));
    voice_ = std::make_unique<voice::VoiceService>(voice::VoiceDeps{
        .bus = deps_.bus,
        .mic = deps_.registry.by_role<hal::AudioIn>(hal::role::kMain),
        .player = *player_,
        .asr = *asr_,
        .tts = *tts_,
        .models = deps_.models,
        .tones = deps_.tones,
        .locks = deps_.locks,
        .network_up = deps_.network_up,
        .network_expected = deps_.network_expected,
        // Read from the settings cell when a sound starts (on wisp.voice):
        // the stored value, whatever the bus has delivered.
        .volume =
            [settings = &deps_.settings] {
                return settings->cell().read(
                    [](const core::SettingsSnapshot& v) { return static_cast<uint8_t>(v.volume); });
            },
    });
    core::ModelProfileService* models = &deps_.models;
    std::function<bool()> network_up = deps_.network_up;
    std::function<bool()> network_expected = deps_.network_expected;
    conv_ = std::make_unique<shell::ConversationController>(shell::ConversationDeps{
        .agent = deps_.agent,
        .voice = *voice_,
        .tts_mute = [settings = &deps_.settings] { return tts_muted(*settings); },
        // In-memory reads only: the triggers come from any thread.
        .readiness =
            [settings = &deps_.settings, models, network_up, network_expected] {
                using Net = shell::Readiness::Net;
                shell::Readiness r;
                if (network_up && !network_up())
                    r.net = network_expected && network_expected() ? Net::Connecting : Net::Down;
                const core::ProfilesState p = models->get();
                r.llm = !p.active_for(core::ModelKind::Llm).empty();
                r.asr = !p.active_for(core::ModelKind::Asr).empty();
                // Muted, no reply is spoken: speech is not missing.
                r.tts = tts_muted(*settings) || !p.active_for(core::ModelKind::Tts).empty();
                return r;
            },
        .now_ms = os::now_ms,
    });
    talk_button_ = shell::find_talk_button(deps_.registry);
    if (talk_button_ && deps_.buttons != nullptr) {
        talk_key_ = std::make_unique<shell::TalkKey>(deps_.bus, *deps_.buttons, *talk_button_,
                                                     *conv_, deps_.screen_on);
    }
}

VoiceWiring::~VoiceWiring() { stop(); }

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
