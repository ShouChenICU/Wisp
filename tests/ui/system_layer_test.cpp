// The system layer (wisp/ui/system_layer.hpp): the back gesture zone, the
// error panel, the on-screen keyboard and the modal confirm dialogs, and
// how they leave the opaque-overlay redraw skip (wisp/ui/overlay.hpp)
// alone.

#include "wisp/ui/system_layer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "lvgl_private.h"  // lv_buttonmatrix_t::button_areas
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/dialog.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/ui_runtime.hpp"

extern "C" {
extern const lv_font_t lv_font_cjk_16;
}

namespace {

using wisp::hal::Area;
using wisp::hal::ScreenShape;
using wisp::hal::ScreenShapeKind;
using wisp::test::check_action_row;
using wisp::test::FakeDisplay;
using wisp::test::FakeTouch;
using wisp::test::find_button_with_label;
using wisp::test::find_label_containing;
using wisp::test::run_on_ui_blocking;
using wisp::ui::StrId;
using wisp::ui::SystemLayer;

constexpr int kSide = 480;

ScreenShape rect480() { return {ScreenShapeKind::Rect, kSide, kSide, 0}; }
ScreenShape circle480() { return {ScreenShapeKind::Circle, kSide, kSide, 0}; }

bool inside(const lv_area_t& a, const Area& safe) {
    return a.x1 >= safe.x1 && a.y1 >= safe.y1 && a.x2 <= safe.x2 && a.y2 <= safe.y2;
}

bool overlaps(const Area& f, const lv_area_t& a) {
    return f.x1 <= a.x2 && f.x2 >= a.x1 && f.y1 <= a.y2 && f.y2 >= a.y1;
}

// Whether all four corners of `a` are visible on `s`.
bool fully_visible(const ScreenShape& s, const lv_area_t& a) {
    return wisp::hal::visible(s, a.x1, a.y1) && wisp::hal::visible(s, a.x2, a.y1) &&
           wisp::hal::visible(s, a.x1, a.y2) && wisp::hal::visible(s, a.x2, a.y2);
}

// "" if `btn` is a big dialog button sharing its row (wisp/ui/dialog.hpp)
// that lies inside `safe` and is visible on `s`; else the problem.
std::string check_dialog_button(lv_obj_t* btn, const ScreenShape& s, const Area& safe) {
    if (btn == nullptr) return "no button";
    std::string row =
        check_action_row(btn, wisp::ui::kDialogButtonMinHeight, wisp::ui::kDialogButtonGap);
    if (!row.empty()) return row;
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    const std::string where = std::to_string(a.x1) + "," + std::to_string(a.y1) + "-" +
                              std::to_string(a.x2) + "," + std::to_string(a.y2);
    if (!inside(a, safe)) return "button " + where + " leaves the safe area";
    if (!fully_visible(s, a)) return "button " + where + " is not visible";
    return "";
}

// The direct child of lv_layer_top() that holds `obj`.
lv_obj_t* top_level(lv_obj_t* obj) {
    while (obj != nullptr && lv_obj_get_parent(obj) != lv_layer_top()) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

// The button of a button matrix that reads `text`, or
// LV_BUTTONMATRIX_BUTTON_NONE.
uint32_t matrix_button(lv_obj_t* matrix, const char* text) {
    for (uint32_t id = 0; id < 128; ++id) {
        const char* t = lv_buttonmatrix_get_button_text(matrix, id);
        if (t == nullptr || t[0] == '\0') break;
        if (std::strcmp(t, text) == 0) return id;
    }
    return LV_BUTTONMATRIX_BUTTON_NONE;
}

// The pinyin candidate row: the shown button matrix on the top layer
// other than `kb`, or nullptr.
lv_obj_t* candidate_panel(lv_obj_t* kb) {
    lv_obj_t* cand = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); ++i) {
        lv_obj_t* c = lv_obj_get_child(lv_layer_top(), static_cast<int32_t>(i));
        if (c != kb && lv_obj_check_type(c, &lv_buttonmatrix_class) &&
            !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            cand = c;
        }
    }
    return cand;
}

lv_point_t center_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

// The keys of a button matrix in screen coordinates, one vector per row
// (hidden buttons left out).
std::vector<std::vector<lv_area_t>> key_rows(lv_obj_t* matrix) {
    auto* m = reinterpret_cast<lv_buttonmatrix_t*>(matrix);
    lv_area_t box;
    lv_obj_get_coords(matrix, &box);
    std::vector<std::vector<lv_area_t>> rows;
    int32_t row_y = INT32_MIN;
    for (uint32_t i = 0; i < m->btn_cnt; ++i) {
        if ((m->ctrl_bits[i] & LV_BUTTONMATRIX_CTRL_HIDDEN) != 0) continue;
        lv_area_t a = m->button_areas[i];
        lv_area_move(&a, box.x1, box.y1);
        if (a.y1 != row_y) {
            rows.emplace_back();
            row_y = a.y1;
        }
        rows.back().push_back(a);
    }
    return rows;
}

// Presses the key reading `text` the way a tap does: selects it and
// reports the value change.
bool press(lv_obj_t* matrix, const char* text) {
    uint32_t id = matrix_button(matrix, text);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return false;
    lv_buttonmatrix_set_selected_button(matrix, id);
    lv_obj_send_event(matrix, LV_EVENT_VALUE_CHANGED, nullptr);
    return true;
}

// A UiRuntime over a fake display and touch panel, with a SystemLayer
// built on its UI thread for `shape` (by default the display's own).
class SystemLayerTest : public ::testing::Test {
protected:
    void start(const std::string& display_shape = "circle",
               wisp::ui::Lang lang = wisp::ui::Lang::Zh, int corner_radius = 40) {
        wisp::ui::set_lang(lang);
        display = std::make_shared<FakeDisplay>("display.main", kSide, kSide, 1, display_shape);
        display->set_cap("corner_radius", corner_radius);
        touch = std::make_shared<FakeTouch>("touch.main");
        ui = std::make_unique<wisp::ui::UiRuntime>(display, touch);
        ASSERT_TRUE(ui->start());
        auto parsed = wisp::hal::screen_shape_from_caps(display->info().caps);
        ASSERT_TRUE(parsed);
        shape = parsed.value();
        safe = wisp::hal::safe_rect(shape);
        on_ui([&] {
            top_children_before = lv_obj_get_child_count(lv_layer_top());
            layer = std::make_unique<SystemLayer>(shape);
            indev = lv_indev_get_next(nullptr);
        });
    }

    void TearDown() override {
        if (ui) {
            on_ui([&] { layer.reset(); });
            ui->stop();
        }
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
    }

    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui.get(), fn); }

    // Feeds one touch read: `points` empty is a release.
    void feed(std::vector<wisp::hal::TouchPoint> points) {
        touch->set_points(std::move(points));
        on_ui([&] { lv_indev_read(indev); });
    }

    void tap(lv_point_t p) { tap(static_cast<int16_t>(p.x), static_cast<int16_t>(p.y)); }

    void tap(int16_t x, int16_t y) {
        feed({{x, y, 0}});
        feed({{x, y, 0}});
        feed({});
    }

    // A button on the active screen counting its clicks.
    lv_obj_t* make_counting_button(int32_t x, int32_t y, int32_t w, int32_t h, int* clicks) {
        lv_obj_t* btn = lv_button_create(lv_screen_active());
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, w, h);
        lv_obj_add_event_cb(
            btn, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_CLICKED, clicks);
        lv_obj_update_layout(btn);
        return btn;
    }

    std::shared_ptr<FakeDisplay> display;
    std::shared_ptr<FakeTouch> touch;
    std::unique_ptr<wisp::ui::UiRuntime> ui;
    std::unique_ptr<SystemLayer> layer;
    lv_indev_t* indev = nullptr;
    ScreenShape shape;
    Area safe{};
    uint32_t top_children_before = 0;
};

TEST(SystemLayerZone, BackZoneFollowsRowSpan) {
    const ScreenShape rect = rect480();
    const Area rs = wisp::hal::safe_rect(rect);
    for (int32_t y : {rs.y1, int32_t{240}, rs.y2}) {
        EXPECT_TRUE(SystemLayer::in_back_zone(rect, 0, y)) << y;
        EXPECT_TRUE(SystemLayer::in_back_zone(rect, 15, y)) << y;
        EXPECT_FALSE(SystemLayer::in_back_zone(rect, 16, y)) << y;
    }

    const ScreenShape circle = circle480();
    const Area cs = wisp::hal::safe_rect(circle);
    for (int32_t x = 0; x < 16; ++x) {
        EXPECT_TRUE(SystemLayer::in_back_zone(circle, x, 240)) << x;
    }
    EXPECT_FALSE(SystemLayer::in_back_zone(circle, 16, 240));

    const wisp::hal::Span s100 = wisp::hal::row_span(circle, 100);
    ASSERT_GT(s100.x1, 0);
    EXPECT_FALSE(SystemLayer::in_back_zone(circle, s100.x1 - 1, 100));
    EXPECT_TRUE(SystemLayer::in_back_zone(circle, s100.x1, 100));
    EXPECT_TRUE(SystemLayer::in_back_zone(circle, s100.x1 + 15, 100));
    EXPECT_FALSE(SystemLayer::in_back_zone(circle, s100.x1 + 16, 100));

    // Rows outside the safe area, even where they are visible.
    for (int32_t y : {int32_t{0}, cs.y1 - 1, cs.y2 + 1, int32_t{kSide - 1}}) {
        const wisp::hal::Span s = wisp::hal::row_span(circle, y);
        for (int32_t x = std::max(0, s.x1); x < s.x1 + 16; ++x) {
            EXPECT_FALSE(SystemLayer::in_back_zone(circle, x, y)) << x << "," << y;
        }
    }
}

TEST(SystemLayerZone, BackSwipeNeedsTriggerDistance) {
    const ScreenShape rect = rect480();
    constexpr int32_t d = SystemLayer::kBackTriggerDx;
    EXPECT_FALSE(SystemLayer::is_back_swipe(rect, {4, 240}, {4 + d - 1, 240}));
    EXPECT_TRUE(SystemLayer::is_back_swipe(rect, {4, 240}, {4 + d, 240}));
    EXPECT_TRUE(SystemLayer::is_back_swipe(rect, {4, 240}, {200, 240}));
    EXPECT_FALSE(SystemLayer::is_back_swipe(rect, {20, 240}, {300, 240}));  // starts outside
    // Any angle: only the distance to the right counts.
    EXPECT_TRUE(SystemLayer::is_back_swipe(rect, {4, 240}, {4 + d, 440}));  // steeply down
    EXPECT_TRUE(SystemLayer::is_back_swipe(rect, {4, 240}, {4 + d, 40}));   // steeply up
    EXPECT_FALSE(SystemLayer::is_back_swipe(rect, {4, 240}, {4 + d - 1, 440}));
    EXPECT_FALSE(SystemLayer::is_back_swipe(rect, {200, 240}, {4, 240}));  // leftwards
}

// A press in the zone goes to the widget under it like any other: a tap
// there clicks the button and never goes back.
TEST_F(SystemLayerTest, TapInTheZoneReachesTheWidgetBelow) {
    for (const char* s : {"rect", "circle"}) {
        SCOPED_TRACE(s);
        TearDown();
        start(s);
        int backs = 0;
        int clicks = 0;
        const int16_t y = 240;
        const int16_t x = static_cast<int16_t>(wisp::hal::row_span(shape, y).x1 + 2);
        ASSERT_TRUE(SystemLayer::in_back_zone(shape, x, y));
        on_ui([&] {
            make_counting_button(0, 200, kSide, 80, &clicks);
            layer->set_back_handler([&] { ++backs; });
        });
        tap(x, y);
        EXPECT_EQ(clicks, 1);
        EXPECT_EQ(backs, 0);
        on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });
    }
}

// Crossing the trigger distance takes the press from the widget: the swipe
// goes back on release and the button under it is neither clicked nor
// left pressed.
TEST_F(SystemLayerTest, SwipeFromTheZoneGoesBackInsteadOfClicking) {
    start("rect");
    int backs = 0;
    int clicks = 0;
    lv_obj_t* btn = nullptr;
    on_ui([&] {
        btn = make_counting_button(0, 200, kSide, 80, &clicks);
        layer->set_back_handler([&] { ++backs; });
    });
    feed({{4, 240, 0}});
    feed({{4, 240, 0}});
    on_ui([&] { EXPECT_TRUE(lv_obj_has_state(btn, LV_STATE_PRESSED)); });
    feed({{4 + SystemLayer::kBackTriggerDx, 242, 0}});
    on_ui([&] { EXPECT_FALSE(lv_obj_has_state(btn, LV_STATE_PRESSED)); });
    feed({{80, 244, 0}});
    feed({{160, 244, 0}});
    EXPECT_EQ(backs, 0);  // only on release
    feed({});
    EXPECT_EQ(backs, 1);
    EXPECT_EQ(clicks, 0);
    on_ui([&] { EXPECT_FALSE(lv_obj_has_state(btn, LV_STATE_PRESSED)); });

    // The next tap is the button's again.
    tap(200, 240);
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(backs, 1);
}

// A short move that ends before the trigger distance is an ordinary press:
// nothing is cancelled and nothing goes back.
TEST_F(SystemLayerTest, ReleaseBelowTheTriggerIsAnOrdinaryPress) {
    start("rect");
    int backs = 0;
    int clicks = 0;
    on_ui([&] {
        make_counting_button(0, 200, kSide, 80, &clicks);
        layer->set_back_handler([&] { ++backs; });
    });
    const int16_t end_x = static_cast<int16_t>(4 + SystemLayer::kBackTriggerDx - 1);
    feed({{4, 240, 0}});
    feed({{12, 240, 0}});
    feed({{end_x, 240, 0}});
    feed({});
    EXPECT_EQ(backs, 0);
    EXPECT_EQ(clicks, 1);
}

// Once past the trigger, dragging back below it before release cancels
// the gesture: no back, and the press stays cancelled.
TEST_F(SystemLayerTest, DraggingBackBelowTheTriggerCancels) {
    start("rect");
    int backs = 0;
    int clicks = 0;
    on_ui([&] {
        make_counting_button(0, 200, kSide, 80, &clicks);
        layer->set_back_handler([&] { ++backs; });
    });
    feed({{4, 240, 0}});
    feed({{60, 240, 0}});
    on_ui([&] {
        ASSERT_NE(layer->back_arrow(), nullptr);
        EXPECT_EQ(lv_obj_get_style_opa(layer->back_arrow(), LV_PART_MAIN), LV_OPA_COVER);
    });
    feed({{10, 240, 0}});
    on_ui([&] {
        ASSERT_NE(layer->back_arrow(), nullptr);
        EXPECT_LT(lv_obj_get_style_opa(layer->back_arrow(), LV_PART_MAIN), LV_OPA_COVER);
    });
    feed({});
    EXPECT_EQ(backs, 0);
    EXPECT_EQ(clicks, 0);
    on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });
}

// A swipe that slants up or down (about 35 degrees, as a thumb swipe
// often does) still goes back, sampled the way a finger moves.
TEST_F(SystemLayerTest, SlantedSwipeGoesBack) {
    for (int sign : {1, -1}) {
        if (ui) TearDown();
        start("rect");
        int backs = 0;
        on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
        feed({{4, 240, 0}});
        for (int16_t step = 1; step <= 10; ++step) {
            feed({{static_cast<int16_t>(4 + step * 10), static_cast<int16_t>(240 + sign * step * 7),
                   0}});
        }
        feed({});
        EXPECT_EQ(backs, 1) << "sign " << sign;
    }
}

// Once armed, a finger drifting up or down before release still goes
// back: only dragging back to the left cancels.
TEST_F(SystemLayerTest, ArmedSwipeSurvivesVerticalDrift) {
    start("rect");
    int backs = 0;
    on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
    feed({{4, 240, 0}});
    feed({{60, 240, 0}});
    feed({{70, 330, 0}});
    feed({});
    EXPECT_EQ(backs, 1);
}

// However steep the swipe, moving far enough right from the zone goes
// back, as on Android.
TEST_F(SystemLayerTest, SteepSwipeGoesBack) {
    start("rect");
    int backs = 0;
    on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
    feed({{4, 100, 0}});
    for (int16_t step = 1; step <= 10; ++step) {
        feed({{static_cast<int16_t>(4 + step * 3), static_cast<int16_t>(100 + step * 30), 0}});
    }
    feed({});
    EXPECT_EQ(backs, 1);
}

// A drag that first runs down the edge and then turns right still goes
// back: going up or down never gives the gesture up.
TEST_F(SystemLayerTest, DownThenRightGoesBack) {
    start("rect");
    int backs = 0;
    on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
    feed({{4, 100, 0}});
    for (int16_t y = 120; y <= 300; y = static_cast<int16_t>(y + 20)) feed({{6, y, 0}});
    feed({{40, 300, 0}});
    feed({});
    EXPECT_EQ(backs, 1);
}

// A drag that starts in the zone but runs up or down scrolls what is
// under it.
TEST_F(SystemLayerTest, VerticalDragFromTheZoneScrolls) {
    start("rect");
    int backs = 0;
    lv_obj_t* list = nullptr;
    on_ui([&] {
        list = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(list, 0, 0);
        lv_obj_set_size(list, kSide, kSide);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        for (int i = 0; i < 40; ++i) {
            lv_obj_t* row = lv_label_create(list);
            lv_label_set_text(row, ("row " + std::to_string(i)).c_str());
        }
        lv_obj_update_layout(list);
        layer->set_back_handler([&] { ++backs; });
    });
    feed({{4, 400, 0}});
    for (int16_t y = 380; y >= 200; y = static_cast<int16_t>(y - 20)) feed({{8, y, 0}});
    feed({});
    EXPECT_EQ(backs, 0);
    on_ui([&] {
        EXPECT_GT(lv_obj_get_scroll_y(list), 0);
        EXPECT_EQ(layer->back_arrow(), nullptr);
    });
}

TEST_F(SystemLayerTest, PressOutsideTheZoneNeverStartsAGesture) {
    start("rect");
    int backs = 0;
    on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
    feed({{SystemLayer::kBackZoneWidth, 240, 0}});
    for (int16_t x : {40, 100, 200, 300}) {
        feed({{x, 240, 0}});
        on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });
    }
    feed({});
    EXPECT_EQ(backs, 0);
}

// The arrow follows the finger's row, slides in and turns opaque as the
// finger nears the trigger distance, and goes away on release.
TEST_F(SystemLayerTest, ArrowFollowsTheFinger) {
    start("rect");
    on_ui([&] { layer->set_back_handler([] {}); });
    feed({{4, 240, 0}});
    on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });

    auto arrow_box = [&](lv_area_t* a, lv_opa_t* opa) {
        on_ui([&] {
            lv_obj_t* arrow = layer->back_arrow();
            ASSERT_NE(arrow, nullptr);
            EXPECT_EQ(lv_obj_get_parent(arrow), lv_layer_top());
            lv_obj_update_layout(arrow);
            lv_obj_get_coords(arrow, a);
            *opa = lv_obj_get_style_opa(arrow, LV_PART_MAIN);
        });
    };
    lv_area_t part{};
    lv_opa_t opa = 0;
    feed({{14, 246, 0}});
    arrow_box(&part, &opa);
    EXPECT_NEAR((part.y1 + part.y2) / 2, 246, 1);
    EXPECT_GT(opa, LV_OPA_TRANSP);
    EXPECT_LT(opa, LV_OPA_COVER);

    lv_area_t full{};
    feed({{4 + SystemLayer::kBackTriggerDx, 251, 0}});
    arrow_box(&full, &opa);
    EXPECT_NEAR((full.y1 + full.y2) / 2, 251, 1);
    EXPECT_GT(full.x1, part.x1);  // further in
    EXPECT_GE(full.x1, 0);
    EXPECT_EQ(opa, LV_OPA_COVER);

    // Past the trigger it keeps following the finger's row, fully in.
    lv_area_t later{};
    feed({{200, 300, 0}});
    arrow_box(&later, &opa);
    EXPECT_NEAR((later.y1 + later.y2) / 2, 300, 1);
    EXPECT_EQ(later.x1, full.x1);
    EXPECT_EQ(opa, LV_OPA_COVER);
    feed({});
    on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });
}

// On rounded and round screens the zone follows the visible edge, and the
// arrow, fully in, lies wholly on the visible screen even on the rows near
// the top and bottom of the safe area.
TEST_F(SystemLayerTest, ArrowStaysVisibleOnEveryShape) {
    for (const char* s : {"rect", "rounded_rect", "circle"}) {
        TearDown();
        start(s);
        int backs = 0;
        on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
        for (int32_t y : {safe.y1, safe.y1 + 30, int32_t{kSide / 2}, safe.y2 - 30, safe.y2}) {
            SCOPED_TRACE(std::string(s) + " y=" + std::to_string(y));
            const int16_t x0 = static_cast<int16_t>(wisp::hal::row_span(shape, y).x1 + 1);
            const auto y16 = static_cast<int16_t>(y);
            ASSERT_TRUE(SystemLayer::in_back_zone(shape, x0, y));
            feed({{x0, y16, 0}});
            feed({{static_cast<int16_t>(x0 + 60), y16, 0}});
            on_ui([&] {
                lv_obj_t* arrow = layer->back_arrow();
                ASSERT_NE(arrow, nullptr);
                lv_obj_update_layout(arrow);
                lv_area_t a;
                lv_obj_get_coords(arrow, &a);
                for (lv_point_t p : {lv_point_t{a.x1, a.y1}, lv_point_t{a.x2, a.y1},
                                     lv_point_t{a.x1, a.y2}, lv_point_t{a.x2, a.y2}}) {
                    EXPECT_TRUE(wisp::hal::visible(shape, p.x, p.y)) << p.x << "," << p.y;
                }
                // ...and close to the edge: a few pixels clear of the
                // visible edge on the most indented of its rows.
                int32_t edge = 0;
                for (int32_t row = a.y1; row <= a.y2; ++row) {
                    edge = std::max(edge, wisp::hal::row_span(shape, row).x1);
                }
                EXPECT_GE(a.x1, edge);
                EXPECT_LE(a.x1, edge + 4);
            });
            const int before = backs;
            feed({});
            EXPECT_EQ(backs, before + 1);
        }
        // Just outside the zone on a rounded row: no gesture.
        const int32_t y = safe.y1 + 30;
        const auto x =
            static_cast<int16_t>(wisp::hal::row_span(shape, y).x1 + SystemLayer::kBackZoneWidth);
        const int before = backs;
        feed({{x, static_cast<int16_t>(y), 0}});
        feed({{static_cast<int16_t>(x + 60), static_cast<int16_t>(y), 0}});
        feed({});
        EXPECT_EQ(backs, before) << s;
    }
}

TEST_F(SystemLayerTest, NoGestureWithoutABackHandler) {
    start("rect");
    int backs = 0;
    auto swipe = [&] {
        feed({{4, 240, 0}});
        feed({{60, 240, 0}});
        on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });
        feed({{200, 240, 0}});
        feed({});
    };
    swipe();

    on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
    feed({{4, 240, 0}});
    feed({{60, 240, 0}});
    // Cleared mid-swipe: the arrow goes and the release does nothing.
    on_ui([&] {
        ASSERT_NE(layer->back_arrow(), nullptr);
        layer->set_back_handler({});
        EXPECT_EQ(layer->back_arrow(), nullptr);
    });
    feed({});
    EXPECT_EQ(backs, 0);

    on_ui([&] { layer->set_back_handler({}); });
    swipe();
    EXPECT_EQ(backs, 0);
}

// Only one layer watches the pointer: a second one with a back handler
// leaves the first in charge (rather than wrapping its wrapper), and takes
// over once the first lets go and its own handler is set again.
TEST_F(SystemLayerTest, OneLayerWatchesThePointer) {
    start("rect");
    std::unique_ptr<SystemLayer> second;
    int first_backs = 0;
    int second_backs = 0;
    on_ui([&] {
        second = std::make_unique<SystemLayer>(shape);
        layer->set_back_handler([&] { ++first_backs; });
        second->set_back_handler([&] { ++second_backs; });
    });
    auto swipe = [&] {
        feed({{4, 240, 0}});
        feed({{60, 240, 0}});
        feed({});
    };
    swipe();
    EXPECT_EQ(first_backs, 1);
    EXPECT_EQ(second_backs, 0);

    on_ui([&] {
        layer->set_back_handler({});
        second->set_back_handler([&] { ++second_backs; });
    });
    swipe();
    EXPECT_EQ(first_backs, 1);
    EXPECT_EQ(second_backs, 1);
    on_ui([&] { second.reset(); });
    // With both gone the pointer reads as before.
    swipe();
    EXPECT_EQ(first_backs, 1);
    EXPECT_EQ(second_backs, 1);
}

// A swipe that started a sideways scroll under the edge and is then
// dragged back below the trigger leaves no overscroll behind: the scroller
// returns into its range.
TEST_F(SystemLayerTest, CancelledSwipeLeavesNoOverscroll) {
    start("rect");
    lv_obj_t* row = nullptr;
    on_ui([&] {
        row = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(row, 0, 200);
        lv_obj_set_size(row, kSide, 80);
        lv_obj_set_scroll_dir(row, LV_DIR_HOR);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        for (int i = 0; i < 8; ++i) {
            lv_obj_t* cell = lv_obj_create(row);
            lv_obj_set_size(cell, 150, 60);
        }
        lv_obj_update_layout(row);
        layer->set_back_handler([] {});
    });
    feed({{4, 240, 0}});
    for (int16_t x : {16, 30, 44, 60, 80}) feed({{x, 240, 0}});
    on_ui([&] { EXPECT_LT(lv_obj_get_scroll_x(row), 0); });  // pulled past its start
    feed({{10, 240, 0}});
    feed({});
    wisp::os::sleep_ms(600);  // the return is animated
    on_ui([&] {
        EXPECT_EQ(lv_obj_get_scroll_x(row), 0);
        EXPECT_FALSE(lv_obj_has_state(row, LV_STATE_SCROLLED));
    });
}

// The dialog's backdrop takes every touch, the edge included.
TEST_F(SystemLayerTest, NoGestureWhileADialogIsOpen) {
    start("rect");
    int backs = 0;
    on_ui([&] {
        layer->set_back_handler([&] { ++backs; });
        ASSERT_NE(layer->open_confirm(StrId::ResetTitle, "Sure?", [](bool) {}), nullptr);
    });
    feed({{4, 240, 0}});
    feed({{60, 240, 0}});
    on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });
    feed({{200, 240, 0}});
    feed({});
    EXPECT_EQ(backs, 0);
    on_ui([&] { EXPECT_TRUE(layer->confirm_open()); });
}

// The handler is set from inside a UI job (a scene's view coming up) and
// the touch panel may be read before the next refresh: a swipe that starts
// right away still goes back.
TEST_F(SystemLayerTest, SwipeRightAfterTheHandlerIsSet) {
    for (const char* s : {"rect", "circle"}) {
        SCOPED_TRACE(s);
        TearDown();
        start(s);
        int backs = 0;
        int clicks = 0;
        const int16_t y = 240;
        const int16_t x0 = static_cast<int16_t>(wisp::hal::row_span(shape, y).x1 + 2);
        on_ui([&] {
            make_counting_button(0, 200, kSide, 80, &clicks);
            layer->set_back_handler([&] { ++backs; });
            // No timer pass between setting the handler and these reads.
            for (int16_t x : {x0, x0, int16_t(x0 + 60), int16_t(x0 + 120), int16_t(x0 + 200)}) {
                touch->set_points({{x, y, 0}});
                lv_indev_read(indev);
            }
            touch->set_points({});
            lv_indev_read(indev);
        });
        EXPECT_EQ(backs, 1);
        EXPECT_EQ(clicks, 0);
        on_ui([&] { layer->set_back_handler({}); });
    }
}

TEST_F(SystemLayerTest, ErrorPanelInsideSafeRect) {
    start("circle");
    int reloads = 0;
    int backs = 0;
    auto show = [&] {
        layer->show_error(
            {"Demo", "main.lua", 12, "attempt to call a nil value"}, [&] { ++reloads; },
            [&] { ++backs; });
    };
    on_ui([&] {
        show();
        lv_obj_update_layout(lv_layer_top());
        ASSERT_TRUE(layer->error_visible());
        lv_obj_t* label = find_label_containing(lv_layer_top(), "main.lua:12");
        ASSERT_NE(label, nullptr);
        EXPECT_NE(find_label_containing(lv_layer_top(), "Demo"), nullptr);
        EXPECT_NE(find_label_containing(lv_layer_top(), "attempt to call a nil value"), nullptr);
        EXPECT_NE(find_label_containing(lv_layer_top(), wisp::ui::tr(StrId::SceneErrorTitle)),
                  nullptr);
        lv_obj_t* panel = top_level(label);
        lv_area_t a;
        lv_obj_get_coords(panel, &a);
        EXPECT_TRUE(inside(a, safe)) << a.x1 << "," << a.y1 << "-" << a.x2 << "," << a.y2;
        EXPECT_TRUE(wisp::test::all_label_glyphs_resolvable(lv_layer_top()));
        // It takes every touch the buttons do not, but neither scrolls
        // nor takes focus.
        EXPECT_TRUE(lv_obj_has_flag(panel, LV_OBJ_FLAG_CLICKABLE));
        EXPECT_FALSE(lv_obj_has_flag(panel, LV_OBJ_FLAG_SCROLLABLE));
        EXPECT_FALSE(lv_obj_has_flag(panel, LV_OBJ_FLAG_CLICK_FOCUSABLE));

        lv_obj_t* reload = find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::SceneReload));
        ASSERT_NE(reload, nullptr);
        lv_obj_send_event(reload, LV_EVENT_CLICKED, nullptr);
        EXPECT_EQ(reloads, 1);
        EXPECT_EQ(backs, 0);
        EXPECT_FALSE(layer->error_visible());

        show();
        lv_obj_t* back = find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::SceneBack));
        ASSERT_NE(back, nullptr);
        lv_obj_send_event(back, LV_EVENT_CLICKED, nullptr);
        EXPECT_EQ(reloads, 1);
        EXPECT_EQ(backs, 1);
        EXPECT_FALSE(layer->error_visible());
        EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before);
    });
}

TEST_F(SystemLayerTest, ErrorPanelButtonsTakeTaps) {
    start("circle");
    int reloads = 0;
    int backs = 0;
    lv_point_t reload_at{};
    lv_point_t back_at{};
    auto show = [&] {
        on_ui([&] {
            layer->show_error(
                {"Demo", "main.lua", 12, "boom"}, [&] { ++reloads; }, [&] { ++backs; });
            reload_at =
                center_of(find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::SceneReload)));
            back_at =
                center_of(find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::SceneBack)));
        });
    };
    show();
    tap(reload_at);
    EXPECT_EQ(reloads, 1);
    EXPECT_EQ(backs, 0);
    on_ui([&] { EXPECT_FALSE(layer->error_visible()); });

    show();
    tap(back_at);
    EXPECT_EQ(reloads, 1);
    EXPECT_EQ(backs, 1);
    on_ui([&] {
        EXPECT_FALSE(layer->error_visible());
        EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before);
    });
}

// The panel replaces the scene's UI (spec 3.6): a tap anywhere, on the
// card or around it, never reaches what it covers.
TEST_F(SystemLayerTest, ErrorPanelSwallowsTaps) {
    start("rect");
    int clicks = 0;
    int edge_clicks = 0;
    on_ui([&] {
        make_counting_button(200, 200, 80, 40, &clicks);
        // Near the top edge, above where the card sits.
        make_counting_button(200, 0, 80, 30, &edge_clicks);
    });
    tap(240, 220);
    tap(240, 15);
    ASSERT_EQ(clicks, 1);
    ASSERT_EQ(edge_clicks, 1);

    on_ui([&] { layer->show_error({"Demo", "main.lua", 12, "boom"}, [] {}, [] {}); });
    tap(240, 220);
    tap(240, 15);
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(edge_clicks, 1);
    on_ui([&] { EXPECT_TRUE(layer->error_visible()); });
}

// Every dialog's buttons are big enough for a thumb and share their row
// equally (wisp/ui/dialog.hpp), on every screen shape.
TEST_F(SystemLayerTest, DialogButtonsAreBigAndShareTheRow) {
    for (const char* s : {"rect", "rounded_rect", "circle"}) {
        SCOPED_TRACE(s);
        TearDown();
        start(s);
        on_ui([&] {
            layer->show_error({"Demo", "main.lua", 12, "boom"}, [] {}, [] {});
            lv_obj_update_layout(lv_layer_top());
            for (StrId id : {StrId::SceneReload, StrId::SceneBack}) {
                EXPECT_EQ(
                    check_dialog_button(find_button_with_label(lv_layer_top(), wisp::ui::tr(id)),
                                        shape, safe),
                    "");
            }
            layer->hide_error();

            lv_obj_t* box = layer->open_confirm(StrId::ResetTitle, "Sure?", [](bool) {});
            ASSERT_NE(box, nullptr);
            lv_obj_update_layout(lv_layer_top());
            for (StrId id : {StrId::Yes, StrId::No}) {
                EXPECT_EQ(
                    check_dialog_button(find_button_with_label(box, wisp::ui::tr(id)), shape, safe),
                    "");
            }
            layer->close_confirm(box);
        });
    }
}

// The error panel is a card as tall as what it holds, centred in the safe
// area: no blank band between the message and the buttons.
TEST_F(SystemLayerTest, ErrorCardFitsAShortMessage) {
    for (const char* s : {"rect", "rounded_rect", "circle"}) {
        SCOPED_TRACE(s);
        TearDown();
        start(s);
        on_ui([&] {
            layer->show_error({"Demo", "main.lua", 12, "boom"}, [] {}, [] {});
            lv_obj_update_layout(lv_layer_top());
            lv_obj_t* card = top_level(find_label_containing(lv_layer_top(), "main.lua:12"));
            ASSERT_NE(card, nullptr);
            lv_area_t a;
            lv_obj_get_coords(card, &a);
            const int32_t safe_h = safe.y2 - safe.y1 + 1;
            EXPECT_TRUE(inside(a, safe));
            EXPECT_EQ(a.x1, safe.x1);
            EXPECT_EQ(a.x2, safe.x2);
            EXPECT_LT(lv_area_get_height(&a), safe_h * 3 / 4);
            EXPECT_LE(std::abs((a.y1 - safe.y1) - (safe.y2 - a.y2)), 1)
                << a.y1 << "-" << a.y2 << " in " << safe.y1 << "-" << safe.y2;
        });
    }
}

// A message too long for the safe area scrolls inside the card, which
// stops at the safe area's height: the buttons stay whole and visible.
TEST_F(SystemLayerTest, ErrorCardScrollsALongMessage) {
    std::string message;
    for (int i = 0; i < 60; ++i) message += "attempt to call a nil value (field 'x') ";
    for (const char* s : {"rect", "rounded_rect", "circle"}) {
        SCOPED_TRACE(s);
        TearDown();
        start(s);
        int reloads = 0;
        lv_point_t reload_at{};
        on_ui([&] {
            layer->show_error({"Demo", "main.lua", 12, message}, [&] { ++reloads; }, [] {});
            lv_obj_update_layout(lv_layer_top());
            lv_obj_t* text = find_label_containing(lv_layer_top(), "main.lua:12");
            lv_obj_t* card = top_level(text);
            ASSERT_NE(card, nullptr);
            lv_area_t a;
            lv_obj_get_coords(card, &a);
            EXPECT_TRUE(inside(a, safe)) << a.y1 << "-" << a.y2;
            // The message, not the card, scrolls.
            lv_obj_t* area = lv_obj_get_parent(text);
            EXPECT_NE(area, card);
            EXPECT_TRUE(lv_obj_has_flag(area, LV_OBJ_FLAG_SCROLLABLE));
            EXPECT_GT(lv_obj_get_scroll_bottom(area), 0);
            for (StrId id : {StrId::SceneReload, StrId::SceneBack}) {
                lv_obj_t* btn = find_button_with_label(lv_layer_top(), wisp::ui::tr(id));
                EXPECT_EQ(check_dialog_button(btn, shape, safe), "");
                lv_area_t b;
                lv_obj_get_coords(btn, &b);
                EXPECT_TRUE(b.y1 >= a.y1 && b.y2 <= a.y2);
            }
            reload_at =
                center_of(find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::SceneReload)));
        });
        tap(reload_at);
        EXPECT_EQ(reloads, 1);
    }
}

TEST_F(SystemLayerTest, KeyboardTargetsTextarea) {
    start("circle");
    on_ui([&] {
        lv_obj_t* ta = lv_textarea_create(lv_screen_active());
        lv_obj_set_pos(ta, safe.x1, safe.y1);
        layer->show_keyboard(ta);
        lv_obj_t* kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        EXPECT_EQ(lv_keyboard_get_textarea(kb), ta);
        EXPECT_EQ(lv_obj_get_parent(kb), lv_layer_top());
        lv_obj_update_layout(lv_layer_top());
        lv_area_t a;

        // Pinyin: "ni" offers 你, which replaces the letters.
        ASSERT_TRUE(press(kb, "n"));
        ASSERT_TRUE(press(kb, "i"));
        EXPECT_STREQ(lv_textarea_get_text(ta), "ni");
        lv_obj_t* cand = candidate_panel(kb);
        ASSERT_NE(cand, nullptr);
        lv_obj_update_layout(cand);
        lv_obj_get_coords(cand, &a);
        EXPECT_EQ(a.y2 + 1, lv_obj_get_y(kb));  // right above the keyboard
        for (const auto& row : key_rows(cand)) {
            for (const lv_area_t& k : row) {
                for (lv_point_t p : {lv_point_t{k.x1, k.y1}, lv_point_t{k.x2, k.y1},
                                     lv_point_t{k.x1, k.y2}, lv_point_t{k.x2, k.y2}}) {
                    EXPECT_TRUE(wisp::hal::visible(shape, p.x, p.y)) << p.x << "," << p.y;
                }
            }
        }
        ASSERT_TRUE(press(cand, "你"));
        EXPECT_STREQ(lv_textarea_get_text(ta), "你");

        // The language key switches to plain letters.
        ASSERT_TRUE(press(kb, SystemLayer::kPinyinKey));
        kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        EXPECT_EQ(lv_keyboard_get_textarea(kb), ta);
        EXPECT_NE(matrix_button(kb, SystemLayer::kLatinKey), LV_BUTTONMATRIX_BUTTON_NONE);
        ASSERT_TRUE(press(kb, "n"));
        ASSERT_TRUE(press(kb, "i"));
        EXPECT_STREQ(lv_textarea_get_text(ta), "你ni");

        // OK puts it away.
        ASSERT_TRUE(press(kb, LV_SYMBOL_OK));
        EXPECT_EQ(layer->keyboard(), nullptr);
    });
    // Whatever went away inside its own event is gone a frame later.
    wisp::os::sleep_ms(100);
    on_ui([&] {
        uint32_t shown = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); ++i) {
            if (!lv_obj_has_flag(lv_obj_get_child(lv_layer_top(), static_cast<int32_t>(i)),
                                 LV_OBJ_FLAG_HIDDEN)) {
                ++shown;
            }
        }
        EXPECT_EQ(shown, 0u);
        EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before);
    });
}

// Every key of a keyboard (or candidate row) lies wholly on the visible
// screen, at least `inset` pixels inside the visible width of its rows.
void expect_keys_visible(const ScreenShape& shape, lv_obj_t* matrix, int32_t inset) {
    for (const auto& row : key_rows(matrix)) {
        for (const lv_area_t& k : row) {
            for (lv_point_t p : {lv_point_t{k.x1, k.y1}, lv_point_t{k.x2, k.y1},
                                 lv_point_t{k.x1, k.y2}, lv_point_t{k.x2, k.y2}}) {
                EXPECT_TRUE(wisp::hal::visible(shape, p.x, p.y)) << p.x << "," << p.y;
            }
            for (int32_t y = k.y1; y <= k.y2; ++y) {
                const wisp::hal::Span s = wisp::hal::row_span(shape, y);
                EXPECT_GE(k.x1, s.x1 + inset) << y;
                EXPECT_LE(k.x2, s.x2 - inset) << y;
            }
        }
    }
}

// The keyboard's background runs to the screen's left, right and bottom
// edges whatever the shape; the screen's own outline clips its corners.
TEST_F(SystemLayerTest, KeyboardBackgroundReachesTheEdges) {
    for (const char* s : {"rect", "rounded_rect", "circle"}) {
        SCOPED_TRACE(s);
        TearDown();
        start(s);
        on_ui([&] {
            lv_obj_t* ta = lv_textarea_create(lv_screen_active());
            lv_obj_set_pos(ta, safe.x1, safe.y1);
            layer->show_keyboard(ta);
            lv_obj_t* kb = layer->keyboard();
            ASSERT_NE(kb, nullptr);
            lv_obj_update_layout(lv_layer_top());
            lv_area_t a;
            lv_obj_get_coords(kb, &a);
            EXPECT_EQ(a.x1, 0);
            EXPECT_EQ(a.x2, kSide - 1);
            EXPECT_EQ(a.y2, kSide - 1);
            EXPECT_GE(a.y1, (safe.y1 + safe.y2) / 2 - kSide / 10);
            EXPECT_EQ(lv_obj_get_style_bg_opa(kb, LV_PART_MAIN), LV_OPA_COVER);
            // The candidate row likewise.
            ASSERT_TRUE(press(kb, "n"));
            lv_obj_t* cand = candidate_panel(kb);
            ASSERT_NE(cand, nullptr);
            lv_obj_update_layout(cand);
            lv_obj_get_coords(cand, &a);
            EXPECT_EQ(a.x1, 0);
            EXPECT_EQ(a.x2, kSide - 1);
            expect_keys_visible(shape, cand, 4);
        });
    }
}

// On a rectangle every key row is the full width, less the keyboard's
// small side inset.
TEST_F(SystemLayerTest, KeyboardRowsFullWidthOnARect) {
    start("rect");
    on_ui([&] {
        lv_obj_t* ta = lv_textarea_create(lv_screen_active());
        layer->show_keyboard(ta);
        lv_obj_t* kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        lv_obj_update_layout(lv_layer_top());
        const auto rows = key_rows(kb);
        ASSERT_EQ(rows.size(), 4u);
        for (const auto& row : rows) {
            EXPECT_EQ(row.front().x1, 4);
            EXPECT_EQ(row.back().x2, kSide - 1 - 4);
        }
        expect_keys_visible(shape, kb, 4);
    });
}

// The reference board's corners (radius 70): the rows clear of the bottom
// corners span the visible width less the inset; the bottom row, which
// reaches into them, is narrower and still wholly visible. Switching to
// capitals keeps the layout.
TEST_F(SystemLayerTest, KeyboardRowsFollowRoundedCorners) {
    start("rounded_rect", wisp::ui::Lang::Zh, 70);
    on_ui([&] {
        lv_obj_t* ta = lv_textarea_create(lv_screen_active());
        layer->show_keyboard(ta);
        lv_obj_t* kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        lv_obj_update_layout(lv_layer_top());
        auto check = [&] {
            const auto rows = key_rows(kb);
            ASSERT_EQ(rows.size(), 4u);
            // Rows clear of the corners span the visible width less the
            // inset; one just reaching into them only by a pixel or two.
            for (size_t r = 0; r + 1 < rows.size(); ++r) {
                SCOPED_TRACE(r);
                if (rows[r].front().y2 < kSide - 70) {
                    EXPECT_EQ(rows[r].front().x1, 4);
                    EXPECT_EQ(rows[r].back().x2, kSide - 1 - 4);
                } else {
                    EXPECT_LE(rows[r].front().x1, 4 + 2);
                    EXPECT_GE(rows[r].back().x2, kSide - 1 - 4 - 2);
                }
            }
            ASSERT_LT(rows[0].front().y2, kSide - 70);
            const auto& bottom = rows.back();
            EXPECT_GT(bottom.front().x1, 4 + 10);
            EXPECT_LT(bottom.back().x2, kSide - 1 - 4 - 10);
            expect_keys_visible(shape, kb, 4);
        };
        check();
        ASSERT_TRUE(press(kb, "ABC"));
        ASSERT_NE(matrix_button(kb, "Q"), LV_BUTTONMATRIX_BUTTON_NONE);
        check();
    });
}

// On a circle every row follows the circle and every key is wholly
// visible; the keyboard rises (at most to 55% of the height) so its rows
// get wide enough, but never over the textarea it types into.
TEST_F(SystemLayerTest, KeyboardRowsFollowACircle) {
    start("circle");
    for (int32_t ta_bottom : {safe.y1 + 40, int32_t{kSide / 2 - 10}}) {
        SCOPED_TRACE(ta_bottom);
        on_ui([&] {
            lv_obj_t* ta = lv_textarea_create(lv_screen_active());
            lv_obj_set_size(ta, 200, 40);
            lv_obj_set_pos(ta, kSide / 2 - 100, ta_bottom - 39);
            lv_obj_update_layout(ta);
            layer->show_keyboard(ta);
            lv_obj_t* kb = layer->keyboard();
            ASSERT_NE(kb, nullptr);
            lv_obj_update_layout(lv_layer_top());
            lv_area_t a;
            lv_obj_get_coords(kb, &a);
            EXPECT_LE(lv_area_get_height(&a), kSide * 55 / 100);
            // The textarea and the candidate row above the keyboard both
            // stay clear of it, unless the keyboard sits where it always
            // did (the lower half of the safe area).
            const int32_t default_top = safe.y1 + (safe.y2 - safe.y1 + 1) / 2;
            EXPECT_TRUE(a.y1 >= default_top || a.y1 - 44 > ta_bottom) << a.y1;
            expect_keys_visible(shape, kb, 4);
            const auto rows = key_rows(kb);
            ASSERT_EQ(rows.size(), 4u);
            layer->hide_keyboard();
            lv_obj_delete(ta);
        });
    }
}

// In an English UI the keyboard starts with plain letters; its language
// key still reaches pinyin.
TEST_F(SystemLayerTest, LanguageKeyInEnglishUi) {
    start("circle", wisp::ui::Lang::En);
    on_ui([&] {
        lv_obj_t* ta = lv_textarea_create(lv_screen_active());
        layer->show_keyboard(ta);
        lv_obj_t* kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        EXPECT_NE(matrix_button(kb, SystemLayer::kLatinKey), LV_BUTTONMATRIX_BUTTON_NONE);
        EXPECT_EQ(matrix_button(kb, SystemLayer::kPinyinKey), LV_BUTTONMATRIX_BUTTON_NONE);
        ASSERT_TRUE(press(kb, "n"));
        ASSERT_TRUE(press(kb, "i"));
        EXPECT_STREQ(lv_textarea_get_text(ta), "ni");
        EXPECT_EQ(candidate_panel(kb), nullptr);

        ASSERT_TRUE(press(kb, SystemLayer::kLatinKey));
        kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        EXPECT_NE(matrix_button(kb, SystemLayer::kPinyinKey), LV_BUTTONMATRIX_BUTTON_NONE);
        ASSERT_TRUE(press(kb, "n"));
        ASSERT_TRUE(press(kb, "i"));
        lv_obj_t* cand = candidate_panel(kb);
        ASSERT_NE(cand, nullptr);
        ASSERT_TRUE(press(cand, "你"));
        EXPECT_STREQ(lv_textarea_get_text(ta), "ni你");

        // Both labels are in the built-in font.
        for (uint32_t cp : {0x4E2Du /* 中 */, uint32_t{'E'}, uint32_t{'N'}}) {
            lv_font_glyph_dsc_t dsc;
            EXPECT_TRUE(lv_font_get_glyph_dsc(&lv_font_cjk_16, &dsc, cp, 0)) << cp;
        }
    });
}

// A keyboard moved to another textarea starts afresh: no half-typed
// pinyin of the old one replaces letters of the new one.
TEST_F(SystemLayerTest, RetargetStartsAfresh) {
    start("circle");
    on_ui([&] {
        lv_obj_t* ta1 = lv_textarea_create(lv_screen_active());
        lv_obj_t* ta2 = lv_textarea_create(lv_screen_active());
        lv_textarea_set_text(ta2, "abc");
        layer->show_keyboard(ta1);
        ASSERT_TRUE(press(layer->keyboard(), "n"));
        ASSERT_TRUE(press(layer->keyboard(), "i"));
        EXPECT_STREQ(lv_textarea_get_text(ta1), "ni");

        layer->show_keyboard(ta2);
        lv_obj_t* kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        EXPECT_EQ(lv_keyboard_get_textarea(kb), ta2);
        ASSERT_TRUE(press(kb, "n"));
        ASSERT_TRUE(press(kb, "i"));
        lv_obj_t* cand = candidate_panel(kb);
        ASSERT_NE(cand, nullptr);
        ASSERT_TRUE(press(cand, "你"));
        EXPECT_STREQ(lv_textarea_get_text(ta2), "abc你");
        EXPECT_STREQ(lv_textarea_get_text(ta1), "ni");
    });
}

// The scene's widgets may go away under the keyboard (a back swipe, a
// scene error, a re-render): the keyboard goes with its textarea, and a
// tap where its keys were types into nothing.
TEST_F(SystemLayerTest, KeyboardGoesWithItsTextarea) {
    start("rect");
    for (bool half_typed : {false, true}) {
        SCOPED_TRACE(half_typed);
        lv_point_t key_at{};
        on_ui([&] {
            lv_obj_t* ta = lv_textarea_create(lv_screen_active());
            layer->show_keyboard(ta);
            lv_obj_t* kb = layer->keyboard();
            ASSERT_NE(kb, nullptr);
            if (half_typed) ASSERT_TRUE(press(kb, "n"));
            lv_area_t a;
            lv_obj_get_coords(kb, &a);
            key_at = {(a.x1 + a.x2) / 2, a.y1 + (a.y2 - a.y1) / 8};  // the top row
            lv_obj_delete(ta);
            EXPECT_EQ(layer->keyboard(), nullptr);
        });
        tap(key_at);
        tap(key_at);
    }
    wisp::os::sleep_ms(100);
    on_ui([&] { EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before); });
}

// The same from inside one of the keyboard's own events: Enter on a
// one-line textarea reports READY to it, and its owner deletes it there.
TEST_F(SystemLayerTest, TextareaDeletedFromKeyboardEvent) {
    start("rect");
    on_ui([&] {
        lv_obj_t* ta = lv_textarea_create(lv_screen_active());
        lv_textarea_set_one_line(ta, true);
        lv_obj_add_event_cb(
            ta, [](lv_event_t* e) { lv_obj_delete(lv_event_get_current_target_obj(e)); },
            LV_EVENT_READY, nullptr);
        layer->show_keyboard(ta);
        lv_obj_t* kb = layer->keyboard();
        ASSERT_NE(kb, nullptr);
        ASSERT_TRUE(press(kb, "n"));
        ASSERT_TRUE(press(kb, LV_SYMBOL_NEW_LINE));
        EXPECT_EQ(layer->keyboard(), nullptr);
    });
    wisp::os::sleep_ms(100);
    on_ui([&] { EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before); });
}

TEST_F(SystemLayerTest, ErrorPanelPutsTheKeyboardAway) {
    start("circle");
    on_ui([&] {
        lv_obj_t* ta = lv_textarea_create(lv_screen_active());
        layer->show_keyboard(ta);
        ASSERT_NE(layer->keyboard(), nullptr);
        layer->show_error({"Demo", "main.lua", 1, "boom"}, [] {}, [] {});
        EXPECT_EQ(layer->keyboard(), nullptr);
        layer->hide_error();
        EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before);
    });
}

TEST_F(SystemLayerTest, PinyinDictGlyphsInFont) {
    start();
    on_ui([&] {
        const lv_pinyin_dict_t* d = wisp::ui::detail::pinyin_dict();
        ASSERT_NE(d, nullptr);
        std::string prev;
        size_t entries = 0;
        bool ni_has_you = false;
        for (; d->py != nullptr; ++d, ++entries) {
            ASSERT_NE(d->py_mb, nullptr);
            const std::string py = d->py;
            const std::string mb = d->py_mb;
            EXPECT_LT(prev, py);  // sorted, so the IME's prefix search works
            prev = py;
            ASSERT_FALSE(mb.empty()) << py;
            // The IME reads candidates as 3-byte UTF-8 sequences.
            ASSERT_EQ(mb.size() % 3, 0u) << py;
            for (size_t i = 0; i < mb.size(); i += 3) {
                const auto* p = reinterpret_cast<const unsigned char*>(mb.data() + i);
                ASSERT_EQ(p[0] & 0xF0, 0xE0) << py;
                const uint32_t cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
                lv_font_glyph_dsc_t dsc;
                EXPECT_TRUE(lv_font_get_glyph_dsc(&lv_font_cjk_16, &dsc, cp, 0))
                    << py << " U+" << std::hex << cp;
            }
            if (py == "ni") ni_has_you = mb.find("你") != std::string::npos;
        }
        EXPECT_GT(entries, 300u);
        EXPECT_TRUE(ni_has_you);
    });
}

TEST_F(SystemLayerTest, ConfirmAnswersAndStaysOnTop) {
    start();
    std::vector<bool> answers;
    on_ui([&] {
        lv_obj_t* box = layer->open_confirm(StrId::ResetTitle, "Really?",
                                            [&](bool yes) { answers.push_back(yes); });
        ASSERT_NE(box, nullptr);
        EXPECT_TRUE(lv_obj_check_type(box, &lv_msgbox_class));
        lv_obj_t* backdrop = lv_obj_get_parent(box);
        ASSERT_EQ(lv_obj_get_parent(backdrop), lv_layer_top());
        lv_obj_update_layout(backdrop);
        EXPECT_EQ(lv_obj_get_width(backdrop), kSide);
        EXPECT_EQ(lv_obj_get_height(backdrop), kSide);
        EXPECT_LE(lv_obj_get_width(box), safe.x2 - safe.x1 + 1);
        EXPECT_NE(find_label_containing(lv_layer_top(), "Really?"), nullptr);

        // A card opened later goes under it again on raise().
        lv_obj_t* card = lv_obj_create(lv_layer_top());
        EXPECT_GT(lv_obj_get_index(card), lv_obj_get_index(backdrop));
        layer->raise();
        EXPECT_EQ(lv_obj_get_index(backdrop),
                  static_cast<int32_t>(lv_obj_get_child_count(lv_layer_top())) - 1);
        lv_obj_delete(card);

        lv_obj_t* yes = find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::Yes));
        ASSERT_NE(yes, nullptr);
        lv_obj_send_event(yes, LV_EVENT_CLICKED, nullptr);
        EXPECT_FALSE(layer->confirm_open());

        ASSERT_NE(layer->open_confirm(StrId::ResetTitle, "Again?",
                                      [&](bool yes) { answers.push_back(yes); }),
                  nullptr);
        lv_obj_t* no = find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::No));
        ASSERT_NE(no, nullptr);
        lv_obj_send_event(no, LV_EVENT_CLICKED, nullptr);
        EXPECT_FALSE(layer->confirm_open());
    });
    EXPECT_EQ(answers, (std::vector<bool>{true, false}));
}

TEST_F(SystemLayerTest, ConfirmIsModal) {
    start("rect");
    int clicks = 0;
    on_ui([&] { make_counting_button(10, 10, 100, 50, &clicks); });
    tap(60, 35);
    ASSERT_EQ(clicks, 1);

    bool answered = false;
    on_ui([&] {
        ASSERT_NE(layer->open_confirm(StrId::ResetTitle, "Sure?", [&](bool) { answered = true; }),
                  nullptr);
    });
    tap(60, 35);
    EXPECT_EQ(clicks, 1);
    EXPECT_FALSE(answered);
    on_ui([&] { EXPECT_TRUE(layer->confirm_open()); });
}

TEST_F(SystemLayerTest, OneConfirmAtATime) {
    start();
    int second_calls = 0;
    on_ui([&] {
        lv_obj_t* first = layer->open_confirm(StrId::ResetTitle, "One", [](bool) {});
        ASSERT_NE(first, nullptr);
        EXPECT_TRUE(layer->confirm_open());
        EXPECT_EQ(layer->open_confirm(StrId::FirmwareTitle, "Two", [&](bool) { ++second_calls; }),
                  nullptr);
        EXPECT_EQ(wisp::test::count_obj_of_class(lv_layer_top(), &lv_msgbox_class), 1);
        layer->close_confirm(first);
        EXPECT_FALSE(layer->confirm_open());
        EXPECT_EQ(wisp::test::count_obj_of_class(lv_layer_top(), &lv_msgbox_class), 0);
    });
    EXPECT_EQ(second_calls, 0);
}

// The back gesture puts nothing on the top layer until a swipe starts,
// and nothing shown once it ends: a shown, unmarked top-layer object would
// stop the opaque-overlay redraw skip (wisp/ui/overlay.hpp).
TEST_F(SystemLayerTest, NoPersistentTopLayerChild) {
    start("circle");
    auto shown_children = [] {
        uint32_t n = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); ++i) {
            if (!lv_obj_has_flag(lv_obj_get_child(lv_layer_top(), static_cast<int32_t>(i)),
                                 LV_OBJ_FLAG_HIDDEN)) {
                ++n;
            }
        }
        return n;
    };
    uint32_t shown_before = 0;
    on_ui([&] {
        shown_before = shown_children();
        EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before);
        layer->set_back_handler([] {});
        EXPECT_EQ(lv_obj_get_child_count(lv_layer_top()), top_children_before);
    });
    const int16_t x0 = static_cast<int16_t>(wisp::hal::row_span(shape, 240).x1 + 2);
    feed({{x0, 240, 0}});
    feed({{static_cast<int16_t>(x0 + 60), 240, 0}});
    on_ui([&] { EXPECT_EQ(shown_children(), shown_before + 1); });
    feed({});
    on_ui([&] {
        EXPECT_EQ(shown_children(), shown_before);
        layer->set_back_handler({});
        EXPECT_EQ(shown_children(), shown_before);
    });
}

// As NetUi.ScrollingUnderTheProvisioningCardLeavesTheCardAlone: what
// scrolls under the error panel or the keyboard is not redrawn where the
// piece covers it. The rows span the whole screen, like the self-check
// screen's: a change that lies wholly inside the error panel's box may be
// the panel's own and is always redrawn (wisp/ui/overlay.hpp). The
// keyboard spans the whole width, so the rows under it lie inside its box;
// it changes only in its own events, so they are cut all the same.
TEST_F(SystemLayerTest, OverlaysKeepOcclusionSkip) {
    start("rounded_rect");
    lv_obj_t* list = nullptr;
    on_ui([&] {
        list = lv_obj_create(lv_screen_active());
        lv_obj_set_size(list, kSide, kSide);
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        for (int i = 0; i < 40; ++i) {
            lv_obj_t* row = lv_label_create(list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_label_set_text(row, ("row " + std::to_string(i)).c_str());
        }
    });

    auto check = [&](const char* what, const std::function<lv_obj_t*()>& show,
                     const std::function<void()>& hide) {
        SCOPED_TRACE(what);
        lv_area_t core{};
        size_t before = 0;
        on_ui([&] {
            lv_obj_t* piece = show();
            ASSERT_NE(piece, nullptr);
            lv_obj_update_layout(lv_layer_top());
            lv_obj_get_coords(piece, &core);
            const int32_t r = lv_obj_get_style_radius(piece, LV_PART_MAIN);
            core.y1 += r;
            core.y2 -= r;
            lv_refr_now(nullptr);
            before = display->areas().size();
            lv_obj_scroll_by(list, 0, -40, LV_ANIM_OFF);
            lv_refr_now(nullptr);
        });
        const std::vector<Area> areas = display->areas();
        ASSERT_GT(areas.size(), before);
        for (size_t i = before; i < areas.size(); ++i) {
            const Area& f = areas[i];
            EXPECT_FALSE(overlaps(f, core)) << f.x1 << "," << f.y1 << "-" << f.x2 << "," << f.y2;
        }
        on_ui(hide);
    };

    check(
        "error panel",
        [&] {
            layer->show_error({"Demo", "main.lua", 3, "boom"}, [] {}, [] {});
            lv_obj_t* label = find_label_containing(lv_layer_top(), "main.lua:3");
            return label != nullptr ? top_level(label) : nullptr;
        },
        [&] { layer->hide_error(); });

    lv_obj_t* ta = nullptr;
    on_ui([&] {
        ta = lv_textarea_create(lv_screen_active());
        lv_obj_set_pos(ta, safe.x1, safe.y1);
    });
    check(
        "keyboard",
        [&] {
            layer->show_keyboard(ta);
            return layer->keyboard();
        },
        [&] { layer->hide_keyboard(); });
}

}  // namespace
