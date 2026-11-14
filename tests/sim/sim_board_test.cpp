// SimBoard::init() must degrade audio_out.main/audio_in.main (rather
// than crash, or silently register a device that will fail every
// open() call) when SDL's audio subsystem never came up at all --
// main.cpp warns and continues rather than aborting if
// SDL_Init(SDL_INIT_AUDIO) fails. This test file deliberately never
// calls SDL_Init() itself, so SDL_WasInit(AUDIO) reliably reports "not
// initialized" within this process -- the same condition
// SimBoard::init() must handle.

#include "wisp/port/sim/sim_board.hpp"

#include <SDL.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dev/sim_devices.hpp"
#include "support/fake_devices.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/device_schema.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/hal/storage.hpp"
#include "wisp/hal/touch.hpp"
#include "wisp/hal/wifi.hpp"
#include "wisp/port/sim/sim_haptic.hpp"
#include "wisp/port/sim/sim_input.hpp"
#include "wisp/shell/conversation.hpp"

TEST(SimBoardTest, AudioDevicesDegradeWhenSdlAudioNeverInitialized) {
    ASSERT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0u)
        << "this test must run without SDL audio ever having been initialized";

    auto input = std::make_shared<wisp::port::sim::SimInput>(/*sd_present_initial=*/true);
    wisp::port::sim::SimBoardConfig config;
    config.data_dir = (std::filesystem::temp_directory_path() / "wisp_sim_board_test").string();
    wisp::port::sim::SimBoard board(config, input, [] {});

    wisp::core::DeviceRegistry registry;
    wisp::core::BootReport report;
    board.init(registry, report);

    // Neither audio device was registered at all.
    EXPECT_EQ(registry.get<wisp::hal::AudioOut>("audio_out.main"), nullptr);
    EXPECT_EQ(registry.get<wisp::hal::AudioIn>("audio_in.main"), nullptr);

    auto audio_out_report = report.find("audio_out.main");
    ASSERT_TRUE(audio_out_report.has_value());
    EXPECT_EQ(audio_out_report->state, wisp::core::DeviceState::Failed);
    EXPECT_EQ(audio_out_report->reason, "SDL audio unavailable");

    auto audio_in_report = report.find("audio_in.main");
    ASSERT_TRUE(audio_in_report.has_value());
    EXPECT_EQ(audio_in_report->state, wisp::core::DeviceState::Failed);
    EXPECT_EQ(audio_in_report->reason, "SDL audio unavailable");

    // Every other device is unaffected by audio being unavailable.
    EXPECT_NE(registry.get<wisp::hal::Touch>("touch.main"), nullptr);
}

// A device the registry refuses is one failed entry in the boot report,
// even when the device would otherwise also report itself (an SD slot
// with no card says Missing).
TEST(SimBoardTest, RefusedRegistrationIsRecordedOnceAsFailed) {
    auto input = std::make_shared<wisp::port::sim::SimInput>(/*sd_present_initial=*/false);
    wisp::port::sim::SimBoardConfig config;
    config.data_dir =
        (std::filesystem::temp_directory_path() / "wisp_sim_board_refused_test").string();
    wisp::port::sim::SimBoard board(config, input, [] {});

    wisp::core::DeviceRegistry registry;
    ASSERT_TRUE(registry.add(std::make_shared<wisp::test::FakeGeneric>("storage.sd")));
    wisp::core::BootReport report;
    board.init(registry, report);

    int entries = 0;
    for (const auto& entry : report.entries()) {
        if (entry.id == "storage.sd") ++entries;
    }
    EXPECT_EQ(entries, 1);
    auto sd = report.find("storage.sd");
    ASSERT_TRUE(sd.has_value());
    EXPECT_EQ(sd->state, wisp::core::DeviceState::Failed);
    EXPECT_EQ(sd->reason, "registry.add: storage.sd");
}

namespace {

// Boots a SimBoard under SDL's "dummy" audio driver so the audio devices
// are registered too. SDL is shut down again on destruction (after the
// registry), so the test above still sees SDL audio uninitialized when
// the whole binary runs in one process.
class SimBoard : public ::testing::Test {
protected:
    void SetUp() override {
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
        ASSERT_EQ(SDL_Init(SDL_INIT_AUDIO), 0) << SDL_GetError();
        auto input = std::make_shared<wisp::port::sim::SimInput>(/*sd_present_initial=*/true);
        wisp::port::sim::SimBoardConfig config;
        config.data_dir =
            (std::filesystem::temp_directory_path() / "wisp_sim_board_power_test").string();
        board_ = std::make_unique<wisp::port::sim::SimBoard>(config, input, [] {});
        registry_ = std::make_unique<wisp::core::DeviceRegistry>();
        board_->init(*registry_, report_);
    }

    void TearDown() override {
        registry_.reset();
        board_.reset();
        SDL_Quit();
    }

    std::string standby_of(const std::string& id) const {
        auto device = registry_->find(id);
        if (!device) return "<missing device>";
        const auto& caps = device->info().caps;
        auto it = caps.find("standby");
        if (it == caps.end()) return "";
        if (it->second.type() != wisp::Value::Type::String) return "<not a string>";
        return it->second.as_string();
    }

    template <class T>
    std::string id_by_role(const char* role) const {
        auto device = registry_->by_role<T>(role);
        return device ? device->info().id : "<none>";
    }

    std::unique_ptr<wisp::port::sim::SimBoard> board_;
    std::unique_ptr<wisp::core::DeviceRegistry> registry_;
    wisp::core::BootReport report_;
};

}  // namespace

TEST_F(SimBoard, DevicesDeclareStandbyPolicy) {
    EXPECT_EQ(standby_of("display.main"), "off");
    EXPECT_EQ(standby_of("touch.main"), "off");
    EXPECT_EQ(standby_of("imu.main"), "off");
    EXPECT_EQ(standby_of("button.power"), "wake_source");
    EXPECT_EQ(standby_of("button.vol_up"), "wake_source");
    EXPECT_EQ(standby_of("button.vol_down"), "wake_source");
    EXPECT_EQ(standby_of("audio_out.main"), "off");
    // Whether the dummy driver offers a capture device depends on the
    // host's SDL build (see sim_audio_test.cpp).
    if (registry_->find("audio_in.main")) {
        EXPECT_EQ(standby_of("audio_in.main"), "off");
    }
    EXPECT_EQ(standby_of("power.main"), "");
    EXPECT_EQ(standby_of("clock.main"), "");
    EXPECT_EQ(standby_of("storage.int"), "");
    EXPECT_EQ(standby_of("storage.sd"), "");
}

TEST_F(SimBoard, EveryTypedDeviceIsFoundByRole) {
    namespace hal = wisp::hal;
    namespace role = wisp::hal::role;
    EXPECT_EQ(id_by_role<hal::Display>(role::kMain), "display.main");
    EXPECT_EQ(id_by_role<hal::Touch>(role::kMain), "touch.main");
    EXPECT_EQ(id_by_role<hal::Imu>(role::kMain), "imu.main");
    EXPECT_EQ(id_by_role<hal::Power>(role::kMain), "power.main");
    EXPECT_EQ(id_by_role<hal::Clock>(role::kMain), "clock.main");
    EXPECT_EQ(id_by_role<hal::Wifi>(role::kMain), "net.wifi");
    EXPECT_EQ(id_by_role<hal::Storage>(role::kInternal), "storage.int");
    EXPECT_EQ(id_by_role<hal::Storage>(role::kRemovable), "storage.sd");
    EXPECT_EQ(id_by_role<hal::Button>(role::kPower), "button.power");
    EXPECT_EQ(id_by_role<hal::Button>(role::kTalk), "button.vol_down");
    // Null on a host whose SDL build offers no such audio device.
    EXPECT_EQ(registry_->by_role<hal::AudioOut>(role::kMain), registry_->find("audio_out.main"));
    EXPECT_EQ(registry_->by_role<hal::AudioIn>(role::kMain), registry_->find("audio_in.main"));
}

TEST_F(SimBoard, DisplayFollowsPowerLevel) {
    auto display = registry_->get<wisp::hal::Display>("display.main");
    ASSERT_NE(display, nullptr);
    ASSERT_TRUE(display->set_power(wisp::hal::PowerLevel::Standby));
    EXPECT_FALSE(display->powered());
    ASSERT_TRUE(display->set_power(wisp::hal::PowerLevel::Dim));
    EXPECT_TRUE(display->powered());
}

TEST_F(SimBoard, AudioFollowsPowerLevel) {
    auto out = registry_->get<wisp::hal::AudioOut>("audio_out.main");
    ASSERT_NE(out, nullptr);
    auto& sim_out = static_cast<wisp::port::sim::SimAudioOut&>(*out);
    EXPECT_TRUE(sim_out.powered());
    ASSERT_TRUE(out->set_power(wisp::hal::PowerLevel::Standby));
    EXPECT_FALSE(sim_out.powered());
    ASSERT_TRUE(out->set_power(wisp::hal::PowerLevel::DeepStandby));
    EXPECT_FALSE(sim_out.powered());
    ASSERT_TRUE(out->set_power(wisp::hal::PowerLevel::Active));
    EXPECT_TRUE(sim_out.powered());

    auto in = registry_->get<wisp::hal::AudioIn>("audio_in.main");
    if (in) {
        auto& sim_in = static_cast<wisp::port::sim::SimAudioIn&>(*in);
        EXPECT_TRUE(sim_in.powered());
        ASSERT_TRUE(in->set_power(wisp::hal::PowerLevel::DeepStandby));
        EXPECT_FALSE(sim_in.powered());
        ASSERT_TRUE(in->set_power(wisp::hal::PowerLevel::Dim));
        EXPECT_TRUE(sim_in.powered());
    }
}

TEST_F(SimBoard, VolDownIsTheTalkKey) {
    EXPECT_EQ(wisp::shell::find_talk_button(*registry_),
              std::optional<std::string>("button.vol_down"));
}

TEST(SimButton, EdgeSourcePreservesActualTransitionsAndTimestamps) {
    using namespace wisp::port::sim;
    auto input = std::make_shared<SimInput>();
    SimButton button("button.power", SimButton::Kind::Power, input);
    EXPECT_EQ(button.input_mode(), wisp::hal::ButtonInputMode::EdgeEvent);
    struct Edges {
        std::vector<bool> down;
        std::vector<uint64_t> at;
    } edges;
    ASSERT_TRUE(button.set_edge_sink({&edges, [](void* ctx, bool down, uint64_t at) noexcept {
                                          auto& e = *static_cast<Edges*>(ctx);
                                          e.down.push_back(down);
                                          e.at.push_back(at);
                                          return true;
                                      }}));
    input->key_power(true);
    input->key_power(true);
    input->key_power(false);
    EXPECT_EQ(edges.down, (std::vector<bool>{true, false}));
    ASSERT_EQ(edges.at.size(), 2u);
    EXPECT_GT(edges.at[0], 0u);
    EXPECT_GE(edges.at[1], edges.at[0]);
    ASSERT_TRUE(button.set_edge_sink({}));
    input->key_power(true);
    EXPECT_EQ(edges.at.size(), 2u);
}

TEST(SimButton, ExplicitTimestampsAndReentrantInjectionRemainOrdered) {
    using namespace wisp::port::sim;
    auto input = std::make_shared<SimInput>();
    SimButton button("button.power", SimButton::Kind::Power, input);
    struct Context {
        SimInput* input;
        std::vector<uint64_t> times;
    } ctx{input.get(), {}};
    ASSERT_TRUE(button.set_edge_sink({&ctx, [](void* raw, bool down, uint64_t at) noexcept {
                                          auto& ctx = *static_cast<Context*>(raw);
                                          ctx.times.push_back(at);
                                          if (down) {
                                              EXPECT_TRUE(ctx.input->power_pressed());
                                              ctx.input->mouse(1, 2, true);
                                              ctx.input->key_power(false, 70000);
                                          }
                                          return true;
                                      }}));
    input->key_power(true, 10000);
    EXPECT_EQ(ctx.times, (std::vector<uint64_t>{10000, 70000}));
    EXPECT_FALSE(button.pressed());
}

TEST(SimButton, DetachWaitsForInFlightCallbackAndDisablesNewDelivery) {
    using namespace wisp::port::sim;
    auto input = std::make_shared<SimInput>();
    SimButton button("button.power", SimButton::Kind::Power, input);
    struct Context {
        wisp::os::Semaphore entered{1, 0};
        wisp::os::Semaphore resume{1, 0};
        std::atomic<size_t> calls{0};
    } ctx;
    ASSERT_TRUE(button.set_edge_sink({&ctx, [](void* raw, bool, uint64_t) noexcept {
                                          auto& ctx = *static_cast<Context*>(raw);
                                          ++ctx.calls;
                                          ctx.entered.give();
                                          ctx.resume.take(wisp::os::kForever);
                                          return true;
                                      }}));
    std::thread producer([&] { input->key_power(true); });
    const bool entered = ctx.entered.take(2000);
    EXPECT_TRUE(entered);
    std::atomic<bool> detached{false};
    wisp::os::Semaphore detaching{1, 0};
    std::thread detach([&] {
        detaching.give();
        EXPECT_TRUE(button.set_edge_sink({}));
        detached.store(true);
    });
    detaching.take(wisp::os::kForever);
    wisp::os::sleep_ms(10);
    EXPECT_FALSE(detached.load());
    // The callback holds neither SimInput lock: state remains accessible.
    EXPECT_TRUE(input->power_pressed());
    input->mouse(3, 4, true);
    ctx.resume.give();
    producer.join();
    detach.join();
    EXPECT_TRUE(detached.load());
    input->key_power(false);
    EXPECT_EQ(ctx.calls.load(), 1u);
}

TEST(SimButton, SamePhysicalKeyCannotAcquireASecondSink) {
    using namespace wisp::port::sim;
    auto input = std::make_shared<SimInput>();
    SimButton first("button.power", SimButton::Kind::Power, input);
    size_t calls = 0;
    const wisp::hal::ButtonEdgeSink sink{&calls, [](void* raw, bool, uint64_t) noexcept {
                                             ++*static_cast<size_t*>(raw);
                                             return true;
                                         }};
    ASSERT_TRUE(first.set_edge_sink(sink));
    {
        SimButton duplicate("button.other", SimButton::Kind::Power, input);
        EXPECT_FALSE(duplicate.set_edge_sink(sink));
    }
    input->key_power(true);
    EXPECT_EQ(calls, 1u);  // destroying a non-owner must not detach the owner
    ASSERT_TRUE(first.set_edge_sink({}));
}

TEST(SimButton, ConcurrentInjectionAndServiceLifecycleAreSafe) {
    using namespace wisp::port::sim;
    auto input = std::make_shared<SimInput>();
    auto button = std::make_shared<SimButton>("button.power", SimButton::Kind::Power, input,
                                              wisp::hal::role::kPower);
    wisp::core::DeviceRegistry registry;
    ASSERT_TRUE(registry.add(button));
    wisp::core::EventBus bus;
    ASSERT_TRUE(bus.start());
    wisp::core::ButtonService service(registry, bus);
    std::atomic<bool> done{false};
    std::thread producer([&] {
        while (!done.load()) {
            input->key_power(true);
            input->key_power(false);
            wisp::os::sleep_ms(1);
        }
    });
    std::thread modes([&] {
        while (!done.load()) {
            service.set_standby(true);
            service.set_standby(false);
            std::this_thread::yield();
        }
    });
    std::thread stopper([&] {
        for (int i = 0; i < 20; ++i) {
            service.stop();
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 20; ++i) EXPECT_TRUE(service.start());
    stopper.join();
    done.store(true);
    producer.join();
    modes.join();
    service.stop();
    input->key_power(false);
    // A fresh owner can attach after all service bindings have been drained.
    ASSERT_TRUE(
        button->set_edge_sink({nullptr, [](void*, bool, uint64_t) noexcept { return true; }}));
    ASSERT_TRUE(button->set_edge_sink({}));
    bus.stop();
}

TEST(SimButton, PairedEdgesBeforeDrainProduceShortWithoutPolling) {
    using namespace wisp::port::sim;
    auto input = std::make_shared<SimInput>();
    auto button =
        std::make_shared<SimButton>("button.vol_down", SimButton::Kind::VolDown, input, "talk");
    wisp::core::DeviceRegistry registry;
    ASSERT_TRUE(registry.add(button));
    wisp::core::EventBus bus;
    ASSERT_TRUE(bus.start());
    std::vector<std::string> topics;
    bus.subscribe("button.",
                  [&](const wisp::core::Event& event) { topics.push_back(event.topic); });
    wisp::core::ButtonService service(registry, bus);
    service.poll(0);
    input->key_vol_down(true, 10000);
    input->key_vol_down(false, 70000);
    service.set_standby(true);
    service.poll(90);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(topics, (std::vector<std::string>{"button.vol_down.down", "button.vol_down.up",
                                                "button.vol_down.short"}));
    service.stop();
    bus.stop();
}

namespace {

// A haptic on a clock the test moves by hand.
struct HapticFixture {
    HapticFixture() : haptic("haptic.main", "main", [this] { return now; }) {}

    bool active() {
        auto v = haptic.get("active");
        EXPECT_TRUE(v);
        return v && v.value().as_bool();
    }
    wisp::Result<wisp::Value> pulse(int64_t ms, int64_t strength) {
        return haptic.invoke("pulse", {{"ms", ms}, {"strength", strength}});
    }

    uint64_t now = 1000;
    wisp::port::sim::SimHaptic haptic;
};

}  // namespace

TEST(SimHaptic, PulseRunsForItsDurationThenStops) {
    HapticFixture f;
    EXPECT_FALSE(f.active());
    ASSERT_TRUE(f.pulse(150, 80));
    f.now = 1000 + 149;
    EXPECT_TRUE(f.active());
    f.now = 1000 + 150;
    EXPECT_FALSE(f.active());
}

TEST(SimHaptic, EffectRunsForAShortFixedTime) {
    HapticFixture f;
    ASSERT_TRUE(f.haptic.invoke("effect", {{"id", 7}}));
    f.now = 1099;
    EXPECT_TRUE(f.active());
    f.now = 1100;
    EXPECT_FALSE(f.active());
}

TEST(SimHaptic, ClampsWhenCalledDirectly) {
    HapticFixture f;
    ASSERT_TRUE(f.pulse(999999, 500));
    f.now = 1000 + 1999;
    EXPECT_TRUE(f.active());
    f.now = 1000 + 2000;
    EXPECT_FALSE(f.active());
    // Too short and a missing strength are clamped / defaulted, not refused.
    ASSERT_TRUE(f.haptic.invoke("pulse", {{"ms", 1}}));
    f.now += 9;
    EXPECT_TRUE(f.active());
    f.now += 1;
    EXPECT_FALSE(f.active());
    ASSERT_TRUE(f.haptic.invoke("effect", {{"id", 99999}}));
}

TEST(SimHaptic, StandbyStopsItAndRefusesCalls) {
    HapticFixture f;
    namespace hal = wisp::hal;
    ASSERT_TRUE(f.pulse(500, 80));
    EXPECT_TRUE(f.active());
    ASSERT_TRUE(f.haptic.set_power(hal::PowerLevel::Standby));
    auto refused = f.pulse(500, 80);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, wisp::ErrorCode::Busy);
    auto get = f.haptic.get("active");
    ASSERT_FALSE(get);
    EXPECT_EQ(get.error().code, wisp::ErrorCode::Busy);
    ASSERT_FALSE(f.haptic.invoke("effect", {{"id", 3}}));
    ASSERT_TRUE(f.haptic.set_power(hal::PowerLevel::DeepStandby));
    // Dim keeps the screen on, so the motor comes back; the pulse that was
    // running when it went down does not.
    ASSERT_TRUE(f.haptic.set_power(hal::PowerLevel::Dim));
    EXPECT_FALSE(f.active());
    ASSERT_TRUE(f.pulse(500, 80));
    EXPECT_TRUE(f.active());
    ASSERT_TRUE(f.haptic.set_power(hal::PowerLevel::Active));
    EXPECT_TRUE(f.active());
}

TEST(SimHaptic, SchemaPassesTheChecks) {
    HapticFixture f;
    const wisp::hal::DeviceSchema* schema = f.haptic.describe();
    ASSERT_NE(schema, nullptr);
    EXPECT_TRUE(wisp::hal::check_schema(*schema));
    EXPECT_EQ(f.haptic.describe(), schema);
    EXPECT_EQ(f.haptic.interface(), wisp::hal::Interface::None);
    EXPECT_EQ(f.haptic.info().cls, wisp::hal::DeviceClass::Other);
    EXPECT_EQ(f.haptic.info().model, "sim-haptic");

    ASSERT_EQ(schema->props.size(), 1u);
    EXPECT_STREQ(schema->props[0].name, "active");
    EXPECT_EQ(schema->props[0].type, wisp::hal::ParamType::Bool);

    ASSERT_EQ(schema->cmds.size(), 2u);
    const auto& pulse = schema->cmds[0];
    EXPECT_STREQ(pulse.name, "pulse");
    EXPECT_EQ(pulse.min_interval_ms, 100u);
    ASSERT_EQ(pulse.args.size(), 2u);
    EXPECT_STREQ(pulse.args[0].name, "ms");
    EXPECT_EQ(pulse.args[0].min, 10);
    EXPECT_EQ(pulse.args[0].max, 2000);
    EXPECT_STREQ(pulse.args[0].unit, "ms");
    EXPECT_FALSE(pulse.args[0].optional);
    EXPECT_STREQ(pulse.args[1].name, "strength");
    EXPECT_EQ(pulse.args[1].min, 0);
    EXPECT_EQ(pulse.args[1].max, 100);
    EXPECT_STREQ(pulse.args[1].unit, "%");
    EXPECT_TRUE(pulse.args[1].optional);
    EXPECT_EQ(pulse.args[1].def.as_int(), 80);

    const auto& effect = schema->cmds[1];
    EXPECT_STREQ(effect.name, "effect");
    ASSERT_EQ(effect.args.size(), 1u);
    EXPECT_STREQ(effect.args[0].name, "id");
    EXPECT_EQ(effect.args[0].min, 1);
    EXPECT_EQ(effect.args[0].max, 123);
}

TEST(SimHaptic, ConcurrentCallsAndStandbyAreSafe) {
    std::atomic<uint64_t> now{1000};
    wisp::port::sim::SimHaptic haptic("haptic.main", "main", [&] { return now.load(); });
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&] {
            while (!stop) {
                (void)haptic.invoke("pulse", {{"ms", 50}, {"strength", 80}});
                (void)haptic.get("active");
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        (void)haptic.set_power(i % 2 ? wisp::hal::PowerLevel::Active
                                     : wisp::hal::PowerLevel::Standby);
        now += 7;
    }
    stop = true;
    for (auto& t : threads) t.join();
    // The last transition was Active; after standby nothing may be running.
    ASSERT_TRUE(haptic.set_power(wisp::hal::PowerLevel::Standby));
    ASSERT_TRUE(haptic.set_power(wisp::hal::PowerLevel::Active));
    EXPECT_FALSE(haptic.get("active").value().as_bool());
}

TEST_F(SimBoard, RegistersTheHaptic) {
    auto device = registry_->find("haptic.main");
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->info().cls, wisp::hal::DeviceClass::Other);
    EXPECT_EQ(standby_of("haptic.main"), "off");
    const auto caps = wisp::hal::caps_of(*device);
    EXPECT_TRUE(caps.count("schema"));
    EXPECT_EQ(caps.at("role").as_string(), "main");
}

TEST(SimBoardTest, HapticCanBeDisabledByIdOrClass) {
    for (const char* what : {"haptic.main", "other"}) {
        auto input = std::make_shared<wisp::port::sim::SimInput>(true);
        wisp::port::sim::SimBoardConfig config;
        config.data_dir =
            (std::filesystem::temp_directory_path() / "wisp_sim_board_haptic").string();
        config.disabled = {what};
        wisp::port::sim::SimBoard board(config, input, [] {});
        wisp::core::DeviceRegistry registry;
        wisp::core::BootReport report;
        board.init(registry, report);
        EXPECT_EQ(registry.find("haptic.main"), nullptr) << what;
        bool recorded = false;
        for (const auto& e : report.entries()) {
            if (e.id == "haptic.main") {
                recorded = true;
                EXPECT_EQ(e.state, wisp::core::DeviceState::Failed);
            }
        }
        EXPECT_TRUE(recorded) << what;
    }
}
