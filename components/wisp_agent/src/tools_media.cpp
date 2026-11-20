// media.tracks, list, read_text, write_text, remove, download, play,
// control and state (media spec 7.3): thin adapters over MediaLibrary and
// MediaPlayer. The agent reads and writes the library without
// confirmation.
//
// Threads. The library's calls queue their card work on wisp.sd.io
// themselves and call back from there, so the file tools run on the agent's
// thread and only wait for that callback (await()). media.download waits
// for a transfer of up to SceneHttp's download deadline, on ToolThread::Io:
// the body goes from the proxy's download worker into a media::LibrarySink,
// whose writer lives on the library's worker, so this task touches no file.
//
// Errors are the media spec's wire names (section 8); a transfer's own
// failure keeps the proxy's code (dns, timeout, ...), as http.fetch does.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tool_io.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/media/errors.hpp"
#include "wisp/media/item_reader.hpp"
#include "wisp/media/library_sink.hpp"
#include "wisp/media/path.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;
using detail::host_of;
using media::MediaLibrary;

// How long a file tool waits for the card; the registered timeout leaves
// room past it.
constexpr uint32_t kCardMs = 10000;
constexpr uint32_t kCardToolMs = 15000;
constexpr uint32_t kPollMs = 20;
constexpr char kCancelled[] = "cancelled";

ToolOutput media_error(const Error& e) {
    if (e.code == ErrorCode::Busy && e.message == kCancelled)
        return tool_error(kCancelled, e.message);
    if (e.code == ErrorCode::Timeout) return tool_error("timeout", e.message);
    return tool_error(media::wire_name(e), e.message);
}

ToolOutput unavailable() { return tool_error("media_unavailable", "no SD card is mounted"); }

// Starts a library call with a callback and waits for it. The callback
// shares only a reference-counted state with this call, so once given up
// on it may still run, on the storage worker, and is dropped.
template <class T>
Result<T> await(const std::function<void(MediaLibrary::Done<T>)>& start, const ToolCtx& ctx) {
    struct State {
        os::Semaphore sem{1, 0};
        std::optional<Result<T>> out;
    };
    auto s = std::make_shared<State>();
    start([s](Result<T> r) {
        s->out.emplace(std::move(r));
        s->sem.give();
    });
    for (uint32_t waited = 0;; waited += kPollMs) {
        if (s->sem.take(kPollMs)) return std::move(*s->out);
        if (ctx.cancelled && ctx.cancelled()) return Result<T>::err(ErrorCode::Busy, kCancelled);
        if (waited >= kCardMs) return Result<T>::err(ErrorCode::Timeout, "card timeout");
    }
}

int64_t int_arg(const Value& a, const char* key, int64_t fallback) {
    const Value* v = a.find(key);
    return v != nullptr && v->type() == Value::Type::Int ? v->as_int() : fallback;
}

// Tags come from the files, which may come from anywhere (a download):
// data, never instructions (agent-core spec 6.3). The id stays bare, since
// media.play takes it back.
Value track_value(const std::string& id, const std::string& title, const std::string& artist,
                  const std::string& album, uint32_t duration_s) {
    return Value(Map{{"id", Value(id)},
                     {"title", Value(external_data("media", title))},
                     {"artist", Value(external_data("media", artist))},
                     {"album", Value(external_data("media", album))},
                     {"duration", Value(static_cast<int64_t>(duration_s))}});
}

// One media.download, shared by the tool, the library's worker and the
// proxy's task.
struct Download {
    // Guards cancelled and sink, and is held across http.submit(), so a
    // cancel either comes before the request (which is then never sent) or
    // reaches it through cancel_scene(). The proxy's done() never takes it:
    // a refused request calls done() inside submit().
    os::Mutex mu;
    bool cancelled = false;
    // Weak: the sink's own callbacks hold this state, and the request and
    // those callbacks are what keep the sink alive.
    std::weak_ptr<media::LibrarySink> sink;
    // Set first thing in the proxy's done(); after cancel_scene() returns
    // it says whether done() ran (it never runs later).
    std::atomic<bool> http_done{false};

    os::Semaphore sem{1, 0};
    // Written once, before sem.give().
    std::string code, message;
    int status = 0;
    uint64_t size = 0;

    void fail(std::string c, std::string m) {
        code = std::move(c);
        message = std::move(m);
        sem.give();
    }
};

struct Media {
    MediaToolDeps deps;

    bool card() const { return deps.library != nullptr && deps.library->state().get().available; }

    ToolOutput tracks(const Value& a) const {
        media::TrackQuery q;
        q.q = arg_str(a, "q");
        q.artist = arg_str(a, "artist");
        q.album = arg_str(a, "album");
        q.offset = static_cast<uint32_t>(int_arg(a, "offset", 0));
        q.limit = static_cast<uint32_t>(int_arg(a, "limit", 50));
        // Memory only: done runs before tracks() returns.
        std::optional<Result<media::TrackPage>> got;
        deps.library->tracks(std::move(q),
                             [&got](Result<media::TrackPage> r) { got.emplace(std::move(r)); });
        if (!got) return tool_error("io_error", "no answer from the library");
        if (!*got) return media_error(got->error());
        List items;
        for (const media::Track& t : got->value().items)
            items.push_back(track_value(t.id, t.title, t.artist, t.album, t.duration_s));
        return done({{"items", Value(std::move(items))},
                     {"total", Value(static_cast<int64_t>(got->value().total))}});
    }

    ToolOutput list(const Value& a, const ToolCtx& ctx) const {
        MediaLibrary* lib = deps.library;
        const std::string dir = arg_str(a, "dir");
        auto r = await<std::vector<media::MediaEntry>>(
            [lib, dir](MediaLibrary::Done<std::vector<media::MediaEntry>> cb) {
                lib->list(dir, std::move(cb));
            },
            ctx);
        if (!r) return media_error(r.error());
        List entries;
        for (const media::MediaEntry& e : r.value()) {
            entries.push_back(Value(Map{{"name", Value(e.name)},
                                        {"dir", Value(e.dir)},
                                        {"size", Value(static_cast<int64_t>(e.size))}}));
        }
        return done({{"entries", Value(std::move(entries))}});
    }

    ToolOutput read_text(const Value& a, const ToolCtx& ctx) const {
        MediaLibrary* lib = deps.library;
        const std::string path = arg_str(a, "path");
        // One byte past the cap tells a file that is longer.
        auto r = await<os::LargeBuffer>(
            [lib, path](MediaLibrary::Done<os::LargeBuffer> cb) {
                lib->read(path, 0, kMediaReadTextMax + 1, std::move(cb));
            },
            ctx);
        if (!r) return media_error(r.error());
        const os::LargeBuffer& buf = r.value();
        std::string_view text(reinterpret_cast<const char*>(buf.data()), buf.size());
        const bool truncated = text.size() > kMediaReadTextMax;
        if (truncated) {
            size_t cut = kMediaReadTextMax;
            // Back to the start of a character the cap splits.
            for (int i = 0;
                 i < 3 && cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80; ++i)
                --cut;
            text = text.substr(0, cut);
        }
        if (!detail::is_text(text))
            return tool_error("unsupported_format", "not a UTF-8 text file");
        // A library file may come from anywhere: data, never instructions
        // (agent-core spec 6.3).
        return done(
            {{"text", Value(external_data("media", text))}, {"truncated", Value(truncated)}});
    }

    ToolOutput write_text(const Value& a, const ToolCtx& ctx) const {
        MediaLibrary* lib = deps.library;
        const std::string path = arg_str(a, "path");
        const std::string& text = arg_str(a, "text");
        // As media.read_text reads them back.
        if (!detail::is_text(text))
            return tool_error("unsupported_format", "not UTF-8 text without control characters");
        os::LargeBuffer data = os::LargeBuffer::allocate(text.size());
        if (!data.ok() && !text.empty()) return tool_error("io_error", "no memory for the text");
        if (!text.empty()) std::copy(text.begin(), text.end(), data.data());
        auto shared = std::make_shared<os::LargeBuffer>(std::move(data));
        auto r = await<void>(
            [lib, path, shared](MediaLibrary::Done<void> cb) {
                lib->write(path, std::move(*shared), std::move(cb));
            },
            ctx);
        if (!r) return media_error(r.error());
        return done();
    }

    ToolOutput remove(const Value& a, const ToolCtx& ctx) const {
        MediaLibrary* lib = deps.library;
        const std::string path = arg_str(a, "path");
        auto r = await<void>(
            [lib, path](MediaLibrary::Done<void> cb) { lib->remove(path, std::move(cb)); }, ctx);
        if (!r) return media_error(r.error());
        return done();
    }

    ToolOutput download(const Value& a, ToolCtx& ctx) const {
        const std::string& url = arg_str(a, "url");
        const std::string& path = arg_str(a, "path");
        // Checked before anything starts, so a bad path costs no request.
        if (!media::normalize_for_write(path))
            return tool_error("invalid_path", "not a writable library path");
        if (!card()) return unavailable();
        net::SceneHttp& http = *deps.http;
        const std::string note_host = host_of(url);
        auto with_note = [&](ToolOutput out) {
            out.note = ui::tr_fill_n(ui::StrId::AgentNoteFetch,
                                     ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh),
                                     {std::string_view(note_host)});
            return out;
        };

        net::SceneHttpRequest req;
        req.principal = net::HttpPrincipal::Agent;
        req.instance = 1;
        req.method = net::Method::Get;
        req.url = url;
        req.download = true;
        // Music hosts commonly answer with a redirect to a CDN.
        req.max_redirects = net::AgentHttpLimits::kMaxRedirects;
        auto shared_req = std::allocate_shared<net::SceneHttpRequest>(
            os::LargeAllocator<net::SceneHttpRequest>(), std::move(req));

        auto d = std::make_shared<Download>();
        // On the library's worker once the writer is open.
        auto opened = [&http, d, shared_req](Result<std::shared_ptr<media::LibrarySink>> r) {
            if (!r) return d->fail(media::wire_name(r.error()), r.error().message);
            std::shared_ptr<media::LibrarySink> sink = std::move(r.value());
            // On the proxy's task once the transfer is over (or inside
            // submit() for a refusal): finish() queues the commit or the
            // abort behind the appends.
            auto finished_http = [d, sink](net::SceneHttpResult res) {
                d->http_done = true;
                const bool ok = res.error.empty() && res.status >= 200 && res.status < 300;
                auto error = std::make_shared<std::string>(std::move(res.error));
                const int status = res.status;
                auto finished = [d, sink, error, ok, status](Result<uint64_t> kept) {
                    d->status = status;
                    if (std::optional<Error> own = sink->error(); own) {
                        return d->fail(media::wire_name(*own), own->message);
                    }
                    if (!error->empty()) return d->fail(*error, *error);
                    if (!kept) return d->fail(media::wire_name(kept.error()), kept.error().message);
                    if (!ok) return d->fail("http_status", "HTTP " + std::to_string(status));
                    d->size = kept.value();
                    d->sem.give();
                };
                if (!sink->finish(ok, std::move(finished))) d->fail("busy", "card queue full");
            };
            std::lock_guard<os::Mutex> lock(d->mu);
            if (d->cancelled) {
                // The tool gave up while this waited for the worker. One
                // try: a refused abort leaves the ".part", which goes at
                // the next listing of its folder (as for a scene's).
                sink->finish(false, [](Result<uint64_t>) {}, 0);
                return;
            }
            d->sink = sink;
            shared_req->on_body = [sink](std::span<const uint8_t> chunk) {
                return sink->push(chunk);
            };
            http.submit(std::move(*shared_req), std::move(finished_http));
        };
        media::LibrarySink::open(*deps.library, path, std::move(opened));

        for (;;) {
            if (d->sem.take(kPollMs)) break;
            if (ctx.cancelled && ctx.cancelled()) {
                std::shared_ptr<media::LibrarySink> sink;
                {
                    std::lock_guard<os::Mutex> lock(d->mu);
                    d->cancelled = true;
                    sink = d->sink.lock();
                }
                if (sink) {
                    http.cancel_scene(std::string(net::kAgentHttpId));
                    // No done() after cancel_scene(): without one this
                    // ends the write, so nothing replaces the target. One
                    // try, as above: a refused abort leaves the ".part"
                    // for the next listing of its folder.
                    if (!d->http_done) {
                        sink->cancel();
                        sink->finish(false, [](Result<uint64_t>) {}, 0);
                    }
                }
                return with_note(tool_error(kCancelled, "the turn was stopped"));
            }
        }
        if (!d->code.empty()) {
            Map extra;
            if (d->code == "http_status") extra["status"] = Value(static_cast<int64_t>(d->status));
            return with_note(tool_error(d->code, d->message, std::move(extra)));
        }
        return with_note(done({{"size", Value(static_cast<int64_t>(d->size))}}));
    }

    ToolOutput play(const Value& a) const {
        const Value* items = a.find("items");
        if (items == nullptr || items->as_list().empty())
            return tool_error("bad_args", "'items' is empty", Map{{"field", "items"}});
        std::vector<std::string> list;
        list.reserve(items->as_list().size());
        for (const Value& v : items->as_list()) {
            const std::string& s = v.as_string();
            // Library ids only: a scene's own files are not the agent's to
            // address.
            Result<std::string> id = media::normalize(s);
            if (std::string_view(s).substr(0, media::kSceneItemPrefix.size()) ==
                    media::kSceneItemPrefix ||
                !id)
                return tool_error("invalid_path", "not a library path: " + s);
            list.push_back(std::move(id.value()));
        }
        if (!card()) return unavailable();
        std::optional<media::PlayMode> mode;
        if (const std::string& m = arg_str(a, "mode"); !m.empty()) mode = media::play_mode_from(m);
        const media::PlayQueue::SetResult r =
            deps.player->play(std::move(list), static_cast<size_t>(int_arg(a, "start", 0)), mode);
        return done({{"truncated", Value(r.truncated)}});
    }

    ToolOutput control(const Value& a) const {
        const std::string& action = arg_str(a, "action");
        media::MediaPlayer& p = *deps.player;
        if (action == "pause") {
            p.pause();
        } else if (action == "resume") {
            p.resume();
        } else if (action == "next") {
            p.next();
        } else if (action == "prev") {
            p.prev();
        } else if (action == "stop") {
            p.stop_playback();
        } else if (action == "seek") {
            const int64_t s = int_arg(a, "seconds", -1);
            if (s < 0)
                return tool_error("bad_args", "seek needs 'seconds'", Map{{"field", "seconds"}});
            p.seek(static_cast<uint32_t>(s));
        } else if (action == "mode") {
            const std::optional<media::PlayMode> m = media::play_mode_from(arg_str(a, "mode"));
            if (!m) return tool_error("bad_args", "mode needs 'mode'", Map{{"field", "mode"}});
            p.set_mode(*m);
        } else {
            return tool_error("bad_args", "unknown action", Map{{"field", "action"}});
        }
        return done();
    }

    ToolOutput state() const {
        Map out;
        const media::MediaLibraryState ls =
            deps.library != nullptr ? deps.library->state().get() : media::MediaLibraryState{};
        out["available"] = Value(ls.available);
        out["scanning"] = Value(ls.scanning);
        out["track_count"] = Value(static_cast<int64_t>(ls.track_count));
        const media::PlayerState ps =
            deps.player != nullptr ? deps.player->state().get() : media::PlayerState{};
        out["status"] = Value(media::to_string(ps.status));
        out["pause_reason"] = Value(media::to_string(ps.pause_reason));
        out["track"] = ps.status == media::PlayerStatus::Idle
                           ? Value()
                           : track_value(ps.track.id, ps.track.title, ps.track.artist,
                                         ps.track.album, ps.track.duration_s);
        out["position"] = Value(static_cast<int64_t>(ps.position_s));
        out["index"] = Value(static_cast<int64_t>(ps.index));
        out["queue_length"] = Value(static_cast<int64_t>(ps.queue_length));
        out["mode"] = Value(media::to_string(ps.mode));
        out["player"] = Value(deps.player != nullptr);
        return done(std::move(out));
    }
};

constexpr char kPathSchema[] =
    R"({"type":"object","properties":{"path":{"type":"string","maxLength":255}},)"
    R"("required":["path"]})";

}  // namespace

Result<void> add_media_tools(ToolRegistry& reg, MediaToolDeps deps) {
    if (deps.library == nullptr && deps.player == nullptr) return Result<void>::ok();
    // Lives as long as the tools, so in PSRAM with the registry.
    auto m = std::allocate_shared<Media>(os::LargeAllocator<Media>(), Media{deps});

    if (deps.library != nullptr) {
        WISP_TRY(reg.add({
            .name = "media.tracks",
            .description =
                "Tracks in the media library's music/ folder (SD card): id (the path, which "
                "media_play takes), title, artist, album and duration in seconds (0 unknown). q "
                "matches title, artist or album (case-insensitive substring); artist and album "
                "match exactly. Pages with offset/limit; total counts every match.",
            .schema_json = R"({"type":"object","properties":{"q":{"type":"string"},)"
                           R"("artist":{"type":"string"},"album":{"type":"string"},)"
                           R"("offset":{"type":"integer","minimum":0},)"
                           R"("limit":{"type":"integer","minimum":1,"maximum":200}}})",
            .thread = ToolThread::Agent,
            .run = [m](const Value& a, ToolCtx&) { return m->tracks(a); },
        }));
        WISP_TRY(reg.add({
            .name = "media.list",
            .description = "Lists a folder of the media library (SD card); no dir is its root, "
                           "which holds music/, download/ and document/. Entries: name, dir, "
                           "size in bytes.",
            .schema_json = R"({"type":"object","properties":{"dir":{"type":"string",)"
                           R"("maxLength":255}}})",
            .thread = ToolThread::Agent,
            .timeout_ms = kCardToolMs,
            .run = [m](const Value& a, ToolCtx& ctx) { return m->list(a, ctx); },
        }));
        WISP_TRY(reg.add({
            .name = "media.read_text",
            .description = "Reads a UTF-8 text file from the media library, at most 32 KiB "
                           "(truncated:true past that). Binary files are refused "
                           "(unsupported_format).",
            .schema_json = kPathSchema,
            .thread = ToolThread::Agent,
            .timeout_ms = kCardToolMs,
            .run = [m](const Value& a, ToolCtx& ctx) { return m->read_text(a, ctx); },
        }));
        WISP_TRY(reg.add({
            .name = "media.write_text",
            .description = "Writes a UTF-8 text file into the media library, replacing it; "
                           "the path lies inside music/, download/ or document/, and missing "
                           "folders are made.",
            .schema_json = R"({"type":"object","properties":{"path":{"type":"string",)"
                           R"("maxLength":255},"text":{"type":"string"}},)"
                           R"("required":["path","text"]})",
            .thread = ToolThread::Agent,
            .timeout_ms = kCardToolMs,
            .run = [m](const Value& a, ToolCtx& ctx) { return m->write_text(a, ctx); },
        }));
        WISP_TRY(reg.add({
            .name = "media.remove",
            .description = "Deletes a file or an empty folder of the media library.",
            .schema_json = kPathSchema,
            .thread = ToolThread::Agent,
            .timeout_ms = kCardToolMs,
            .run = [m](const Value& a, ToolCtx& ctx) { return m->remove(a, ctx); },
        }));
        if (deps.http != nullptr) {
            WISP_TRY(reg.add({
                .name = "media.download",
                .description =
                    "Downloads a URL (http:// or https://, redirects followed) into a media "
                    "library path inside music/, download/ or document/, streamed to the card "
                    "and replacing the file only once complete; at most 5 minutes. Returns the "
                    "size in bytes. A non-2xx answer is http_status with the status.",
                .schema_json = R"({"type":"object","properties":{"url":{"type":"string",)"
                               R"("maxLength":8192},"path":{"type":"string","maxLength":255}},)"
                               R"("required":["url","path"]})",
                .risk = ToolRisk::Note,
                .thread = ToolThread::Io,
                .timeout_ms = kMediaDownloadToolMs,
                .run = [m](const Value& a, ToolCtx& ctx) { return m->download(a, ctx); },
            }));
        }
    }

    if (deps.player != nullptr) {
        WISP_TRY(reg.add({
            .name = "media.play",
            .description = "Replaces the play queue with library track ids (media_tracks' id) "
                           "and plays from index start (0 first). mode: sequence, repeat_all, "
                           "repeat_one or shuffle. A queue past 1000 items is cut "
                           "(truncated:true).",
            .schema_json = R"({"type":"object","properties":{"items":{"type":"array",)"
                           R"("items":{"type":"string","maxLength":255}},)"
                           R"("start":{"type":"integer","minimum":0},)"
                           R"("mode":{"type":"string","enum":["sequence","repeat_all",)"
                           R"("repeat_one","shuffle"]}},"required":["items"]})",
            .thread = ToolThread::Agent,
            .run = [m](const Value& a, ToolCtx&) { return m->play(a); },
        }));
        WISP_TRY(reg.add({
            .name = "media.control",
            .description = "Controls the music player: pause, resume, next, prev, seek (to "
                           "seconds), mode (to mode) or stop.",
            .schema_json = R"({"type":"object","properties":{"action":{"type":"string",)"
                           R"("enum":["pause","resume","next","prev","seek","mode","stop"]},)"
                           R"("seconds":{"type":"integer","minimum":0},)"
                           R"("mode":{"type":"string","enum":["sequence","repeat_all",)"
                           R"("repeat_one","shuffle"]}},"required":["action"]})",
            .thread = ToolThread::Agent,
            .run = [m](const Value& a, ToolCtx&) { return m->control(a); },
        }));
    }

    return reg.add({
        .name = "media.state",
        .description = "The music player's state (status, pause_reason, track, position in "
                       "seconds, index, queue_length, mode; player:false without an audio "
                       "output) and the media library's available (SD card mounted), scanning "
                       "and track_count.",
        .thread = ToolThread::Agent,
        .run = [m](const Value&, ToolCtx&) { return m->state(); },
    });
}

}  // namespace wisp::agent
