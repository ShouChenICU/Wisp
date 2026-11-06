// wisp.notify and wisp.notify_cancel (scene-device spec 5.1), and
// SceneRuntime::deliver_notification(): a tapped notification brings its
// scene forward, or launches it, and calls scene.on_notification.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "scene_message.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::core::Notification;
using wisp::core::NotificationChange;
using wisp::core::NotificationService;
using wisp::core::NotifySound;
using wisp::core::SceneState;
using wisp::scene::LaunchReason;
using wisp::test::wait_until;

std::string bg_manifest(const std::string& id, const std::string& name = "Demo") {
    return R"({"id":")" + id + R"(","name":")" + name +
           R"(","version":"1.0.0","schema_version":1,"background":true})";
}

class NotifyFixture : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        // No storage worker: nothing is saved, which these tests do not need.
        notes = std::make_unique<NotificationService>(wisp::core::NotificationDeps{
            .bus = bus, .storage = *storage, .submit_io = {}, .make_timer = {}});
        notes->set_listener([this](const NotificationChange& c) {
            std::lock_guard<std::mutex> lock(changes_mu);
            changes.push_back(c);
        });
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.notifications = notes.get();
        };
    }

    void TearDown() override {
        rt.reset();
        notes->set_listener(nullptr);
        SceneRuntimeFixture::TearDown();
        notes.reset();
    }

    void install_bg(const std::string& id, const std::string& lua) {
        install(id, lua, {{"scene.json", bg_manifest(id)}});
    }

    bool wait_state(const std::string& id, SceneState state, uint32_t ms = 2000) {
        return wait_until([&] { return rt->status(id).state == state; }, ms);
    }

    bool wait_log(const std::string& id, const std::string& line, uint32_t ms = 2000) {
        return wait_until(
            [&] {
                auto log = scene_log(id);
                return std::find(log.begin(), log.end(), line) != log.end();
            },
            ms);
    }

    // What the scene printed: its log without the runtime's own lines.
    std::vector<std::string> printed(const std::string& id) {
        std::vector<std::string> out;
        for (auto& line : scene_log(id))
            if (line.rfind("rebuilt in ", 0) != 0 && line.rfind("slow message ", 0) != 0)
                out.push_back(std::move(line));
        return out;
    }

    std::vector<NotificationChange> seen() {
        std::lock_guard<std::mutex> lock(changes_mu);
        return changes;
    }

    std::unique_ptr<NotificationService> notes;
    std::mutex changes_mu;
    std::vector<NotificationChange> changes;
};

using NotifyBinding = NotifyFixture;
using SceneNotification = NotifyFixture;

TEST_F(NotifyBinding, PostsAndReturnsId) {
    start();
    install_bg("demo", R"(
print("id", wisp.notify{title = "t", body = "b", sound = "alarm", wake = true, id = "pomo"})
return ui.screen {}
)");
    ASSERT_TRUE(rt->launch("demo", LaunchReason::Boot));
    ASSERT_TRUE(wait_log("demo", "id\tpomo"));
    auto n = notes->find("demo", "pomo");
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->source_name, "Demo");
    EXPECT_EQ(n->title, "t");
    EXPECT_EQ(n->body, "b");
    EXPECT_EQ(n->sound, NotifySound::Alarm);
    EXPECT_TRUE(n->wake);
    auto c = seen();
    ASSERT_EQ(c.size(), 1u);
    EXPECT_TRUE(c[0].listed);
}

TEST_F(NotifyBinding, GeneratesIdWhenNoneGiven) {
    start();
    install_bg("demo", R"(
local id = wisp.notify{title = "t"}
print(type(id), #id > 0)
print(wisp.notify_cancel(id))
return ui.screen {}
)");
    ASSERT_TRUE(rt->launch("demo", LaunchReason::Boot));
    ASSERT_TRUE(wait_log("demo", "true"));
    EXPECT_EQ(printed("demo"), (std::vector<std::string>{"string\ttrue", "true"}));
}

TEST_F(NotifyBinding, RateLimitedReturnsNilAndMessage) {
    start();
    install_bg("demo", R"(
for i = 1, 10 do
  if wisp.notify{title = "t" .. i} == nil then print("early") end
end
print(wisp.notify{title = "eleventh"})
return ui.screen {}
)");
    ASSERT_TRUE(rt->launch("demo", LaunchReason::Boot));
    ASSERT_TRUE(wait_log("demo", "nil\trate_limited"));
    EXPECT_EQ(printed("demo"), (std::vector<std::string>{"nil\trate_limited"}));
    EXPECT_EQ(rt->status("demo").state, SceneState::Background);
}

TEST_F(NotifyBinding, BadArgumentsRaise) {
    start();
    struct Case {
        const char* id;
        const char* call;
        const char* field;
    };
    const Case cases[] = {
        {"a", R"(wisp.notify{title = ""})", "title"},
        {"b", R"(wisp.notify{title = "t", sound = "beep"})", "sound"},
        {"c", R"(wisp.notify{title = "t", id = "A"})", "id"},
        {"d", R"(wisp.notify{body = "no title"})", "title"},
        {"e", R"(wisp.notify{title = string.rep("x", 33)})", "title"},
        {"f", R"(wisp.notify{title = "t", body = string.rep("x", 201)})", "body"},
        {"g", R"(wisp.notify{title = "t", wake = "yes"})", "wake"},
        {"h", R"(wisp.notify("t"))", "table"},
    };
    for (const Case& c : cases) {
        install(c.id, std::string(c.call) + "\nreturn ui.screen {}");
        EXPECT_FALSE(rt->open(c.id)) << c.id;
        ASSERT_TRUE(wait_state(c.id, SceneState::Error)) << c.id;
        const std::string error = rt->status(c.id).error;
        EXPECT_NE(error.find(c.field), std::string::npos) << c.id << ": " << error;
    }
    EXPECT_EQ(notes->unread_count(), 0u);
    EXPECT_TRUE(seen().empty());
}

TEST_F(NotifyBinding, CancelById) {
    start();
    install_bg("demo", R"(
wisp.notify{title = "t", id = "x"}
print(wisp.notify_cancel("x"))
print(wisp.notify_cancel("x"))
print(wisp.notify_cancel("never"))
return ui.screen {}
)");
    ASSERT_TRUE(rt->launch("demo", LaunchReason::Boot));
    ASSERT_TRUE(wait_until([&] { return printed("demo").size() == 3; }, 2000));
    EXPECT_EQ(printed("demo"), (std::vector<std::string>{"true", "false", "false"}));
    EXPECT_FALSE(notes->find("demo", "x").has_value());
    auto c = seen();
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[1].kind, NotificationChange::Kind::Dismissed);
    EXPECT_EQ(c[1].reason, wisp::core::DismissReason::Cancel);
}

TEST_F(NotifyBinding, WorksInBackground) {
    start();
    install_bg("demo", R"(
wisp.timer.after(50, function()
  print(wisp.scene.state(), wisp.notify{title = "later", id = "later"})
end)
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(rt->close("demo"));
    ASSERT_EQ(rt->status("demo").state, SceneState::Background);
    ASSERT_TRUE(wait_log("demo", "background\tlater"));
    ASSERT_TRUE(notes->find("demo", "later").has_value());
    auto c = seen();
    ASSERT_EQ(c.size(), 1u);
    EXPECT_TRUE(c[0].listed);
}

TEST_F(NotifyBinding, ForegroundPostIsBannerOnly) {
    start();
    install_bg("demo", R"(
print(wisp.notify{title = "here", id = "here"})
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(wait_log("demo", "here"));
    auto c = seen();
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].kind, NotificationChange::Kind::Posted);
    EXPECT_FALSE(c[0].listed);
    EXPECT_EQ(c[0].n.source_name, "Demo");
    EXPECT_EQ(notes->unread_count(), 0u);
}

TEST_F(NotifyBinding, UnavailableWithoutService) {
    configure_deps = nullptr;
    start();
    install_bg("demo", R"(
print(wisp.notify{title = "t"})
print(wisp.notify_cancel("x"))
return ui.screen {}
)");
    ASSERT_TRUE(rt->launch("demo", LaunchReason::Boot));
    ASSERT_TRUE(wait_until([&] { return printed("demo").size() == 2; }, 2000));
    EXPECT_EQ(printed("demo"), (std::vector<std::string>{"nil\tunavailable", "false"}));
}

const char* kNotified = R"(
print("main", wisp.scene.launch())
scene.on_notification = function(id) print("notified", id, wisp.scene.state()) end
return ui.screen { ui.label { text = "scene demo" } }
)";

TEST_F(SceneNotification, TapBringsBackgroundSceneForward) {
    start();
    install_bg("demo", kNotified);
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(rt->close("demo"));
    ASSERT_EQ(rt->status("demo").state, SceneState::Background);
    const uint32_t instance = wisp::scene::scene_instance(*rt, "demo");

    ASSERT_TRUE(rt->deliver_notification("demo", "pomo"));
    ASSERT_TRUE(wait_log("demo", "notified\tpomo\tforeground"));
    EXPECT_EQ(rt->status("demo").state, SceneState::Running);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("demo"));
    EXPECT_EQ(wisp::scene::scene_instance(*rt, "demo"), instance);
    EXPECT_EQ(printed("demo"),
              (std::vector<std::string>{"main\tuser", "notified\tpomo\tforeground"}));
    EXPECT_TRUE(wait_label("scene demo"));
}

TEST_F(SceneNotification, TapLaunchesStoppedScene) {
    start();
    install("demo", kNotified);
    ASSERT_TRUE(rt->deliver_notification("demo", "pomo"));
    ASSERT_TRUE(wait_log("demo", "notified\tpomo\tforeground"));
    EXPECT_EQ(rt->status("demo").state, SceneState::Running);
    EXPECT_EQ(printed("demo"),
              (std::vector<std::string>{"main\tnotification", "notified\tpomo\tforeground"}));
    EXPECT_TRUE(wait_label("scene demo"));
}

TEST_F(SceneNotification, TapOnSceneInFrontOnlyNotifies) {
    start();
    install("demo", kNotified);
    ASSERT_TRUE(rt->open("demo"));
    const uint32_t instance = wisp::scene::scene_instance(*rt, "demo");
    ASSERT_TRUE(rt->deliver_notification("demo", "pomo"));
    ASSERT_TRUE(wait_log("demo", "notified\tpomo\tforeground"));
    EXPECT_EQ(wisp::scene::scene_instance(*rt, "demo"), instance);
    EXPECT_EQ(printed("demo"),
              (std::vector<std::string>{"main\tuser", "notified\tpomo\tforeground"}));
}

TEST_F(SceneNotification, TapForMissingSceneChangesNothing) {
    start();
    install("other", "return ui.screen { ui.label { text = 'other' } }");
    ASSERT_TRUE(rt->open("other"));
    ASSERT_TRUE(rt->deliver_notification("gone", "x"));
    ASSERT_TRUE(wait_until([&] { return log_has("scene", "gone: no such scene"); }, 2000));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("other"));
    EXPECT_EQ(rt->status("other").state, SceneState::Running);
    EXPECT_EQ(rt->status("gone").state, SceneState::Stopped);
}

// The screen going off while the scene queue is full: the message that says
// so is dropped, and the scene task still catches up once it drains. A
// scene held inside wisp.notify (its listener blocks) keeps the task busy
// while the queue fills.
TEST_F(NotifyFixture, ScreenOffAgainstAFullQueueStillHides) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.queue_capacity = 4;
        c.vm.exec_ms = 10000;
    };
    std::atomic<bool> hold{false}, held{false};
    notes->set_listener([&](const NotificationChange&) {
        held = true;
        while (hold.load()) wisp::os::sleep_ms(1);
    });
    start();
    install("a", R"(
scene.on_show = function() print("show") end
scene.on_hide = function() print("hide") end
return ui.screen {
  ui.button { ref = "go", text = "go", on_click = function() wisp.notify{ title = "t" } end },
}
)");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_log("a", "show"));
    ASSERT_TRUE(wait_label("go"));

    hold = true;
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return held.load(); }, 2000));
    int queued = 0;
    while (true) {
        auto msg = wisp::os::make_large<wisp::scene::SceneMessage>();
        msg->body = wisp::scene::msg::StoreDone{};
        if (!wisp::scene::post_scene_message(*rt, std::move(msg))) break;
        ASSERT_LT(++queued, 100);
    }
    rt->set_visible(false);  // dropped: the queue is full
    hold = false;

    EXPECT_TRUE(wait_log("a", "hide"));
    rt->set_visible(true);
    EXPECT_TRUE(wait_until([&] { return printed("a").size() == 3; }, 2000));
    EXPECT_EQ(printed("a"), (std::vector<std::string>{"show", "hide", "show"}));
}

}  // namespace
