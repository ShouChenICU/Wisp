#include "wisp/media/player.hpp"

#include <algorithm>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/media/item_reader.hpp"
#include "wisp/media/library.hpp"
#include "wisp/os/large.hpp"

namespace wisp::media {

namespace {

constexpr const char* kTag = "media";
// Controls are posted, never waited for; a burst of key presses fits.
constexpr size_t kCommandDepth = 32;
// How often an idle task looks at the clock while the wake locks linger.
constexpr uint32_t kLingerPollMs = 20;

const char* reason_of(const Error& e) {
    switch (e.code) {
        case ErrorCode::NotFound:
            return "not_found";
        case ErrorCode::Unsupported:
            return "unsupported_format";
        case ErrorCode::Corrupt:
            return "decode_error";
        default:
            return "io_error";
    }
}

std::string_view base_name(std::string_view item) {
    const size_t slash = item.rfind('/');
    return slash == std::string_view::npos ? item : item.substr(slash + 1);
}

}  // namespace

const char* to_string(PlayerStatus s) {
    switch (s) {
        case PlayerStatus::Idle:
            return "idle";
        case PlayerStatus::Playing:
            return "playing";
        case PlayerStatus::Paused:
            return "paused";
    }
    return "idle";
}

const char* to_string(PauseReason r) {
    switch (r) {
        case PauseReason::None:
            return "none";
        case PauseReason::User:
            return "user";
        case PauseReason::System:
            return "system";
    }
    return "none";
}

struct MediaPlayer::Command {
    enum class Kind : uint8_t {
        None,
        Play,
        Pause,
        Resume,
        ResumeSystem,  // the arbiter's notification; `value` is pause_seq_ then
        Next,
        Prev,
        Seek,
        SetMode,
        Stop,
        ForgetScene,
        CardRemoved,
        FileRemoved,
        Volume,
        Quit,
    };
    Kind kind = Kind::None;
    std::vector<std::string> items;
    size_t start = 0;
    std::optional<PlayMode> mode;
    uint32_t value = 0;
    std::string text;
};

// Everything the task owns. Only the task touches it, so it needs no lock.
struct MediaPlayer::Session {
    explicit Session(MediaPlayer& player) : p(player) {}

    MediaPlayer& p;
    PlayQueue queue;
    std::string item;  // the current item ("" when idle)
    // The decoder reads from the reader, so it goes first.
    std::unique_ptr<ItemReader> reader;
    std::unique_ptr<Decoder> decoder;
    StreamInfo info{};
    uint64_t frames = 0;  // played (or sought to) in the current item
    TrackInfo track;
    PlayerStatus status = PlayerStatus::Idle;
    PauseReason reason = PauseReason::None;
    uint32_t failures = 0;  // failed items in a row
    uint32_t published_pos = 0;

    core::AudioArbiter::Lease lease;
    bool out_open = false;
    uint32_t out_rate = 0;
    core::WakeLock cpu, audio;
    std::optional<uint64_t> linger_until;

    // kChunkFrames interleaved stereo frames; also the probe's buffer.
    os::LargeBuffer pcm;

    enum class Output : uint8_t { Ok, Refused, Failed };

    uint64_t now() const { return p.deps_.now_ms(); }
    int16_t* samples() { return reinterpret_cast<int16_t*>(pcm.data()); }

    uint32_t position() const {
        return info.sample_rate == 0 ? 0 : static_cast<uint32_t>(frames / info.sample_rate);
    }

    void publish() {
        PlayerState s;
        s.status = status;
        s.pause_reason = reason;
        s.track = track;
        s.position_s = published_pos = position();
        s.index = queue.current().value_or(0);
        s.queue_length = queue.size();
        s.mode = queue.mode();
        p.state_.set(s);
    }

    // Wake locks by the rule in the file header.
    void sync_locks() {
        const bool want = status == PlayerStatus::Playing ||
                          (status == PlayerStatus::Paused && reason == PauseReason::System);
        if (want) {
            linger_until.reset();
            if (!cpu.held()) {
                cpu = core::WakeLock(p.deps_.locks, core::kMediaWakeLockHolder,
                                     core::WakeLockKind::Cpu, "music");
            }
            if (!audio.held()) {
                audio = core::WakeLock(p.deps_.locks, core::kMediaWakeLockHolder,
                                       core::WakeLockKind::Audio, "music");
            }
        } else if ((cpu.held() || audio.held()) && !linger_until) {
            linger_until = now() + p.config_.linger_ms;
        }
    }

    void check_linger() {
        if (linger_until && now() >= *linger_until) release_locks();
    }

    // The output first: PowerManager powers it down once the audio lock
    // goes, which fails while it is open.
    void release_locks() {
        close_output();
        linger_until.reset();
        audio.release();
        cpu.release();
    }

    // Closes the output and returns the lease, in that order (the lease
    // says the output is free). No lock of the player's is held here, as
    // releasing a Music lease may wait for its preempt callback.
    void close_output() {
        if (out_open) {
            p.deps_.out->close();
            out_open = false;
        }
        lease.release();
    }

    Output open_output() {
        if (!lease.valid()) {
            close_output();
            // A preempt flag left by a lease already released is stale.
            p.preempted_.store(false);
            lease = p.deps_.arbiter.try_acquire_music([&player = p] { player.preempted_ = true; });
            if (!lease.valid()) return Output::Refused;
        }
        if (out_open && out_rate == info.sample_rate) return Output::Ok;
        if (out_open) {
            p.deps_.out->close();
            out_open = false;
        }
        if (Result<void> r = p.deps_.out->open(hal::AudioFormat{info.sample_rate, 1}); !r) {
            WLOG_W(kTag, "output refused %u Hz: %s", static_cast<unsigned>(info.sample_rate),
                   r.error().message.c_str());
            lease.release();
            return Output::Failed;
        }
        out_open = true;
        out_rate = info.sample_rate;
        if (p.deps_.volume) p.deps_.out->set_volume(p.deps_.volume());
        return Output::Ok;
    }

    void drop_item() {
        decoder.reset();
        reader.reset();
        info = {};
        frames = 0;
    }

    void go_idle() {
        close_output();
        drop_item();
        queue.clear();
        item.clear();
        track = {};
        status = PlayerStatus::Idle;
        reason = PauseReason::None;
        failures = 0;
        sync_locks();
        publish();
    }

    void go_paused(PauseReason why) {
        close_output();
        status = PlayerStatus::Paused;
        reason = why;
        sync_locks();
        publish();
    }

    void report(const char* why, const std::string& what) {
        WLOG_W(kTag, "%s: %s", why, what.c_str());
        if (p.deps_.on_error) p.deps_.on_error(why, what);
    }

    // An item failed. True when playback stopped for it (the card is gone,
    // or too many failures in a row); otherwise the caller skips on.
    bool fail(const Error& e) {
        if (e.code == ErrorCode::StorageUnavailable && !item.empty() &&
            item.substr(0, kSceneItemPrefix.size()) != kSceneItemPrefix) {
            const std::string what = item;
            report("card_removed", what);
            go_idle();
            return true;
        }
        report(reason_of(e), item);
        if (++failures >= kMaxFailures) {
            const std::string what = item;
            report("too_many_errors", what);
            go_idle();
            return true;
        }
        return false;
    }

    // Opens `item`: reader, probe, decoder, track info.
    Result<void> open_item() {
        drop_item();
        ItemReaderDeps d;
        d.library = p.deps_.library;
        d.scene_files = p.deps_.scene_files;
        d.router = p.deps_.router;
        d.ring_bytes = p.config_.ring_bytes;
        d.read_bytes = p.config_.read_bytes;
        d.abort = [&player = p] { return player.quit_.load(); };
        auto rd = ItemReader::open(item, d);
        if (!rd) return Result<void>::err(rd.error().code, rd.error().message);
        reader = std::move(rd.value());

        const size_t want = std::min(kProbeBytes, pcm.size());
        size_t got = 0;
        while (got < want) {
            auto n = reader->read(pcm.data() + got, want - got);
            if (!n) return Result<void>::err(n.error().code, n.error().message);
            if (n.value() == 0) break;
            got += n.value();
        }
        auto fmt = probe({pcm.data(), got});
        if (!fmt) return Result<void>::err(fmt.error().code, fmt.error().message);
        decoder = p.deps_.decoders ? p.deps_.decoders(fmt.value()) : nullptr;
        if (!decoder) return Result<void>::err(ErrorCode::Unsupported, "no decoder");
        WISP_TRY(reader->seek(0));
        auto si = decoder->open(*reader);
        if (!si) return Result<void>::err(si.error().code, si.error().message);
        info = si.value();
        if (info.sample_rate == 0 || info.channels == 0 || info.channels > 2) {
            return Result<void>::err(ErrorCode::Unsupported, "channel layout");
        }

        track = {};
        track.id = item;
        std::optional<Track> t;
        if (p.deps_.library != nullptr &&
            item.substr(0, kSceneItemPrefix.size()) != kSceneItemPrefix) {
            t = p.deps_.library->track(item);
        }
        if (t) {
            track.title = t->title;
            track.artist = t->artist;
            track.album = t->album;
            track.duration_s = t->duration_s;
        } else {
            track.title = std::string(base_name(item));
        }
        if (info.duration_ms != 0) track.duration_s = (info.duration_ms + 500) / 1000;
        return Result<void>::ok();
    }

    // Starts the queue's current item, skipping failed ones. `play` false
    // keeps a paused session paused (at the new item's start).
    void start_current(bool play) {
        for (;;) {
            const std::optional<size_t> cur = queue.current();
            if (!cur) {
                go_idle();
                return;
            }
            item = std::string(queue.item(*cur));
            Result<void> opened = open_item();
            if (!opened && p.quit_.load()) return;
            if (opened && play) {
                switch (open_output()) {
                    case Output::Ok:
                        status = PlayerStatus::Playing;
                        reason = PauseReason::None;
                        break;
                    case Output::Refused:
                        status = PlayerStatus::Paused;
                        reason = PauseReason::System;
                        break;
                    case Output::Failed:
                        opened = Result<void>::err(ErrorCode::Unsupported, "sample rate");
                        break;
                }
            }
            if (opened) {
                sync_locks();
                publish();
                return;
            }
            drop_item();
            if (fail(opened.error())) return;
            if (!queue.next()) {
                go_idle();
                return;
            }
        }
    }

    // resume() and the arbiter's notification.
    void resume_play() {
        if (status != PlayerStatus::Paused || !decoder) return;
        switch (open_output()) {
            case Output::Ok:
                status = PlayerStatus::Playing;
                reason = PauseReason::None;
                sync_locks();
                publish();
                return;
            case Output::Refused:
                // The arbiter notifies again once the higher sound is over.
                go_paused(PauseReason::System);
                return;
            case Output::Failed:
                drop_item();
                if (fail(Error{ErrorCode::Unsupported, "sample rate"})) return;
                if (!queue.next()) {
                    go_idle();
                    return;
                }
                start_current(true);
                return;
        }
    }

    // A skip in the queue keeps the session playing or paused as it was.
    void moved(std::optional<size_t> to) {
        if (!to) {
            go_idle();
            return;
        }
        start_current(status == PlayerStatus::Playing);
    }

    void play_chunk() {
        auto n = decoder->decode(samples(), kChunkFrames);
        if (!n) {
            if (p.quit_.load()) return;
            drop_item();
            if (fail(n.error())) return;
            moved(queue.next());
            return;
        }
        if (n.value() == 0) {
            failures = 0;
            moved(queue.advance_auto());
            return;
        }
        const size_t count = n.value();
        int16_t* pcm16 = samples();
        if (info.channels == 2) {
            for (size_t i = 0; i < count; ++i) {
                pcm16[i] = static_cast<int16_t>(
                    (static_cast<int32_t>(pcm16[2 * i]) + static_cast<int32_t>(pcm16[2 * i + 1])) /
                    2);
            }
        }
        size_t done = 0;
        while (done < count) {
            auto w = p.deps_.out->write(pcm16 + done, count - done);
            if (!w || w.value() == 0) {
                drop_item();
                close_output();
                if (fail(w ? Error{ErrorCode::IoError, "output wrote nothing"} : w.error())) return;
                moved(queue.next());
                return;
            }
            done += w.value();
        }
        frames += count;
        failures = 0;
        if (position() != published_pos) publish();
    }

    void seek(uint32_t s) {
        if (!decoder) return;
        uint64_t ms = uint64_t{s} * 1000;
        const uint64_t dur =
            info.duration_ms != 0 ? info.duration_ms : uint64_t{track.duration_s} * 1000;
        if (dur != 0) ms = std::min(ms, dur);
        if (Result<void> r = decoder->seek_ms(static_cast<uint32_t>(ms)); !r) {
            WLOG_W(kTag, "seek in %s: %s", item.c_str(), r.error().message.c_str());
            return;
        }
        frames = ms * info.sample_rate / 1000;
        publish();
    }

    void forget_scene(const std::string& id) {
        const std::string prefix = std::string(kSceneItemPrefix) + id + "/";
        if (queue.remove_prefix(prefix) == 0) return;
        if (!queue.current()) {
            go_idle();
            return;
        }
        if (item.compare(0, prefix.size(), prefix) == 0) {
            start_current(status == PlayerStatus::Playing);
            return;
        }
        publish();
    }

    void handle(Command& c) {
        using K = Command::Kind;
        switch (c.kind) {
            case K::Play:
                queue.set(std::move(c.items), c.start);
                if (c.mode) queue.set_mode(*c.mode, p.deps_.random);
                failures = 0;
                start_current(true);
                return;
            case K::Pause:
                if (status == PlayerStatus::Playing ||
                    (status == PlayerStatus::Paused && reason == PauseReason::System)) {
                    go_paused(PauseReason::User);
                }
                return;
            case K::Resume:
                resume_play();
                return;
            case K::ResumeSystem:
                if (status == PlayerStatus::Paused && reason == PauseReason::System &&
                    c.value == p.pause_seq_.load()) {
                    resume_play();
                }
                return;
            case K::Next:
                if (status != PlayerStatus::Idle) moved(queue.next());
                return;
            case K::Prev:
                if (status != PlayerStatus::Idle) moved(queue.prev(position()));
                return;
            case K::Seek:
                seek(c.value);
                return;
            case K::SetMode:
                queue.set_mode(*c.mode, p.deps_.random);
                publish();
                return;
            case K::Stop:
                go_idle();
                return;
            case K::ForgetScene:
                if (status != PlayerStatus::Idle) forget_scene(c.text);
                return;
            case K::CardRemoved:
                if (status != PlayerStatus::Idle) {
                    const std::string what = item;
                    report("card_removed", what);
                    go_idle();
                }
                return;
            case K::FileRemoved:
                if (status != PlayerStatus::Idle && c.text == item) {
                    drop_item();
                    moved(queue.next());
                }
                return;
            case K::Volume:
                if (p.deps_.volume) {
                    const uint8_t v = p.deps_.volume();
                    if (out_open) p.deps_.out->set_volume(v);
                }
                return;
            case K::None:
            case K::Quit:
                return;
        }
    }

    void shutdown() {
        drop_item();
        release_locks();
        queue.clear();
        item.clear();
        track = {};
        status = PlayerStatus::Idle;
        reason = PauseReason::None;
        publish();
    }
};

MediaPlayer::MediaPlayer(MediaPlayerDeps deps, Config config)
    : deps_(std::move(deps)), config_(config), state_(deps_.bus, PlayerState{}, deps_.mirror) {}

MediaPlayer::~MediaPlayer() { stop(); }

os::TaskConfig MediaPlayer::task_config(const Config& c) {
    os::TaskConfig t;
    t.name = "wisp.media";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    t.flash_access = false;
    t.hot_stack = false;
    return t;
}

Result<void> MediaPlayer::start() {
    if (started_) return Result<void>::err(ErrorCode::Busy, "started before");
    started_ = true;
    if (!deps_.out) return Result<void>::ok();
    commands_ = std::make_unique<os::Queue<Command>>(kCommandDepth);
    session_ = std::make_unique<Session>(*this);
    session_->pcm =
        os::LargeBuffer::allocate(std::max(kChunkFrames * 2 * sizeof(int16_t), kProbeBytes));
    if (!session_->pcm.ok()) {
        quit_ = true;
        return Result<void>::err(ErrorCode::NoMemory, "media buffers");
    }
    // Only posts (core::AudioArbiter, Threads); the pause sequence taken
    // now tells the task whether a user pause came after it.
    deps_.arbiter.set_resume_listener([this] {
        Command c;
        c.kind = Command::Kind::ResumeSystem;
        c.value = pause_seq_.load();
        if (!commands_->push(std::move(c), 0)) WLOG_W(kTag, "resume notification dropped");
    });
    task_ = os::Task::spawn(task_config(config_), [this] { run(); });
    if (!task_) {
        quit_ = true;
        deps_.arbiter.set_resume_listener(nullptr);
        return Result<void>::err(ErrorCode::NoMemory, "wisp.media task");
    }
    return Result<void>::ok();
}

void MediaPlayer::stop() {
    if (!task_) return;
    deps_.arbiter.set_resume_listener(nullptr);
    quit_ = true;
    Command c;
    c.kind = Command::Kind::Quit;
    commands_->push(std::move(c), os::kForever);
    task_->join();
    task_.reset();
}

bool MediaPlayer::post(Command&& c) {
    // commands_ is made before the task and never replaced, so a control
    // racing stop() at worst queues a command nobody takes.
    if (!commands_ || !session_ || quit_.load()) return false;
    if (!commands_->push(std::move(c), 0)) {
        WLOG_W(kTag, "command dropped: queue full");
        return false;
    }
    return true;
}

void MediaPlayer::run() {
    Session& s = *session_;
    for (;;) {
        uint32_t wait = os::kForever;
        if (s.status == PlayerStatus::Playing) {
            wait = 0;
        } else if (s.linger_until) {
            wait = kLingerPollMs;
        }
        Command c;
        while (commands_->pop(c, wait)) {
            if (c.kind == Command::Kind::Quit || quit_.load()) {
                s.shutdown();
                return;
            }
            s.handle(c);
            c = Command{};
            wait = 0;
        }
        if (quit_.load()) {
            s.shutdown();
            return;
        }
        if (preempted_.exchange(false) && s.status == PlayerStatus::Playing && !s.lease.valid()) {
            s.go_paused(PauseReason::System);
        }
        s.check_linger();
        if (s.status == PlayerStatus::Playing) s.play_chunk();
    }
}

PlayQueue::SetResult MediaPlayer::play(std::vector<std::string> items, size_t start,
                                       std::optional<PlayMode> mode) {
    PlayQueue::SetResult r;
    if (items.size() > kMaxQueue) {
        r.truncated = true;
        items.resize(kMaxQueue);
    }
    Command c;
    c.kind = Command::Kind::Play;
    c.items = std::move(items);
    c.start = start;
    c.mode = mode;
    post(std::move(c));
    return r;
}

namespace {
template <class C>
C simple(typename C::Kind k) {
    C c;
    c.kind = k;
    return c;
}
}  // namespace

void MediaPlayer::pause() {
    pause_seq_.fetch_add(1);
    post(simple<Command>(Command::Kind::Pause));
}

void MediaPlayer::resume() { post(simple<Command>(Command::Kind::Resume)); }
void MediaPlayer::next() { post(simple<Command>(Command::Kind::Next)); }
void MediaPlayer::prev() { post(simple<Command>(Command::Kind::Prev)); }

void MediaPlayer::seek(uint32_t s) {
    Command c = simple<Command>(Command::Kind::Seek);
    c.value = s;
    post(std::move(c));
}

void MediaPlayer::set_mode(PlayMode mode) {
    Command c = simple<Command>(Command::Kind::SetMode);
    c.mode = mode;
    post(std::move(c));
}

void MediaPlayer::stop_playback() { post(simple<Command>(Command::Kind::Stop)); }

void MediaPlayer::forget_scene(std::string_view scene_id) {
    Command c = simple<Command>(Command::Kind::ForgetScene);
    c.text = std::string(scene_id);
    post(std::move(c));
}

void MediaPlayer::on_card_removed() { post(simple<Command>(Command::Kind::CardRemoved)); }

void MediaPlayer::on_file_removed(const std::string& path) {
    Command c = simple<Command>(Command::Kind::FileRemoved);
    c.text = path;
    post(std::move(c));
}

void MediaPlayer::on_volume_changed() { post(simple<Command>(Command::Kind::Volume)); }

}  // namespace wisp::media
