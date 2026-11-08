// The conversation history page (wisp/shell/history.hpp, mvp spec 6.4):
// how messages fold into turns, and the page over a real AgentService
// whose conversation the test seeds on the storage worker.

#include "wisp/shell/history.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "shell/page_fixture.hpp"
#include "support/agent_fixture.hpp"
#include "support/fake_llm.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/symbols.hpp"

using wisp::agent::Message;
using wisp::agent::Role;
using wisp::agent::Source;
using wisp::shell::group_turns;
using wisp::shell::HistoryPage;
using wisp::shell::Page;
using wisp::test::coords_of;
using wisp::test::is_hidden;
using wisp::test::text_of;

namespace {

Message msg(uint32_t turn, Role role, const std::string& text, Source source = Source::Web) {
    Message m;
    m.turn = turn;
    m.role = role;
    m.text.assign(text.data(), text.size());
    m.source = source;
    return m;
}

std::string str(const wisp::os::LargeString& s) { return {s.data(), s.size()}; }

}  // namespace

TEST(GroupTurns, UserSayAndBodyPerTurn) {
    std::vector<Message> msgs;
    Message begin = msg(1, Role::Marker, "");
    begin.marker.assign("turn_begin");
    msgs.push_back(begin);
    msgs.push_back(msg(1, Role::User, "开灯", Source::Voice));
    Message a = msg(1, Role::Assistant, "<say>好。</say>细节");
    msgs.push_back(a);
    msgs.push_back(msg(1, Role::Tool, R"({"ok":true})"));
    msgs.push_back(msg(1, Role::Assistant, "更多"));
    msgs.push_back(msg(2, Role::User, "今天几号"));
    msgs.push_back(msg(2, Role::Assistant, "<say>七号。</say>"));
    msgs.push_back(msg(2, Role::Note, "没有网络"));
    msgs.push_back(msg(3, Role::Summary, "之前聊过天气"));

    const auto turns = group_turns(msgs);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].turn, 1u);
    EXPECT_EQ(turns[0].source, Source::Voice);
    EXPECT_EQ(str(turns[0].heard), "开灯");
    EXPECT_EQ(str(turns[0].say), "好。");
    EXPECT_EQ(str(turns[0].body), "细节更多");
    EXPECT_EQ(turns[1].source, Source::Web);
    EXPECT_EQ(str(turns[1].say), "七号。");
    EXPECT_EQ(str(turns[1].body), "没有网络");
}

namespace {

class HistoryTest : public ::testing::Test {
protected:
    void TearDown() override {
        h_.reset();  // the shell first: it reads the agent
        if (agent_) agent_->stop();
        agent_.reset();
        if (harness_) harness_->server.release();
        harness_.reset();
    }

    // `turns` turns, every third one spoken, each with a body.
    void start(size_t turns, const std::string& shape = "rect") {
        harness_ = std::make_unique<wisp::test::AgentHarness>();
        ASSERT_TRUE(harness_->worker.run_sync(
            [&] {
                for (size_t i = 1; i <= turns; ++i) {
                    const auto t = static_cast<uint32_t>(i);
                    const Source src = i % 3 == 0 ? Source::Voice : Source::Web;
                    ASSERT_TRUE(harness_->store->append(
                        msg(t, Role::User, "问题 " + std::to_string(i), src)));
                    ASSERT_TRUE(harness_->store->append(msg(
                        t, Role::Assistant,
                        "<say>回答 " + std::to_string(i) + "。</say>细节 " + std::to_string(i))));
                }
            },
            wisp::os::kForever));
        agent_ = std::make_unique<wisp::agent::AgentService>(harness_->deps());
        ASSERT_TRUE(agent_->start());
        h_ = std::make_unique<wisp::test::PageHarness>(shape);
        ASSERT_TRUE(h_->started());
        h_->deps().agent = agent_.get();
        h_->boot();
        h_->show(Page::History);
        ASSERT_NE(page(), nullptr);
    }

    HistoryPage* page() { return h_->shell().history(); }
    size_t archives() {
        size_t n = 0;
        (void)harness_->worker.run_sync([&] { n = harness_->store->archives().value().size(); },
                                        wisp::os::kForever);
        return n;
    }

    std::unique_ptr<wisp::test::AgentHarness> harness_;
    std::unique_ptr<wisp::agent::AgentService> agent_;
    std::unique_ptr<wisp::test::PageHarness> h_;
};

}  // namespace

TEST_F(HistoryTest, OpensAtTheBottomWithFiftyTurns) {
    start(120);
    h_->on_ui([&] {
        EXPECT_EQ(page()->total(), 120u);
        EXPECT_EQ(page()->built(), wisp::shell::kHistoryBatch);
        EXPECT_TRUE(page()->at_bottom());
        // The newest turn and the button under it are on the screen.
        const lv_area_t last = coords_of(page()->card(page()->built() - 1));
        EXPECT_TRUE(wisp::test::box_visible(h_->shape(), last));
        EXPECT_EQ(text_of(page()->say_label(page()->built() - 1)), "回答 120。");
        EXPECT_TRUE(wisp::test::box_visible(h_->shape(), coords_of(page()->new_button())));
        EXPECT_EQ(text_of(page()->say_label(0)), "回答 71。");
    });
}

TEST_F(HistoryTest, ScrollToTopLoadsFiftyMore) {
    start(120);
    lv_obj_t* old_first = nullptr;
    h_->on_ui([&] {
        old_first = page()->card(0);
        lv_obj_scroll_to_y(page()->list(), 0, LV_ANIM_OFF);
    });
    ASSERT_TRUE(h_->ui_until([&] { return page()->built() == 100; }));
    h_->on_ui([&] {
        lv_obj_update_layout(page()->list());
        // Turn 71, the top one before, is still where the list shows it.
        EXPECT_EQ(page()->card(50), old_first);
        EXPECT_EQ(text_of(page()->say_label(0)), "回答 21。");
        const lv_area_t list = coords_of(page()->list());
        const lv_area_t a = coords_of(old_first);
        EXPECT_GE(a.y1, list.y1);
        EXPECT_LT(a.y1, list.y2);
        EXPECT_GT(lv_obj_get_scroll_top(page()->list()), 0);
    });
    // And the last 20.
    h_->on_ui([&] { lv_obj_scroll_to_y(page()->list(), 0, LV_ANIM_OFF); });
    ASSERT_TRUE(h_->ui_until([&] { return page()->built() == 120; }));
}

TEST_F(HistoryTest, VoiceIconAndCollapsedBody) {
    start(3);
    h_->on_ui([&] {
        ASSERT_EQ(page()->built(), 3u);
        // Turn 3 was spoken, turn 2 typed.
        EXPECT_EQ(text_of(page()->heard_label(2)), WISP_SYMBOL_MICROPHONE " 问题 3");
        EXPECT_EQ(text_of(page()->heard_label(1)), "问题 2");
        // The label's font draws the microphone rather than a placeholder.
        lv_font_glyph_dsc_t mic{};
        const lv_font_t* f = lv_obj_get_style_text_font(page()->heard_label(2), LV_PART_MAIN);
        ASSERT_TRUE(lv_font_get_glyph_dsc(f, &mic, 0xF130, 0));
        EXPECT_GT(mic.box_w, 0);
        EXPECT_GT(mic.box_h, 0);
        EXPECT_EQ(page()->body_box(2), nullptr);
        ASSERT_NE(page()->fold_label(2), nullptr);
        EXPECT_FALSE(page()->expanded(2));
        lv_obj_send_event(page()->card(2), LV_EVENT_CLICKED, nullptr);
        ASSERT_TRUE(page()->expanded(2));
        ASSERT_NE(page()->body_box(2), nullptr);
        EXPECT_FALSE(is_hidden(page()->body_box(2)));
        EXPECT_NE(wisp::test::find_label_containing(page()->body_box(2), "细节 3"), nullptr);
        lv_obj_send_event(page()->card(2), LV_EVENT_CLICKED, nullptr);
        EXPECT_FALSE(page()->expanded(2));
        EXPECT_TRUE(is_hidden(page()->body_box(2)));
    });
}

TEST_F(HistoryTest, NewConversationAsksThenArchives) {
    start(5);
    EXPECT_EQ(archives(), 0u);
    h_->on_ui([&] { lv_obj_send_event(page()->new_button(), LV_EVENT_CLICKED, nullptr); });
    ASSERT_TRUE(h_->ui_until([&] { return h_->layer().confirm_open(); }));
    h_->on_ui([&] {
        EXPECT_NE(wisp::test::find_label_containing(lv_layer_top(), "开始新对话？当前对话会归档"),
                  nullptr);
    });
    ASSERT_TRUE(h_->click_dialog("是"));
    ASSERT_TRUE(h_->ui_until([&] { return page()->total() == 0; }));
    EXPECT_EQ(archives(), 1u);
    h_->on_ui([&] {
        EXPECT_NE(wisp::test::find_label_containing(page()->list(), "还没有对话"), nullptr);
    });

    // A turn that does not end: "new conversation" says it is busy.
    harness_->server.push({.status = 200,
                           .chunks = {wisp::test::FakeLlmServer::sse_text("想"),
                                      wisp::test::FakeLlmServer::sse_text("想")},
                           .hold_after = 1});
    ASSERT_TRUE(agent_->submit("你好", Source::Web));
    ASSERT_TRUE(
        wisp::test::wait_until([&] { return agent_->view().running_turn.has_value(); }, 5000));
    h_->on_ui([&] {
        lv_obj_send_event(page()->new_button(), LV_EVENT_CLICKED, nullptr);
        EXPECT_FALSE(h_->layer().confirm_open());
        EXPECT_FALSE(is_hidden(page()->busy_label()));
        EXPECT_EQ(text_of(page()->busy_label()), "Agent 正在回复，稍后再试");
    });
    agent_->stop_turn();
    harness_->server.release();
    EXPECT_EQ(archives(), 1u);
}

// Spec 6.3: an up swipe goes home only from the bottom end.
TEST_F(HistoryTest, UpSwipeHomeOnlyAtTheBottom) {
    start(30);
    lv_point_t mid{240, 300};
    h_->on_ui([&] { lv_obj_scroll_to_y(page()->list(), 200, LV_ANIM_OFF); });
    h_->drag(mid, {240, 150});
    EXPECT_EQ(h_->shell().page(), Page::History);
    h_->on_ui([&] { lv_obj_scroll_to_y(page()->list(), LV_COORD_MAX, LV_ANIM_OFF); });
    h_->drag(mid, {240, 150});
    ASSERT_TRUE(h_->ui_until([&] { return h_->shell().page() == Page::Home; }));
}

// A drag up on the list away from its bottom end scrolls the list and
// starts no page drag; at the bottom end the same drag goes home.
TEST_F(HistoryTest, HistoryListMidScrollDoesNotStartBack) {
    start(30);
    int32_t before = 0;
    h_->on_ui([&] {
        lv_obj_scroll_to_y(page()->list(), 200, LV_ANIM_OFF);
        before = lv_obj_get_scroll_y(page()->list());
    });
    h_->feed({{240, 300, 0}});
    for (int y = 290; y >= 200; y -= 10) {
        h_->feed({{240, static_cast<int16_t>(y), 0}});
        h_->on_ui([&] { EXPECT_FALSE(h_->shell().sliding()); });
    }
    h_->on_ui([&] { EXPECT_GT(lv_obj_get_scroll_y(page()->list()), before); });
    h_->feed({});
    wisp::os::sleep_ms(300);
    EXPECT_EQ(h_->shell().page(), Page::History);
}

TEST_F(HistoryTest, HistoryAtEdgeStartsBack) {
    start(30);
    h_->on_ui([&] { lv_obj_scroll_to_y(page()->list(), LV_COORD_MAX, LV_ANIM_OFF); });
    int32_t bottom = 0;
    h_->on_ui([&] { bottom = lv_obj_get_scroll_y(page()->list()); });
    h_->feed({{240, 300, 0}});
    h_->feed({{240, 280, 0}});
    h_->on_ui([&] { EXPECT_TRUE(h_->shell().sliding()); });
    for (int y = 260; y >= 100; y -= 20) h_->feed({{240, static_cast<int16_t>(y), 0}});
    // The list itself stays where it was.
    h_->on_ui([&] { EXPECT_EQ(lv_obj_get_scroll_y(page()->list()), bottom); });
    h_->feed({});
    ASSERT_TRUE(h_->ui_until([&] { return h_->shell().page() == Page::Home; }));
}
