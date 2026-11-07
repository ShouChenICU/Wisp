// The home screen (wisp/shell/home.hpp, mvp spec 6.2, 6.3): its layout on
// every screen shape, the fade, and the screen itself over a real
// ConversationController (shell/shell_fixture.hpp) on a live UiRuntime
// with a touch panel the tests drive one read at a time.

#include "wisp/shell/home.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "shell/shell_fixture.hpp"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/visible.hpp"
#include "support/wait.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/shell/shell.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

using wisp::shell::Home;
using wisp::shell::home_layout;
using wisp::shell::HomeLayout;
using wisp::shell::kFadeAfterMs;
using wisp::shell::Page;
using wisp::shell::TurnFade;
using wisp::shell::TurnView;
using wisp::test::box_visible;
using wisp::test::wait_until;

namespace {

std::string label_text(lv_obj_t* l) {
    const char* t = lv_label_get_text(l);
    return t != nullptr ? t : "";
}

lv_area_t coords(lv_obj_t* o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a;
}

bool hidden(lv_obj_t* o) {
    for (; o != nullptr; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return true;
    return false;
}

}  // namespace

TEST(TurnFadeTest, FifteenSecondsAfterTheLaterOfEndAndScroll) {
    TurnFade f;
    EXPECT_TRUE(f.faded(0));  // nothing shown yet
    f.turn_started();
    EXPECT_FALSE(f.faded(1'000'000));  // never while it runs
    f.interacted(5000);                // no effect while it runs
    f.turn_ended(10000);
    EXPECT_EQ(f.fades_at(), 10000 + kFadeAfterMs);
    EXPECT_FALSE(f.faded(10000 + kFadeAfterMs - 1));
    EXPECT_TRUE(f.faded(10000 + kFadeAfterMs));
    // A scroll after the end starts the wait again.
    f.interacted(20000);
    EXPECT_FALSE(f.faded(20000 + kFadeAfterMs - 1));
    EXPECT_TRUE(f.faded(20000 + kFadeAfterMs));
    // An earlier interaction does not shorten it.
    f.interacted(15000);
    EXPECT_FALSE(f.faded(20000 + kFadeAfterMs - 1));
    // Once faded, a touch does not bring the text back.
    f.interacted(20000 + kFadeAfterMs + 1);
    EXPECT_TRUE(f.faded(20000 + kFadeAfterMs + 2));
    EXPECT_EQ(f.fades_at(), 20000 + kFadeAfterMs);
    // A new turn shows again.
    f.turn_started();
    EXPECT_FALSE(f.faded(100000));
}

// --- layout on every shape --------------------------------------------------

namespace {

struct ShapeCase {
    const char* shape;
    int32_t width;
    int32_t height;
    // The turn's column with a hint up, at least.
    int32_t min_column_with_hint = wisp::shell::kMinColumnPx;
};

class HomeShapes : public ::testing::TestWithParam<ShapeCase> {
protected:
    void SetUp() override {
        const ShapeCase& c = GetParam();
        display_ = std::make_shared<wisp::test::FakeDisplay>("display.main", c.width, c.height, 1,
                                                             c.shape);
        shape_ = wisp::hal::screen_shape_from_caps(display_->info().caps).value();
    }

    wisp::hal::ScreenShape shape_;
    std::shared_ptr<wisp::test::FakeDisplay> display_;
};

}  // namespace

namespace {

bool overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

// Where the idle wisp's centre sits: kWispIdleCentrePercent down the safe
// area.
int32_t idle_centre_y(const wisp::hal::Area& safe) {
    return safe.y1 + (safe.y2 - safe.y1 + 1) * wisp::shell::kWispIdleCentrePercent / 100;
}

bool inside(const lv_area_t& in, const lv_area_t& out) {
    return in.x1 >= out.x1 && in.x2 <= out.x2 && in.y1 >= out.y1 && in.y2 <= out.y2;
}

std::string str(const lv_area_t& a) {
    return std::to_string(a.x1) + "," + std::to_string(a.y1) + " " + std::to_string(a.x2) + "," +
           std::to_string(a.y2);
}

}  // namespace

TEST_P(HomeShapes, LayoutInsideVisibleArea) {
    using wisp::shell::kBodyLinePx;
    const wisp::hal::Area safe = wisp::hal::safe_rect(shape_);
    for (const bool hint : {false, true}) {
        SCOPED_TRACE(hint ? "with a hint" : "no hint");
        const HomeLayout l = home_layout(shape_, false, hint);
        EXPECT_EQ(lv_area_get_width(&l.wisp),
                  wisp::ui::wisp_size_px(safe, wisp::shell::kWispIdlePercent));
        // Centred across the safe area, a little above its middle.
        EXPECT_NEAR((l.wisp.x1 + l.wisp.x2) / 2.0, (safe.x1 + safe.x2) / 2.0, 1.0);
        EXPECT_NEAR((l.wisp.y1 + l.wisp.y2) / 2.0, idle_centre_y(safe), 1.0);
        // The sparks die where they leave the wisp's box (WispModel), so
        // the box on the screen keeps them there too.
        EXPECT_TRUE(box_visible(shape_, l.wisp));
        EXPECT_GT(l.heard.x1, l.heard.x2);  // nothing else idle
        EXPECT_GT(l.column.x1, l.column.x2);
        if (hint) {
            EXPECT_GE(lv_area_get_height(&l.hint), 2 * kBodyLinePx);
            EXPECT_TRUE(box_visible(shape_, l.hint));
            EXPECT_GT(l.hint.y1, l.wisp.y2);
        } else {
            EXPECT_GT(l.hint.x1, l.hint.x2);
        }
    }
    for (const bool hint : {false, true}) {
        SCOPED_TRACE(hint ? "turn with a hint" : "turn, no hint");
        const HomeLayout l = home_layout(shape_, true, hint);
        EXPECT_EQ(lv_area_get_width(&l.wisp),
                  wisp::ui::wisp_size_px(safe, wisp::shell::kWispTurnPercent));
        std::vector<const lv_area_t*> boxes{&l.wisp, &l.heard, &l.column};
        if (hint) boxes.push_back(&l.hint);
        for (const lv_area_t* a : boxes) {
            EXPECT_GT(lv_area_get_width(a), 0);
            EXPECT_GT(lv_area_get_height(a), 0);
            EXPECT_TRUE(box_visible(shape_, *a)) << str(*a);
        }
        for (size_t i = 0; i < boxes.size(); ++i)
            for (size_t j = i + 1; j < boxes.size(); ++j)
                EXPECT_FALSE(overlap(*boxes[i], *boxes[j]))
                    << str(*boxes[i]) << " / " << str(*boxes[j]);
        // Heard above the wisp, the column below it, starting with say,
        // ending with the "more below" strip; the hint below it all.
        EXPECT_GE(lv_area_get_height(&l.heard), kBodyLinePx);
        EXPECT_LT(l.heard.y2, l.wisp.y1);
        EXPECT_LT(l.wisp.y2, l.column.y1);
        EXPECT_EQ(l.say.y1, l.column.y1);
        EXPECT_EQ(lv_area_get_height(&l.say), wisp::shell::kSayLinePx);
        EXPECT_TRUE(inside(l.say, l.column));
        EXPECT_EQ(l.more.y2, l.column.y2);
        EXPECT_EQ(lv_area_get_height(&l.more), wisp::shell::kMorePx);
        EXPECT_TRUE(inside(l.more, l.column));
        if (hint) {
            EXPECT_EQ(lv_area_get_height(&l.hint), wisp::shell::kHintTurnLines * kBodyLinePx);
            EXPECT_LT(l.column.y2 + 8, l.hint.y1);  // a clear gap
        } else {
            EXPECT_GT(l.hint.x1, l.hint.x2);
        }
        // Clear of the status capsule.
        EXPECT_GT(l.heard.y1, wisp::ui::SystemLayer::status_area(shape_).y2);
        // The say text's first two lines and the body's first line under
        // them; a 360 px circle with a hint has room for the two say lines,
        // and the "more below" strip says the body follows.
        EXPECT_GE(lv_area_get_height(&l.column),
                  hint ? GetParam().min_column_with_hint : wisp::shell::kMinColumnPx)
            << str(l.column);
    }
}

// A turn with a three-line say text and a 40-line body, with and without
// a hint, as drawn: no two text boxes overlap; scrolled to the top, the
// say text's first line and the "more below" strip are on the visible
// screen; at either scroll end the column's first and last lines are too.
TEST_P(HomeShapes, TurnTextReadableAtBothEnds) {
    wisp::ui::UiRuntime ui(display_, nullptr);
    ASSERT_TRUE(ui.start());
    wisp::core::EventBus bus;
    ASSERT_TRUE(bus.start());
    auto on_ui = [&](const std::function<void()>& fn) { wisp::test::run_on_ui_blocking(&ui, fn); };
    Home home(wisp::shell::HomeDeps{
        .ui = ui, .shape = shape_, .bus = bus, .now_ms = {}, .admin_url = {}});
    lv_obj_t* before = nullptr;
    on_ui([&] {
        before = lv_screen_active();
        ASSERT_TRUE(home.start());
        lv_screen_load(home.screen());
    });
    const std::string say =
        "好的，这是很长的回答，它的第一句话就足够长，要在屏幕上折成好几行才能放下，"
        "后面还有更多的字，一直写到第三行以后，才轮到下面的正文细节出场。";
    for (const bool with_hint : {false, true}) {
        SCOPED_TRACE(with_hint ? "with a hint" : "no hint");
        on_ui([&] {
            TurnView v;
            v.turn = with_hint ? 2 : 1;
            v.heard = "讲个长的";
            v.say.assign(say.data(), say.size());
            std::string body;
            for (int i = 1; i <= 40; ++i) body += "第 " + std::to_string(i) + " 行\n\n";
            v.body.assign(body.data(), body.size());
            if (with_hint) v.error = wisp::voice::VoiceError::NoTtsProfile;
            home.show_view(v);
            lv_obj_update_layout(home.screen());
            ASSERT_TRUE(home.in_turn());
            lv_obj_t* col = home.column();
            ASSERT_GT(lv_obj_get_child_count(col), 1u);
            lv_obj_t* first = lv_obj_get_child(col, 0);
            lv_obj_t* last = lv_obj_get_child(col, lv_obj_get_child_count(col) - 1);
            ASSERT_EQ(first, home.say_label());
            // The body font (with the CJK glyphs), not LVGL's built-in default.
            EXPECT_EQ(lv_obj_get_style_text_font(last, LV_PART_MAIN), wisp::ui::default_font());
            EXPECT_EQ(lv_obj_get_style_text_font(home.heard_label(), LV_PART_MAIN),
                      wisp::ui::default_font());
            // The say text is whole, over several lines.
            EXPECT_EQ(label_text(first), say);
            EXPECT_GE(lv_obj_get_height(first),
                      3 * lv_font_get_line_height(wisp::ui::font(wisp::shell::kSayFontPx)));
            EXPECT_GT(lv_obj_get_scroll_bottom(col), 0);  // it does scroll

            const lv_area_t box = coords(col);
            const lv_area_t heard = coords(home.heard_label());
            const lv_area_t wisp = coords(home.wisp()->obj());
            std::vector<lv_area_t> drawn{box, heard, wisp};
            EXPECT_EQ(hidden(home.hint_label()), !with_hint);
            if (with_hint) {
                const lv_area_t hint = coords(home.hint_label());
                EXPECT_TRUE(box_visible(shape_, hint)) << str(hint);
                EXPECT_EQ(lv_area_get_height(&hint), wisp::shell::kBodyLinePx);
                // The short form: one line, no address.
                const std::string t = label_text(home.hint_label());
                EXPECT_EQ(t.find('\n'), std::string::npos) << t;
                EXPECT_EQ(t.rfind("还没配置语音合成", 0), 0u) << t;
                drawn.push_back(hint);
            }
            for (size_t i = 0; i < drawn.size(); ++i)
                for (size_t j = i + 1; j < drawn.size(); ++j)
                    EXPECT_FALSE(overlap(drawn[i], drawn[j]))
                        << str(drawn[i]) << " / " << str(drawn[j]);
            EXPECT_TRUE(box_visible(shape_, box));
            EXPECT_GE(lv_area_get_height(&box),
                      with_hint ? GetParam().min_column_with_hint : wisp::shell::kMinColumnPx);

            lv_obj_scroll_to_y(col, 0, LV_ANIM_OFF);
            lv_obj_update_layout(home.screen());
            lv_area_t a = coords(first);
            EXPECT_EQ(a.y1, box.y1);
            a.y2 = a.y1 + wisp::shell::kSayLinePx - 1;
            EXPECT_TRUE(inside(a, box));
            EXPECT_TRUE(box_visible(shape_, a));
            ASSERT_FALSE(hidden(home.more()));
            const lv_area_t more = coords(home.more());
            EXPECT_TRUE(inside(more, box)) << str(more);
            EXPECT_EQ(more.y2, box.y2);
            EXPECT_TRUE(box_visible(shape_, more));

            lv_obj_scroll_to_y(col, LV_COORD_MAX, LV_ANIM_OFF);
            lv_obj_update_layout(home.screen());
            EXPECT_EQ(lv_obj_get_scroll_bottom(col), 0);
            EXPECT_TRUE(hidden(home.more()));  // nothing more below
            // The body is one tall block: its last line, at the bottom of it.
            a = coords(last);
            EXPECT_LE(a.y2, box.y2);
            a.y1 = a.y2 - wisp::shell::kBodyLinePx + 1;
            EXPECT_GE(a.y1, box.y1);
            EXPECT_TRUE(box_visible(shape_, a));
        });
    }
    on_ui([&] {
        lv_screen_load(before);
        home.stop();
    });
    bus.stop();
    ui.stop();
}

// The step list, as laid out: it takes the empty column's room, one line
// a row, on rows the screen shows and clear of the wisp, the heard text
// and the hint, and lays nothing else out differently; the summary alone
// is the column's first line, so the layout is the plain one.
TEST_P(HomeShapes, StepRowsInsideVisibleArea) {
    using wisp::shell::Activity;
    using wisp::shell::kBodyLinePx;
    for (const bool hint : {false, true}) {
        SCOPED_TRACE(hint ? "hint" : "no hint");
        const HomeLayout plain = home_layout(shape_, true, hint);
        const HomeLayout sum = home_layout(shape_, true, hint, Activity::Summary);
        const HomeLayout l = home_layout(shape_, true, hint, Activity::List);
        EXPECT_EQ(sum.row_count, 0u);
        EXPECT_EQ(plain.row_count, 0u);
        for (const HomeLayout* o : {&sum, &l}) {
            EXPECT_EQ(str(o->column), str(plain.column));
            EXPECT_EQ(str(o->heard), str(plain.heard));
            EXPECT_EQ(str(o->wisp), str(plain.wisp));
            EXPECT_EQ(str(o->hint), str(plain.hint));
        }
        // At least two steps and the summary.
        EXPECT_GE(l.row_count, 3u);
        EXPECT_LE(l.row_count, wisp::shell::kActivityRows);
        std::vector<lv_area_t> boxes{l.wisp, l.heard};
        if (hint) boxes.push_back(l.hint);
        for (size_t i = 0; i < l.row_count; ++i) {
            const lv_area_t& r = l.rows[i];
            EXPECT_EQ(lv_area_get_height(&r), kBodyLinePx);
            EXPECT_GT(lv_area_get_width(&r), 0);
            EXPECT_TRUE(box_visible(shape_, r)) << str(r);
            // Within the column's rows, each as wide as its own rows allow.
            EXPECT_GE(r.y1, l.column.y1);
            EXPECT_LE(r.y2, l.column.y2);
            if (i > 0) EXPECT_GT(r.y1, l.rows[i - 1].y2);
            for (const lv_area_t& b : boxes)
                EXPECT_FALSE(overlap(r, b)) << str(r) << " / " << str(b);
        }
    }
}

namespace {

TurnView steps_view(int n, bool thinking) {
    TurnView v;
    v.turn = 3;
    v.heard = "做一个时钟场景";
    v.running = true;
    v.started_ms = 1;
    const char* labels[] = {"查看场景列表", "写入文件 main.lua",
                            "写入文件 ui/a/very/long/path/to/some/deeply/nested/file.lua",
                            "安装场景 clock", "打开场景 clock"};
    for (int i = 0; i < n; ++i) {
        const std::string l = labels[i % 5];
        if (v.step_count == wisp::shell::kTurnSteps) {
            for (size_t k = 1; k < wisp::shell::kTurnSteps; ++k) v.steps[k - 1] = v.steps[k];
            --v.step_count;
        }
        wisp::shell::TurnStep& st = v.steps[v.step_count++];
        st.label.assign(l.data(), l.size());
        st.st = i + 1 == n && !thinking ? wisp::shell::TurnStep::State::Running
                : i == 1                ? wisp::shell::TurnStep::State::Failed
                                        : wisp::shell::TurnStep::State::Done;
        ++v.steps_total;
    }
    v.thinking = thinking;
    return v;
}

}  // namespace

// The steps as drawn on every shape, with and without a hint: each row
// one line on the visible screen, the hint still one line; once text
// comes only the summary stays, and the say text's first line is whole;
// after the turn the summary says how long it took.
TEST_P(HomeShapes, StepListAsDrawn) {
    using wisp::shell::Activity;
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    wisp::ui::UiRuntime ui(display_, nullptr);
    ASSERT_TRUE(ui.start());
    wisp::core::EventBus bus;
    ASSERT_TRUE(bus.start());
    auto on_ui = [&](const std::function<void()>& fn) { wisp::test::run_on_ui_blocking(&ui, fn); };
    std::atomic<uint64_t> now{42'001};
    Home home(wisp::shell::HomeDeps{.ui = ui,
                                    .shape = shape_,
                                    .bus = bus,
                                    .now_ms = [&] { return now.load(); },
                                    .admin_url = {}});
    lv_obj_t* before = nullptr;
    on_ui([&] {
        before = lv_screen_active();
        ASSERT_TRUE(home.start());
        lv_screen_load(home.screen());
    });
    const int32_t line_h = lv_font_get_line_height(wisp::ui::default_font());
    for (const bool with_hint : {false, true}) {
        SCOPED_TRACE(with_hint ? "with a hint" : "no hint");
        now = 42'001;
        on_ui([&] {
            TurnView v = steps_view(6, /*thinking=*/true);
            v.turn = with_hint ? 5 : 4;
            if (with_hint) v.error = wisp::voice::VoiceError::NoTtsProfile;
            home.show_view(v);
            lv_obj_update_layout(home.screen());
            ASSERT_TRUE(home.in_turn());
            ASSERT_EQ(home.activity(), Activity::List);
            std::vector<lv_area_t> drawn{coords(home.heard_label()), coords(home.wisp()->obj())};
            if (with_hint) {
                const lv_area_t hint = coords(home.hint_label());
                EXPECT_EQ(lv_area_get_height(&hint), wisp::shell::kBodyLinePx);
                const std::string t = label_text(home.hint_label());
                EXPECT_EQ(t.find('\n'), std::string::npos) << t;
                drawn.push_back(hint);
            }
            size_t shown = 0;
            std::string last;
            for (size_t i = 0; i < wisp::shell::kTurnSteps + 1; ++i) {
                if (hidden(home.step_label(i))) continue;
                ++shown;
                ASSERT_FALSE(hidden(home.step_mark(i)));
                for (lv_obj_t* o : {home.step_mark(i), home.step_label(i)}) {
                    const lv_area_t a = coords(o);
                    EXPECT_TRUE(box_visible(shape_, a)) << str(a);
                    // One line, cut rather than wrapped.
                    EXPECT_EQ(lv_area_get_height(&a), wisp::shell::kBodyLinePx);
                    EXPECT_LE(lv_obj_get_self_height(o), line_h);
                    for (const lv_area_t& d : drawn)
                        EXPECT_FALSE(overlap(a, d)) << str(a) << " / " << str(d);
                }
                last = label_text(home.step_label(i));
            }
            EXPECT_GE(shown, 2u);
            // The newest last: the passing thinking step.
            EXPECT_EQ(last, wisp::ui::tr(wisp::ui::StrId::TalkThinking));
            ASSERT_FALSE(hidden(home.summary_label()));
            const lv_area_t sum = coords(home.summary_label());
            EXPECT_TRUE(box_visible(shape_, sum)) << str(sum);
            for (const lv_area_t& d : drawn)
                EXPECT_FALSE(overlap(sum, d)) << str(sum) << " / " << str(d);
            EXPECT_EQ(label_text(home.summary_label()), "第 6 步 · 42 秒");
        });
        // The time counts on without the controller.
        now += 30'000;
        ASSERT_TRUE(wait_until(
            [&] {
                std::string t;
                on_ui([&] { t = label_text(home.summary_label()); });
                return t == "第 6 步 · 1 分 12 秒";
            },
            3000));

        // Text comes: only the summary stays, above the column.
        on_ui([&] {
            TurnView v = steps_view(6, false);
            v.turn = with_hint ? 5 : 4;
            if (with_hint) v.error = wisp::voice::VoiceError::NoTtsProfile;
            const std::string say = "好了，时钟做好了，已经打开，你看看喜不喜欢。";
            v.say.assign(say.data(), say.size());
            home.show_view(v);
            lv_obj_update_layout(home.screen());
            ASSERT_EQ(home.activity(), Activity::Summary);
            for (size_t i = 0; i < wisp::shell::kTurnSteps + 1; ++i)
                EXPECT_TRUE(hidden(home.step_label(i)));
            std::vector<lv_area_t> drawn{coords(home.heard_label()), coords(home.wisp()->obj())};
            if (with_hint) drawn.push_back(coords(home.hint_label()));
            ASSERT_FALSE(hidden(home.summary_label()));
            // The column's first line, the say text right under it.
            lv_obj_t* col_obj = home.column();
            ASSERT_EQ(lv_obj_get_child(col_obj, 0), home.summary_label());
            ASSERT_EQ(lv_obj_get_child(col_obj, 1), home.say_label());
            lv_obj_scroll_to_y(col_obj, 0, LV_ANIM_OFF);
            lv_obj_update_layout(home.screen());
            const lv_area_t sum = coords(home.summary_label());
            const lv_area_t col = coords(col_obj);
            EXPECT_EQ(sum.y1, col.y1);
            EXPECT_EQ(lv_area_get_height(&sum), wisp::shell::kBodyLinePx);
            EXPECT_TRUE(inside(sum, col)) << str(sum);
            EXPECT_TRUE(box_visible(shape_, sum)) << str(sum);
            for (const lv_area_t& d : drawn)
                EXPECT_FALSE(overlap(sum, d)) << str(sum) << " / " << str(d);
            lv_area_t a = coords(home.say_label());
            EXPECT_GT(a.y1, sum.y2);
            a.y2 = a.y1 + wisp::shell::kSayLinePx - 1;
            EXPECT_TRUE(inside(a, col)) << str(a) << " in " << str(col);
            EXPECT_TRUE(box_visible(shape_, a));
            EXPECT_EQ(label_text(home.say_label()), say);
        });

        // The turn ends: what it took.
        on_ui([&] {
            TurnView v = steps_view(6, false);
            v.turn = with_hint ? 5 : 4;
            if (with_hint) v.error = wisp::voice::VoiceError::NoTtsProfile;
            v.say.assign("好了");
            v.running = false;
            v.ended_ms = v.started_ms + 72'000;
            home.show_view(v);
            lv_obj_update_layout(home.screen());
            ASSERT_EQ(home.activity(), Activity::Summary);
            ASSERT_FALSE(hidden(home.summary_label()));
            EXPECT_EQ(label_text(home.summary_label()), "用了 6 步 · 1 分 12 秒");
        });
    }
    // A turn without steps shows neither.
    on_ui([&] {
        TurnView v;
        v.turn = 9;
        v.heard = "你好";
        v.running = true;
        home.show_view(v);
        lv_obj_update_layout(home.screen());
        EXPECT_EQ(home.activity(), Activity::None);
        EXPECT_TRUE(hidden(home.summary_label()));
        for (size_t i = 0; i < wisp::shell::kTurnSteps + 1; ++i)
            EXPECT_TRUE(hidden(home.step_label(i)));
    });
    on_ui([&] {
        lv_screen_load(before);
        home.stop();
    });
    bus.stop();
    ui.stop();
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, HomeShapes,
    ::testing::Values(ShapeCase{"rect", 480, 480}, ShapeCase{"rounded_rect", 480, 480},
                      ShapeCase{"circle", 480, 480}, ShapeCase{"rounded_rect", 368, 448},
                      ShapeCase{"circle", 360, 480, 2 * wisp::shell::kSayLinePx}),
    [](const ::testing::TestParamInfo<ShapeCase>& info) {
        return std::string(info.param.shape) + "_" + std::to_string(info.param.width) + "x" +
               std::to_string(info.param.height);
    });

// --- the home screen over the conversation -------------------------------------

namespace {

class HomeTest : public wisp::test::ShellFixture {
protected:
    void boot(bool with_llm = true) {
        start(with_llm);
        display_ = std::make_shared<wisp::test::FakeDisplay>("display.main", 480, 480, 1, "rect");
        shape_ = wisp::hal::screen_shape_from_caps(display_->info().caps).value();
        touch_ = std::make_shared<wisp::test::FakeTouch>("touch.main");
        ui_ = std::make_unique<wisp::ui::UiRuntime>(display_, touch_);
        ASSERT_TRUE(ui_->start());
        on_ui([&] {
            wisp::ui::set_lang(wisp::ui::Lang::Zh);
            layer_ = std::make_unique<wisp::ui::SystemLayer>(shape_);
            indev_ = lv_indev_get_next(nullptr);
        });
        ASSERT_NE(indev_, nullptr);
        shell_ = std::make_unique<wisp::shell::Shell>(wisp::shell::ShellDeps{
            .ui = *ui_,
            .layer = *layer_,
            .shape = shape_,
            .bus = h->bus,
            .conversation = conv.get(),
            .open_diagnostics = {},
            .close_diagnostics = {},
            .leave_scene = {},
            .admin_url = [] { return std::string("http://10.0.0.7:8900"); },
            .now_ms = [this] { return wisp::os::now_ms() + home_ms.load(); },
        });
        on_ui([&] { shell_->start(); });
    }

    void TearDown() override {
        if (ui_) {
            on_ui([&] {
                if (shell_) shell_->stop();
                layer_.reset();
            });
            shell_.reset();
        }
        ShellFixture::TearDown();
        if (ui_) ui_->stop();
    }

    void on_ui(const std::function<void()>& fn) { wisp::test::run_on_ui_blocking(ui_.get(), fn); }
    Home& home() { return *shell_->home(); }

    // Waits until `pred` holds on the UI thread.
    bool ui_until(const std::function<bool()>& pred, uint32_t ms = 5000) {
        return wait_until(
            [&] {
                bool ok = false;
                on_ui([&] { ok = pred(); });
                return ok;
            },
            ms);
    }

    // One touch read: `points` empty is a release.
    void feed(std::vector<wisp::hal::TouchPoint> points) {
        touch_->set_points(std::move(points));
        on_ui([&] { lv_indev_read(indev_); });
    }
    void drag(lv_point_t from, lv_point_t to) {
        constexpr int kSteps = 6;
        for (int i = 0; i <= kSteps; ++i) {
            feed({{static_cast<int16_t>(from.x + (to.x - from.x) * i / kSteps),
                   static_cast<int16_t>(from.y + (to.y - from.y) * i / kSteps), 0}});
        }
        feed({});
    }
    lv_point_t wisp_centre() {
        lv_point_t p{};
        on_ui([&] {
            lv_obj_update_layout(home().screen());
            const lv_area_t a = coords(shell_->wisp()->obj());
            p = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        });
        return p;
    }
    // A finished turn whose column scrolls.
    void long_turn() {
        std::string body;
        for (int i = 1; i <= 40; ++i) body += "第 " + std::to_string(i) + " 行\n\n";
        server.llm().push(wisp::test::llm_text("<say>好的。</say>" + body));
        server.push_tts(wisp::test::tts_pcm());
        ASSERT_TRUE(conv->voice_text("讲个长的"));
        ASSERT_TRUE(wait_turns(1));
        ASSERT_TRUE(ui_until([&] {
            return home().in_turn() && !conv->current().running &&
                   lv_obj_get_scroll_bottom(home().column()) > 0;
        }));
        ASSERT_TRUE(settled());
    }
    // Moves the clock past the fade and waits for the idle layout.
    void fade_out() {
        home_ms.fetch_add(kFadeAfterMs + 1000);
        ASSERT_TRUE(ui_until([&] { return !home().in_turn() && hidden(home().heard_label()); }));
    }
    bool idle_and_centred() {
        bool ok = false;
        on_ui([&] {
            home().refresh();
            lv_obj_update_layout(home().screen());
            const wisp::hal::Area safe = wisp::hal::safe_rect(shape_);
            const lv_area_t a = coords(shell_->wisp()->obj());
            ok = !home().in_turn() && hidden(home().heard_label()) &&
                 lv_area_get_width(&a) ==
                     wisp::ui::wisp_size_px(safe, wisp::shell::kWispIdlePercent);
        });
        return ok;
    }
    lv_obj_t* active() {
        lv_obj_t* s = nullptr;
        on_ui([&] { s = lv_screen_active(); });
        return s;
    }
    bool logged(const std::string& needle) {
        for (auto& e : wisp::log::ring_entries())
            if (e.tag == "shell" && e.msg.find(needle) != std::string::npos) return true;
        return false;
    }

    std::shared_ptr<wisp::test::FakeDisplay> display_;
    std::shared_ptr<wisp::test::FakeTouch> touch_;
    wisp::hal::ScreenShape shape_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
    std::unique_ptr<wisp::ui::SystemLayer> layer_;
    std::unique_ptr<wisp::shell::Shell> shell_;
    lv_indev_t* indev_ = nullptr;
    std::atomic<uint64_t> home_ms{0};
};

}  // namespace

// A voice turn and a web turn both show their heard text above the wisp,
// the say text under it and the body below.
TEST_F(HomeTest, TurnShowsHeardSayAndBody) {
    boot();
    server.llm().push(wisp::test::llm_text("<say>好的。</say>细节 **粗**\n- 一"));
    server.push_tts(wisp::test::tts_pcm());
    ASSERT_TRUE(conv->voice_text("你好"));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(ui_until([&] {
        Home& m = home();
        return m.in_turn() && label_text(m.heard_label()) == "你好" &&
               label_text(m.say_label()) == "好的。" && lv_obj_get_child_count(m.column()) == 2 &&
               label_text(lv_obj_get_child(m.column(), 1)) == "细节 粗\n• 一";
    }));
    on_ui([&] {
        EXPECT_FALSE(hidden(home().heard_label()));
        EXPECT_FALSE(hidden(home().say_label()));
        const wisp::hal::Area safe = wisp::hal::safe_rect(shape_);
        EXPECT_EQ(lv_obj_get_width(shell_->wisp()->obj()),
                  wisp::ui::wisp_size_px(safe, wisp::shell::kWispTurnPercent));
    });
    ASSERT_TRUE(settled());

    // From the web page: shown the same way.
    server.llm().push(wisp::test::llm_text("<say>网页的回答</say>"));
    ASSERT_TRUE(agent->submit("网页问", wisp::agent::Source::Web));
    ASSERT_TRUE(wait_turns(2));
    EXPECT_TRUE(ui_until([&] {
        return label_text(home().heard_label()) == "网页问" &&
               label_text(home().say_label()) == "网页的回答" &&
               lv_obj_get_child_count(home().column()) == 1;
    }));
}

// kFadeAfterMs after the turn the text fades and the wisp is back in the
// centre at its idle size.
TEST_F(HomeTest, ReturnsToCentreAfterFade) {
    boot();
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());
    ASSERT_TRUE(conv->voice_text("在吗"));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(ui_until([&] { return home().in_turn() && !conv->current().running; }));
    // Not yet.
    home_ms.store(kFadeAfterMs - 1000);
    on_ui([&] { home().refresh(); });
    on_ui([&] { EXPECT_TRUE(home().in_turn()); });

    home_ms.store(kFadeAfterMs + 1000);
    const wisp::hal::Area safe = wisp::hal::safe_rect(shape_);
    EXPECT_TRUE(ui_until([&] {
        if (home().in_turn()) return false;
        lv_obj_update_layout(home().screen());
        const lv_area_t a = coords(shell_->wisp()->obj());
        return lv_area_get_width(&a) ==
                   wisp::ui::wisp_size_px(safe, wisp::shell::kWispIdlePercent) &&
               std::abs((a.x1 + a.x2) / 2 - (safe.x1 + safe.x2) / 2) <= 1 &&
               std::abs((a.y1 + a.y2) / 2 - idle_centre_y(safe)) <= 1 &&
               hidden(home().heard_label());
    }));
}

// The hint under the wisp: what is not set up, then the voice's errors and
// "not heard", each with the admin address where a setting is missing.
TEST_F(HomeTest, ReadinessAndErrorHints) {
    boot();
    const std::string url = "http://10.0.0.7:8900";
    auto hint = [&] {
        std::string s;
        on_ui([&] {
            home().refresh();
            s = hidden(home().hint_label()) ? "" : label_text(home().hint_label());
        });
        return s;
    };
    EXPECT_EQ(hint(), "");

    ready.store(wisp::shell::Readiness::NoLlm);
    EXPECT_EQ(hint(), std::string("还没配置模型，请在管理后台的模型页设置\n") + url);
    ready.store(wisp::shell::Readiness::NoWifi);
    EXPECT_EQ(hint(), "还没连接 Wi-Fi，右划进入设置 → Wi-Fi");
    ready.store(wisp::shell::Readiness::Ready);
    EXPECT_EQ(hint(), "");

    // "Not heard" from the voice chain.
    h->bus.publish(
        {"voice.state", "voice", 0,
         wisp::Properties{{"state", wisp::Value("idle")}, {"result", wisp::Value("not_heard")}}});
    EXPECT_TRUE(wait_until([&] { return hint() == "没听清"; }, 3000));

    h->bus.publish({"voice.state", "voice", 0,
                    wisp::Properties{{"state", wisp::Value("error")},
                                     {"reason", wisp::Value("no_asr_profile")}}});
    EXPECT_TRUE(wait_until(
        [&] { return hint() == "还没配置语音识别，请在管理后台的模型页设置\n" + url; }, 3000));
    // The error shows the wisp's error mood for a moment.
    EXPECT_TRUE(ui_until([&] {
        home().refresh();
        return shell_->wisp()->model().mood() == wisp::ui::WispMood::Error;
    }));

    // Gone kFadeAfterMs later.
    home_ms.store(kFadeAfterMs + 1000);
    EXPECT_EQ(hint(), "");
    on_ui([&] { EXPECT_EQ(shell_->wisp()->model().mood(), wisp::ui::WispMood::Idle); });
}

// Swipes on the home screen open the sub-pages and the opposite swipe comes
// back; a down swipe from the top edge is the panel's.
TEST_F(HomeTest, SwipesOpenPages) {
    boot();
    lv_obj_t* home_scr = active();
    ASSERT_EQ(home_scr, shell_->home_screen());

    drag({200, 300}, {340, 300});
    EXPECT_TRUE(ui_until([&] { return shell_->page() == Page::Settings; }));
    EXPECT_TRUE(logged("page settings"));
    EXPECT_EQ(active(), shell_->page_screen());
    drag({340, 300}, {200, 300});
    EXPECT_TRUE(ui_until([&] { return shell_->page() == Page::Home; }));
    EXPECT_EQ(active(), home_scr);

    drag({340, 300}, {200, 300});
    EXPECT_TRUE(ui_until([&] { return shell_->page() == Page::Scenes; }));
    drag({200, 300}, {340, 300});
    EXPECT_TRUE(ui_until([&] { return shell_->page() == Page::Home; }));

    drag({100, 100}, {100, 250});
    EXPECT_TRUE(ui_until([&] { return shell_->page() == Page::History; }));
    drag({100, 300}, {100, 150});
    EXPECT_TRUE(ui_until([&] { return shell_->page() == Page::Home; }));

    // From the top zone: not the history.
    drag({100, 2}, {100, 200});
    wisp::os::sleep_ms(100);
    on_ui([&] {});
    EXPECT_EQ(shell_->page(), Page::Home);
    EXPECT_EQ(active(), home_scr);
}

// Holding the wisp is hold-to-talk; a tap starts nothing (it is kept for
// character interaction).
TEST_F(HomeTest, WispTapAndHoldReachTheController) {
    boot();
    const lv_point_t c = wisp_centre();
    const wisp::hal::TouchPoint at{static_cast<int16_t>(c.x), static_cast<int16_t>(c.y), 0};

    // A tap.
    feed({at});
    feed({at});
    feed({});
    wisp::os::sleep_ms(100);
    EXPECT_FALSE(conv->holding());
    EXPECT_FALSE(voice->capturing());
    EXPECT_FALSE(saw_state("listening"));

    // A hold: past kHoldStartMs the capture starts; the release sends it.
    say_into_mic();
    feed({at});
    feed({at});
    EXPECT_FALSE(conv->holding());
    home_ms.store(wisp::voice::kHoldStartMs + 10);
    feed({at});
    EXPECT_TRUE(conv->holding());
    EXPECT_TRUE(wait_until([&] { return voice->capturing(); }, 2000));
    feed({at});
    feed({});
    EXPECT_FALSE(conv->holding());
    EXPECT_TRUE(settled());
    EXPECT_EQ(shell_->page(), Page::Home);
}

// While the talk key holds, a swipe up on the home screen cancels the
// recording and opens nothing.
TEST_F(HomeTest, UpSwipeWhileKeyHeldCancels) {
    boot();
    conv->hold_begin();
    ASSERT_TRUE(conv->holding());
    drag({100, 350}, {100, 200});
    EXPECT_FALSE(conv->holding());
    EXPECT_EQ(shell_->page(), Page::Home);
    EXPECT_TRUE(settled());
    EXPECT_EQ(server.requests().size(), 0u);  // nothing was sent
}

// Screen off: the wisp draws no frames; on again: it does.
TEST_F(HomeTest, PausedWhileScreenOff) {
    boot();
    auto frames = [&] {
        uint32_t n = 0;
        on_ui([&] { n = shell_->wisp()->frames(); });
        return n;
    };
    ASSERT_TRUE(wait_until([&] { return frames() > 2; }, 3000));
    h->bus.publish(
        {"power.state", "power", 0, wisp::Properties{{"state", wisp::Value("standby")}}});
    h->bus.wait_idle(1000);
    wisp::os::sleep_ms(200);
    const uint32_t off = frames();
    wisp::os::sleep_ms(400);
    EXPECT_EQ(frames(), off);
    h->bus.publish({"power.state", "power", 0, wisp::Properties{{"state", wisp::Value("active")}}});
    EXPECT_TRUE(wait_until([&] { return frames() > off + 2; }, 3000));
}

// A finger scrolling the column is reading: the swipe opens nothing. Once
// the text has faded, the column's own reset to the top does not count as
// reading, and the text stays faded.
TEST_F(HomeTest, ScrolledTextStaysFaded) {
    boot();
    long_turn();
    lv_area_t col{};
    on_ui([&] { col = coords(home().column()); });
    const int32_t x = (col.x1 + col.x2) / 2;
    drag({x, col.y2 - 20}, {x, col.y2 - 160});
    wisp::os::sleep_ms(500);  // the throw runs out
    on_ui([&] { EXPECT_GT(lv_obj_get_scroll_y(home().column()), 0); });
    EXPECT_EQ(shell_->page(), Page::Home);
    EXPECT_EQ(active(), shell_->home_screen());

    fade_out();
    on_ui([&] { EXPECT_EQ(lv_obj_get_scroll_y(home().column()), 0); });
    wisp::os::sleep_ms(2 * Home::kTickMs);
    EXPECT_TRUE(idle_and_centred());
    EXPECT_TRUE(idle_and_centred());
}

// After the fade, a tap on the centred wisp and a hold to talk leave the
// screen idle: the last turn's text does not come back.
TEST_F(HomeTest, TouchAfterFadeKeepsIdle) {
    boot();
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());
    ASSERT_TRUE(conv->voice_text("在吗"));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(ui_until([&] { return home().in_turn() && !conv->current().running; }));
    ASSERT_TRUE(settled());
    fade_out();

    const lv_point_t c = wisp_centre();
    const wisp::hal::TouchPoint at{static_cast<int16_t>(c.x), static_cast<int16_t>(c.y), 0};
    feed({at});
    feed({at});
    feed({});
    EXPECT_TRUE(idle_and_centred());

    // A hold, then a slide up that cancels it.
    say_into_mic();
    feed({at});
    home_ms.fetch_add(wisp::voice::kHoldStartMs + 10);
    feed({at});
    ASSERT_TRUE(conv->holding());
    on_ui([&] {
        home().refresh();
        EXPECT_TRUE(hidden(home().heard_label()));
        EXPECT_TRUE(hidden(home().say_label()) || hidden(home().column()));
    });
    feed({{at.x, static_cast<int16_t>(at.y - 100), 0}});
    feed({});
    EXPECT_FALSE(conv->holding());
    EXPECT_TRUE(settled());
    EXPECT_TRUE(idle_and_centred());
    EXPECT_EQ(shell_->page(), Page::Home);
}

// A hold on the wisp that then slides sideways ends the hold on release
// and opens no page.
TEST_F(HomeTest, WispHoldThenSidewaysDoesNotNavigate) {
    boot();
    const lv_point_t c = wisp_centre();
    const wisp::hal::TouchPoint at{static_cast<int16_t>(c.x), static_cast<int16_t>(c.y), 0};
    say_into_mic();
    feed({at});
    home_ms.store(wisp::voice::kHoldStartMs + 10);
    feed({at});
    ASSERT_TRUE(conv->holding());
    const wisp::hal::TouchPoint side{static_cast<int16_t>(c.x + 150), at.y, 0};
    feed({side});
    feed({side});
    EXPECT_TRUE(conv->holding());
    feed({});
    EXPECT_FALSE(conv->holding());
    EXPECT_TRUE(settled());
    wisp::os::sleep_ms(100);
    on_ui([&] {});
    EXPECT_EQ(shell_->page(), Page::Home);
    EXPECT_EQ(active(), shell_->home_screen());
}

// A hint during a turn is one line without the address; once the text has
// faded the full hint shows, address included, for its own kFadeAfterMs.
TEST_F(HomeTest, TurnHintIsShortUntilTheTextFades) {
    boot();
    const std::string url = "http://10.0.0.7:8900";
    const std::string full = std::string(wisp::ui::tr(wisp::ui::StrId::VoiceErrNoTts)) + "\n" + url;
    auto hint = [&] {
        std::string s;
        on_ui([&] {
            home().refresh();
            s = hidden(home().hint_label()) ? "" : label_text(home().hint_label());
        });
        return s;
    };
    on_ui([&] {
        TurnView v;
        v.turn = 7;
        v.heard = "念一下";
        v.say.assign("好的。");
        v.error = wisp::voice::VoiceError::NoTtsProfile;
        home().show_view(v);
        EXPECT_TRUE(home().in_turn());
    });
    std::string h = hint();
    EXPECT_FALSE(h.empty());
    EXPECT_EQ(h.find('\n'), std::string::npos) << h;
    // Shown with the text all the while: the hint's own wait has not run.
    home_ms.store(kFadeAfterMs - 1000);
    EXPECT_FALSE(hint().empty());
    home_ms.store(kFadeAfterMs + 1000);
    ASSERT_TRUE(ui_until([&] { return !home().in_turn() && hidden(home().heard_label()); }));
    EXPECT_EQ(hint(), full);
    home_ms.store(2 * kFadeAfterMs);
    EXPECT_EQ(hint(), "");
}
