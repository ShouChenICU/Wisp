#pragma once

// The media library and the music player in App's boot (media spec 2, 9;
// app.hpp's file header), kept apart from app.cpp.
//
// Built by App::start():
//   - make_media_library(), right after the storage workers start (the
//     library's card jobs go to wisp.sd.io, which must already run, or a
//     scan's jobs could run on two threads): only with a card slot;
//   - make_media_player(), right after the notification sounds: only with
//     an AudioOut of role main. It follows the card (follow_card()) and the
//     volume setting (follow_volume()); the library's removals and the
//     scene lifecycle's deletes reach it from App.
// Their state cells publish media.library and media.player through the
// mirrors here; the player's errors become media.error.
//
// App::stop() releases both subscriptions and stops the player right
// after the status shell (before the voice, so no lease is held when the
// voice goes, and before the arbiter); the library stops just before the
// storage workers. The player object stays until then: the library's
// removal callback and the lifecycle handler may still call it, and
// MediaPlayer ignores commands once stopped.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/core/audio_arbiter.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/media/decoder.hpp"
#include "wisp/media/library.hpp"
#include "wisp/media/player.hpp"
#include "wisp/os/large.hpp"

namespace wisp::app {

// Source of the media.* events.
inline constexpr char kMediaSource[] = "media";

// media.library: every change of the library's cell (media spec 9).
void media_library_mirror(core::EventBus& bus, const media::MediaLibraryState& before,
                          const media::MediaLibraryState& after);
// media.player: every change of the player's cell but the position alone
// (media spec 9: not per position tick).
void media_player_mirror(core::EventBus& bus, const media::PlayerState& before,
                         const media::PlayerState& after);

// The library over "<sd>/media", started; null without a card slot or the
// memory for it.
os::LargeUnique<media::MediaLibrary> make_media_library(core::StorageManager& storage,
                                                        core::StorageRouter& router,
                                                        core::EventBus& bus);

struct MediaPlayerWiring {
    std::shared_ptr<hal::AudioOut> out;  // role main; null: no player
    core::AudioArbiter& arbiter;
    core::WakeLockRegistry& locks;
    core::EventBus& bus;
    media::MediaLibrary* library;            // null without a card slot
    core::SceneFiles* scene_files;           // null without internal storage
    core::StorageRouter* router;             // null without the storage workers
    media::DecoderFactory platform_decoder;  // Platform::audio_decoder
    const core::StateCell<core::SettingsSnapshot>& settings;
    // Platform::random; empty: a clock-seeded generator (shuffle order
    // needs no strong randomness).
    std::function<void(uint8_t*, size_t)> random;
};

// The player, started; null without an output, or when it cannot start
// (logged).
os::LargeUnique<media::MediaPlayer> make_media_player(MediaPlayerWiring w);

// The card going out (or swapped between two deliveries) stops the
// player's session (media spec 4.5). On wisp.bus.
[[nodiscard]] core::CellSubscription follow_card(const core::StateCell<core::SdStatus>& sd,
                                                 media::MediaPlayer& player);
// A change of audio/volume reaches the open output. On wisp.bus.
[[nodiscard]] core::CellSubscription follow_volume(
    const core::StateCell<core::SettingsSnapshot>& settings, media::MediaPlayer& player);

}  // namespace wisp::app
