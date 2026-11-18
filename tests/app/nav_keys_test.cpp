#include "nav_keys.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/key_nav.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::core::ButtonEdge;
using wisp::test::FakeButton;
using wisp::test::FakeDisplay;
using wisp::test::wait_until;
using wisp::ui::NavKey;

namespace {

wisp::Properties role(wisp::Value r) { return wisp::Properties{{"role", std::move(r)}}; }

// A registry of buttons, a ButtonService over it and a UiRuntime with the
// keypad whose hook records every key entry (and consumes it).
struct Rig {
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    wisp::core::ButtonService buttons{registry, bus};
    std::shared_ptr<FakeDisplay> disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui{disp, nullptr, wisp::ui::UiConfig{.keypad = true}};
    std::mutex mu;
    std::vector<std::pair<NavKey, bool>> seen;

    Rig() {
        EXPECT_TRUE(ui.start());
        ui.set_nav_key_hook([this](NavKey k, bool p) {
            std::lock_guard<std::mutex> lock(mu);
            seen.emplace_back(k, p);
            return true;
        });
    }
    ~Rig() {
        ui.set_nav_key_hook({});
        ui.stop();
    }

    void add(const std::string& id, wisp::Properties caps = {}) {
        registry.add(std::make_shared<FakeButton>(id, std::move(caps)));
    }
    void add_default() {
        add("button.a", role(wisp::Value(wisp::hal::role::kNavNext)));
        add("button.b", role(wisp::Value(wisp::hal::role::kNavOk)));
        add("button.c");
    }
    void emit(const char* id, ButtonEdge::Kind kind) {
        buttons.emit_for_test(id, kind, wisp::os::now_us());
    }
    std::vector<std::pair<NavKey, bool>> entries() {
        std::lock_guard<std::mutex> lock(mu);
        return seen;
    }
    bool wait_for(size_t n) {
        return wait_until([&] { return entries().size() >= n; }, 2000);
    }
};

// Whether a line logged after `after_seq` contains `text`.
bool logged_since(uint32_t after_seq, const std::string& text) {
    const auto entries = wisp::log::ring_entries();
    return std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
        return e.seq > after_seq && e.msg.find(text) != std::string::npos;
    });
}

uint32_t last_seq() {
    wisp::log::init();
    const auto entries = wisp::log::ring_entries();
    return entries.empty() ? 0 : entries.back().seq;
}

}  // namespace

TEST(NavKeys, DownAndUpBecomeKeys) {
    Rig rig;
    rig.add_default();
    wisp::app::NavKeys keys(rig.registry, rig.buttons, rig.ui);
    rig.emit("button.a", ButtonEdge::Kind::Down);
    rig.emit("button.a", ButtonEdge::Kind::Up);
    ASSERT_TRUE(rig.wait_for(2));
    const std::vector<std::pair<NavKey, bool>> want = {{NavKey::Next, true}, {NavKey::Next, false}};
    EXPECT_EQ(rig.entries(), want);
}

TEST(NavKeys, ShortLongAndOtherButtonsAreIgnored) {
    Rig rig;
    rig.add_default();
    wisp::app::NavKeys keys(rig.registry, rig.buttons, rig.ui);
    rig.emit("button.a", ButtonEdge::Kind::Short);
    rig.emit("button.a", ButtonEdge::Kind::Long);
    rig.emit("button.c", ButtonEdge::Kind::Down);
    rig.emit("button.c", ButtonEdge::Kind::Up);
    // A key after them: once it is seen, anything queued before it was too.
    rig.emit("button.b", ButtonEdge::Kind::Down);
    rig.emit("button.b", ButtonEdge::Kind::Up);
    ASSERT_TRUE(rig.wait_for(2));
    const std::vector<std::pair<NavKey, bool>> want = {{NavKey::Ok, true}, {NavKey::Ok, false}};
    EXPECT_EQ(rig.entries(), want);
}

TEST(NavKeys, HasNavKeys) {
    wisp::core::DeviceRegistry registry;
    registry.add(
        std::make_shared<FakeButton>("button.power", role(wisp::Value(wisp::hal::role::kPower))));
    EXPECT_FALSE(wisp::app::has_nav_keys(registry));
    registry.add(
        std::make_shared<FakeButton>("button.a", role(wisp::Value(wisp::hal::role::kNavNext))));
    EXPECT_TRUE(wisp::app::has_nav_keys(registry));
}

TEST(NavKeys, WarnsWithoutNextOrOk) {
    Rig rig;
    rig.add("button.back", role(wisp::Value(wisp::hal::role::kNavBack)));
    const uint32_t since = last_seq();
    wisp::app::NavKeys keys(rig.registry, rig.buttons, rig.ui);
    EXPECT_TRUE(logged_since(since, "nav keys without nav_next and nav_ok: focus cannot move"));
}

TEST(NavKeys, WarnsWhenANavKeyTalks) {
    Rig rig;
    rig.add("button.x", role(wisp::Value(wisp::List{wisp::Value(wisp::hal::role::kTalk),
                                                    wisp::Value(wisp::hal::role::kNavOk)})));
    const uint32_t since = last_seq();
    wisp::app::NavKeys keys(rig.registry, rig.buttons, rig.ui);
    EXPECT_TRUE(logged_since(since, "button.x holds a nav role and talk"));
}

TEST(NavKeys, StopsListeningWhenDestroyed) {
    Rig rig;
    rig.add_default();
    {
        wisp::app::NavKeys keys(rig.registry, rig.buttons, rig.ui);
    }
    // No listener is left: these reach nobody, and ButtonService's
    // destructor (end of the test) finds none to report.
    rig.emit("button.a", ButtonEdge::Kind::Down);
    rig.emit("button.a", ButtonEdge::Kind::Up);
    rig.emit("button.c", ButtonEdge::Kind::Down);
    wisp::os::sleep_ms(100);
    EXPECT_TRUE(rig.entries().empty());
}
