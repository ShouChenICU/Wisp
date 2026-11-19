#pragma once
// MediaPlayer: the one system music session (media spec 4, 5.2, 6).
//
// Commands. Every control (play, pause, next, ..., and the notifications
// on_card_removed(), on_file_removed(), forget_scene(), on_volume_changed())
// only posts a command to the player task "wisp.media" and returns; the
// task owns the queue, the decoder and the output, so none of them needs a
// lock. A command the task cannot take (its queue full) is dropped and
// logged.
//
// The task. Per chunk of kChunkFrames frames it decodes, downmixes to mono
// ((L+R)/2) and writes to the AudioOut, opened at the item's sample rate
// with one channel (reopened when the next item's rate differs; an item
// whose rate the output refuses is skipped as unsupported_format). The
// file's bytes come through an ItemReader (wisp/media/item_reader.hpp),
// so the task touches no file and no flash. Between two chunks it takes
// the pending commands and the arbiter's preemption, so either takes
// effect within one chunk.
//
// State. state() is the source of truth (AGENTS.md section 3): status,
// pause reason, the current track, position_s (published when the whole
// second changes, so at most once per second of audio, and on every seek
// or change of item), index, queue length and mode.
//
// Arbiter (spec 5.2). The output is opened only under a Music lease. The
// lease's preempt callback and the resume listener only set an atomic and
// post a command (core::AudioArbiter, Threads). Preempted, the task closes
// the output, drops the lease and is paused/system at the same position;
// the resume listener's command resumes it only if it is still
// paused/system when the task takes it and no user pause was posted after
// the notification, so a user pause is final. A refused Music acquire
// leaves it paused/system until that notification.
//
// Wake locks (spec 6). `cpu` and `audio` (holder core::kMediaWakeLockHolder)
// are held while playing and while paused/system; after a user pause, a
// stop or the end of the queue they are kept `linger_ms` more (the output
// is closed at once), and stop() releases them at once. The output is
// always closed before the audio lock goes, so PowerManager can power it
// down.
//
// Errors (spec 4.5) go to on_error(reason, item) on the task: not_found,
// decode_error, unsupported_format and io_error skip to the next item;
// three failed items in a row stop with too_many_errors; the card gone
// (on_card_removed(), or a read that finds it gone) stops with
// card_removed. The session is not persisted.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/audio_arbiter.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/media/decoder.hpp"
#include "wisp/media/play_queue.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"

namespace wisp::media {

class MediaLibrary;

struct TrackInfo {
    std::string id, title, artist, album;
    uint32_t duration_s = 0;
    bool operator==(const TrackInfo&) const = default;
};

enum class PlayerStatus : uint8_t { Idle, Playing, Paused };
enum class PauseReason : uint8_t { None, User, System };
// "idle" "playing" "paused"
const char* to_string(PlayerStatus s);
// "none" "user" "system"
const char* to_string(PauseReason r);

struct PlayerState {
    PlayerStatus status = PlayerStatus::Idle;
    PauseReason pause_reason = PauseReason::None;
    TrackInfo track;
    uint32_t position_s = 0;
    size_t index = 0, queue_length = 0;
    PlayMode mode = PlayMode::Sequence;
    bool operator==(const PlayerState&) const = default;
};

struct MediaPlayerDeps {
    std::shared_ptr<hal::AudioOut> out;
    core::AudioArbiter& arbiter;
    core::WakeLockRegistry& locks;
    core::EventBus& bus;            // the state cell's
    MediaLibrary* library;          // null: library items fail
    core::SceneFiles* scene_files;  // null: scene items fail
    core::StorageRouter* router;    // the library's reads
    DecoderFactory decoders;
    std::function<uint8_t()> volume;
    std::function<uint32_t()> random;
    std::function<uint64_t()> now_ms = os::now_ms;
    // On the player task; keeps to memory work and posting.
    std::function<void(const std::string& reason, const std::string& item)> on_error;
    // Publishes media.player on each state change; may be null.
    core::StateCell<PlayerState>::Mirror mirror = nullptr;
};

class MediaPlayer {
public:
    struct Config {
        size_t ring_bytes = 64 * 1024;
        size_t read_bytes = 16 * 1024;
        uint32_t linger_ms = 10000;
        // minimp3 keeps about 16 KiB of scratch on the caller's stack
        // (mp3dec_decode_frame), and the device's decoder is a library
        // whose stack use is not documented: twice that leaves room for
        // either plus the task's own frames. In PSRAM (task_config()).
        size_t stack_bytes = 32 * 1024;
        // The scene task runs at 3, the UI at 4 and the speech player at
        // 5; there is no level strictly between the scene task and the
        // UI, so music shares the UI's level and round-robins with it:
        // above the scene task, below speech, and a long frame drawn by
        // the UI cannot starve the output for its whole length.
        int priority = 4;
    };

    // Frames decoded and written at a time: one MPEG Layer III frame.
    static constexpr size_t kChunkFrames = 1152;
    // Failed items in a row that stop playback (spec 4.5).
    static constexpr uint32_t kMaxFailures = 3;

    MediaPlayer(MediaPlayerDeps deps, Config config);
    explicit MediaPlayer(MediaPlayerDeps deps) : MediaPlayer(std::move(deps), Config{}) {}
    ~MediaPlayer();  // stop()

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // "wisp.media", flash_access = false, hot_stack = false.
    static os::TaskConfig task_config(const Config& c);

    // Spawns the task. Busy if started before, NoMemory if it cannot be
    // made. Without an output there is no task (Ok) and every command is
    // ignored.
    Result<void> start();
    // Joins the task: the output closed, the lease returned and the wake
    // locks released before it returns. Idempotent; not from a callback.
    void stop();

    // Replaces the queue and plays from `start` (media spec 4.2).
    PlayQueue::SetResult play(std::vector<std::string> items, size_t start = 0,
                              std::optional<PlayMode> mode = std::nullopt);
    void pause();
    void resume();
    void next();
    void prev();
    void seek(uint32_t s);
    void set_mode(PlayMode mode);
    // Idle, queue cleared.
    void stop_playback();

    // Scene uninstall: drops the items "scene:<id>/...".
    void forget_scene(std::string_view scene_id);
    void on_card_removed();
    // A file removed through the library: the current item skips.
    void on_file_removed(const std::string& path);
    // The volume setting changed: read volume() again.
    void on_volume_changed();

    const core::StateCell<PlayerState>& state() const { return state_; }

private:
    struct Command;
    struct Session;

    bool post(Command&& c);
    void run();

    MediaPlayerDeps deps_;
    Config config_;
    core::StateCell<PlayerState> state_;
    std::unique_ptr<os::Queue<Command>> commands_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<os::Task> task_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> preempted_{false};
    // Moves on every pause(): a resume notification taken after a user
    // pause was posted is stale (see the file header).
    std::atomic<uint32_t> pause_seq_{0};
    bool started_ = false;
};

}  // namespace wisp::media
