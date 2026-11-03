#include "wisp/core/firmware_update.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "support/fake_firmware.hpp"
#include "wisp/base/value.hpp"

using wisp::Properties;
using wisp::core::FirmwareFailure;
using wisp::core::FirmwareOutcome;
using wisp::core::FirmwareUpdateService;
using wisp::test::FakeFirmwareUpdater;
using wisp::test::ImageSpec;
using wisp::test::make_firmware_image;

namespace {

struct Fixture {
    explicit Fixture(bool with_updater = true, bool can_confirm = true, bool can_restart = true) {
        FirmwareUpdateService::Deps deps;
        deps.updater = with_updater ? &updater : nullptr;
        deps.running_version = "0.1.0-10-g1111111";
        deps.publish = [this](Properties p) { events.push_back(std::move(p)); };
        if (can_confirm) deps.request_confirm = [this] { ++confirm_requests; };
        if (can_restart) deps.restart = [this] { ++restarts; };
        deps.now_ms = [this] { return now; };
        service = std::make_unique<FirmwareUpdateService>(std::move(deps));
    }

    // Streams `image` in 4 KiB pieces; the first failure ends it.
    FirmwareOutcome upload(const std::vector<uint8_t>& image, uint32_t* id_out = nullptr) {
        uint32_t id = 0;
        FirmwareOutcome r = service->begin(image.size(), &id);
        if (id_out != nullptr) *id_out = id;
        if (!r.ok()) return r;
        std::span<const uint8_t> rest(image);
        while (!rest.empty()) {
            const size_t n = std::min<size_t>(rest.size(), 4096);
            r = service->write(id, rest.first(n));
            if (!r.ok()) return r;
            rest = rest.subspan(n);
        }
        return service->end(id);
    }

    std::string state(size_t i) const { return events.at(i).at("state").as_string(); }
    std::string last_state() const { return events.back().at("state").as_string(); }

    FakeFirmwareUpdater updater;
    std::vector<Properties> events;
    int confirm_requests = 0;
    int restarts = 0;
    uint64_t now = 1000;
    std::unique_ptr<FirmwareUpdateService> service;
};

}  // namespace

TEST(FirmwareUpdate, StatusReportsTheRunningImage) {
    Fixture fx;
    auto st = fx.service->status();
    EXPECT_TRUE(st.supported);
    EXPECT_EQ(st.version, "0.1.0-10-g1111111");
    EXPECT_EQ(st.board, "test_board");
    EXPECT_EQ(st.running, "ota_0");
    EXPECT_FALSE(st.pending);

    Fixture none(/*with_updater=*/false);
    st = none.service->status();
    EXPECT_FALSE(st.supported);
    EXPECT_EQ(st.version, "0.1.0-10-g1111111");
    uint32_t id = 0;
    EXPECT_EQ(none.service->begin(4096, &id).failure, FirmwareFailure::Unsupported);
}

TEST(FirmwareUpdate, NoConfirmationNoUpload) {
    Fixture fx(/*with_updater=*/true, /*can_confirm=*/false);
    uint32_t id = 0;
    EXPECT_EQ(fx.service->begin(4096, &id).failure, FirmwareFailure::Unavailable);
    EXPECT_EQ(fx.updater.begins, 0);
}

TEST(FirmwareUpdate, OneUploadAtATime) {
    Fixture fx;
    const auto image = make_firmware_image();
    uint32_t first = 0;
    ASSERT_TRUE(fx.service->begin(image.size(), &first).ok());

    uint32_t second = 0;
    FirmwareOutcome r = fx.service->begin(image.size(), &second);
    EXPECT_EQ(r.failure, FirmwareFailure::Busy);
    EXPECT_STREQ(wisp::core::to_string(r.failure), "busy");
    // The refused request does not disturb the one in progress.
    EXPECT_TRUE(fx.service->write(first, image).ok());
    EXPECT_TRUE(fx.service->end(first).ok());
    EXPECT_EQ(fx.updater.written, image);

    // Still busy while the image waits for the device's answer.
    EXPECT_EQ(fx.service->begin(image.size(), &second).failure, FirmwareFailure::Busy);
    // A stale id cannot touch anything.
    EXPECT_FALSE(fx.service->write(first + 100, image).ok());
    EXPECT_EQ(fx.updater.begins, 1);
}

TEST(FirmwareUpdate, RefusesWhatCannotFit) {
    Fixture fx;
    fx.updater.capacity_bytes = 64 * 1024;
    uint32_t id = 0;
    EXPECT_EQ(fx.service->begin(64 * 1024 + 1, &id).failure, FirmwareFailure::TooLarge);
    EXPECT_EQ(fx.service->begin(100, &id).failure, FirmwareFailure::InvalidImage);
    EXPECT_EQ(fx.updater.begins, 0);
    // Nothing started, so nothing is announced.
    EXPECT_TRUE(fx.events.empty());
}

TEST(FirmwareUpdate, WrongBoardRejectedBeforeWriting) {
    Fixture fx;
    ImageSpec spec;
    spec.board = "another_board";
    spec.version = "0.3.0";
    const auto image = make_firmware_image(spec);

    uint32_t id = 0;
    ASSERT_TRUE(fx.service->begin(image.size(), &id).ok());
    // Pieces smaller than the head: nothing can be decided yet.
    std::span<const uint8_t> all(image);
    EXPECT_TRUE(fx.service->write(id, all.first(100)).ok());
    FirmwareOutcome r = fx.service->write(id, all.subspan(100, 4000));
    EXPECT_EQ(r.failure, FirmwareFailure::WrongBoard);
    EXPECT_STREQ(wisp::core::to_string(r.failure), "wrong_board");
    EXPECT_EQ(fx.updater.writes, 0);
    EXPECT_GE(fx.updater.aborts, 1);
    ASSERT_EQ(fx.events.size(), 1u);
    EXPECT_EQ(fx.state(0), "failed");
    EXPECT_EQ(fx.events[0].at("error").as_string(), "wrong_board");
    EXPECT_EQ(fx.events[0].at("version").as_string(), "0.3.0");
    EXPECT_EQ(fx.confirm_requests, 0);

    // The upload is over: later pieces are refused, and the slot is free.
    EXPECT_FALSE(fx.service->write(id, all.subspan(4100, 100)).ok());
    EXPECT_TRUE(fx.upload(make_firmware_image()).ok());
}

TEST(FirmwareUpdate, NotAnImageRejectedBeforeWriting) {
    Fixture fx;
    std::vector<uint8_t> junk(8192, 0x41);
    FirmwareOutcome r = fx.upload(junk);
    EXPECT_EQ(r.failure, FirmwareFailure::InvalidImage);
    EXPECT_EQ(fx.updater.writes, 0);
    EXPECT_EQ(fx.last_state(), "failed");
    EXPECT_EQ(fx.events.back().at("error").as_string(), "invalid_image");
}

TEST(FirmwareUpdate, CorruptImageFailsVerification) {
    Fixture fx;
    auto image = make_firmware_image();
    image[image.size() - 40] ^= 0x01;
    FirmwareOutcome r = fx.upload(image);
    EXPECT_EQ(r.failure, FirmwareFailure::InvalidImage);
    EXPECT_EQ(fx.updater.finishes, 1);
    EXPECT_EQ(fx.last_state(), "failed");
    EXPECT_FALSE(fx.service->pending());
    EXPECT_EQ(fx.confirm_requests, 0);
    EXPECT_TRUE(fx.upload(make_firmware_image()).ok());
}

TEST(FirmwareUpdate, BodyLongerOrShorterThanAnnouncedFails) {
    Fixture fx;
    const auto image = make_firmware_image();
    uint32_t id = 0;
    ASSERT_TRUE(fx.service->begin(image.size() - 16, &id).ok());
    EXPECT_FALSE(fx.service->write(id, image).ok());
    EXPECT_EQ(fx.last_state(), "failed");

    ASSERT_TRUE(fx.service->begin(image.size(), &id).ok());
    ASSERT_TRUE(fx.service->write(id, std::span<const uint8_t>(image).first(1000)).ok());
    EXPECT_FALSE(fx.service->end(id).ok());
    EXPECT_EQ(fx.last_state(), "failed");
    EXPECT_TRUE(fx.upload(image).ok());
}

TEST(FirmwareUpdate, WriteFailureFreesTheSlot) {
    Fixture fx;
    fx.updater.fail_write = true;
    FirmwareOutcome r = fx.upload(make_firmware_image());
    EXPECT_EQ(r.failure, FirmwareFailure::WriteFailed);
    EXPECT_EQ(fx.last_state(), "failed");
    EXPECT_EQ(fx.events.back().at("error").as_string(), "write_failed");
    fx.updater.fail_write = false;
    EXPECT_TRUE(fx.upload(make_firmware_image()).ok());
}

TEST(FirmwareUpdate, ReceivedAsksTheDevice) {
    Fixture fx;
    ImageSpec spec;
    spec.version = "0.1.0-20-g2222222";
    ASSERT_TRUE(fx.upload(make_firmware_image(spec)).ok());
    ASSERT_EQ(fx.events.size(), 1u);
    EXPECT_EQ(fx.state(0), "received");
    EXPECT_EQ(fx.events[0].at("version").as_string(), "0.1.0-20-g2222222");
    EXPECT_EQ(fx.confirm_requests, 1);

    auto pending = fx.service->pending();
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending->current, "0.1.0-10-g1111111");
    EXPECT_EQ(pending->next, "0.1.0-20-g2222222");
    EXPECT_FALSE(pending->downgrade);
    EXPECT_EQ(fx.service->status().pending, "0.1.0-20-g2222222");
}

TEST(FirmwareUpdate, EndNamesTheReceivedVersion) {
    // The route answers 202 with it; pending() may already be empty by
    // then if the device answered at once.
    Fixture fx;
    ImageSpec spec;
    spec.version = "0.1.0-20-g2222222";
    FirmwareOutcome r = fx.upload(make_firmware_image(spec));
    ASSERT_TRUE(r.ok()) << r.message;
    EXPECT_EQ(r.version, "0.1.0-20-g2222222");
}

TEST(FirmwareUpdate, UnprintableVersionRejectedBeforeWriting) {
    Fixture fx;
    ImageSpec spec;
    spec.version = "1.0.0\nWISP_REBOOT";
    FirmwareOutcome r = fx.upload(make_firmware_image(spec));
    EXPECT_EQ(r.failure, FirmwareFailure::InvalidImage);
    EXPECT_EQ(r.message.find('\n'), std::string::npos) << r.message;
    EXPECT_EQ(fx.updater.begins, 0);
    ASSERT_EQ(fx.events.size(), 1u);
    EXPECT_EQ(fx.state(0), "failed");
    EXPECT_EQ(fx.events[0].at("version").as_string(), "");
    EXPECT_EQ(fx.confirm_requests, 0);
}

TEST(FirmwareUpdate, OlderImageIsMarkedDowngrade) {
    Fixture fx;
    ImageSpec spec;
    spec.version = "0.1.0-3-g0000000";
    ASSERT_TRUE(fx.upload(make_firmware_image(spec)).ok());
    ASSERT_TRUE(fx.service->pending());
    EXPECT_TRUE(fx.service->pending()->downgrade);
}

TEST(FirmwareUpdate, ConfirmActivatesAndReboots) {
    Fixture fx;
    ASSERT_TRUE(fx.upload(make_firmware_image()).ok());
    FirmwareOutcome r = fx.service->confirm();
    EXPECT_TRUE(r.ok()) << r.message;
    EXPECT_EQ(fx.updater.activations, 1);
    EXPECT_EQ(fx.restarts, 1);
    EXPECT_EQ(fx.last_state(), "confirmed");
    EXPECT_FALSE(fx.service->pending());
    // Restarting: no second upload, no second answer.
    uint32_t id = 0;
    EXPECT_EQ(fx.service->begin(4096, &id).failure, FirmwareFailure::Busy);
    EXPECT_EQ(fx.service->confirm().failure, FirmwareFailure::NotPending);
    EXPECT_EQ(fx.restarts, 1);
}

TEST(FirmwareUpdate, ConfirmWithoutRestartFreesTheSlot) {
    // Nothing can restart the device: the activated image boots at the
    // next restart, and the slot must not stay Installing for ever.
    Fixture fx(true, true, false);
    ASSERT_TRUE(fx.upload(make_firmware_image()).ok());
    FirmwareOutcome r = fx.service->confirm();
    EXPECT_TRUE(r.ok()) << r.message;
    EXPECT_EQ(fx.updater.activations, 1);
    EXPECT_EQ(fx.last_state(), "confirmed");
    EXPECT_TRUE(fx.upload(make_firmware_image()).ok());
}

TEST(FirmwareUpdate, FailedActivationReportsAndFreesTheSlot) {
    Fixture fx;
    fx.updater.fail_activate = true;
    ASSERT_TRUE(fx.upload(make_firmware_image()).ok());
    FirmwareOutcome r = fx.service->confirm();
    EXPECT_EQ(r.failure, FirmwareFailure::WriteFailed);
    EXPECT_EQ(fx.restarts, 0);
    EXPECT_EQ(fx.last_state(), "failed");
    fx.updater.fail_activate = false;
    EXPECT_TRUE(fx.upload(make_firmware_image()).ok());
}

TEST(FirmwareUpdate, DeclineOrTimeoutDiscards) {
    Fixture fx;
    // Declined on the device.
    ASSERT_TRUE(fx.upload(make_firmware_image()).ok());
    const int aborts = fx.updater.aborts;
    EXPECT_TRUE(fx.service->decline().ok());
    EXPECT_EQ(fx.last_state(), "rejected");
    EXPECT_EQ(fx.updater.aborts, aborts + 1);
    EXPECT_EQ(fx.updater.activations, 0);
    EXPECT_FALSE(fx.service->pending());
    EXPECT_EQ(fx.service->decline().failure, FirmwareFailure::NotPending);

    // Nobody answered for two minutes.
    ASSERT_TRUE(fx.upload(make_firmware_image()).ok());
    const size_t before = fx.events.size();
    fx.service->tick(fx.now + FirmwareUpdateService::kConfirmTimeoutMs - 1);
    EXPECT_TRUE(fx.service->pending());
    EXPECT_EQ(fx.events.size(), before);
    fx.service->tick(fx.now + FirmwareUpdateService::kConfirmTimeoutMs);
    EXPECT_FALSE(fx.service->pending());
    EXPECT_EQ(fx.last_state(), "rejected");
    EXPECT_EQ(fx.updater.activations, 0);
    EXPECT_EQ(fx.service->confirm().failure, FirmwareFailure::NotPending);

    // And the next upload goes through.
    EXPECT_TRUE(fx.upload(make_firmware_image()).ok());
    EXPECT_TRUE(fx.service->pending());
}

TEST(FirmwareUpdate, AbortedUploadFreesTheSlot) {
    Fixture fx;
    const auto image = make_firmware_image();
    uint32_t id = 0;
    ASSERT_TRUE(fx.service->begin(image.size(), &id).ok());
    ASSERT_TRUE(fx.service->write(id, std::span<const uint8_t>(image).first(2048)).ok());
    fx.service->abort(id);
    EXPECT_EQ(fx.last_state(), "failed");
    EXPECT_EQ(fx.events.back().at("error").as_string(), "aborted");
    EXPECT_GE(fx.updater.aborts, 1);

    // A second abort for the same (now stale) id changes nothing.
    const size_t events = fx.events.size();
    fx.service->abort(id);
    EXPECT_EQ(fx.events.size(), events);

    EXPECT_TRUE(fx.upload(image).ok());
    // Nor can a stale abort cancel the pending image.
    fx.service->abort(id);
    EXPECT_TRUE(fx.service->pending());
}

TEST(FirmwareUpdate, MarkRunningValidReachesThePlatform) {
    Fixture fx;
    fx.service->mark_running_valid();
    EXPECT_EQ(fx.updater.mark_valid_calls, 1);
    Fixture none(/*with_updater=*/false);
    none.service->mark_running_valid();  // nothing to mark; must not crash
}
