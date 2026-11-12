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
#include "wisp/core/boot_report.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/touch.hpp"
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
    auto button = std::make_shared<SimButton>("button.power", SimButton::Kind::Power, input);
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
