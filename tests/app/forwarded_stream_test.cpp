// The frames the admin WebSocket sends an "events" client for every topic a
// state cell mirrors (support/forwarded_recorder.hpp), pinned as literal
// golden lists: one for a scripted sequence that moves every such topic at
// least once, one for what boot publishes. Moving a publisher behind a
// cell must leave both byte-identical (state-cells design, section 5).
//
// Ordering. Each step waits until its frames have arrived (and the bus is
// idle) before the next step starts, so frames of different steps never
// interleave. Within a step every pair of frames is ordered by the code:
// either one thread publishes both, in that order, or the second one is
// caused by the first (it is published by work that starts only after the
// first was published). The comment on each step says which. The only
// timer that could add a frame inside the sequence is the Wi-Fi retry after
// a failed attempt (5 s, WifiServiceConfig::backoff_min_ms), which the
// script pre-empts within milliseconds by a direct attempt; time sync
// retries after 30 s, the pairing code lives two minutes, the hotspot's
// idle timeout is ten minutes and the screen never turns off by itself
// (screen_off_s 0).

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "factory_reset.hpp"
#include "storage_io.hpp"
#include "support/fake_devices.hpp"
#include "support/fake_firmware.hpp"
#include "support/fake_http.hpp"
#include "support/forwarded_recorder.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/time_sync.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

using wisp::net::Method;
using wisp::test::count_obj_of_class;
using wisp::test::find_button_with_label;
using wisp::test::ForwardedRecorder;
using wisp::test::ImageSpec;
using wisp::test::make_firmware_image;
using wisp::test::make_req;
using wisp::test::run_on_ui_blocking;
using wisp::test::run_on_ui_thread;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;
namespace topics = wisp::core::topics;

namespace {

// Generous: a step's frames normally arrive within a few ms, or within the
// PowerMonitor's 1 s and the storage refresh's 2 s periods.
constexpr uint32_t kStepMs = 10000;

// Everything the script needs, and nothing that publishes on a timer of
// its own: the screen never turns off by idleness.
UiNetOptions stream_opts() {
    UiNetOptions opts;
    opts.with_sntp = true;
    opts.with_server = true;
    opts.with_firmware = true;
    opts.with_power = true;
    opts.with_sd = true;
    opts.screen_off_s = 0;
    // Two codes are issued in one run (pairing, then the firmware upload).
    opts.pairing_interval_ms = 0;
    return opts;
}

// The frames a step produces: waits until `n` have arrived (or kStepMs),
// then until the bus is idle, and returns every frame recorded, so a
// surplus shows in the comparison as well as a shortfall.
std::vector<std::string> frames(UiNetHarness& h, ForwardedRecorder& rec, size_t n) {
    (void)wait_until([&] { return rec.size() >= n; }, kStepMs);
    EXPECT_TRUE(h.app->bus().wait_idle(2000));
    return rec.take();
}

// Compares the frames the step just run produced with `expected` (a
// braced list in parentheses); a failure names the line of the step.
#define EXPECT_STEP(h, rec, expected)                   \
    do {                                                \
        const std::vector<std::string> want = expected; \
        EXPECT_EQ(frames(h, rec, want.size()), want);   \
    } while (0)

int msgboxes(UiNetHarness& h) {
    return std::stoi(run_on_ui_thread(h.app->ui(), [] {
        return std::to_string(count_obj_of_class(lv_layer_top(), &lv_msgbox_class));
    }));
}

bool click(UiNetHarness& h, const std::string& text) {
    return run_on_ui_thread(h.app->ui(), [&] {
               lv_obj_t* btn = find_button_with_label(lv_layer_top(), text.c_str());
               if (btn == nullptr) return "";
               lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
               return "clicked";
           }) == "clicked";
}

// Uploads `image` through the admin router's streamed route, as the HTTP
// server would (firmware_confirm_test.cpp does the same).
int upload(UiNetHarness& h, const std::string& token, const std::vector<uint8_t>& image) {
    wisp::net::HttpRequest head =
        with_csrf(with_token(make_req(Method::Post, "/api/v1/system/firmware"), token));
    head.content_length = image.size();
    auto stream = h.server->stream_handler();
    if (!stream) return 0;
    auto start = stream(head);
    if (!start) return 0;
    if (!start->sink) return start->response.status;
    std::span<const uint8_t> rest(image);
    while (!rest.empty()) {
        const size_t n = std::min(rest.size(), wisp::net::kBodyChunkBytes);
        if (auto early = start->sink->on_data(rest.first(n))) return early->status;
        rest = rest.subspan(n);
    }
    return start->sink->on_end().status;
}

// The frames are literal, topic names included: a topic constant whose
// value changed is a wire change the golden lists must catch.
//
// The settings payload once the language is English, with the harness's
// screen_off_s 0 and every other key at its default.
constexpr char kSettingsEn[] =
    R"({"payload":{"agent_max_steps":60,"agent_turn_min":15,"brightness":200,)"
    R"("deep_after_s":300,"language":"en","notify_vol":60,)"
    R"("ntp_servers":"pool.ntp.org,ntp.aliyun.com","screen_off_s":0,"time_24h":true,)"
    R"("tts_mute":false,"utc_offset_min":480,"volume":60},"t":"event","topic":"system.settings"})";

constexpr char kWifiConnectingHome[] =
    R"({"payload":{"ip":"","reason":"","rssi":0,"ssid":"Home","ssid_hex":"486f6d65",)"
    R"("state":"connecting"},"t":"event","topic":"net.wifi.state"})";
constexpr char kWifiConnectingAny[] =
    R"({"payload":{"ip":"","reason":"","rssi":0,"ssid":"","ssid_hex":"",)"
    R"("state":"connecting"},"t":"event","topic":"net.wifi.state"})";
constexpr char kWifiConnectedHome[] =
    R"({"payload":{"ip":"192.168.1.50","reason":"","rssi":-50,"ssid":"Home",)"
    R"("ssid_hex":"486f6d65","state":"connected"},"t":"event","topic":"net.wifi.state"})";
constexpr char kTimeSynced[] =
    R"({"payload":{"server":"pool.ntp.org","state":"synced","time":1767225600},)"
    R"("t":"event","topic":"system.time"})";
constexpr char kPairingActive[] =
    R"({"payload":{"active":true,"expires_ms":"<now+ttl>"},"t":"event","topic":"net.pairing.code"})";
constexpr char kPairingInactive[] =
    R"({"payload":{"active":false,"expires_ms":0},"t":"event","topic":"net.pairing.code"})";

using Frames = std::vector<std::string>;

}  // namespace

// The topics the golden lists cover, derived from the registry: a change of
// a kind or a forwarded flag shows here first, so the golden lists below
// can be checked for the topic it adds or drops.
TEST(ForwardedStream, PinsTheMirroredForwardedTopics) {
    EXPECT_EQ(wisp::test::pinned_topics(),
              (std::vector<std::string>{
                  topics::kPowerState, topics::kPowerChargingChanged, topics::kPowerBatteryLow,
                  topics::kStorageSdInserted, topics::kStorageSdRemoved,
                  topics::kSystemFactoryReset, topics::kSystemFirmware, topics::kSystemSettings,
                  topics::kSystemTime, topics::kNetWifiEnabled, topics::kNetWifiState,
                  topics::kNetApState, topics::kNetPairingCode}));
}

// Only a non-zero integer expires_ms is replaced: "no code" (0) and any
// other type stay as published, so a change of either is still seen.
TEST(ForwardedStream, RecorderNormalisesTimeFields) {
    wisp::core::EventBus bus;
    ForwardedRecorder rec(bus);
    ASSERT_TRUE(bus.start());
    bus.publish({topics::kNetPairingCode,
                 "pairing",
                 0,
                 {{"active", wisp::Value(true)}, {"expires_ms", wisp::Value(int64_t{123456})}}});
    bus.publish({topics::kNetPairingCode,
                 "pairing",
                 0,
                 {{"active", wisp::Value(false)}, {"expires_ms", wisp::Value(int64_t{0})}}});
    bus.publish({topics::kNetPairingCode,
                 "pairing",
                 0,
                 {{"active", wisp::Value(true)}, {"expires_ms", wisp::Value("soon")}}});
    // Not pinned: forwarded, but no cell mirrors it.
    bus.publish({topics::kPowerWakelock, "app", 0, {}});
    // Not pinned: a mirror the WebSocket does not forward.
    bus.publish({topics::kAgentState, "agent", 0, {}});
    ASSERT_TRUE(bus.wait_idle(2000));
    EXPECT_EQ(rec.take(), (Frames{kPairingActive, kPairingInactive,
                                  R"({"payload":{"active":true,"expires_ms":"soon"},"t":"event",)"
                                  R"("topic":"net.pairing.code"})"}));
    bus.stop();
}

TEST(ForwardedStream, ScriptedSequenceIsUnchanged) {
    UiNetHarness h(stream_opts());
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(h.app->bus().wait_idle(2000));
    ForwardedRecorder rec(h.app->bus());
    wisp::app::PowerManager* pm = h.app->power_manager();
    wisp::net::WifiService* wifi = h.app->wifi();
    wisp::net::PairingService* pairing = h.app->pairing();
    wisp::net::TimeSyncService* time = h.app->time_sync();
    ASSERT_NE(pm, nullptr);
    ASSERT_NE(wifi, nullptr);
    ASSERT_NE(pairing, nullptr);
    ASSERT_NE(time, nullptr);
    // Nothing between boot and the first step (no saved network: Wi-Fi
    // stays off; the power sample and the SD probe find nothing new).
    EXPECT_STEP(h, rec, Frames{});

    // Power level, PowerManager::publish_state() on the UI thread, one
    // transition after the other.
    run_on_ui_blocking(h.app->ui(), [&] { pm->enter_standby(); });
    EXPECT_STEP(h, rec,
                (Frames{R"({"payload":{"state":"standby"},"t":"event","topic":"power.state"})"}));
    run_on_ui_blocking(h.app->ui(), [&] { pm->exit_standby(); });
    EXPECT_STEP(h, rec,
                (Frames{R"({"payload":{"state":"active"},"t":"event","topic":"power.state"})"}));

    // Settings, published by App's on_changed hook inside set() on this
    // thread. A batch that changes nothing still publishes; a rejected one
    // does not.
    ASSERT_TRUE(h.app->system_settings().set({{"language", wisp::Value("en")}}));
    ASSERT_TRUE(h.app->system_settings().set({{"language", wisp::Value("en")}}));
    EXPECT_FALSE(h.app->system_settings().set({{"brightness", wisp::Value(int64_t{300})}}));
    EXPECT_STEP(h, rec, (Frames{kSettingsEn, kSettingsEn}));

    // A failed attempt: "connecting" from add_network() on this thread,
    // then the failure from the net task's tick, which only attempts after
    // add_network() scheduled it.
    ASSERT_TRUE(wifi->add_network("Home", "wrong-password", wisp::os::now_ms()));
    EXPECT_STEP(h, rec,
                (Frames{kWifiConnectingHome,
                        R"({"payload":{"ip":"","reason":"auth_failed","rssi":0,"ssid":"Home",)"
                        R"("ssid_hex":"486f6d65","state":"backoff"},"t":"event",)"
                        R"("topic":"net.wifi.state"})"}));

    // The right password: "connecting" (this thread), "connected" (the net
    // task's tick), then the time sync the new link starts (the ntp task,
    // which the net task wakes only after publishing "connected").
    ASSERT_TRUE(wifi->add_network("Home", "password1", wisp::os::now_ms()));
    EXPECT_STEP(h, rec, (Frames{kWifiConnectingHome, kWifiConnectedHome, kTimeSynced}));

    // A failed time sync: every server down; the last one tried is named.
    h.sntp->set_down("pool.ntp.org", true);
    h.sntp->set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(time->sync_now().has_value());
    EXPECT_STEP(h, rec,
                (Frames{R"({"payload":{"server":"ntp.aliyun.com","state":"failed"},"t":"event",)"
                        R"("topic":"system.time"})"}));
    h.sntp->all_up();

    // The hotspot, opened and closed on this thread.
    ASSERT_TRUE(wifi->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(wifi->stop_provisioning());
    EXPECT_STEP(
        h, rec,
        (Frames{
            R"({"payload":{"active":true,"clients":0,"ssid":"Wisp-3F2A"},"t":"event","topic":"net.ap.state"})",
            R"({"payload":{"active":false,"clients":0,"ssid":"Wisp-3F2A"},"t":"event","topic":"net.ap.state"})"}));

    // Wi-Fi off: set_enabled() publishes the state, then the flag.
    ASSERT_TRUE(wifi->set_enabled(false, wisp::os::now_ms()));
    EXPECT_STEP(
        h, rec,
        (Frames{R"({"payload":{"ip":"","reason":"","rssi":0,"ssid":"","ssid_hex":"",)"
                R"("state":"off"},"t":"event","topic":"net.wifi.state"})",
                R"({"payload":{"enabled":false},"t":"event","topic":"net.wifi.enabled"})"}));

    // Wi-Fi on: the state and the flag from set_enabled() on this thread,
    // then the scan-and-connect of the net task and the time sync, as above.
    ASSERT_TRUE(wifi->set_enabled(true, wisp::os::now_ms()));
    EXPECT_STEP(h, rec,
                (Frames{kWifiConnectingAny,
                        R"({"payload":{"enabled":true},"t":"event","topic":"net.wifi.enabled"})",
                        kWifiConnectedHome, kTimeSynced}));

    // A pairing code shown, then dismissed on the device (this thread).
    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    pairing->cancel_code();
    EXPECT_STEP(h, rec, (Frames{kPairingActive, kPairingInactive}));

    // The SD card out, then in: StorageManager::refresh() on the flash
    // thread, queued here or by the 2 s timer, whichever comes first.
    h.sd->online_flag.store(false);
    (void)wisp::app::queue_storage_refresh(*h.app);
    EXPECT_STEP(h, rec, (Frames{R"({"payload":{},"t":"event","topic":"storage.sd.removed"})"}));
    h.sd->online_flag.store(true);
    (void)wisp::app::queue_storage_refresh(*h.app);
    EXPECT_STEP(h, rec, (Frames{R"({"payload":{},"t":"event","topic":"storage.sd.inserted"})"}));

    // The power supply, sampled by PowerMonitor's 1 s timer. The first
    // sample only records, so the changes come after it. One sample
    // publishes the charging change before the low-battery event.
    ASSERT_TRUE(wait_until([&] { return h.power->status_calls.load() > 0; }, kStepMs));
    h.power->set_status({.battery_percent = 80, .charging = true, .usb_present = true});
    EXPECT_STEP(h, rec,
                (Frames{R"({"payload":{"battery_percent":80,"charging":true,"usb_present":true},)"
                        R"("t":"event","topic":"power.charging.changed"})"}));
    h.power->set_status({.battery_percent = 10, .charging = false, .usb_present = false});
    EXPECT_STEP(
        h, rec,
        (Frames{R"({"payload":{"battery_percent":10,"charging":false,"usb_present":false},)"
                R"("t":"event","topic":"power.charging.changed"})",
                R"({"payload":{"battery_percent":10},"t":"event","topic":"power.battery.low"})"}));

    // A factory reset requested (this thread), then refused on the screen:
    // "cancelled" comes from the dialog's answer on the UI thread, which
    // exists only after "requested" went out.
    wisp::app::request_factory_reset(*h.app);
    EXPECT_STEP(
        h, rec,
        (Frames{
            R"({"payload":{"state":"requested"},"t":"event","topic":"system.factory_reset"})"}));
    ASSERT_TRUE(wait_until([&] { return msgboxes(h) == 1; }, kStepMs));
    ASSERT_TRUE(click(h, wisp::ui::tr(wisp::ui::StrId::No, wisp::ui::Lang::En)));
    EXPECT_STEP(
        h, rec,
        (Frames{
            R"({"payload":{"state":"cancelled"},"t":"event","topic":"system.factory_reset"})"}));

    // A firmware upload: the browser pairs (a code issued and used up on
    // this thread), the image is received (this thread), then refused on
    // the screen (the UI thread, once the dialog is up).
    ASSERT_TRUE(wait_until([&] { return msgboxes(h) == 0; }, kStepMs));
    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    const auto code = pairing->active_code(wisp::os::now_ms());
    ASSERT_TRUE(code.has_value());
    auto token = pairing->confirm(*code, wisp::net::ClientInfo{}, wisp::os::now_ms());
    ASSERT_TRUE(token);
    ImageSpec spec;
    spec.version = "0.3.0";
    EXPECT_EQ(upload(h, token.value(), make_firmware_image(spec)), 202);
    EXPECT_STEP(
        h, rec,
        (Frames{
            kPairingActive, kPairingInactive,
            R"({"payload":{"state":"received","version":"0.3.0"},"t":"event","topic":"system.firmware"})"}));
    ASSERT_TRUE(wait_until([&] { return msgboxes(h) == 1; }, kStepMs));
    ASSERT_TRUE(click(h, wisp::ui::tr(wisp::ui::StrId::No, wisp::ui::Lang::En)));
    EXPECT_STEP(
        h, rec,
        (Frames{
            R"({"payload":{"state":"rejected","version":"0.3.0"},"t":"event","topic":"system.firmware"})"}));
}

// What boot publishes with a saved network in range: the Wi-Fi state from
// WifiService::init() (inside App::start()), then the connection made by
// the net task's first attempt, then the time sync it starts. Nothing for
// the power level, the settings, the power supply (the first sample only
// records) or the SD card (StorageManager::init() publishes nothing), and
// no net.wifi.enabled: the flag did not change.
TEST(ForwardedStream, BootPublishesTheSame) {
    UiNetOptions opts = stream_opts();
    opts.saved = {{"Home", "password1"}};
    UiNetHarness h(opts);
    // Before start(): the bus queues what is published until it runs.
    ForwardedRecorder rec(h.app->bus());
    ASSERT_TRUE(h.start());
    EXPECT_STEP(h, rec, (Frames{kWifiConnectingAny, kWifiConnectedHome, kTimeSynced}));
    // The first power sample and SD probe have run by now and added nothing.
    ASSERT_TRUE(wait_until([&] { return h.power->status_calls.load() > 0; }, kStepMs));
    ASSERT_TRUE(wait_until([&] { return wisp::app::queue_storage_refresh(*h.app); }, kStepMs));
    ASSERT_TRUE(h.app->storage_worker()->drain(kStepMs));
    EXPECT_STEP(h, rec, Frames{});
}
