// /scenes/{id}/files (scene-device spec 7.2): thin adapters over
// core::SceneFiles. The list carries where the files live (null before the
// first write), what the manifest declares, the quota, the usage and at
// most SceneFilesConfig::max_list names ("truncated" when there were
// more). A download is streamed (HttpResponse::stream) one read of at most
// net::kStreamChunkBytes at a time under the file's size as its
// Content-Length, so a 64 MB file never sits in memory. Nothing here
// deletes a file unless DELETE asks for that one file.
//
// Every SceneFiles call touches flash: these routes run on the admin
// server's threads, which may (AdminDeps::files).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/large.hpp"

namespace wisp::admin::detail {

namespace {

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

net::HttpResponse list_files(AdminDeps& deps, const RequestContext& ctx) {
    net::HttpResponse refusal;
    std::optional<std::string> id = installed_scene(deps, ctx, refusal);
    if (!id) return refusal;
    auto info = deps.files->info(*id);
    if (!info) return files_error(info.error());
    auto listed = deps.files->list(*id);
    if (!listed) return files_error(listed.error());
    const core::SceneFilesInfo& i = info.value();
    List files;
    files.reserve(listed.value().entries.size());
    for (const core::SceneFileEntry& e : listed.value().entries) {
        files.push_back(Value(Map{
            {"name", Value(e.name)},
            {"size", Value(static_cast<int64_t>(e.size))},
            {"mtime", Value(e.mtime)},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{
                                            {"location", i.location == core::FilesLocation::None
                                                             ? Value()
                                                             : Value(core::to_string(i.location))},
                                            {"want", Value(core::to_string(i.want))},
                                            {"quota", Value(static_cast<int64_t>(i.quota))},
                                            {"used", Value(static_cast<int64_t>(i.used))},
                                            {"truncated", Value(listed.value().truncated)},
                                            {"files", Value(std::move(files))},
                                        }));
}

// What a download's stream reads from. In PSRAM with its control block
// (LargeAllocator), and reached from the stream through a plain pointer
// (a lambda that small lives inside std::function, not on the heap), so
// nothing of a download stays in internal RAM; HttpResponse::body_owner
// keeps it alive until the response has been sent. The network lock keeps
// deep standby from turning Wi-Fi off under a long one.
struct Download {
    core::SceneFiles* files = nullptr;
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
    auto entry = deps.files->stat(*id, *name);
    if (!entry) return files_error(entry.error());

    auto dl = std::allocate_shared<Download>(os::LargeAllocator<Download>());
    dl->files = deps.files;
    dl->scene.assign(id->data(), id->size());
    // On the card, the spelling on disk.
    dl->name.assign(entry.value().name.data(), entry.value().name.size());
    if (deps.wakelocks != nullptr) {
        dl->lock = core::WakeLock(*deps.wakelocks, "system.download", core::WakeLockKind::Network,
                                  "file download");
    }

    net::HttpResponse res;
    res.content_type = "application/octet-stream";
    // valid_file_name() allows only [A-Za-z0-9._-]: nothing to quote.
    res.headers.emplace_back("Content-Disposition",
                             "attachment; filename=\"" + entry.value().name + "\"");
    res.stream_length = entry.value().size;
    Download* state = dl.get();
    res.stream = [state](std::span<uint8_t> buf) -> Result<size_t> {
        const size_t want = std::min(buf.size(), net::kStreamChunkBytes);
        auto data = state->files->read(std::string_view(state->scene.data(), state->scene.size()),
                                       std::string_view(state->name.data(), state->name.size()),
                                       state->offset, want);
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
    Result<void> r = deps.files->remove(*id, *name);
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
