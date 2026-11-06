// GET/POST /system/firmware (spec 6.5): the info route and the streamed
// upload, driven through AdminServer::stream_handler() the way both HTTP
// servers drive it.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_firmware.hpp"
#include "support/fake_http.hpp"
#include "support/fake_settings_backend.hpp"
#include "support/wait.hpp"
#include "wisp/core/cached_settings.hpp"
#include "wisp/core/firmware_update.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/net/http.hpp"
#include "wisp/os/os.hpp"

using wisp::Properties;
using wisp::Value;
using wisp::core::FirmwareUpdateService;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::error_code;
using wisp::test::FakeFirmwareUpdater;
using wisp::test::ImageSpec;
using wisp::test::make_firmware_image;
using wisp::test::make_req;
using wisp::test::response_header;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

constexpr const char* kPath = "/api/v1/system/firmware";

// v[key], failing the test (and yielding Null) when it is missing.
const Value& at(const Value& v, const char* key) {
    static const Value null;
    const Value* f = v.find(key);
    EXPECT_NE(f, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return f != nullptr ? *f : null;
}

struct FirmwareFixture {
    explicit FirmwareFixture(bool with_service = true,
                             wisp::core::StorageWorker* worker = nullptr) {
        if (with_service) {
            FirmwareUpdateService::Deps deps;
            deps.updater = &updater;
            deps.running_version = "0.2.0-test";
            deps.publish = [this](Properties p) { events.push_back(std::move(p)); };
            deps.request_confirm = [this] {
                ++confirm_requests;
                if (on_confirm_request) on_confirm_request();
            };
            service = std::make_unique<FirmwareUpdateService>(std::move(deps), worker);
        }
        f.srv.reset();
        wisp::admin::AdminDeps deps = f.make_deps();
        deps.firmware = service.get();
        f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
        token = f.token();
        stream = f.srv->stream_handler();
    }

    // POST head for an upload of `length` bytes, authenticated and with
    // X-Wisp.
    HttpRequest head(uint64_t length) const {
        HttpRequest req = with_csrf(with_token(make_req(Method::Post, kPath), token));
        req.headers["content-type"] = "application/octet-stream";
        req.content_length = length;
        return req;
    }

    // Feeds `body` the way a server does: 4 KiB pieces until a response.
    HttpResponse send(const HttpRequest& req, std::span<const uint8_t> body) {
        std::optional<wisp::net::StreamStart> start = stream(req);
        EXPECT_TRUE(start.has_value()) << "upload was not streamed";
        if (!start) return {};
        if (!start->sink) return start->response;
        while (!body.empty()) {
            const size_t n = std::min(body.size(), wisp::net::kBodyChunkBytes);
            if (auto early = start->sink->on_data(body.first(n))) return *early;
            body = body.subspan(n);
        }
        return start->sink->on_end();
    }

    HttpResponse upload(const std::vector<uint8_t>& image) {
        return send(head(image.size()), image);
    }

    Value info() {
        HttpResponse res = f.srv->handle(with_token(make_req(Method::Get, kPath), token));
        EXPECT_EQ(res.status, 200) << res.body;
        return body_json(res);
    }

    AdminFixture f;
    FakeFirmwareUpdater updater;
    std::vector<Properties> events;
    int confirm_requests = 0;
    // Runs inside the service's request_confirm, like a screen that
    // answers at once.
    std::function<void()> on_confirm_request;
    std::unique_ptr<FirmwareUpdateService> service;
    std::string token;
    wisp::net::StreamHandler stream;
};

}  // namespace

TEST(AdminFirmware, InfoAndUploadRoutes) {
    FirmwareFixture fx;

    // Info: the running image and its board, nothing pending.
    Value info = fx.info();
    EXPECT_EQ(at(info, "supported").as_bool(), true);
    EXPECT_EQ(at(info, "version").as_string(), "0.2.0-test");
    EXPECT_EQ(at(info, "board").as_string(), "test_board");
    EXPECT_EQ(at(info, "running").as_string(), "ota_0");
    EXPECT_EQ(at(info, "pending").type(), Value::Type::Null);

    // An image for this board goes through: 202 with its version, the
    // device is asked, and the info shows it pending.
    ImageSpec spec;
    spec.version = "0.3.0";
    spec.extra_bytes = 20000;
    const auto image = make_firmware_image(spec);
    HttpResponse res = fx.upload(image);
    EXPECT_EQ(res.status, 202) << res.body;
    EXPECT_EQ(at(body_json(res), "version").as_string(), "0.3.0");
    EXPECT_EQ(response_header(res, "Cache-Control"), "no-store");
    EXPECT_EQ(fx.confirm_requests, 1);
    EXPECT_EQ(fx.updater.written, image);
    EXPECT_EQ(at(fx.info(), "pending").as_string(), "0.3.0");

    // Another upload while that one waits for an answer: 409 busy, read
    // from the head alone.
    std::optional<wisp::net::StreamStart> second = fx.stream(fx.head(image.size()));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->sink, nullptr);
    EXPECT_EQ(second->response.status, 409);
    EXPECT_EQ(error_code(second->response), "busy");

    ASSERT_TRUE(fx.service->decline().ok());
    EXPECT_EQ(at(fx.info(), "pending").type(), Value::Type::Null);

    // Only that one path is streamed; everything else is buffered as usual.
    EXPECT_FALSE(fx.stream(make_req(Method::Get, kPath)).has_value());
    EXPECT_FALSE(fx.stream(make_req(Method::Post, "/api/v1/system/reboot")).has_value());
    EXPECT_FALSE(fx.stream(make_req(Method::Post, "/api/v1/system/firmware/x")).has_value());
}

TEST(AdminFirmware, AcceptedVersionSurvivesAnImmediateAnswer) {
    // Another dialog is open, so the device declines inside end(): the
    // 202 still names the image that was received.
    FirmwareFixture fx;
    fx.on_confirm_request = [&fx] { (void)fx.service->decline(); };
    ImageSpec spec;
    spec.version = "0.3.0";
    HttpResponse res = fx.upload(make_firmware_image(spec));
    EXPECT_EQ(res.status, 202) << res.body;
    EXPECT_EQ(at(body_json(res), "version").as_string(), "0.3.0");
    ASSERT_EQ(fx.events.size(), 2u);
    EXPECT_EQ(fx.events[1].at("state").as_string(), "rejected");
}

TEST(AdminFirmware, UploadPassesTheSameChecksAsTheApi) {
    FirmwareFixture fx;
    const auto image = make_firmware_image();

    // Foreign Host (DNS rebinding).
    HttpRequest req = fx.head(image.size());
    req.headers["host"] = "evil.example";
    HttpResponse res = fx.send(req, image);
    EXPECT_EQ(res.status, 403);
    EXPECT_EQ(error_code(res), "bad_host");

    // No token, a bad token.
    req = with_csrf(make_req(Method::Post, kPath));
    req.content_length = image.size();
    res = fx.send(req, image);
    EXPECT_EQ(res.status, 401);
    req = fx.head(image.size());
    req.headers["cookie"] = "wisp_token=nope";
    EXPECT_EQ(fx.send(req, image).status, 401);

    // No X-Wisp.
    req = with_token(make_req(Method::Post, kPath), fx.token);
    req.content_length = image.size();
    res = fx.send(req, image);
    EXPECT_EQ(res.status, 403);
    EXPECT_EQ(error_code(res), "csrf");

    // No Content-Length.
    res = fx.send(fx.head(0), {});
    EXPECT_EQ(res.status, 411);
    EXPECT_EQ(error_code(res), "length_required");

    // Larger than the slot.
    fx.updater.capacity_bytes = image.size() - 1;
    res = fx.send(fx.head(image.size()), image);
    EXPECT_EQ(res.status, 413);
    EXPECT_EQ(error_code(res), "payload_too_large");
    fx.updater.capacity_bytes = 5 * 1024 * 1024;

    // None of it reached the platform or the device.
    EXPECT_EQ(fx.updater.begins, 0);
    EXPECT_EQ(fx.confirm_requests, 0);
    EXPECT_TRUE(fx.events.empty());
}

TEST(AdminFirmware, ForeignOrBrokenImagesAreRefused) {
    FirmwareFixture fx;

    ImageSpec other;
    other.board = "other_board";
    HttpResponse res = fx.upload(make_firmware_image(other));
    EXPECT_EQ(res.status, 400);
    EXPECT_EQ(error_code(res), "wrong_board");
    EXPECT_EQ(fx.updater.begins, 0);  // refused before writing a byte

    std::vector<uint8_t> junk(9000, 0x5A);
    res = fx.upload(junk);
    EXPECT_EQ(res.status, 400);
    EXPECT_EQ(error_code(res), "invalid_image");

    auto corrupt = make_firmware_image();
    corrupt[corrupt.size() / 2] ^= 0x01;
    res = fx.upload(corrupt);
    EXPECT_EQ(res.status, 400);
    EXPECT_EQ(error_code(res), "invalid_image");

    fx.updater.fail_write = true;
    res = fx.upload(make_firmware_image());
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(error_code(res), "write_failed");
    // A platform failure's own wording stays on the device.
    EXPECT_EQ(res.body.find("fake"), std::string::npos) << res.body;
    fx.updater.fail_write = false;

    // Every failure freed the slot; a good image still goes through.
    EXPECT_EQ(fx.upload(make_firmware_image()).status, 202);
}

TEST(AdminFirmware, DroppedConnectionFreesTheSlot) {
    FirmwareFixture fx;
    const auto image = make_firmware_image();
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.head(image.size()));
        ASSERT_TRUE(start && start->sink);
        EXPECT_FALSE(start->sink->on_data(std::span<const uint8_t>(image).first(4096)));
        start->sink->on_abort();
    }
    ASSERT_FALSE(fx.events.empty());
    EXPECT_EQ(fx.events.back().at("state").as_string(), "failed");
    EXPECT_EQ(fx.events.back().at("error").as_string(), "aborted");

    // A sink dropped without either ending (a server stopping mid-request)
    // gives the slot back too.
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.head(image.size()));
        ASSERT_TRUE(start && start->sink);
    }
    EXPECT_EQ(fx.upload(image).status, 202);
}

// An upload keeps Wi-Fi on and light sleep off until it ends, however it
// ends (spec 4.3).
TEST(AdminFirmware, UploadHoldsNetworkAndCpuLocks) {
    FirmwareFixture fx;
    auto ota_locks = [&fx] {
        std::vector<std::string> kinds;
        for (const auto& l : fx.f.wakelocks.list())
            if (l.holder == "system.ota") kinds.emplace_back(wisp::core::to_string(l.kind));
        std::sort(kinds.begin(), kinds.end());
        return kinds;
    };
    const std::vector<std::string> both = {"cpu", "network"};
    const auto image = make_firmware_image();
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.head(image.size()));
        ASSERT_TRUE(start && start->sink);
        EXPECT_EQ(ota_locks(), both);
        std::span<const uint8_t> body(image);
        while (!body.empty()) {
            const size_t n = std::min(body.size(), wisp::net::kBodyChunkBytes);
            ASSERT_FALSE(start->sink->on_data(body.first(n)));
            body = body.subspan(n);
        }
        EXPECT_EQ(ota_locks(), both);
        EXPECT_EQ(start->sink->on_end().status, 202);
        EXPECT_TRUE(ota_locks().empty());
    }
    ASSERT_TRUE(fx.service->decline().ok());
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.head(image.size()));
        ASSERT_TRUE(start && start->sink);
        EXPECT_EQ(ota_locks(), both);
        start->sink->on_abort();
        EXPECT_TRUE(ota_locks().empty());
    }
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.head(image.size()));
        ASSERT_TRUE(start && start->sink);
        EXPECT_EQ(ota_locks(), both);
    }
    EXPECT_TRUE(ota_locks().empty());
    // A write that fails ends the request from on_data(): the locks go at
    // once, while the server is still draining the rest of the body. The
    // platform sees the image a full chunk at a time, so it takes more
    // than one to fail before the end.
    fx.updater.fail_write = true;
    {
        ImageSpec big;
        big.extra_bytes = 2 * wisp::core::kIoChunk;
        const auto large = make_firmware_image(big);
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.head(large.size()));
        ASSERT_TRUE(start && start->sink);
        std::optional<HttpResponse> early;
        std::span<const uint8_t> body(large);
        while (!early && !body.empty()) {
            const size_t n = std::min(body.size(), wisp::net::kBodyChunkBytes);
            early = start->sink->on_data(body.first(n));
            body = body.subspan(n);
        }
        ASSERT_TRUE(early.has_value());
        EXPECT_EQ(early->status, 500);
        EXPECT_TRUE(ota_locks().empty());
    }
    fx.updater.fail_write = false;
    // A refused upload takes nothing.
    EXPECT_EQ(fx.stream(fx.head(0))->response.status, 411);
    EXPECT_TRUE(ota_locks().empty());
}

TEST(AdminFirmware, WithoutOtaSlots) {
    FirmwareFixture fx(/*with_service=*/false);
    Value info = fx.info();
    EXPECT_EQ(at(info, "supported").as_bool(), false);
    EXPECT_EQ(at(info, "version").as_string(), "0.2.0-test");
    EXPECT_EQ(at(info, "board").as_string(), "sim");
    EXPECT_EQ(at(info, "running").as_string(), "");

    const auto image = make_firmware_image();
    HttpResponse res = fx.upload(image);
    EXPECT_EQ(res.status, 501);
    EXPECT_EQ(error_code(res), "unsupported");
}

TEST(AdminFirmware, BufferedUploadIsRefused) {
    // A server that cannot stream hands the route an ordinary request.
    FirmwareFixture fx;
    HttpRequest req = with_csrf(with_token(make_req(Method::Post, kPath), fx.token));
    HttpResponse res = fx.f.srv->handle(req);
    EXPECT_EQ(res.status, 501);
    EXPECT_EQ(error_code(res), "unsupported");
}

// A settings change made while an upload's chunk waits for the flash
// thread is written first: Normal work overtakes queued Bulk chunks, so
// an OTA never holds up a save by more than the chunk being written.
TEST(AdminFirmware, SettingsWriteDuringUploadIsNotDelayed) {
    wisp::core::StorageWorker worker;
    // The fixture sets up its storage first, as boot does before the flash
    // thread starts.
    FirmwareFixture fx(true, &worker);
    ASSERT_TRUE(worker.start());
    wisp::test::FakePlatformBackend platform;
    wisp::core::CachedSettingsBackend cache(platform, worker, {});
    ASSERT_TRUE(cache.load());

    // Park the flash thread in a Bulk job, so the upload's first chunk
    // queues behind it.
    wisp::os::Semaphore started(1, 0);
    wisp::os::Semaphore release(1, 0);
    // A failed assertion below must not leave the worker parked.
    struct ReleaseOnExit {
        wisp::os::Semaphore& s;
        ~ReleaseOnExit() { s.give(); }
    } guard{release};
    ASSERT_TRUE(worker.submit(
        [&] {
            started.give();
            (void)release.take(wisp::os::kForever);
        },
        wisp::core::IoLane::Bulk));
    ASSERT_TRUE(started.take(5000));

    std::atomic<int> saved_before_begin{-1};
    fx.updater.on_begin = [&] {
        std::lock_guard<std::mutex> lock(platform.m);
        saved_before_begin = platform.batches.empty() ? 0 : 1;
    };
    const auto image = make_firmware_image();
    HttpResponse res;
    std::thread upload([&] { res = fx.upload(image); });
    ASSERT_TRUE(wisp::test::wait_until([&] { return worker.pending() == 2; }, 5000));

    ASSERT_TRUE(cache.set("system", "brightness", Value(int64_t{40})));
    ASSERT_TRUE(wisp::test::wait_until([&] { return worker.pending() == 3; }, 5000));
    release.give();
    upload.join();

    EXPECT_EQ(res.status, 202) << res.body;
    EXPECT_EQ(saved_before_begin.load(), 1);
    EXPECT_EQ(fx.updater.written, image);
    EXPECT_EQ(fx.updater.off_flash_thread.load(), 0);
    ASSERT_TRUE(fx.service->decline().ok());
    worker.stop();
}
