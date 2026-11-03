// GET/POST /system/firmware (spec 6.5). The upload is the one streamed
// request: AdminServer::stream_handler() runs the API's checks on its head
// and pipes the body into core::FirmwareUpdateService, which owns every
// decision about the image. These are thin adapters over that service.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/core/firmware_update.hpp"
#include "wisp/core/log.hpp"

namespace wisp::admin::detail {

namespace {

using core::FirmwareFailure;
using core::FirmwareOutcome;
using core::FirmwareUpdateService;

constexpr const char* kTag = "admin";

// API responses are never cached (AdminServer::handle() adds the same).
net::HttpResponse no_store(net::HttpResponse res) {
    res.headers.emplace_back("Cache-Control", "no-store");
    return res;
}

net::HttpResponse failure_response(const FirmwareOutcome& r) {
    const char* code = core::to_string(r.failure);
    switch (r.failure) {
        case FirmwareFailure::Unsupported:
            return net::HttpResponse::error(501, code, r.message);
        case FirmwareFailure::Unavailable:
            return net::HttpResponse::error(503, code, r.message);
        case FirmwareFailure::Busy:
        case FirmwareFailure::NotPending:
            return net::HttpResponse::error(409, code, r.message);
        case FirmwareFailure::TooLarge:
            return net::HttpResponse::error(413, code, r.message);
        case FirmwareFailure::InvalidImage:
        case FirmwareFailure::WrongBoard:
            return net::HttpResponse::error(400, code, r.message);
        case FirmwareFailure::WriteFailed:
        case FirmwareFailure::None:
            break;
    }
    // The platform's own wording can name flash partitions or files.
    return net::HttpResponse::error(500, "write_failed", "could not write the firmware image");
}

net::HttpResponse firmware_info(AdminDeps& deps, const RequestContext&) {
    core::FirmwareStatus st;
    if (deps.firmware != nullptr) {
        st = deps.firmware->status();
    } else {
        st.version = deps.firmware_version;
    }
    if (st.board.empty()) st.board = deps.board;
    return net::HttpResponse::json(200, Value(Map{
                                            {"supported", Value(st.supported)},
                                            {"version", Value(st.version)},
                                            {"board", Value(st.board)},
                                            {"running", Value(st.running)},
                                            {"pending", st.pending ? Value(*st.pending) : Value()},
                                        }));
}

// Reached only through a server that buffers every body (the streamed
// path never gets here): an image never fits the 8 KiB limit anyway.
net::HttpResponse firmware_buffered(AdminDeps&, const RequestContext&) {
    return net::HttpResponse::error(501, "unsupported", "this server cannot receive firmware");
}

// One upload's body, piped into the service. Gives the slot back however
// the request ends, including a sink dropped without on_end()/on_abort().
class FirmwareSink final : public net::BodySink {
public:
    FirmwareSink(FirmwareUpdateService& service, uint32_t id) : service_(service), id_(id) {}
    ~FirmwareSink() override {
        if (!done_) service_.abort(id_);
    }

    std::optional<net::HttpResponse> on_data(std::span<const uint8_t> chunk) override {
        if (done_) return std::nullopt;
        FirmwareOutcome r = service_.write(id_, chunk);
        if (r.ok()) return std::nullopt;
        done_ = true;
        return no_store(failure_response(r));
    }

    net::HttpResponse on_end() override {
        done_ = true;
        FirmwareOutcome r = service_.end(id_);
        if (!r.ok()) return no_store(failure_response(r));
        return no_store(net::HttpResponse::json(202, Value(Map{{"version", Value(r.version)}})));
    }

    void on_abort() override {
        if (done_) return;
        done_ = true;
        service_.abort(id_);
    }

private:
    FirmwareUpdateService& service_;
    const uint32_t id_;
    bool done_ = false;
};

net::StreamStart answer(net::HttpResponse res) {
    return net::StreamStart{nullptr, no_store(std::move(res))};
}

}  // namespace

void add_firmware_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/system/firmware", false, firmware_info});
    routes.push_back({Method::Post, "/system/firmware", false, firmware_buffered});
}

std::optional<net::StreamStart> firmware_stream(AdminDeps& deps, const net::HttpRequest& head) {
    if (head.method != net::Method::Post || head.path != kFirmwarePath) return std::nullopt;
    // The same gate as every API call (AdminServer::handle()); a foreign
    // Host gets no hotspot leniency on a POST.
    if (!host_allowed(head)) {
        return answer(net::HttpResponse::error(403, "bad_host", "unexpected Host header"));
    }
    const std::string token = head.cookie("wisp_token");
    if (token.empty() || !deps.pairing.authenticate(token, deps.now_ms())) {
        return answer(net::HttpResponse::error(401, "unauthorized", "pair this browser first"));
    }
    if (!csrf_ok(head)) {
        return answer(net::HttpResponse::error(403, "csrf", "missing X-Wisp: 1 header"));
    }
    if (deps.firmware == nullptr) {
        return answer(net::HttpResponse::error(501, "unsupported", "this board has no OTA slots"));
    }
    if (head.content_length == 0) {
        return answer(net::HttpResponse::error(411, "length_required",
                                               "upload the image with a Content-Length"));
    }
    if (head.content_length > SIZE_MAX) {
        return answer(failure_response({FirmwareFailure::TooLarge, "image too large"}));
    }
    uint32_t id = 0;
    FirmwareOutcome r = deps.firmware->begin(static_cast<size_t>(head.content_length), &id);
    if (!r.ok()) return answer(failure_response(r));
    WLOG_I(kTag, "firmware upload started");
    return net::StreamStart{std::make_unique<FirmwareSink>(*deps.firmware, id), {}};
}

}  // namespace wisp::admin::detail
