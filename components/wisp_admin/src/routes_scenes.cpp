// /scenes (scene-runtime-core spec 7.1): thin adapters over
// core::SceneRepository (the packages on disk) and core::SceneHost (running
// them). The upload is streamed like the firmware's: AdminServer::
// stream_handler() gates its head, the body is collected in PSRAM, and the
// install runs on the HTTP server thread when it is complete -- that thread
// may wait out SceneHost::check_sources(), which the UI thread and the
// storage worker may not. One upload at a time (409 busy).
//
// Deleting needs the user's yes on the device: the route only asks
// (AdminDeps::request_scene_delete) and answers 202; the outcome arrives as
// a "scene.state" event.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_host.hpp"
#include "wisp/core/scene_install.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/os/large.hpp"

namespace wisp::admin::detail {

namespace {

constexpr const char* kTag = "admin";

// A package holds at most 512 KiB of files (spec 6.2); stored entries plus
// zip headers for its up to 256 files stay well inside this.
constexpr uint64_t kMaxUploadBytes = 576 * 1024;

// One scene upload at a time, whichever server or socket it comes in on:
// each holds up to kMaxUploadBytes of PSRAM until it ends, a handful of
// them could take most of it, and a PSRAM allocation that fails anywhere
// else aborts the device (os::LargeAllocator). Held by the SceneSink.
std::atomic<bool> g_upload_busy{false};

net::HttpResponse unavailable() {
    return net::HttpResponse::error(503, "unavailable", "scenes are not available");
}

net::HttpResponse dev_mode() {
    return net::HttpResponse::error(409, "dev_mode",
                                    "scenes come from a working directory and are read-only");
}

// The value of `name` in an application/x-www-form-urlencoded query, or
// "" when absent. Only for flags whose values never need decoding.
std::string_view query_value(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const size_t eq = pair.find('=');
        if (pair.substr(0, eq) == name) {
            return eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return {};
}

bool with_data(const RequestContext& ctx) { return query_value(ctx.req.query, "data") == "1"; }

// The scene id a route's "*" matched, decoded and checked; 400 otherwise.
std::optional<std::string> scene_id(const RequestContext& ctx, net::HttpResponse& refusal) {
    std::optional<std::string> id = percent_decode(ctx.param);
    if (!id || !core::valid_scene_id(*id)) {
        refusal = bad_request("invalid scene id");
        return std::nullopt;
    }
    return id;
}

// One entry of GET /scenes (also the body of a 201 upload).
Value scene_item(const core::SceneInfo& info, const core::SceneHost* host) {
    const core::SceneManifest& m = info.manifest;
    core::SceneStatus status;
    if (host != nullptr) status = host->status(m.id);
    List hosts;
    hosts.reserve(m.http_hosts.size());
    for (const std::string& h : m.http_hosts) hosts.emplace_back(h);
    return Value(Map{
        {"id", Value(m.id)},
        {"name", Value(m.name)},
        {"version", Value(m.version)},
        {"code_bytes", Value(static_cast<int64_t>(info.code_bytes))},
        {"data_bytes", Value(static_cast<int64_t>(info.data_bytes))},
        {"state", Value(core::to_string(status.state))},
        {"hosts", Value(std::move(hosts))},
        {"error", status.error.empty() ? Value() : Value(status.error)},
    });
}

// 400 invalid_scene for a package the repository or the compiler refused.
// A compile error reads "lib/util.lua:3: ..."; its file and line are also
// given on their own so a client can point at them.
net::HttpResponse invalid_scene(const std::string& message) {
    Map err{{"code", Value("invalid_scene")}, {"message", Value(message)}};
    const size_t lua = message.find(".lua:");
    if (lua != std::string::npos && message.find(' ') > lua) {
        size_t pos = lua + 5;
        int64_t line = 0;
        while (pos < message.size() && message[pos] >= '0' && message[pos] <= '9' &&
               line < 1000000) {
            line = line * 10 + (message[pos] - '0');
            ++pos;
        }
        if (pos > lua + 5 && pos < message.size() && message[pos] == ':') {
            err.emplace("file", Value(message.substr(0, lua + 4)));
            err.emplace("line", Value(line));
        }
    }
    return net::HttpResponse::json(400, Value(Map{{"error", Value(std::move(err))}}));
}

net::HttpResponse list_scenes(AdminDeps& deps, const RequestContext&) {
    if (deps.scenes == nullptr) return unavailable();
    auto listed = deps.scenes->list();
    if (!listed) return service_error(listed.error());
    List scenes;
    scenes.reserve(listed.value().size());
    for (const core::SceneInfo& info : listed.value()) {
        scenes.push_back(scene_item(info, deps.scene_host));
    }
    return net::HttpResponse::json(200, Value(Map{{"scenes", Value(std::move(scenes))}}));
}

// Reached only through a server that buffers every body (the streamed path
// never gets here), and a package never fits the 8 KiB limit anyway.
net::HttpResponse upload_buffered(AdminDeps&, const RequestContext&) {
    return net::HttpResponse::error(501, "unsupported", "this server cannot receive scenes");
}

net::HttpResponse export_scene(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.scenes == nullptr) return unavailable();
    net::HttpResponse refusal;
    std::optional<std::string> id = scene_id(ctx, refusal);
    if (!id) return refusal;
    auto info = deps.scenes->info(*id);
    if (!info) return service_error(info.error());
    auto zip = deps.scenes->export_zip(*id, with_data(ctx));
    if (!zip) return service_error(zip.error());

    // The zip stays in PSRAM; the response points into it and keeps it
    // alive until it has been sent.
    auto owner = std::make_shared<os::LargeBuffer>(std::move(zip.value()));
    net::HttpResponse res;
    res.content_type = "application/zip";
    // The id is [a-z0-9_-] and the version SemVer, so neither needs quoting.
    res.headers.emplace_back("Content-Disposition", "attachment; filename=\"" + *id + "-" +
                                                        info.value().manifest.version + ".zip\"");
    res.static_body = owner->span();
    res.body_owner = std::move(owner);
    return res;
}

using HostCall = Result<void> (*)(core::SceneHost& host, const std::string& id);

// open/close/reload: 404 for a scene that is not installed, then the
// host's own answer; the body is the state the scene is in afterwards.
net::HttpResponse lifecycle(AdminDeps& deps, const RequestContext& ctx, HostCall call) {
    if (deps.scenes == nullptr) return unavailable();
    net::HttpResponse refusal;
    std::optional<std::string> id = scene_id(ctx, refusal);
    if (!id) return refusal;
    if (deps.scene_host == nullptr) return unavailable();
    auto info = deps.scenes->info(*id);
    if (!info) return service_error(info.error());
    Result<void> r = call(*deps.scene_host, *id);
    if (!r) return service_error(r.error());
    const core::SceneStatus status = deps.scene_host->status(*id);
    return net::HttpResponse::json(200,
                                   Value(Map{{"state", Value(core::to_string(status.state))}}));
}

net::HttpResponse open_route(AdminDeps& deps, const RequestContext& ctx) {
    return lifecycle(deps, ctx,
                     [](core::SceneHost& h, const std::string& id) { return h.open(id); });
}

net::HttpResponse close_route(AdminDeps& deps, const RequestContext& ctx) {
    return lifecycle(deps, ctx,
                     [](core::SceneHost& h, const std::string& id) { return h.close(id); });
}

net::HttpResponse reload_route(AdminDeps& deps, const RequestContext& ctx) {
    return lifecycle(deps, ctx,
                     [](core::SceneHost& h, const std::string& id) { return h.reload(id); });
}

net::HttpResponse delete_scene(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.scenes == nullptr) return unavailable();
    net::HttpResponse refusal;
    std::optional<std::string> id = scene_id(ctx, refusal);
    if (!id) return refusal;
    if (deps.scenes->dev()) return dev_mode();
    // 404 exactly when there is nothing to delete; a package whose
    // scene.json no longer validates, or data left behind by one that is
    // gone (with data=1), is still something.
    const bool data = with_data(ctx);
    auto removal = deps.scenes->removal(*id, data);
    if (!removal) return service_error(removal.error());
    if (!deps.request_scene_delete) return unavailable();
    Result<void> r = deps.request_scene_delete(*id, data);
    if (!r) {
        if (r.error().code == ErrorCode::Busy) {
            return net::HttpResponse::error(409, "busy", "the device is showing another dialog");
        }
        if (r.error().code == ErrorCode::Unsupported) return dev_mode();
        return service_error(r.error());
    }
    return net::HttpResponse::json(202, Value(Map{{"pending", Value(true)}}));
}

// One upload's body, collected into a PSRAM buffer of the announced size
// and installed once it is complete.
class SceneSink final : public net::BodySink {
public:
    // Takes over g_upload_busy, already set by the caller.
    SceneSink(AdminDeps& deps, os::LargeBuffer buffer) : deps_(deps), buffer_(std::move(buffer)) {}
    ~SceneSink() override { release(); }
    SceneSink(const SceneSink&) = delete;
    SceneSink& operator=(const SceneSink&) = delete;

    std::optional<net::HttpResponse> on_data(std::span<const uint8_t> chunk) override {
        if (chunk.size() > buffer_.size() - received_) {
            release();
            net::HttpResponse res = bad_request("body longer than its Content-Length");
            res.headers.emplace_back("Cache-Control", "no-store");
            return res;
        }
        std::memcpy(buffer_.data() + received_, chunk.data(), chunk.size());
        received_ += chunk.size();
        return std::nullopt;
    }

    net::HttpResponse on_end() override {
        net::HttpResponse res = install();
        release();
        res.headers.emplace_back("Cache-Control", "no-store");
        return res;
    }

    void on_abort() override { release(); }

private:
    // Frees the body and lets the next upload in. Idempotent.
    void release() {
        buffer_ = os::LargeBuffer();
        if (holds_slot_) g_upload_busy.store(false);
        holds_slot_ = false;
    }

    net::HttpResponse install() {
        if (received_ != buffer_.size()) return bad_request("body shorter than its Content-Length");
        core::SceneHost& host = *deps_.scene_host;
        auto installed = core::install_scene(*deps_.scenes, host, buffer_.span());
        if (!installed) {
            const Error& e = installed.error();
            switch (e.code) {
                case ErrorCode::InvalidArgument:
                case ErrorCode::QuotaExceeded:
                    return invalid_scene(e.message);
                case ErrorCode::Unsupported:
                    return dev_mode();
                default:
                    return service_error(e);
            }
        }
        return net::HttpResponse::json(201, scene_item(installed.value(), &host));
    }

    AdminDeps& deps_;
    os::LargeBuffer buffer_;
    size_t received_ = 0;
    bool holds_slot_ = true;
};

}  // namespace

void add_scene_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/scenes", false, list_scenes});
    routes.push_back({Method::Post, "/scenes", false, upload_buffered});
    routes.push_back({Method::Get, "/scenes/*/export", false, export_scene});
    routes.push_back({Method::Post, "/scenes/*/open", false, open_route});
    routes.push_back({Method::Post, "/scenes/*/close", false, close_route});
    routes.push_back({Method::Post, "/scenes/*/reload", false, reload_route});
    routes.push_back({Method::Delete, "/scenes/*", false, delete_scene});
}

std::optional<net::StreamStart> scene_stream(AdminDeps& deps, const net::HttpRequest& head) {
    if (head.method != net::Method::Post || head.path != kScenesPath) return std::nullopt;
    if (auto refused = stream_refusal(deps, head)) return stream_answer(std::move(*refused));
    if (deps.scenes == nullptr || deps.scene_host == nullptr) return stream_answer(unavailable());
    if (deps.scenes->dev()) return stream_answer(dev_mode());
    if (head.content_length == 0) {
        return stream_answer(net::HttpResponse::error(411, "length_required",
                                                      "upload the zip with a Content-Length"));
    }
    if (head.content_length > kMaxUploadBytes) {
        return stream_answer(
            net::HttpResponse::error(413, "payload_too_large", "a scene zip is at most 576 KiB"));
    }
    if (g_upload_busy.exchange(true)) {
        return stream_answer(
            net::HttpResponse::error(409, "busy", "another scene upload is in progress"));
    }
    // In PSRAM: a whole package must never land in internal RAM.
    os::LargeBuffer buffer = os::LargeBuffer::allocate(static_cast<size_t>(head.content_length));
    if (!buffer.ok()) {
        g_upload_busy.store(false);
        return stream_answer(
            net::HttpResponse::error(503, "no_memory", "no memory for the upload"));
    }
    WLOG_I(kTag, "scene upload started");
    return net::StreamStart{std::make_unique<SceneSink>(deps, std::move(buffer)), {}};
}

}  // namespace wisp::admin::detail
