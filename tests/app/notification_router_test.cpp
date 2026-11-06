// NotificationRouter (scene-device spec 5.1, 5.4): a posted notification
// wakes the screen and sounds; a tap dismisses it and goes to its scene;
// the release action releases the wake lock.

#include "notification_router.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "support/scene_runtime_fixture.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/tone_player.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

namespace {

using wisp::app::NotificationRouter;
using wisp::app::NotificationRouterDeps;
using wisp::app::PowerManager;
using wisp::core::DismissReason;
using wisp::core::Notification;
using wisp::core::NotificationChange;
using wisp::core::NotificationService;
using wisp::core::NotifySound;
using wisp::core::SceneState;
using wisp::core::WakeLockKind;
using wisp::hal::PowerLevel;
using wisp::test::wait_until;

// Counts the frames written to it.
class CountingAudioOut : public wisp::hal::AudioOut {
public:
    explicit CountingAudioOut(uint32_t pace_ms = 0) : pace_ms_(pace_ms) {
        info_.id = "audio_out.main";
        info_.cls = kClass;
        info_.model = "FAKE";
    }
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<void> open(const wisp::hal::AudioFormat&) override {
        return wisp::Result<void>::ok();
    }
    wisp::Result<size_t> write(const int16_t*, size_t count) override {
        if (pace_ms_ > 0) wisp::os::sleep_ms(pace_ms_);
        frames_ += count;
        return wisp::Result<size_t>::ok(count);
    }
    void close() override {}
    wisp::Result<void> set_volume(uint8_t) override { return wisp::Result<void>::ok(); }
    size_t frames() const { return frames_.load(); }

private:
    wisp::hal::DeviceInfo info_;
    uint32_t pace_ms_;
    std::atomic<size_t> frames_{0};
};

class NotificationRouterTest : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        notes = std::make_unique<NotificationService>(wisp::core::NotificationDeps{
            .bus = bus, .storage = *storage, .submit_io = {}, .make_timer = {}});
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.notifications = notes.get();
            d.locks = &locks;
        };
        settings_backend = std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(
            (wisp::test::temp_dir() / "settings.json").string());
        // Headless: transitions run on the caller's thread.
        power = std::make_unique<PowerManager>(wisp::app::PowerManagerDeps{
            .bus = bus,
            .registry = registry,
            .locks = locks,
            .settings = wisp::core::Settings(*settings_backend, "power"),
            .display = nullptr,
        });
        out = std::make_shared<CountingAudioOut>(pace_ms);
        tones = std::make_unique<wisp::core::TonePlayer>(out, &locks);
        ASSERT_TRUE(tones->start());
        bus.subscribe("notification.dismissed", [this](const wisp::core::Event& e) {
            std::lock_guard<std::mutex> lock(mu);
            dismissed.push_back(e.payload.at("reason").as_string());
        });
    }

    void TearDown() override {
        router.reset();
        rt.reset();
        tones.reset();
        power.reset();
        SceneRuntimeFixture::TearDown();
        notes.reset();
    }

    void make_router() {
        router = std::make_unique<NotificationRouter>(NotificationRouterDeps{
            .notes = *notes,
            .power = power.get(),
            .tones = tones.get(),
            .scenes = rt.get(),
            .locks = &locks,
            .notify_volume = [] { return uint8_t{50}; },
        });
        router->start();
    }

    std::string post(const std::string& source, const std::string& id, NotifySound sound,
                     bool wake) {
        Notification n;
        n.source = source;
        n.id = id;
        n.source_name = "Demo";
        n.title = "t";
        n.sound = sound;
        n.wake = wake;
        auto r = notes->post(std::move(n), false);
        EXPECT_TRUE(r) << r.error().message;
        return r ? r.value() : std::string();
    }

    std::vector<std::string> dismissed_reasons() {
        bus.wait_idle(1000);
        std::lock_guard<std::mutex> lock(mu);
        return dismissed;
    }

    uint32_t pace_ms = 0;
    wisp::core::WakeLockRegistry locks{bus};
    std::unique_ptr<NotificationService> notes;
    std::unique_ptr<wisp::port::sim::JsonFileSettingsBackend> settings_backend;
    std::unique_ptr<PowerManager> power;
    std::shared_ptr<CountingAudioOut> out;
    std::unique_ptr<wisp::core::TonePlayer> tones;
    std::unique_ptr<NotificationRouter> router;
    std::mutex mu;
    std::vector<std::string> dismissed;
};

class NotificationRouterPaced : public NotificationRouterTest {
protected:
    void SetUp() override {
        pace_ms = 5;
        NotificationRouterTest::SetUp();
    }
};

TEST_F(NotificationRouterTest, WakeTrueWakesFromStandby) {
    make_router();
    power->enter_standby();
    ASSERT_TRUE(power->standby());
    post("demo", "quiet", NotifySound::None, false);
    EXPECT_TRUE(power->standby());
    post("demo", "loud", NotifySound::None, true);
    EXPECT_EQ(power->level(), PowerLevel::Active);
}

TEST_F(NotificationRouterTest, SoundPlaysTone) {
    make_router();
    post("demo", "a", NotifySound::Tone, false);
    ASSERT_TRUE(wait_until([&] { return out->frames() == 2400; }, 2000));
    ASSERT_TRUE(wait_until([&] { return !tones->playing(); }, 1000));

    // With the screen off there is no audio: a sound plays only for a
    // notification that wakes the screen.
    power->enter_standby();
    post("demo", "b", NotifySound::Tone, false);
    wisp::os::sleep_ms(50);
    EXPECT_EQ(out->frames(), 2400u);
    post("demo", "c", NotifySound::Tone, true);
    ASSERT_TRUE(wait_until([&] { return out->frames() == 4800; }, 2000));
    EXPECT_EQ(power->level(), PowerLevel::Active);
}

// An alarm rings until the user touches the device, so it wakes the
// screen (and the audio with it) even without wake = true.
TEST_F(NotificationRouterTest, AlarmWakesFromStandbyEvenWithoutWake) {
    make_router();
    power->enter_standby();
    ASSERT_TRUE(power->standby());
    post("demo", "pomo", NotifySound::Alarm, false);
    EXPECT_EQ(power->level(), PowerLevel::Active);
    ASSERT_TRUE(wait_until([&] { return out->frames() > 0; }, 2000));
    tones->silence();
    EXPECT_TRUE(wait_until([&] { return !tones->playing(); }, 1000));
}

TEST_F(NotificationRouterTest, BannerShowsEveryPost) {
    make_router();
    std::mutex banner_mu;
    std::vector<std::string> shown;
    router->set_banner([&](const Notification& n) {
        std::lock_guard<std::mutex> lock(banner_mu);
        shown.push_back(n.id);
    });
    post("demo", "a", NotifySound::None, false);
    notes->post_system(wisp::core::notice::kSceneError, "demo");
    std::lock_guard<std::mutex> lock(banner_mu);
    EXPECT_EQ(shown, (std::vector<std::string>{"a", "scene_error:demo"}));
}

TEST_F(NotificationRouterPaced, TapSilencesAndDelivers) {
    start();
    install("demo", R"(
scene.on_notification = function(id) print("notified", id) end
return ui.screen {}
)");
    make_router();
    post("demo", "pomo", NotifySound::Alarm, false);
    ASSERT_TRUE(wait_until([&] { return out->frames() > 0; }, 2000));
    router->tap("demo", "pomo");
    EXPECT_TRUE(wait_until([&] { return !tones->playing(); }, 1000));
    EXPECT_FALSE(notes->find("demo", "pomo").has_value());
    EXPECT_EQ(dismissed_reasons(), (std::vector<std::string>{"tap"}));
    ASSERT_TRUE(wait_until(
        [&] {
            auto log = scene_log("demo");
            return std::find(log.begin(), log.end(), "notified\tpomo") != log.end();
        },
        2000));
    EXPECT_EQ(rt->status("demo").state, SceneState::Running);
}

TEST_F(NotificationRouterPaced, DismissSilencesItsAlarm) {
    make_router();
    post("demo", "other", NotifySound::None, false);
    post("demo", "pomo", NotifySound::Alarm, false);
    ASSERT_TRUE(wait_until([&] { return out->frames() > 0; }, 2000));
    notes->cancel("demo", "other");
    wisp::os::sleep_ms(30);
    EXPECT_TRUE(tones->playing());
    notes->cancel("demo", "pomo");
    EXPECT_TRUE(wait_until([&] { return !tones->playing(); }, 1000));
}

TEST_F(NotificationRouterTest, TapForMissingSceneJustDismisses) {
    start();
    install("other", "return ui.screen { ui.label { text = 'other' } }");
    ASSERT_TRUE(rt->open("other"));
    make_router();
    post("gone", "x", NotifySound::None, false);
    router->tap("gone", "x");
    EXPECT_FALSE(notes->find("gone", "x").has_value());
    EXPECT_EQ(dismissed_reasons(), (std::vector<std::string>{"tap"}));
    ASSERT_TRUE(wait_until([&] { return log_has("scene", "gone: no such scene"); }, 2000));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("other"));
    EXPECT_EQ(rt->status("other").state, SceneState::Running);
    EXPECT_EQ(rt->status("gone").state, SceneState::Stopped);
}

TEST_F(NotificationRouterTest, ReleaseActionReleasesLock) {
    start();
    make_router();
    const uint32_t lock = locks.acquire("scene.demo", WakeLockKind::Screen, "keep_screen_on");
    const std::string id =
        notes->post_system(wisp::core::notice::kWakeLockLong, "scene.demo",
                           wisp::core::notice::kActionReleaseLock, std::to_string(lock));
    router->act(wisp::core::kSystemSource, id);
    EXPECT_FALSE(locks.held(WakeLockKind::Screen));
    EXPECT_FALSE(notes->find(wisp::core::kSystemSource, id).has_value());

    // A notice whose lock id is not a lock: nothing is released or launched,
    // the notice still goes.
    const uint32_t kept = locks.acquire("scene.demo", WakeLockKind::Screen, "keep_screen_on");
    for (const char* arg : {"abc", "0", "12x", "", "4294967296", "999999"}) {
        const std::string bad = notes->post_system(wisp::core::notice::kWakeLockLong,
                                                   std::string("scene.") + (*arg ? arg : "empty"),
                                                   wisp::core::notice::kActionReleaseLock, arg);
        router->act(wisp::core::kSystemSource, bad);
        EXPECT_FALSE(notes->find(wisp::core::kSystemSource, bad).has_value()) << arg;
    }
    EXPECT_TRUE(locks.held(WakeLockKind::Screen));
    locks.release(kept);
    // Unknown notices are ignored.
    router->act(wisp::core::kSystemSource, "no_such_notice");
    router->act("demo", "nothing");
    EXPECT_EQ(rt->foreground(), std::nullopt);
    EXPECT_TRUE(rt->background_scenes().empty());

    // A notice without the action releases nothing.
    const uint32_t sys = locks.acquire("system.ota", WakeLockKind::Cpu, "ota");
    const std::string plain = notes->post_system(wisp::core::notice::kWakeLockLong, "system.ota");
    router->act(wisp::core::kSystemSource, plain);
    EXPECT_TRUE(locks.held(WakeLockKind::Cpu));
    locks.release(sys);
}

// A reminder names its holder: a lock id that now belongs to another
// holder (ids restart at 1 each boot) is left alone, and the notice goes.
TEST_F(NotificationRouterTest, ReleaseActionLeavesAnotherHoldersLock) {
    start();
    make_router();
    const uint32_t admin = locks.acquire("system.admin", WakeLockKind::Network, "ws");
    const std::string id =
        notes->post_system(wisp::core::notice::kWakeLockLong, "scene.a",
                           wisp::core::notice::kActionReleaseLock, std::to_string(admin));
    router->act(wisp::core::kSystemSource, id);
    EXPECT_TRUE(locks.holds(admin, "system.admin"));
    EXPECT_TRUE(locks.held(WakeLockKind::Network));
    EXPECT_FALSE(notes->find(wisp::core::kSystemSource, id).has_value());
    locks.release(admin);
}

}  // namespace
