// ConfirmService (wisp/agent/confirm.hpp): the device confirmation dialog
// on a real UiRuntime and SystemLayer, alone and behind AgentService's
// ConfirmFn.

#include "wisp/agent/confirm.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lvgl.h"
#include "support/agent_fixture.hpp"
#include "support/confirm_harness.hpp"
#include "support/lvgl_probe.hpp"
#include "support/visible.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"

using wisp::Map;
using wisp::Value;
using wisp::agent::AgentService;
using wisp::agent::Answer;
using wisp::agent::ConfirmFn;
using wisp::agent::ConfirmService;
using wisp::agent::Message;
using wisp::agent::Role;
using wisp::agent::Source;
using wisp::agent::ToolCtx;
using wisp::agent::ToolDef;
using wisp::agent::ToolOutput;
using wisp::agent::ToolRisk;
using wisp::test::AgentHarness;
using wisp::test::ConfirmHarness;
using wisp::test::FakeLlmServer;
using wisp::test::FakeReply;
using wisp::test::wait_until;
using wisp::ui::StrId;

static_assert(wisp::agent::kConfirmMs == 30000);

namespace {

std::string str(const wisp::os::LargeString& s) { return std::string(s.data(), s.size()); }

// One question asked on a thread of its own.
class Asker {
public:
    Asker(ConfirmService& c, std::string text, std::function<bool()> cancelled = nullptr)
        : thread_([this, &c, text, cancelled] {
              answer_ = static_cast<int>(c.ask(text, cancelled));
              done_ = true;
          }) {}
    ~Asker() { join(); }
    Asker(const Asker&) = delete;
    Asker& operator=(const Asker&) = delete;

    void join() {
        if (thread_.joinable()) thread_.join();
    }
    bool done() const { return done_.load(); }
    std::optional<Answer> answer() {
        if (!done_.load()) return std::nullopt;
        return static_cast<Answer>(answer_.load());
    }
    // Waits for the answer.
    std::optional<Answer> wait(uint32_t ms = 5000) {
        if (!wait_until([this] { return done_.load(); }, ms)) return std::nullopt;
        join();
        return answer();
    }

private:
    std::atomic<int> answer_{-1};
    std::atomic<bool> done_{false};
    std::thread thread_;
};

class Confirm : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
    }

    void TearDown() override {
        if (h) h->server.release();
        svc.reset();
        h.reset();
        c.reset();
        wisp::log::reset_for_test();
    }

    void make(uint32_t timeout_ms = wisp::agent::kConfirmMs) {
        c = std::make_unique<ConfirmHarness>("rect", 0, timeout_ms);
        ASSERT_TRUE(c->ok);
    }

    int msgboxes() {
        int n = 0;
        c->on_ui([&] { n = wisp::test::count_obj_of_class(lv_layer_top(), &lv_msgbox_class); });
        return n;
    }

    // An agent with one confirm-risk tool, "scene.delete", that asks with
    // the agent's ConfirmFn and records whether it went on.
    void start_agent() {
        h = std::make_unique<AgentHarness>();
        ToolDef def;
        def.name = "scene.delete";
        def.description = "test tool";
        def.risk = ToolRisk::Confirm;
        def.timeout_ms = 60000;
        def.run = [this](const Value&, ToolCtx& ctx) -> ToolOutput {
            if (!fn("Agent 要删除场景「番茄钟」", ctx))
                return wisp::agent::tool_error("denied", "the user said no");
            ran = true;
            return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
        };
        ASSERT_TRUE(h->tools->add(std::move(def)));
        svc = std::make_unique<AgentService>(h->deps());
        svc->set_confirm(c->confirm.get());
        fn = svc->confirm_fn();
        ASSERT_TRUE(svc->start());
        h->server.push(
            {.status = 200,
             .chunks = {FakeLlmServer::sse_tool(0, "c1", "scene_delete", "{}"),
                        FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}});
        h->server.push({.status = 200,
                        .chunks = {FakeLlmServer::sse_text("ok"), FakeLlmServer::sse_finish("stop"),
                                   FakeLlmServer::sse_done()}});
        ASSERT_TRUE(svc->submit("delete it", Source::Web));
    }

    // The stored tool result, once the turn has ended.
    std::optional<Message> tool_result() {
        const bool ended = wait_until(
            [&] {
                for (const Message& m : *h->store->history())
                    if (m.role == Role::Marker && str(m.marker) == "turn_end") return true;
                return false;
            },
            5000);
        if (!ended) return std::nullopt;
        for (const Message& m : *h->store->history())
            if (m.role == Role::Tool) return m;
        return std::nullopt;
    }

    std::unique_ptr<ConfirmHarness> c;
    std::unique_ptr<AgentHarness> h;
    std::unique_ptr<AgentService> svc;
    ConfirmFn fn;
    std::atomic<bool> ran{false};
};

}  // namespace

TEST_F(Confirm, AllowedRunsTheTool) {
    make();
    start_agent();
    ASSERT_TRUE(c->wait_showing("番茄钟"));
    // The dialog's title and buttons.
    EXPECT_TRUE(c->showing(wisp::ui::tr(StrId::AgentConfirmTitle)));
    ASSERT_TRUE(c->press(true));
    std::optional<Message> m = tool_result();
    ASSERT_TRUE(m.has_value());
    EXPECT_TRUE(m->ok);
    EXPECT_TRUE(ran.load());
    EXPECT_FALSE(c->dialog_open());
}

TEST_F(Confirm, DeniedReturnsDenied) {
    make();
    start_agent();
    ASSERT_TRUE(c->wait_showing("番茄钟"));
    ASSERT_TRUE(c->press(false));
    std::optional<Message> m = tool_result();
    ASSERT_TRUE(m.has_value());
    EXPECT_FALSE(m->ok);
    auto v = wisp::from_json(str(m->text));
    ASSERT_TRUE(v);
    EXPECT_EQ(v.value().find("error")->as_string(), "denied");
    EXPECT_FALSE(ran.load());
}

TEST_F(Confirm, SilenceIsDenied) {
    make(100);
    Asker a(*c->confirm, "Sure?");
    ASSERT_TRUE(c->wait_showing("Sure?"));
    EXPECT_EQ(a.wait(), Answer::TimedOut);
    EXPECT_TRUE(wait_until([&] { return !c->dialog_open(); }, 2000));
    EXPECT_FALSE(c->confirm->pending().has_value());
}

TEST_F(Confirm, WakesTheScreenFirst) {
    make();
    Asker a(*c->confirm, "Wake?");
    ASSERT_TRUE(c->wait_showing("Wake?"));
    EXPECT_EQ(c->wakes.load(), 1);
    EXPECT_FALSE(c->dialog_at_wake.load());
    ASSERT_TRUE(c->press(true));
    EXPECT_EQ(a.wait(), Answer::Allowed);
}

TEST_F(Confirm, OneAtATime) {
    make();
    std::vector<std::optional<std::string>> seen;
    std::mutex mu;
    c->confirm->set_listener([&](const std::optional<std::string>& t) {
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back(t);
    });
    Asker first(*c->confirm, "First?");
    ASSERT_TRUE(c->wait_showing("First?"));
    EXPECT_EQ(c->confirm->pending(), std::optional<std::string>("First?"));
    Asker second(*c->confirm, "Second?");
    wisp::os::sleep_ms(150);
    EXPECT_FALSE(c->showing("Second?"));
    EXPECT_EQ(msgboxes(), 1);
    EXPECT_FALSE(second.done());

    ASSERT_TRUE(c->press(true));
    EXPECT_EQ(first.wait(), Answer::Allowed);
    ASSERT_TRUE(c->wait_showing("Second?"));
    EXPECT_EQ(msgboxes(), 1);
    ASSERT_TRUE(c->press(false));
    EXPECT_EQ(second.wait(), Answer::Denied);

    std::lock_guard<std::mutex> lock(mu);
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], std::optional<std::string>("First?"));
    EXPECT_EQ(seen[1], std::nullopt);
    EXPECT_EQ(seen[2], std::optional<std::string>("Second?"));
    EXPECT_EQ(seen[3], std::nullopt);
}

TEST_F(Confirm, WaitsForAnotherSystemDialog) {
    make();
    lv_obj_t* reset = nullptr;
    std::atomic<int> reset_answers{0};
    auto open_reset = [&] {
        c->on_ui([&] {
            reset =
                c->layer->open_confirm(StrId::ResetTitle, "Reset?", [&](bool) { ++reset_answers; });
        });
        ASSERT_NE(reset, nullptr);
    };

    // It opens once the factory-reset dialog has closed.
    open_reset();
    {
        Asker a(*c->confirm, "Agent asks");
        wisp::os::sleep_ms(150);
        EXPECT_FALSE(c->showing("Agent asks"));
        EXPECT_TRUE(c->showing("Reset?"));
        c->on_ui([&] { c->layer->close_confirm(reset); });
        ASSERT_TRUE(c->wait_showing("Agent asks"));
        ASSERT_TRUE(c->press(true));
        EXPECT_EQ(a.wait(), Answer::Allowed);
    }

    // A dialog that stays up past the window leaves it unanswered, and is
    // itself left alone.
    ConfirmService brief({.ui = *c->ui, .layer = *c->layer, .timeout_ms = 200});
    open_reset();
    {
        Asker a(brief, "Agent asks again");
        EXPECT_EQ(a.wait(), Answer::TimedOut);
    }
    wisp::os::sleep_ms(50);
    EXPECT_FALSE(c->showing("Agent asks again"));
    EXPECT_TRUE(c->showing("Reset?"));
    EXPECT_TRUE(c->dialog_open());
    EXPECT_EQ(reset_answers.load(), 0);
    c->on_ui([&] { c->layer->close_confirm(reset); });
}

TEST_F(Confirm, StopCancelsPendingConfirm) {
    make();
    std::atomic<bool> stop{false};
    Asker a(*c->confirm, "Stop me?", [&] { return stop.load(); });
    ASSERT_TRUE(c->wait_showing("Stop me?"));
    stop = true;
    EXPECT_EQ(a.wait(), Answer::Cancelled);
    EXPECT_TRUE(wait_until([&] { return !c->dialog_open(); }, 2000));
    EXPECT_EQ(msgboxes(), 0);
}

// The dialog with a long question, on each of the three screen shapes:
// the box and both buttons lie wholly on the visible screen.
struct ShapeCase {
    const char* shape;
    int corner_radius;
};

class ConfirmShapes : public ::testing::TestWithParam<ShapeCase> {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
    }
    void TearDown() override { wisp::log::reset_for_test(); }
};

TEST_P(ConfirmShapes, DialogInsideVisibleArea) {
    ConfirmHarness c(GetParam().shape, GetParam().corner_radius);
    ASSERT_TRUE(c.ok);
    std::string text = wisp::ui::tr_fill(StrId::AgentConfirmDelete, "番茄钟番茄钟番茄钟");
    text += wisp::ui::tr(StrId::AgentConfirmWithData);
    Asker a(*c.confirm, text);
    ASSERT_TRUE(c.wait_showing(text));
    c.on_ui([&] {
        lv_obj_t* box = wisp::test::find_obj_of_class(lv_layer_top(), &lv_msgbox_class);
        ASSERT_NE(box, nullptr);
        lv_obj_update_layout(box);
        std::vector<lv_obj_t*> objs{box};
        for (StrId id : {StrId::AgentAllow, StrId::AgentDeny}) {
            lv_obj_t* btn = wisp::test::find_button_with_label(lv_layer_top(), wisp::ui::tr(id));
            ASSERT_NE(btn, nullptr);
            objs.push_back(btn);
        }
        for (lv_obj_t* o : objs) {
            lv_area_t area;
            lv_obj_get_coords(o, &area);
            EXPECT_TRUE(wisp::test::box_visible(c.screen, area))
                << GetParam().shape << " " << area.x1 << "," << area.y1 << "-" << area.x2 << ","
                << area.y2;
        }
    });
    ASSERT_TRUE(c.press(false));
    EXPECT_EQ(a.wait(), Answer::Denied);
}

INSTANTIATE_TEST_SUITE_P(Shapes, ConfirmShapes,
                         ::testing::Values(ShapeCase{"rect", 0}, ShapeCase{"rounded_rect", 70},
                                           ShapeCase{"circle", 0}),
                         [](const ::testing::TestParamInfo<ShapeCase>& info) {
                             return std::string(info.param.shape);
                         });
