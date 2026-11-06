// /scenes/{id}/files (scene-device spec 7.2): thin adapters over
// core::SceneFiles. The list carries where the files live (null before the
// first write), what the manifest declares, the quota, the usage and at
// most SceneFilesConfig::max_list names ("truncated" when there were
// more). A download is streamed (HttpResponse::stream) one read of at most
// net::kStreamChunkBytes at a time under the file's size as its
// Content-Length, so a 64 MB file never sits in memory. Nothing here
// deletes a file unless DELETE asks for that one file.
//
// Every SceneFiles call touches flash, so each one runs on the flash
// thread (AdminDeps::worker; spec 3.6 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md) while the
// server's thread waits: a download one read of at most kIoChunk at a
// time in the Bulk lane, so saves queued meanwhile wait for one chunk at
// most, the rest in the Normal lane. A wait that runs out fails the
// request (or ends the download short).

#include <algorithm>
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
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/large.hpp"

namespace wisp::admin::detail {

namespace {

// How long a request waits for the flash thread to run one call: one
// chunk of a download (a 32 KiB read), a listing, a stat or a delete.
constexpr uint32_t kFlashWaitMs = 5000;
// A full lane (an OTA upload and other downloads share Bulk) drains
// within a few chunks.
constexpr uint32_t kBusyRetryMs = 2000;
constexpr uint32_t kBusyPollMs = 10;

// fn() run on the flash thread, or right here without a worker (tests,
// boards without scenes). The job may outlive a timed-out wait, so `fn`
// holds copies of what it needs and its result goes to a shared slot; a
// read's bytes land in the PSRAM buffer SceneFiles::read() returns.
template <typename T, typename Fn>
Result<T> on_flash(core::StorageWorker* worker, Fn fn, core::IoLane lane = core::IoLane::Normal) {
    if (worker == nullptr) return fn();
    auto out = std::make_shared<std::optional<Result<T>>>();
    std::function<void()> job = [out, fn] { out->emplace(fn()); };
    Result<void> r = worker->run_sync(job, kFlashWaitMs, lane);
    for (uint32_t waited = 0; !r && r.error().code == ErrorCode::Busy && waited < kBusyRetryMs;
         waited += kBusyPollMs) {
        os::sleep_ms(kBusyPollMs);
        r = worker->run_sync(job, kFlashWaitMs, lane);
    }
    if (!r) return Result<T>::err(r.error().code, r.error().message);
    return std::move(**out);
}

net::HttpResponse unavailable() {
    return net::HttpResponse::error(503, "unavailable", "scene files are not available");
}

// A SceneFiles error as the spec's codes: bad_name 400, not_found 404,
// unavailable 503 (the card holding them is out); anything else as any
// service error.
net::HttpResponse files_error(const Error& e) {
    const std::string code = core::scene_files_code(e);
    if (code == "bad_name") return net::HttpResponse::error(400, "bad_name", "invalid file name");
    if (code == "not_found") return net::HttpResponse::error(404, "not_found", "no such file");
    if (code == "unavailable") {
        return net::HttpResponse::error(503, "unavailable",
                                        "the scene's files are not available (no SD card?)");
    }
    return service_error(e);
}

// The scene id the first "*" matched, checked and installed; otherwise
// the refusal. `files` first: without it nothing below matters.
std::optional<std::string> installed_scene(AdminDeps& deps, const RequestContext& ctx,
                                           net::HttpResponse& refusal) {
    if (deps.files == nullptr || deps.scenes == nullptr) {
        refusal = unavailable();
        return std::nullopt;
    }
    std::optional<std::string> id = percent_decode(ctx.param);
    if (!id || !core::valid_scene_id(*id)) {
        refusal = bad_request("invalid scene id");
        return std::nullopt;
    }
    auto info = deps.scenes->info(*id);
    if (!info) {
        refusal = service_error(info.error());
        return std::nullopt;
    }
    return id;
}

// The file name the second "*" matched, decoded (SceneFiles checks it).
std::optional<std::string> file_name(const RequestContext& ctx, net::HttpResponse& refusal) {
    std::optional<std::string> name = percent_decode(ctx.param2);
    if (!name) refusal = net::HttpResponse::error(400, "bad_name", "invalid file name");
    return name;
}

// Appends `s` as a JSON string. Names are [A-Za-z0-9._-] (valid_file_name),
// so nothing needs escaping in practice; quotes, backslashes and control
// bytes are escaped all the same, so a name that ever got past that check
// could not break the document.
void append_json_string(os::LargeString& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", u);
            out.append(esc);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void append_int(os::LargeString& out, int64_t v) {
    char num[24];
    std::snprintf(num, sizeof num, "%lld", static_cast<long long>(v));
    out.append(num);
}

// The list is written straight into one PSRAM string rather than built as
// a Value tree and serialized: with max_list (256) entries the tree's many
// small nodes and the serializer's pools would all land in internal RAM
// (AGENTS.md section 8) and could take 100 KB of it. Keys in the order a
// Value map would give them, so the body is what HttpResponse::json made.
net::HttpResponse list_files(AdminDeps& deps, const RequestContext& ctx) {
    net::HttpResponse refusal;
    std::optional<std::string> id = installed_scene(deps, ctx, refusal);
    if (!id) return refusal;
    core::SceneFiles* files = deps.files;
    auto info =
        on_flash<core::SceneFilesInfo>(deps.worker, [files, id = *id] { return files->info(id); });
    if (!info) return files_error(info.error());
    auto listed =
        on_flash<core::SceneFileList>(deps.worker, [files, id = *id] { return files->list(id); });
    if (!listed) return files_error(listed.error());
    const core::SceneFilesInfo& i = info.value();
    const auto& entries = listed.value().entries;

    auto body = std::allocate_shared<os::LargeString>(os::LargeAllocator<os::LargeString>());
    os::LargeString& out = *body;
    size_t estimate = 128;
    for (const core::SceneFileEntry& e : entries) estimate += e.name.size() + 64;
    out.reserve(estimate);
    out.append("{\"files\":[");
    for (size_t n = 0; n < entries.size(); ++n) {
        const core::SceneFileEntry& e = entries[n];
        if (n > 0) out.push_back(',');
        out.append("{\"mtime\":");
        append_int(out, e.mtime);
        out.append(",\"name\":");
        append_json_string(out, e.name);
        out.append(",\"size\":");
        append_int(out, static_cast<int64_t>(e.size));
        out.push_back('}');
    }
    out.append("],\"location\":");
    if (i.location == core::FilesLocation::None) {
        out.append("null");
    } else {
        append_json_string(out, core::to_string(i.location));
    }
    out.append(",\"quota\":");
    append_int(out, static_cast<int64_t>(i.quota));
    out.append(",\"truncated\":");
    out.append(listed.value().truncated ? "true" : "false");
    out.append(",\"used\":");
    append_int(out, static_cast<int64_t>(i.used));
    out.append(",\"want\":");
    append_json_string(out, core::to_string(i.want));
    out.push_back('}');

    net::HttpResponse res;
    res.static_body = {reinterpret_cast<const uint8_t*>(out.data()), out.size()};
    res.body_owner = std::move(body);
    return res;
}

// What a download's stream reads from. In PSRAM with its control block
// (LargeAllocator), and reached from the stream through a plain pointer
// (a lambda that small lives inside std::function, not on the heap), so
// nothing of a download stays in internal RAM; HttpResponse::body_owner
// keeps it alive until the response has been sent. The network lock keeps
// deep standby from turning Wi-Fi off under a long one.
struct Download {
    core::SceneFiles* files = nullptr;
    core::StorageWorker* worker = nullptr;
    os::LargeString scene;
    os::LargeString name;
    uint64_t offset = 0;
    core::WakeLock lock;
};

net::HttpResponse download_file(AdminDeps& deps, const RequestContext& ctx) {
    net::HttpResponse refusal;
    std::optional<std::string> id = installed_scene(deps, ctx, refusal);
    if (!id) return refusal;
    std::optional<std::string> name = file_name(ctx, refusal);
    if (!name) return refusal;
    core::SceneFiles* files = deps.files;
    auto entry = on_flash<core::SceneFileEntry>(
        deps.worker, [files, id = *id, name = *name] { return files->stat(id, name); });
    if (!entry) return files_error(entry.error());

    net::HttpResponse res;
    res.content_type = "application/octet-stream";
    // valid_file_name() allows only [A-Za-z0-9._-]: nothing to quote.
    res.headers.emplace_back("Content-Disposition",
                             "attachment; filename=\"" + std::string(entry.value().name) + "\"");
    // An empty file is an empty body under Content-Length 0: a stream with
    // stream_length 0 would go out chunked instead.
    if (entry.value().size == 0) return res;

    auto dl = std::allocate_shared<Download>(os::LargeAllocator<Download>());
    dl->files = deps.files;
    dl->worker = deps.worker;
    dl->scene.assign(id->data(), id->size());
    // On the card, the spelling on disk.
    dl->name.assign(entry.value().name.data(), entry.value().name.size());
    if (deps.wakelocks != nullptr) {
        dl->lock = core::WakeLock(*deps.wakelocks, "system.download", core::WakeLockKind::Network,
                                  "file download");
    }
    res.stream_length = entry.value().size;
    Download* state = dl.get();
    res.stream = [state](std::span<uint8_t> buf) -> Result<size_t> {
        const size_t want = std::min({buf.size(), net::kStreamChunkBytes, core::kIoChunk});
        core::SceneFiles* files = state->files;
        auto data = on_flash<os::LargeBuffer>(
            state->worker,
            [files, scene = std::string(state->scene.data(), state->scene.size()),
             name = std::string(state->name.data(), state->name.size()), offset = state->offset,
             want] { return files->read(scene, name, offset, want); },
            core::IoLane::Bulk);
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
    net::HttpResponse refusal;
    std::optional<std::string> id = installed_scene(deps, ctx, refusal);
    if (!id) return refusal;
    std::optional<std::string> name = file_name(ctx, refusal);
    if (!name) return refusal;
    core::SceneFiles* files = deps.files;
    Result<void> r = on_flash<void>(
        deps.worker, [files, id = *id, name = *name] { return files->remove(id, name); });
    if (!r) return files_error(r.error());
    return no_content();
}

}  // namespace

void add_scene_file_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/scenes/*/files", false, list_files});
    routes.push_back({Method::Get, "/scenes/*/files/*", false, download_file});
    routes.push_back({Method::Delete, "/scenes/*/files/*", false, delete_file});
}

}  // namespace wisp::admin::detail
