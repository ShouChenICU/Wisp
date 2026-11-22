#include "media_wiring.hpp"

#include <string>
#include <utility>

#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/media/play_queue.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

namespace {

constexpr char kTag[] = "media";

void publish_error(core::EventBus& bus, const std::string& reason, const std::string& item) {
    bus.publish(core::Event{core::topics::kMediaError,
                            kMediaSource,
                            0,
                            {{"reason", Value(reason)}, {"track_id", Value(item)}}});
}

// xorshift32 for a platform without Platform::random.
std::function<uint32_t()> shuffle_random(std::function<void(uint8_t*, size_t)> random) {
    if (random) {
        return [random = std::move(random)] {
            uint8_t b[4] = {};
            random(b, sizeof(b));
            return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 |
                   static_cast<uint32_t>(b[2]) << 16 | static_cast<uint32_t>(b[3]) << 24;
        };
    }
    uint32_t seed = static_cast<uint32_t>(os::now_ms()) | 1u;
    return [seed]() mutable {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
}

}  // namespace

void media_library_mirror(core::EventBus& bus, const media::MediaLibraryState&,
                          const media::MediaLibraryState& after) {
    bus.publish(core::Event{core::topics::kMediaLibrary,
                            kMediaSource,
                            0,
                            {{"available", Value(after.available)},
                             {"scanning", Value(after.scanning)},
                             {"track_count", Value(static_cast<int64_t>(after.track_count))}}});
}

void media_player_mirror(core::EventBus& bus, const media::PlayerState& before,
                         const media::PlayerState& after) {
    media::PlayerState moved = before;
    moved.position_s = after.position_s;
    if (moved == after) return;
    bus.publish(core::Event{core::topics::kMediaPlayer,
                            kMediaSource,
                            0,
                            {{"status", Value(media::to_string(after.status))},
                             {"pause_reason", Value(media::to_string(after.pause_reason))},
                             {"track_id", Value(after.track.id)},
                             {"index", Value(static_cast<int64_t>(after.index))},
                             {"mode", Value(media::to_string(after.mode))}}});
}

os::LargeUnique<media::MediaLibrary> make_media_library(core::StorageManager& storage,
                                                        core::StorageRouter& router,
                                                        core::EventBus& bus) {
    Result<std::string> sd = storage.sd_root();
    if (!sd) return {};
    os::LargeUnique<media::MediaLibrary> lib =
        os::make_large<media::MediaLibrary>(media::MediaLibraryDeps{
            .storage = storage,
            .router = &router,
            .bus = bus,
            .root = sd.value() + "/media",
            .mirror = &media_library_mirror,
        });
    if (!lib) {
        WLOG_E(kTag, "no memory for the media library; it is off");
        return {};
    }
    lib->start();
    return lib;
}

os::LargeUnique<media::MediaPlayer> make_media_player(MediaPlayerWiring w) {
    if (!w.out) return {};
    core::EventBus* bus = &w.bus;
    const core::StateCell<core::SettingsSnapshot>* settings = &w.settings;
    os::LargeUnique<media::MediaPlayer> player =
        os::make_large<media::MediaPlayer>(media::MediaPlayerDeps{
            .out = std::move(w.out),
            .arbiter = w.arbiter,
            .locks = w.locks,
            .bus = w.bus,
            .library = w.library,
            .scene_files = w.scene_files,
            .router = w.router,
            .decoders = media::with_wav(std::move(w.platform_decoder)),
            // The settings cell: memory only, as wisp.media must not touch
            // flash (AGENTS.md section 5).
            .volume =
                [settings] {
                    return settings->read([](const core::SettingsSnapshot& v) {
                        return static_cast<uint8_t>(v.volume);
                    });
                },
            .random = shuffle_random(std::move(w.random)),
            .now_ms = os::now_ms,
            .on_error = [bus](const std::string& reason,
                              const std::string& item) { publish_error(*bus, reason, item); },
            .mirror = &media_player_mirror,
        });
    if (!player) {
        WLOG_E(kTag, "no memory for the music player; it is off");
        return {};
    }
    if (auto r = player->start(); !r) {
        WLOG_E(kTag, "music player not started: %s", r.error().message.c_str());
        return {};
    }
    return player;
}

core::CellSubscription follow_card(const core::StateCell<core::SdStatus>& sd,
                                   media::MediaPlayer& player) {
    // `changes` moves on every insertion and removal: a move seen while the
    // card was in means it went out, even if it is back by this delivery.
    const core::SdStatus now = sd.get();
    return sd.subscribe(
        [&player, online = now.online, changes = now.changes](const core::SdStatus& s) mutable {
            if (s.changes != changes && online) player.on_card_removed();
            online = s.online;
            changes = s.changes;
        });
}

core::CellSubscription follow_volume(const core::StateCell<core::SettingsSnapshot>& settings,
                                     media::MediaPlayer& player) {
    const auto now = settings.read([](const core::SettingsSnapshot& v) { return v.volume; });
    return settings.subscribe([&player, volume = now](const core::SettingsSnapshot& v) mutable {
        if (v.volume == volume) return;
        volume = v.volume;
        player.on_volume_changed();
    });
}

}  // namespace wisp::app
