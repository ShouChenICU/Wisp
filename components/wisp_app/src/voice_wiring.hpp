#pragma once

// The voice half of App's boot (MVP spec 4, 6.3): the ASR and TTS clients
// over the platform's HTTP client, the PcmPlayer on "audio_out.main"
// (wisp.play), the VoiceService on "audio_in.main" (wisp.voice), the
// ConversationController that joins it to the agent and, when a button's
// caps "role" is "talk", the TalkKey on it.
//
// App builds it with the agent (App::start(), right after build_agent())
// and start()s it once the agent runs; stop() comes before the agent
// stops and the object goes right after. A missing mic or speaker is no
// failure: the voice reports mic_unavailable, the speech plays nothing.
//
// Cached for the voice's tasks, which never read Settings: voice/tts_mute
// and audio/volume, seeded at construction and kept from system.settings.
// The TonePlayer's output guard is the voice's may_play_tone() from
// start() to stop() (wisp/voice/voice_service.hpp, "Sharing the output").

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "wisp/agent/agent_service.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/tone_player.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/http.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/shell/talk_key.hpp"
#include "wisp/voice/asr_client.hpp"
#include "wisp/voice/pcm_player.hpp"
#include "wisp/voice/tts_client.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::app {

struct VoiceWiringDeps {
    core::EventBus& bus;
    core::DeviceRegistry& registry;
    core::ModelProfileService& models;
    core::SystemSettings& settings;  // read once, at construction
    agent::AgentService& agent;
    core::WakeLockRegistry* locks = nullptr;
    core::TonePlayer* tones = nullptr;
    // Null: every model request fails as a transport error.
    net::HttpClient* http = nullptr;
    // Read on wisp.voice and the callers of the triggers: cached values
    // only.
    std::function<bool()> network_up;
    std::function<bool()> screen_on;
};

class VoiceWiring {
public:
    explicit VoiceWiring(VoiceWiringDeps deps);
    ~VoiceWiring();  // stop()

    VoiceWiring(const VoiceWiring&) = delete;
    VoiceWiring& operator=(const VoiceWiring&) = delete;

    // Starts wisp.play and wisp.voice, then the controller and the talk
    // key. The first failure is returned and the rest is left stopped.
    Result<void> start();
    void stop();

    shell::ConversationController& conversation() { return *conv_; }
    voice::VoiceService& voice() { return *voice_; }
    voice::PcmPlayer& player() { return *player_; }
    // The talk key's button, if the board marked one.
    const std::optional<std::string>& talk_button() const { return talk_button_; }

private:
    VoiceWiringDeps deps_;
    std::atomic<bool> tts_mute_{false};
    std::atomic<uint8_t> volume_{60};
    core::SubscriptionId settings_sub_ = 0;
    std::unique_ptr<net::HttpClient> no_http_;
    std::unique_ptr<voice::AsrClient> asr_;
    std::unique_ptr<voice::TtsClient> tts_;
    std::unique_ptr<voice::PcmPlayer> player_;
    std::unique_ptr<voice::VoiceService> voice_;
    std::unique_ptr<shell::ConversationController> conv_;
    std::optional<std::string> talk_button_;
    std::unique_ptr<shell::TalkKey> talk_key_;
    bool started_ = false;
};

}  // namespace wisp::app
