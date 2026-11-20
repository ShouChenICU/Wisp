// /media (media spec 3.7): thin adapters over media::MediaLibrary and
// media::MediaPlayer, for paired clients only (the router's token and
// X-Wisp checks; the upload passes stream_refusal() before any byte).
//
// Threads. The server's connection thread never touches the card: every
// library call queues a job on wisp.sd.io through the library (Normal
// lane) and the thread waits for its callback, which shares only a
// reference-counted state with the wait, so a callback that comes after a
// timed-out wait is dropped. A download is one read of at most
// min(net::kStreamChunkBytes, core::kIoChunk) per job. It uses the
// library's own Normal lane: below the player's Interactive refills, so a
// download never starves playback, and above the Bulk scans, so it does
// not wait behind a rescan for more than one tag read. An upload goes
// through a media::LibrarySink, whose push() blocks the connection thread
// while more than LibrarySink::kMaxQueued bytes wait for the card -- the
// back-pressure that keeps a fast client from piling the body up in PSRAM.
//
// Errors are {"error":{"code","message"}} (connectivity spec 6.1) with the
// media spec's wire name as the code and media::http_status() as the
// status (section 8); a 5xx carries a generic message, since the
// service's could name on-device paths.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/media/errors.hpp"
#include "wisp/media/item_reader.hpp"
#include "wisp/media/library.hpp"
#include "wisp/media/library_sink.hpp"
#include "wisp/media/path.hpp"
#include "wisp/media/play_queue.hpp"
#include "wisp/media/player.hpp"
#include "wisp/os/large.hpp"

namespace wisp::admin::detail {

namespace {

constexpr const char* kTag = "admin";

constexpr std::string_view kMediaFilesPath = "/api/v1/media/files/";

// How long a request waits for one library job: a listing, a stat, a
// removal or one chunk of a download.
constexpr uint32_t kCardWaitMs = 5000;
// How long a request retries a full lane (shared with scans, the player
// and downloads into the library), which drains within a few jobs.
constexpr uint32_t kBusyRetryMs = 2000;
constexpr uint32_t kBusyPollMs = 10;
// How long the end of an upload waits for its commit: what push() let
// queue (at most LibrarySink::kMaxQueued) plus the renames.
constexpr uint32_t kCommitWaitMs = 15000;

// One media upload at a time, like the scene upload: each holds a writer,
// PSRAM staging and the Bulk lane.
std::atomic<bool> g_upload_busy{false};

net::HttpResponse media_error(const Error& e) {
    const int status = media::http_status(e);
    const char* code = media::wire_name(e);
    if (status < 500) return net::HttpResponse::error(status, code, e.message);
    std::string_view message = "card read or write failed";
    if (e.code == ErrorCode::StorageUnavailable) message = "no SD card is mounted";
    if (e.code == ErrorCode::Busy) message = "the card is busy; retry later";
    return net::HttpResponse::error(status, code, message);
}

net::HttpResponse library_unavailable() {
    return net::HttpResponse::error(503, "media_unavailable", "no media library on this device");
}

net::HttpResponse player_unavailable() {
    return net::HttpResponse::error(503, "media_unavailable", "no audio output for music");
}

net::HttpResponse invalid_path(std::string_view message) {
    return net::HttpResponse::error(400, "invalid_path", message);
}

// Starts a library call and waits for its callback, which runs on the
// card's worker (or at once on this thread for a refusal). A full lane is
// retried for kBusyRetryMs; a callback that does not come in time is a
// Busy failure, and it is dropped when it comes later.
template <class T>
Result<T> await(const std::function<void(media::MediaLibrary::Done<T>)>& start) {
    struct State {
        os::Semaphore sem{1, 0};
        std::optional<Result<T>> out;
    };
    for (uint32_t waited = 0;; waited += kBusyPollMs) {
        auto s = std::make_shared<State>();
        start([s](Result<T> r) {
            s->out.emplace(std::move(r));
            s->sem.give();
        });
        if (!s->sem.take(kCardWaitMs)) return Result<T>::err(ErrorCode::Busy, "card timeout");
        Result<T> r = std::move(*s->out);
        if (r || r.error().code != ErrorCode::Busy || waited >= kBusyRetryMs) return r;
        os::sleep_ms(kBusyPollMs);
    }
}

// The library path a route's "**" matched, decoded; nullopt for a broken
// escape (the library checks the rest).
std::optional<std::string> route_path(const RequestContext& ctx) {
    return percent_decode(ctx.param);
}

// The value of `name` in a form-encoded query, decoded ('+' is a space);
// "" when absent, nullopt for a broken escape.
std::optional<std::string> query_param(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const size_t eq = pair.find('=');
        if (pair.substr(0, eq) == name) {
            if (eq == std::string_view::npos) return std::string();
            std::string raw(pair.substr(eq + 1));
            std::replace(raw.begin(), raw.end(), '+', ' ');
            return percent_decode(raw);
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return std::string();
}

// A non-negative decimal of at most 9 digits; nullopt otherwise.
std::optional<uint32_t> parse_count(std::string_view s) {
    if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string_view::npos)
        return std::nullopt;
    uint32_t v = 0;
    for (char c : s) v = v * 10 + static_cast<uint32_t>(c - '0');
    return v;
}

void append_int(os::LargeString& out, int64_t v) {
    char num[24];
    std::snprintf(num, sizeof num, "%lld", static_cast<long long>(v));
    out.append(num);
}

// A body written into one PSRAM string: a folder or a page of tracks as a
// Value tree would put its many small nodes in internal RAM (AGENTS.md
// section 8). Keys in the order a Value map gives them.
net::HttpResponse large_json(std::shared_ptr<os::LargeString> body) {
    net::HttpResponse res;
    res.static_body = {reinterpret_cast<const uint8_t*>(body->data()), body->size()};
    res.body_owner = std::move(body);
    return res;
}

std::shared_ptr<os::LargeString> new_body() {
    return std::allocate_shared<os::LargeString>(os::LargeAllocator<os::LargeString>());
}

net::HttpResponse list_files(AdminDeps& deps, const RequestContext& ctx) {
    media::MediaLibrary* lib = deps.media_library;
    if (lib == nullptr) return library_unavailable();
    std::optional<std::string> dir = query_param(ctx.req.query, "dir");
    if (!dir) return invalid_path("broken escape in dir");
    auto listed = await<std::vector<media::MediaEntry>>(
        [lib, d = *dir](media::MediaLibrary::Done<std::vector<media::MediaEntry>> cb) {
            lib->list(d, std::move(cb));
        });
    if (!listed) return media_error(listed.error());
    const std::vector<media::MediaEntry>& entries = listed.value();

    auto body = new_body();
    os::LargeString& out = *body;
    size_t estimate = 64 + dir->size();
    for (const media::MediaEntry& e : entries) estimate += e.name.size() + 64;
    out.reserve(estimate);
    out.append("{\"dir\":");
    append_json_string(out, *dir);
    out.append(",\"entries\":[");
    for (size_t n = 0; n < entries.size(); ++n) {
        const media::MediaEntry& e = entries[n];
        if (n > 0) out.push_back(',');
        out.append(e.dir ? "{\"dir\":true,\"mtime\":" : "{\"dir\":false,\"mtime\":");
        append_int(out, e.mtime);
        out.append(",\"name\":");
        append_json_string(out, e.name);
        out.append(",\"size\":");
        append_int(out, static_cast<int64_t>(e.size));
        out.push_back('}');
    }
    out.append("]}");
    return large_json(std::move(body));
}

// The response type a browser can use for a track or a text file; the
// rest is a plain download.
const char* content_type_for(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    std::string ext(name.substr(dot + 1));
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "wav") return "audio/wav";
    if (ext == "txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// attachment with an ASCII fallback name (anything else, quotes and
// backslashes become '_') and the exact UTF-8 name in RFC 5987 form.
std::string content_disposition(std::string_view name) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string plain;
    std::string encoded;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        const bool ascii = u >= 0x20 && u < 0x7F;
        if (!ascii) {
            // One '_' per character, not per byte.
            if ((u & 0xC0) != 0x80) plain.push_back('_');
        } else {
            plain.push_back(c == '"' || c == '\\' ? '_' : c);
        }
        const bool attr = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                          (u >= '0' && u <= '9') || std::strchr("!#$&+-.^_`|~", c) != nullptr;
        if (attr && u != 0) {
            encoded.push_back(c);
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[u >> 4]);
            encoded.push_back(kHex[u & 0xF]);
        }
    }
    return "attachment; filename=\"" + plain + "\"; filename*=UTF-8''" + encoded;
}

// What a download's stream reads from: in PSRAM with its control block and
// reached through a plain pointer (a lambda that small lives inside
// std::function), so nothing of a download stays in internal RAM. The
// network lock keeps deep standby from turning Wi-Fi off under it. The
// library outlives the server's connections (App stops net first).
struct Download {
    media::MediaLibrary* lib = nullptr;
    os::LargeString path;
    uint64_t offset = 0;
    uint64_t size = 0;
    core::WakeLock lock;
};

net::HttpResponse download_file(AdminDeps& deps, const RequestContext& ctx) {
    media::MediaLibrary* lib = deps.media_library;
    if (lib == nullptr) return library_unavailable();
    std::optional<std::string> path = route_path(ctx);
    if (!path) return invalid_path("broken escape in the path");
    auto entry =
        await<media::MediaEntry>([lib, p = *path](media::MediaLibrary::Done<media::MediaEntry> cb) {
            lib->stat(p, std::move(cb));
        });
    if (!entry) return media_error(entry.error());
    if (entry.value().dir) return invalid_path("a folder cannot be downloaded");

    net::HttpResponse res;
    res.content_type = content_type_for(entry.value().name);
    res.headers.emplace_back("Content-Disposition", content_disposition(entry.value().name));
    // An empty file is an empty body under Content-Length 0: a stream with
    // stream_length 0 would go out chunked instead.
    if (entry.value().size == 0) return res;

    auto dl = std::allocate_shared<Download>(os::LargeAllocator<Download>());
    dl->lib = lib;
    dl->path.assign(path->data(), path->size());
    dl->size = entry.value().size;
    if (deps.wakelocks != nullptr) {
        dl->lock = core::WakeLock(*deps.wakelocks, "system.download", core::WakeLockKind::Network,
                                  "media download");
    }
    res.stream_length = entry.value().size;
    Download* state = dl.get();
    res.stream = [state](std::span<uint8_t> buf) -> Result<size_t> {
        const uint64_t left = state->size - state->offset;
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>({buf.size(), net::kStreamChunkBytes, core::kIoChunk, left}));
        if (want == 0) return Result<size_t>::ok(0);
        media::MediaLibrary* l = state->lib;
        auto data = await<os::LargeBuffer>(
            [l, p = std::string(state->path.data(), state->path.size()), off = state->offset,
             want](media::MediaLibrary::Done<os::LargeBuffer> cb) {
                l->read(p, off, want, std::move(cb));
            });
        if (!data) return Result<size_t>::err(data.error().code, data.error().message);
        const size_t n = std::min(data.value().size(), want);
        std::memcpy(buf.data(), data.value().data(), n);
        state->offset += n;
        return Result<size_t>::ok(n);
    };
    res.body_owner = std::move(dl);
    return res;
}

net::HttpResponse delete_file(AdminDeps& deps, const RequestContext& ctx) {
    media::MediaLibrary* lib = deps.media_library;
    if (lib == nullptr) return library_unavailable();
    std::optional<std::string> path = route_path(ctx);
    if (!path) return invalid_path("broken escape in the path");
    auto r = await<void>(
        [lib, p = *path](media::MediaLibrary::Done<void> cb) { lib->remove(p, std::move(cb)); });
    if (!r) return media_error(r.error());
    return no_content();
}

net::HttpResponse make_folder(AdminDeps& deps, const RequestContext& ctx) {
    media::MediaLibrary* lib = deps.media_library;
    if (lib == nullptr) return library_unavailable();
    std::optional<std::string> path = route_path(ctx);
    if (!path) return invalid_path("broken escape in the path");
    auto r = await<void>(
        [lib, p = *path](media::MediaLibrary::Done<void> cb) { lib->mkdir(p, std::move(cb)); });
    if (!r) return media_error(r.error());
    return ok_json(201);
}

net::HttpResponse list_tracks(AdminDeps& deps, const RequestContext& ctx) {
    media::MediaLibrary* lib = deps.media_library;
    if (lib == nullptr) return library_unavailable();
    media::TrackQuery q;
    std::optional<std::string> text = query_param(ctx.req.query, "q");
    std::optional<std::string> artist = query_param(ctx.req.query, "artist");
    std::optional<std::string> album = query_param(ctx.req.query, "album");
    std::optional<std::string> offset = query_param(ctx.req.query, "offset");
    std::optional<std::string> limit = query_param(ctx.req.query, "limit");
    if (!text || !artist || !album || !offset || !limit) {
        return bad_request("broken escape in the query");
    }
    q.q = std::move(*text);
    q.artist = std::move(*artist);
    q.album = std::move(*album);
    if (!offset->empty()) {
        std::optional<uint32_t> v = parse_count(*offset);
        if (!v) return bad_request("offset must be a non-negative integer");
        q.offset = *v;
    }
    if (!limit->empty()) {
        std::optional<uint32_t> v = parse_count(*limit);
        if (!v) return bad_request("limit must be a non-negative integer");
        q.limit = std::min(*v, media::kMaxTrackPage);
    }
    // Memory only: `done` runs before tracks() returns.
    std::optional<Result<media::TrackPage>> got;
    lib->tracks(std::move(q), [&got](Result<media::TrackPage> r) { got.emplace(std::move(r)); });
    if (!got) return media_error(Error{ErrorCode::IoError, "no answer from the library"});
    if (!*got) return media_error(got->error());
    const media::TrackPage& page = got->value();

    auto body = new_body();
    os::LargeString& out = *body;
    size_t estimate = 64;
    for (const media::Track& t : page.items) {
        estimate += t.id.size() + t.title.size() + t.artist.size() + t.album.size() + 128;
    }
    out.reserve(estimate);
    out.append("{\"items\":[");
    for (size_t n = 0; n < page.items.size(); ++n) {
        const media::Track& t = page.items[n];
        if (n > 0) out.push_back(',');
        out.append("{\"album\":");
        append_json_string(out, t.album);
        out.append(",\"artist\":");
        append_json_string(out, t.artist);
        out.append(",\"duration\":");
        append_int(out, t.duration_s);
        out.append(",\"id\":");
        append_json_string(out, t.id);
        out.append(",\"mtime\":");
        append_int(out, t.mtime);
        out.append(",\"size\":");
        append_int(out, static_cast<int64_t>(t.size));
        out.append(",\"title\":");
        append_json_string(out, t.title);
        out.push_back('}');
    }
    out.append("],\"total\":");
    append_int(out, page.total);
    out.push_back('}');
    return large_json(std::move(body));
}

net::HttpResponse rescan(AdminDeps& deps, const RequestContext&) {
    if (deps.media_library == nullptr) return library_unavailable();
    // Its progress and end show in GET /media/player's "scanning" and
    // "track_count" (the library's state cell).
    deps.media_library->rescan();
    return ok_json(202);
}

// The media.state shape of the agent tools (media spec 7.3), with the
// tags as they are: the page shows them as text.
net::HttpResponse player_state(AdminDeps& deps, const RequestContext&) {
    if (deps.media_player == nullptr) return player_unavailable();
    const media::MediaLibraryState ls = deps.media_library != nullptr
                                            ? deps.media_library->state().get()
                                            : media::MediaLibraryState{};
    const media::PlayerState ps = deps.media_player->state().get();
    Map out;
    out["available"] = Value(ls.available);
    out["scanning"] = Value(ls.scanning);
    out["track_count"] = Value(static_cast<int64_t>(ls.track_count));
    out["status"] = Value(media::to_string(ps.status));
    out["pause_reason"] = Value(media::to_string(ps.pause_reason));
    if (ps.status == media::PlayerStatus::Idle) {
        out["track"] = Value();
    } else {
        out["track"] = Value(Map{{"id", Value(ps.track.id)},
                                 {"title", Value(ps.track.title)},
                                 {"artist", Value(ps.track.artist)},
                                 {"album", Value(ps.track.album)},
                                 {"duration", Value(static_cast<int64_t>(ps.track.duration_s))}});
    }
    out["position"] = Value(static_cast<int64_t>(ps.position_s));
    out["index"] = Value(static_cast<int64_t>(ps.index));
    out["queue_length"] = Value(static_cast<int64_t>(ps.queue_length));
    out["mode"] = Value(media::to_string(ps.mode));
    return net::HttpResponse::json(200, Value(std::move(out)));
}

net::HttpResponse play(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.media_player == nullptr) return player_unavailable();
    const Value* items = ctx.body.find("items");
    if (items == nullptr || items->type() != Value::Type::List || items->as_list().empty()) {
        return bad_request("'items' must be a non-empty list of track ids");
    }
    std::vector<std::string> list;
    list.reserve(items->as_list().size());
    for (const Value& v : items->as_list()) {
        if (v.type() != Value::Type::String) return bad_request("'items' holds a non-string");
        const std::string& s = v.as_string();
        // Library ids only: a scene's own files are the scene's to play.
        Result<std::string> id = media::normalize(s);
        if (std::string_view(s).substr(0, media::kSceneItemPrefix.size()) ==
                media::kSceneItemPrefix ||
            !id) {
            return invalid_path("not a library path");
        }
        list.push_back(std::move(id.value()));
    }
    size_t start = 0;
    if (const Value* st = ctx.body.find("start");
        st != nullptr && st->type() != Value::Type::Null) {
        if (st->type() != Value::Type::Int || st->as_int() < 0) {
            return bad_request("'start' must be a non-negative integer");
        }
        start = static_cast<size_t>(st->as_int());
    }
    std::optional<media::PlayMode> mode;
    if (const Value* m = ctx.body.find("mode"); m != nullptr && m->type() != Value::Type::Null) {
        mode =
            m->type() == Value::Type::String ? media::play_mode_from(m->as_string()) : std::nullopt;
        if (!mode) return bad_request("unknown mode");
    }
    if (deps.media_library == nullptr || !deps.media_library->state().get().available) {
        return media_error(Error{ErrorCode::StorageUnavailable, "no card"});
    }
    const media::PlayQueue::SetResult r = deps.media_player->play(std::move(list), start, mode);
    return net::HttpResponse::json(200, Value(Map{{"truncated", Value(r.truncated)}}));
}

// The controls only post a command to the player's task.
template <void (media::MediaPlayer::*Fn)()>
net::HttpResponse control(AdminDeps& deps, const RequestContext&) {
    if (deps.media_player == nullptr) return player_unavailable();
    (deps.media_player->*Fn)();
    return no_content();
}

net::HttpResponse seek(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.media_player == nullptr) return player_unavailable();
    const Value* s = ctx.body.find("seconds");
    if (s == nullptr || s->type() != Value::Type::Int || s->as_int() < 0 ||
        s->as_int() > UINT32_MAX) {
        return bad_request("'seconds' must be a non-negative integer");
    }
    deps.media_player->seek(static_cast<uint32_t>(s->as_int()));
    return no_content();
}

net::HttpResponse set_mode(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.media_player == nullptr) return player_unavailable();
    const Value* m = ctx.body.find("mode");
    std::optional<media::PlayMode> mode;
    if (m != nullptr && m->type() == Value::Type::String)
        mode = media::play_mode_from(m->as_string());
    if (!mode) return bad_request("'mode' must be sequence, repeat_all, repeat_one or shuffle");
    deps.media_player->set_mode(*mode);
    return no_content();
}

// An upload's body into its LibrarySink. Every callback runs on the
// connection's thread and only copies into the sink (push(), which may
// wait for the card's worker) or queues its end; the writer itself lives
// on wisp.sd.io. A body that does not end in on_end() -- an abort, an
// early refusal, the server dropping the sink -- removes what was
// written and leaves any older file at the path as it was.
class MediaSink final : public net::BodySink {
public:
    // Takes over g_upload_busy, already set by the caller.
    MediaSink(AdminDeps& deps, std::shared_ptr<media::LibrarySink> sink, std::string path,
              uint64_t length)
        : sink_(std::move(sink)), path_(std::move(path)), length_(length) {
        if (deps.wakelocks != nullptr) {
            lock_ = core::WakeLock(*deps.wakelocks, "system.upload", core::WakeLockKind::Network,
                                   "media upload");
        }
    }
    ~MediaSink() override {
        abort_write();
        release();
    }
    MediaSink(const MediaSink&) = delete;
    MediaSink& operator=(const MediaSink&) = delete;

    std::optional<net::HttpResponse> on_data(std::span<const uint8_t> chunk) override {
        if (chunk.size() > length_ - received_) {
            return refuse(bad_request("body longer than its Content-Length"));
        }
        if (!sink_->push(chunk)) {
            std::optional<Error> e = sink_->error();
            return refuse(media_error(e ? *e : Error{ErrorCode::IoError, "write failed"}));
        }
        received_ += chunk.size();
        return std::nullopt;
    }

    net::HttpResponse on_end() override {
        net::HttpResponse res = commit();
        release();
        res.headers.emplace_back("Cache-Control", "no-store");
        return res;
    }

    void on_abort() override {
        abort_write();
        release();
    }

private:
    // The request ends with `res`; the server still drains the rest of the
    // body, which needs neither the slot nor the lock.
    net::HttpResponse refuse(net::HttpResponse res) {
        abort_write();
        release();
        res.headers.emplace_back("Cache-Control", "no-store");
        return res;
    }

    net::HttpResponse commit() {
        if (received_ != length_) {
            abort_write();
            return bad_request("body shorter than its Content-Length");
        }
        struct Wait {
            os::Semaphore sem{1, 0};
            std::optional<Result<uint64_t>> got;
        };
        auto w = std::make_shared<Wait>();
        ended_ = true;
        const bool queued = sink_->finish(true, [w](Result<uint64_t> r) {
            w->got.emplace(std::move(r));
            w->sem.give();
        });
        if (!queued) {
            ended_ = false;
            abort_write();
            return media_error(Error{ErrorCode::Busy, "card queue full"});
        }
        if (!w->sem.take(kCommitWaitMs)) {
            return media_error(Error{ErrorCode::Busy, "card timeout"});
        }
        if (std::optional<Error> e = sink_->error(); e) return media_error(*e);
        if (!*w->got) return media_error(w->got->error());
        WLOG_I(kTag, "media upload of %llu bytes done",
               static_cast<unsigned long long>(w->got->value()));
        return net::HttpResponse::json(
            201, Value(Map{{"path", Value(path_)},
                           {"size", Value(static_cast<int64_t>(w->got->value()))}}));
    }

    // Stops the writes and queues the removal of what was written. Once.
    void abort_write() {
        if (ended_) return;
        ended_ = true;
        sink_->cancel();
        sink_->finish(false, [](Result<uint64_t>) {});
    }

    // Lets the next upload in and lets Wi-Fi go. Idempotent.
    void release() {
        if (holds_slot_) g_upload_busy.store(false);
        holds_slot_ = false;
        lock_.release();
    }

    std::shared_ptr<media::LibrarySink> sink_;
    std::string path_;
    uint64_t length_;
    uint64_t received_ = 0;
    bool ended_ = false;
    bool holds_slot_ = true;
    core::WakeLock lock_;
};

// Opens the sink for `path` on the card's worker and waits for it. A sink
// that arrives after the wait gave up is aborted there.
Result<std::shared_ptr<media::LibrarySink>> open_sink(media::MediaLibrary& lib,
                                                      const std::string& path) {
    using R = Result<std::shared_ptr<media::LibrarySink>>;
    struct Wait {
        os::Mutex mu;
        os::Semaphore sem{1, 0};
        bool abandoned = false;
        std::optional<R> got;
    };
    for (uint32_t waited = 0;; waited += kBusyPollMs) {
        auto w = std::make_shared<Wait>();
        media::LibrarySink::open(lib, path, [w](R r) {
            std::lock_guard<os::Mutex> lock(w->mu);
            if (w->abandoned) {
                // On the worker itself: one try.
                if (r) r.value()->finish(false, [](Result<uint64_t>) {}, 0);
                return;
            }
            w->got.emplace(std::move(r));
            w->sem.give();
        });
        if (!w->sem.take(kCardWaitMs)) {
            std::lock_guard<os::Mutex> lock(w->mu);
            if (!w->got) {
                w->abandoned = true;
                return R::err(ErrorCode::Busy, "card timeout");
            }
        }
        R r = std::move(*w->got);
        if (r || r.error().code != ErrorCode::Busy || waited >= kBusyRetryMs) return r;
        os::sleep_ms(kBusyPollMs);
    }
}

}  // namespace

void add_media_routes(std::vector<Route>& routes) {
    using media::MediaPlayer;
    using net::Method;
    routes.push_back({Method::Get, "/media/files", false, list_files});
    routes.push_back({Method::Get, "/media/files/**", false, download_file});
    routes.push_back({Method::Delete, "/media/files/**", false, delete_file});
    routes.push_back({Method::Post, "/media/folders/**", false, make_folder});
    routes.push_back({Method::Get, "/media/tracks", false, list_tracks});
    routes.push_back({Method::Post, "/media/rescan", false, rescan});
    routes.push_back({Method::Get, "/media/player", false, player_state});
    routes.push_back({Method::Post, "/media/player/play", false, play});
    routes.push_back({Method::Post, "/media/player/pause", false, control<&MediaPlayer::pause>});
    routes.push_back({Method::Post, "/media/player/resume", false, control<&MediaPlayer::resume>});
    routes.push_back({Method::Post, "/media/player/next", false, control<&MediaPlayer::next>});
    routes.push_back({Method::Post, "/media/player/prev", false, control<&MediaPlayer::prev>});
    routes.push_back({Method::Post, "/media/player/seek", false, seek});
    routes.push_back({Method::Post, "/media/player/mode", false, set_mode});
    routes.push_back(
        {Method::Post, "/media/player/stop", false, control<&MediaPlayer::stop_playback>});
}

std::optional<net::StreamStart> media_stream(AdminDeps& deps, const net::HttpRequest& head) {
    if (head.method != net::Method::Put) return std::nullopt;
    std::string_view p = head.path;
    if (p.size() <= kMediaFilesPath.size() ||
        p.substr(0, kMediaFilesPath.size()) != kMediaFilesPath)
        return std::nullopt;
    if (auto refused = stream_refusal(deps, head)) return stream_answer(std::move(*refused));
    if (deps.media_library == nullptr) return stream_answer(library_unavailable());
    std::optional<std::string> path = percent_decode(p.substr(kMediaFilesPath.size()));
    if (!path) return stream_answer(invalid_path("broken escape in the path"));
    Result<std::string> rel = media::normalize_for_write(*path);
    if (!rel) return stream_answer(media_error(rel.error()));
    // The device's server reports a missing Content-Length as 0, so an
    // empty file cannot be told from a chunked body: both are refused.
    if (head.content_length == 0) {
        return stream_answer(net::HttpResponse::error(411, "length_required",
                                                      "upload the file with a Content-Length"));
    }
    if (g_upload_busy.exchange(true)) {
        return stream_answer(
            net::HttpResponse::error(409, "busy", "another media upload is in progress"));
    }
    Result<std::shared_ptr<media::LibrarySink>> sink = open_sink(*deps.media_library, rel.value());
    if (!sink) {
        g_upload_busy.store(false);
        return stream_answer(media_error(sink.error()));
    }
    WLOG_I(kTag, "media upload started");
    return net::StreamStart{
        std::make_unique<MediaSink>(deps, std::move(sink.value()), std::move(rel.value()),
                                    head.content_length),
        {}};
}

}  // namespace wisp::admin::detail
