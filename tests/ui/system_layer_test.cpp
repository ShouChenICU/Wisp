// The system layer (wisp/ui/system_layer.hpp): the back gesture zone, the
// error panel, the on-screen keyboard and the modal confirm dialogs, and
// how they leave the opaque-overlay redraw skip (wisp/ui/overlay.hpp)
// alone.

#include "wisp/ui/system_layer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "lvgl_private.h"  // lv_buttonmatrix_t::button_areas
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/visible.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/dialog.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/theme.hpp"
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
using wisp::test::find_obj_of_class;
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
            // A clock that only moves when a test says: a finger moving
            // between two reads moves infinitely fast, so every swipe
            // that is not made slow on purpose (step()) is a fling.
            clock_ms = 1000;
            layer->set_clock([this] { return clock_ms.load(); });
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

    // Feeds one touch read `ms` later on the layer's clock. The clock
    // moves on the UI thread with the read, so no read of LVGL's own
    // timer comes in between with the old point at the new time.
    void step(std::vector<wisp::hal::TouchPoint> points, uint32_t ms) {
        touch->set_points(std::move(points));
        on_ui([&] {
            clock_ms += ms;
            lv_indev_read(indev);
        });
    }

    // Waits (on the real clock, which runs LVGL's animations) until the
    // pull-down panel neither follows a finger nor slides.
    bool panel_settled(uint32_t timeout_ms = 2000) {
        const uint64_t until = wisp::os::now_ms() + timeout_ms;
        for (;;) {
            bool moving = true;
            on_ui([&] { moving = layer->panel_moving(); });
            if (!moving) return true;
            if (wisp::os::now_ms() > until) return false;
            wisp::os::sleep_ms(5);
        }
    }

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
    std::atomic<uint64_t> clock_ms{1000};
    ScreenShape shape;
    Area safe{};
    uint32_t top_children_before = 0;
};

// The three screens every shape-dependent test of the capsule, the
// pull-down panel and the banners runs on: 480 x 480 as a rectangle, as
// the reference board's rounded rectangle and as a circle.
struct ShapeCase {
    const char* shape;
    int corner_radius;
};
constexpr ShapeCase kShapeCases[] = {{"rect", 0}, {"rounded_rect", 70}, {"circle", 0}};

ScreenShape rounded480() { return {ScreenShapeKind::RoundedRect, kSide, kSide, 70}; }

bool same_area(const Area& a, const Area& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

std::string area_str(const lv_area_t& a) {
    return std::to_string(a.x1) + "," + std::to_string(a.y1) + "-" + std::to_string(a.x2) + "," +
           std::to_string(a.y2);
}

// The panel's model holds up to NotificationService::kStoredMax notes with
// their bodies: it lives in PSRAM (AGENTS.md section 5), not as small
// std::string and std::vector blocks in internal RAM.
using LStr = wisp::os::LargeString;
static_assert(std::is_same_v<decltype(wisp::ui::PanelNote::source), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelNote::id), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelNote::badge), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelNote::title), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelNote::body), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelNote::action_label), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelScene::id), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelScene::name), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelHeader::time), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelHeader::wifi), LStr>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelModel::notes),
                             wisp::os::LargeVector<wisp::ui::PanelNote>>);
static_assert(std::is_same_v<decltype(wisp::ui::PanelModel::scenes),
                             wisp::os::LargeVector<wisp::ui::PanelScene>>);

// A panel model with two background scenes and three notifications, the
// newest first, one of them with an action.
wisp::ui::PanelModel sample_model() {
    wisp::ui::PanelModel m;
    m.header = {"09:41", 85, false, "HomeNet"};
    m.now_ms = 10 * 3600 * 1000;
    m.scenes = {{"a", "Alpha", m.now_ms - 30 * 1000}, {"b", "Beta", m.now_ms - 5 * 60 * 1000}};
    m.notes = {{"system", "w1", "", "System", "Alpha kept it awake", "Release"},
               {"a", "n1", "A", "Note one", "First body", ""},
               {"b", "n2", "B", "Note two", "Second body", ""}};
    return m;
}

// Every shown, clickable object in `obj`'s subtree, `obj` included.
void collect_clickables(lv_obj_t* obj, std::vector<lv_obj_t*>& out) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) out.push_back(obj);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        collect_clickables(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

// The nearest clickable ancestor of `obj` (or `obj` itself).
lv_obj_t* clickable_of(lv_obj_t* obj) {
    while (obj != nullptr && !lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

// A fake LVGL clock: the real one plus an offset a test moves forward.
std::atomic<uint32_t> g_tick_offset{0};
uint32_t fake_tick() { return static_cast<uint32_t>(wisp::os::now_ms()) + g_tick_offset.load(); }

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

// The dialogs wear the device theme (wisp/ui/theme.hpp): the action the
// dialog is for is a primary button, the way out a secondary one, and a
// confirm that destroys data asks with a danger button.
TEST_F(SystemLayerTest, DialogButtonsTakeTheirRoles) {
    namespace theme = wisp::ui::theme;
    auto fill = [](lv_obj_t* obj) {
        return lv_color_to_u32(lv_obj_get_style_bg_color(obj, LV_PART_MAIN)) & 0xFFFFFF;
    };
    auto ink = [](lv_obj_t* obj) {
        return lv_color_to_u32(lv_obj_get_style_text_color(obj, LV_PART_MAIN)) & 0xFFFFFF;
    };
    start("rounded_rect");
    on_ui([&] {
        layer->show_error({"Demo", "main.lua", 12, "boom"}, [] {}, [] {});
        lv_obj_t* reload = find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::SceneReload));
        lv_obj_t* back = find_button_with_label(lv_layer_top(), wisp::ui::tr(StrId::SceneBack));
        ASSERT_NE(reload, nullptr);
        ASSERT_NE(back, nullptr);
        EXPECT_EQ(fill(reload), theme::kAccent);
        EXPECT_EQ(ink(reload), theme::kAccentInk);
        EXPECT_EQ(fill(back), theme::kSurface3);
        EXPECT_EQ(ink(back), theme::kText);
        lv_obj_t* title =
            find_label_containing(lv_layer_top(), wisp::ui::tr(StrId::SceneErrorTitle));
        ASSERT_NE(title, nullptr);
        EXPECT_EQ(ink(title), theme::kErr);
        lv_obj_t* card = top_level(title);
        EXPECT_EQ(fill(card), theme::kSurface);
        EXPECT_EQ(lv_obj_get_style_radius(card, LV_PART_MAIN), theme::kRadiusLg);
        layer->hide_error();

        lv_obj_t* box = layer->open_confirm(StrId::PowerOffTitle, "Sure?", [](bool) {});
        ASSERT_NE(box, nullptr);
        EXPECT_EQ(fill(box), theme::kSurface);
        EXPECT_EQ(fill(find_button_with_label(box, wisp::ui::tr(StrId::Yes))), theme::kAccent);
        EXPECT_EQ(fill(find_button_with_label(box, wisp::ui::tr(StrId::No))), theme::kSurface3);
        layer->close_confirm(box);

        box = layer->open_confirm(
            StrId::ResetTitle, "Erase?", [](bool) {}, wisp::ui::ButtonRole::Danger);
        ASSERT_NE(box, nullptr);
        lv_obj_t* yes = find_button_with_label(box, wisp::ui::tr(StrId::Yes));
        EXPECT_EQ(fill(yes), theme::kErr);
        EXPECT_EQ(ink(yes), theme::kAccentInk);
        EXPECT_EQ(fill(find_button_with_label(box, wisp::ui::tr(StrId::No))), theme::kSurface3);
        layer->close_confirm(box);
    });
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
            // As the overlay counts it: a pill's radius is LV_RADIUS_CIRCLE.
            const int32_t r =
                std::min({lv_obj_get_style_radius(piece, LV_PART_MAIN),
                          lv_area_get_width(&core) / 2, lv_area_get_height(&core) / 2});
            core.y1 += r;
            core.y2 -= r;
            lv_refr_now(nullptr);
            before = display->areas().size();
            lv_obj_scroll_by(list, 0, -40, LV_ANIM_OFF);
            lv_refr_now(nullptr);
        });
        const std::vector<Area> areas = display->areas();
        ASSERT_GT(areas.size(), before);
        // A pill has no row it covers edge to edge.
        for (size_t i = before; i < areas.size() && core.y1 <= core.y2; ++i) {
            const Area& f = areas[i];
            EXPECT_FALSE(overlaps(f, core)) << f.x1 << "," << f.y1 << "-" << f.x2 << "," << f.y2;
        }
        on_ui(hide);
    };

    // The capsule stays up for the checks after it: shown and unmarked, it
    // would keep every area that touches it from being cut.
    on_ui([&] { layer->set_panel_handlers({.on_open = [] {}}); });
    check(
        "capsule",
        [&] {
            layer->set_status({1, 2});
            return layer->capsule();
        },
        [] {});

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

    check(
        "banner",
        [&] {
            layer->show_banner({"s1", "n1", "D", "Demo", "Title", "Body"}, [] {});
            return layer->banner();
        },
        [] {});

    check(
        "panel",
        [&] {
            layer->update_panel(sample_model());
            layer->open_panel();
            return layer->panel();
        },
        [&] { layer->close_panel(); });
    on_ui([&] { EXPECT_NE(layer->capsule(), nullptr); });
}

// --- Status capsule, top-edge swipe, pull-down panel, banners ---

TEST(StatusCapsule, AreaPerShape) {
    EXPECT_TRUE(same_area(SystemLayer::status_area(rect480()), {204, 4, 275, 27}));
    EXPECT_TRUE(same_area(SystemLayer::status_area(rounded480()), {204, 28, 275, 51}));
    EXPECT_TRUE(same_area(SystemLayer::status_area(circle480()), {204, 78, 275, 101}));
    const ScreenShape sim{ScreenShapeKind::RoundedRect, kSide, kSide, 40};
    EXPECT_TRUE(same_area(SystemLayer::status_area(sim), {204, 19, 275, 42}));
}

TEST_F(SystemLayerTest, StatusCapsuleHiddenWhenBothZeroAndShowsCounts) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        on_ui([&] {
            layer->set_status({0, 0});
            EXPECT_EQ(layer->capsule(), nullptr);

            layer->set_status({2, 0});
            lv_obj_t* cap = layer->capsule();
            ASSERT_NE(cap, nullptr);
            EXPECT_EQ(lv_obj_get_parent(cap), lv_layer_top());
            lv_obj_update_layout(cap);
            lv_area_t a;
            lv_obj_get_coords(cap, &a);
            const Area want = SystemLayer::status_area(shape);
            EXPECT_TRUE(same_area({a.x1, a.y1, a.x2, a.y2}, want)) << area_str(a);
            EXPECT_TRUE(fully_visible(shape, a)) << area_str(a);
            std::string text = lv_label_get_text(find_obj_of_class(cap, &lv_label_class));
            EXPECT_NE(text.find('2'), std::string::npos) << text;
            EXPECT_NE(text.find(LV_SYMBOL_PLAY), std::string::npos) << text;
            EXPECT_EQ(text.find(LV_SYMBOL_BELL), std::string::npos) << text;

            layer->set_status({0, 3});
            cap = layer->capsule();
            ASSERT_NE(cap, nullptr);
            text = lv_label_get_text(find_obj_of_class(cap, &lv_label_class));
            EXPECT_NE(text.find('3'), std::string::npos) << text;
            EXPECT_NE(text.find(LV_SYMBOL_BELL), std::string::npos) << text;
            EXPECT_EQ(text.find(LV_SYMBOL_PLAY), std::string::npos) << text;

            // Counts above 9 read "9+"; the widest text fits the capsule,
            // which is always exactly status_area().
            layer->set_status({12, 50});
            cap = layer->capsule();
            lv_obj_t* label = find_obj_of_class(cap, &lv_label_class);
            text = lv_label_get_text(label);
            EXPECT_EQ(text, LV_SYMBOL_PLAY "9+ " LV_SYMBOL_BELL "9+");
            lv_obj_update_layout(cap);
            EXPECT_LE(lv_obj_get_width(label), lv_obj_get_content_width(cap));
            lv_obj_get_coords(cap, &a);
            EXPECT_TRUE(same_area({a.x1, a.y1, a.x2, a.y2}, want)) << area_str(a);
            layer->set_status({9, 10});
            text = lv_label_get_text(label);
            EXPECT_EQ(text, LV_SYMBOL_PLAY "9 " LV_SYMBOL_BELL "9+");

            layer->set_status({0, 0});
            EXPECT_EQ(layer->capsule(), nullptr);
        });
    }
}

TEST_F(SystemLayerTest, StatusCapsuleTapOpensPanel) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        int opens = 0;
        int clicks = 0;
        lv_point_t at{};
        on_ui([&] {
            make_counting_button(0, 0, kSide, 200, &clicks);
            layer->set_status({1, 1});
            ASSERT_NE(layer->capsule(), nullptr);
            at = center_of(layer->capsule());
        });
        // Without panel handlers the capsule takes no touches.
        tap(at);
        EXPECT_EQ(clicks, 1);
        on_ui([&] {
            EXPECT_FALSE(layer->panel_open());
            layer->set_panel_handlers({.on_open = [&] { ++opens; }});
        });
        tap(at);
        EXPECT_EQ(clicks, 1);
        EXPECT_EQ(opens, 1);
        on_ui([&] {
            EXPECT_TRUE(layer->panel_open());
            EXPECT_NE(layer->panel(), nullptr);
            layer->close_panel();
        });
        // The touch target reaches kPanelRowMinH: a tap just below the
        // drawn capsule still opens the panel.
        tap(at.x, static_cast<int16_t>(at.y + SystemLayer::kPanelRowMinH / 2 - 2));
        EXPECT_EQ(clicks, 1);
        EXPECT_EQ(opens, 2);
        on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    }
}

// The panel never opens under the keyboard, the error panel or a dialog:
// not from the capsule, a top swipe or the owner.
TEST_F(SystemLayerTest, PanelRefusesToOpenUnderOtherPieces) {
    start("rect");
    int opens = 0;
    lv_point_t at{};
    lv_obj_t* ta = nullptr;
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [&] { ++opens; }});
        layer->set_status({1, 1});
        at = center_of(layer->capsule());
        ta = lv_textarea_create(lv_screen_active());
        lv_obj_set_pos(ta, 100, 150);
        layer->show_keyboard(ta);
    });
    tap(at);
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        layer->open_panel();
        EXPECT_FALSE(layer->panel_open());
        layer->hide_keyboard();
        layer->show_error({"Demo", "main.lua", 3, "boom"}, [] {}, [] {});
    });
    feed({{240, 4, 0}});
    feed({{240, 4, 0}});
    feed({{240, 60, 0}});
    feed({});
    lv_obj_t* box = nullptr;
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        layer->open_panel();
        EXPECT_FALSE(layer->panel_open());
        layer->hide_error();
        box = layer->open_confirm(StrId::ResetTitle, "Sure?", [](bool) {});
        ASSERT_NE(box, nullptr);
        layer->open_panel();
        EXPECT_FALSE(layer->panel_open());
        layer->close_confirm(box);
    });
    EXPECT_EQ(opens, 0);
    tap(at);
    EXPECT_EQ(opens, 1);
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
}

// A press from the top-left corner is in both zones: whichever swipe
// crosses its trigger first owns it.
TEST_F(SystemLayerTest, FirstGestureToCrossItsTriggerWins) {
    start("rect");
    int backs = 0;
    int opens = 0;
    on_ui([&] {
        layer->set_back_handler([&] { ++backs; });
        layer->set_panel_handlers({.on_open = [&] { ++opens; }});
    });
    ASSERT_TRUE(SystemLayer::in_back_zone(shape, 4, 4));
    ASSERT_TRUE(SystemLayer::in_top_zone(shape, 4, 4));
    // Right first: back, though it then moves far down too.
    feed({{4, 4, 0}});
    feed({{4, 4, 0}});
    feed({{40, 10, 0}});
    feed({{44, 80, 0}});
    feed({});
    EXPECT_EQ(backs, 1);
    EXPECT_EQ(opens, 0);
    // Down first: the panel, and the arrow it had started to show goes.
    feed({{4, 4, 0}});
    feed({{4, 4, 0}});
    feed({{14, 10, 0}});
    on_ui([&] { EXPECT_NE(layer->back_arrow(), nullptr); });
    feed({{16, 40, 0}});
    on_ui([&] { EXPECT_EQ(layer->back_arrow(), nullptr); });
    feed({{80, 60, 0}});
    feed({});
    EXPECT_EQ(backs, 1);
    EXPECT_EQ(opens, 1);
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
}

TEST(TopSwipe, ZoneFollowsRowSpan) {
    const ScreenShape circle = circle480();
    const Area cs = wisp::hal::safe_rect(circle);
    // The middle column starts at the physical top edge.
    EXPECT_TRUE(SystemLayer::in_top_zone(circle, 240, 0));
    EXPECT_TRUE(SystemLayer::in_top_zone(circle, 240, 10));
    EXPECT_TRUE(SystemLayer::in_top_zone(circle, 240, 15));
    EXPECT_FALSE(SystemLayer::in_top_zone(circle, 240, 16));
    EXPECT_FALSE(SystemLayer::in_top_zone(circle, 100, 10));  // not visible
    EXPECT_FALSE(SystemLayer::in_top_zone(circle, 240, cs.y1 + 16));
    // A column further out starts lower, and so does its zone.
    int32_t top100 = 0;
    while (!wisp::hal::visible(circle, 100, top100)) ++top100;
    EXPECT_FALSE(SystemLayer::in_top_zone(circle, 100, top100 - 1));
    EXPECT_TRUE(SystemLayer::in_top_zone(circle, 100, top100));
    EXPECT_TRUE(SystemLayer::in_top_zone(circle, 100, top100 + 15));
    EXPECT_FALSE(SystemLayer::in_top_zone(circle, 100, top100 + 16));

    const ScreenShape rect = rect480();
    EXPECT_TRUE(SystemLayer::in_top_zone(rect, 0, 0));
    EXPECT_TRUE(SystemLayer::in_top_zone(rect, 479, 15));
    EXPECT_FALSE(SystemLayer::in_top_zone(rect, 0, 16));

    const ScreenShape rounded = rounded480();
    EXPECT_FALSE(SystemLayer::in_top_zone(rounded, 0, 0));
    int32_t top0 = 0;
    while (!wisp::hal::visible(rounded, 0, top0)) ++top0;
    EXPECT_TRUE(SystemLayer::in_top_zone(rounded, 0, top0 + 15));
    EXPECT_FALSE(SystemLayer::in_top_zone(rounded, 0, top0 + 16));
    EXPECT_TRUE(SystemLayer::in_top_zone(rounded, 240, 0));

    constexpr int32_t d = SystemLayer::kTopTriggerDy;
    EXPECT_FALSE(SystemLayer::is_top_swipe(circle, {240, 10}, {240, 10 + d - 1}));
    EXPECT_TRUE(SystemLayer::is_top_swipe(circle, {240, 10}, {240, 10 + d}));
    EXPECT_TRUE(SystemLayer::is_top_swipe(circle, {240, 10}, {400, 10 + d}));  // any angle
    EXPECT_FALSE(SystemLayer::is_top_swipe(circle, {240, 30}, {240, 200}));    // outside
}

TEST_F(SystemLayerTest, TopSwipeTapInZoneReachesWidget) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        int clicks = 0;
        int opens = 0;
        on_ui([&] {
            make_counting_button(0, 0, kSide, 120, &clicks);
            layer->set_panel_handlers({.on_open = [&] { ++opens; }});
        });
        ASSERT_TRUE(SystemLayer::in_top_zone(shape, 240, 10));
        tap(240, 10);
        EXPECT_EQ(clicks, 1);
        EXPECT_EQ(opens, 0);
        on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    }
}

TEST_F(SystemLayerTest, TopSwipeDownTakesPressAndOpens) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        int clicks = 0;
        int lost = 0;
        int opens = 0;
        lv_obj_t* btn = nullptr;
        on_ui([&] {
            btn = make_counting_button(0, 0, kSide, 120, &clicks);
            lv_obj_add_event_cb(
                btn, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
                LV_EVENT_PRESS_LOST, &lost);
            layer->set_panel_handlers({.on_open = [&] { ++opens; }});
        });
        feed({{240, 10, 0}});
        feed({{240, 10, 0}});
        on_ui([&] { EXPECT_TRUE(lv_obj_has_state(btn, LV_STATE_PRESSED)); });
        feed({{240, 40, 0}});
        on_ui([&] {
            EXPECT_FALSE(lv_obj_has_state(btn, LV_STATE_PRESSED));
            // It comes down with the finger from here on.
            EXPECT_TRUE(layer->panel_open());
            EXPECT_TRUE(layer->panel_moving());
        });
        EXPECT_EQ(lost, 1);
        EXPECT_EQ(opens, 1);
        // Fast: a fling, so it opens though short.
        feed({{240, 90, 0}});
        feed({});
        EXPECT_EQ(clicks, 0);
        EXPECT_EQ(opens, 1);
        ASSERT_TRUE(panel_settled());
        on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    }
}

TEST_F(SystemLayerTest, TopSwipeIgnoredWhileKeyboardConfirmOrPanelOpen) {
    start("rect");
    int opens = 0;
    lv_obj_t* ta = nullptr;
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [&] { ++opens; }});
        ta = lv_textarea_create(lv_screen_active());
        lv_obj_set_pos(ta, 100, 150);
        layer->show_keyboard(ta);
    });
    auto swipe = [&] {
        feed({{240, 4, 0}});
        feed({{240, 4, 0}});
        feed({{240, 60, 0}});
        feed({});
    };
    swipe();
    lv_obj_t* box = nullptr;
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        layer->hide_keyboard();
        box = layer->open_confirm(StrId::ResetTitle, "Sure?", [](bool) {});
        ASSERT_NE(box, nullptr);
    });
    swipe();
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        layer->close_confirm(box);
    });
    EXPECT_EQ(opens, 0);
    swipe();  // now it opens
    EXPECT_EQ(opens, 1);
    swipe();  // already open
    EXPECT_EQ(opens, 1);
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
}

TEST_F(SystemLayerTest, PanelLayoutInsideVisibleArea) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        on_ui([&] {
            layer->set_panel_handlers({.on_open = [] {}});
            layer->update_panel(sample_model());
            layer->open_panel();
            lv_obj_t* panel = layer->panel();
            ASSERT_NE(panel, nullptr);
            lv_obj_update_layout(lv_layer_top());

            // The panel covers the whole screen.
            bool covered = false;
            for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); ++i) {
                lv_obj_t* o = lv_obj_get_child(lv_layer_top(), static_cast<int32_t>(i));
                lv_area_t a;
                lv_obj_get_coords(o, &a);
                if (a.x1 <= 0 && a.y1 <= 0 && a.x2 >= kSide - 1 && a.y2 >= kSide - 1 &&
                    lv_obj_get_style_bg_opa(o, LV_PART_MAIN) == LV_OPA_COVER &&
                    !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
                    covered = true;
                }
            }
            EXPECT_TRUE(covered);

            // Each text the model gives is there.
            for (const char* text : {"09:41", "85%", "HomeNet", "Alpha", "Beta", "Note one",
                                     "Second body", "Release"}) {
                EXPECT_NE(find_label_containing(panel, text), nullptr) << text;
            }
            EXPECT_NE(find_label_containing(panel, wisp::ui::tr(StrId::PanelJustNow)), nullptr);
            EXPECT_NE(find_label_containing(panel, "5 分钟"), nullptr);
            EXPECT_NE(find_label_containing(panel, wisp::ui::tr(StrId::PanelClearAll)), nullptr);

            std::vector<lv_obj_t*> clickables;
            collect_clickables(panel, clickables);
            EXPECT_GE(clickables.size(), 8u);
            lv_area_t box;
            lv_obj_get_coords(panel, &box);
            for (lv_obj_t* o : clickables) {
                lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
                lv_obj_update_layout(panel);
                lv_area_t a;
                lv_obj_get_coords(o, &a);
                EXPECT_TRUE(fully_visible(shape, a)) << area_str(a);
                EXPECT_TRUE(inside(a, {box.x1, box.y1, box.x2, box.y2})) << area_str(a);
                EXPECT_GE(lv_area_get_height(&a), SystemLayer::kPanelRowMinH) << area_str(a);
            }
        });
    }
}

TEST_F(SystemLayerTest, PanelBackGestureOnlyClosesPanel) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        int backs = 0;
        on_ui([&] {
            layer->set_back_handler([&] { ++backs; });
            layer->set_panel_handlers({.on_open = [] {}});
            layer->update_panel(sample_model());
            layer->open_panel();
        });
        const int16_t x0 = static_cast<int16_t>(wisp::hal::row_span(shape, 240).x1 + 2);
        auto swipe = [&] {
            feed({{x0, 240, 0}});
            feed({{x0, 240, 0}});
            feed({{static_cast<int16_t>(x0 + 60), 240, 0}});
            feed({});
        };
        swipe();
        EXPECT_EQ(backs, 0);
        ASSERT_TRUE(panel_settled());
        on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
        swipe();
        EXPECT_EQ(backs, 1);
    }
}

// The panel's back gesture works without a back handler too.
TEST_F(SystemLayerTest, PanelClosesWithoutABackHandler) {
    start("circle");
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [] {}});
        layer->open_panel();
    });
    const int16_t x0 = static_cast<int16_t>(wisp::hal::row_span(shape, 240).x1 + 2);
    feed({{x0, 240, 0}});
    feed({{static_cast<int16_t>(x0 + 60), 240, 0}});
    feed({});
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
}

TEST_F(SystemLayerTest, PanelRowsCallHandlers) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        std::vector<std::string> calls;
        wisp::ui::PanelHandlers h;
        // The panel lets its model go when it closes: each opening gets a
        // new one, as StatusShell gives it.
        h.on_open = [&] { layer->update_panel(sample_model()); };
        h.open_scene = [&](const std::string& id) { calls.push_back("open " + id); };
        h.exit_scene = [&](const std::string& id) { calls.push_back("exit " + id); };
        h.tap_note = [&](const std::string& s, const std::string& id) {
            calls.push_back("tap " + s + "/" + id);
        };
        h.dismiss_note = [&](const std::string& s, const std::string& id) {
            calls.push_back("dismiss " + s + "/" + id);
        };
        h.note_action = [&](const std::string& s, const std::string& id, int index) {
            calls.push_back("action " + s + "/" + id + (index == 0 ? "" : " #1"));
        };
        h.clear_all = [&] { calls.push_back("clear"); };
        on_ui([&] {
            layer->set_panel_handlers(h);
            layer->update_panel(sample_model());
        });

        // Opens the panel if needed and returns the centre of what
        // `find` picks out of it, scrolled into view.
        auto locate = [&](const std::function<lv_obj_t*(lv_obj_t*)>& find) {
            lv_point_t p{-1, -1};
            on_ui([&] {
                if (!layer->panel_open()) layer->open_panel();
                lv_obj_t* o = find(layer->panel());
                ASSERT_NE(o, nullptr);
                lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
                lv_obj_update_layout(lv_layer_top());
                p = center_of(o);
            });
            return p;
        };
        auto row_of = [](const char* text) {
            return [text](lv_obj_t* panel) {
                return clickable_of(find_label_containing(panel, text));
            };
        };
        auto close_in = [](const char* text) {
            return [text](lv_obj_t* panel) {
                lv_obj_t* row = clickable_of(find_label_containing(panel, text));
                return row != nullptr ? find_button_with_label(row, LV_SYMBOL_CLOSE) : nullptr;
            };
        };
        auto button = [](const char* text) {
            return [text](lv_obj_t* panel) { return find_button_with_label(panel, text); };
        };

        tap(locate(row_of("Alpha")));
        on_ui([&] { EXPECT_FALSE(layer->panel_open()); });  // it opened the scene
        tap(locate(close_in("Alpha")));
        on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
        tap(locate(row_of("Note one")));
        on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
        tap(locate(close_in("Note one")));
        // A swipe to the left on a row dismisses it.
        const lv_point_t p = locate(row_of("Note two"));
        feed({{static_cast<int16_t>(p.x + 20), static_cast<int16_t>(p.y), 0}});
        feed({{static_cast<int16_t>(p.x + 20), static_cast<int16_t>(p.y), 0}});
        feed({{static_cast<int16_t>(p.x - 10), static_cast<int16_t>(p.y), 0}});
        feed({{static_cast<int16_t>(p.x - 40), static_cast<int16_t>(p.y), 0}});
        feed({});
        tap(locate(button("Release")));
        tap(locate(button(wisp::ui::tr(StrId::PanelClearAll))));

        EXPECT_EQ(calls, (std::vector<std::string>{"open a", "exit a", "tap a/n1", "dismiss a/n1",
                                                   "dismiss b/n2", "action system/w1", "clear"}));
    }
}

// tap_panel_row(i) taps the i-th row (the background scenes, then the
// notifications) the way a finger does: it calls that row's handler and
// closes the panel.
TEST_F(SystemLayerTest, TapPanelRowTapsTheIndexedRow) {
    start("circle");
    std::vector<std::string> calls;
    wisp::ui::PanelHandlers h;
    h.on_open = [&] { layer->update_panel(sample_model()); };
    h.open_scene = [&](const std::string& id) { calls.push_back("open " + id); };
    h.tap_note = [&](const std::string& s, const std::string& id) {
        calls.push_back("tap " + s + "/" + id);
    };
    on_ui([&] {
        layer->set_panel_handlers(h);
        layer->update_panel(sample_model());
        auto closed = layer->tap_panel_row(0);
        ASSERT_FALSE(closed);
        EXPECT_EQ(closed.error().message, "the panel is not open");

        layer->open_panel();
        ASSERT_TRUE(layer->tap_panel_row(1));
        EXPECT_FALSE(layer->panel_open());
        layer->open_panel();
        ASSERT_TRUE(layer->tap_panel_row(3));
        EXPECT_FALSE(layer->panel_open());
        layer->open_panel();
        auto past = layer->tap_panel_row(5);
        ASSERT_FALSE(past);
        EXPECT_EQ(past.error().message, "no such row");
        EXPECT_TRUE(layer->panel_open());
    });
    EXPECT_EQ(calls, (std::vector<std::string>{"open b", "tap a/n1"}));
}

TEST_F(SystemLayerTest, PanelUpdatesWhileOpen) {
    start("circle");
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [] {}});
        wisp::ui::PanelModel m = sample_model();
        m.notes.resize(1);
        layer->update_panel(m);
        layer->open_panel();
        EXPECT_NE(find_label_containing(layer->panel(), "Alpha kept"), nullptr);
        EXPECT_EQ(find_label_containing(layer->panel(), "Note one"), nullptr);

        layer->update_panel(sample_model());
        EXPECT_TRUE(layer->panel_open());
        EXPECT_NE(find_label_containing(layer->panel(), "Note one"), nullptr);
        EXPECT_NE(find_label_containing(layer->panel(), "Note two"), nullptr);

        wisp::ui::PanelModel empty;
        empty.header = {"09:42", -1, false, ""};
        layer->update_panel(empty);
        EXPECT_EQ(find_label_containing(layer->panel(), "Note one"), nullptr);
        EXPECT_EQ(find_label_containing(layer->panel(), "Alpha"), nullptr);
        EXPECT_NE(find_label_containing(layer->panel(), wisp::ui::tr(StrId::PanelEmpty)), nullptr);
        EXPECT_EQ(find_label_containing(layer->panel(), wisp::ui::tr(StrId::PanelClearAll)),
                  nullptr);
    });
}

// The model is let go once the panel closes, however it closes: a closed
// panel keeps none of the notes it showed.
TEST_F(SystemLayerTest, PanelModelIsClearedOnClose) {
    start("circle");
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [] {}});
        layer->update_panel(sample_model());
        layer->open_panel();
        ASSERT_TRUE(layer->panel_open());
        EXPECT_EQ(layer->panel_model().notes.size(), 3u);
        EXPECT_EQ(layer->panel_model().scenes.size(), 2u);
        layer->close_panel();
        EXPECT_TRUE(layer->panel_model().notes.empty());
        EXPECT_TRUE(layer->panel_model().scenes.empty());
        EXPECT_TRUE(layer->panel_model().header.time.empty());

        layer->update_panel(sample_model());
        layer->open_panel();
        EXPECT_NE(find_label_containing(layer->panel(), "Note one"), nullptr);
    });
    tap(240, 470);  // a tap on blank space
    ASSERT_TRUE(panel_settled());
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        EXPECT_TRUE(layer->panel_model().notes.empty());
    });
}

// on_close follows every way an open panel goes, once each; never while
// it is closed, nor from the destructor.
TEST_F(SystemLayerTest, PanelOnCloseFollowsEveryClose) {
    start("circle");
    int opens = 0, closes = 0;
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [&] { ++opens; }, .on_close = [&] { ++closes; }});
        layer->close_panel();
        EXPECT_EQ(closes, 0);
        layer->open_panel();
        layer->close_panel();
        layer->close_panel();
        EXPECT_EQ(closes, 1);
        layer->open_panel();
    });
    tap(240, 470);  // a tap on blank space
    ASSERT_TRUE(panel_settled());
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        EXPECT_EQ(closes, 2);
        layer->open_panel();
        layer->set_panel_handlers({.on_open = [] {}, .on_close = [&] { ++closes; }});
        EXPECT_EQ(closes, 2);  // still open
        layer->open_panel();
        layer.reset();
    });
    EXPECT_EQ(opens, 3);
    EXPECT_EQ(closes, 2);
}

// A tap on the panel where nothing is closes it.
TEST_F(SystemLayerTest, PanelTapOnBlankCloses) {
    start("circle");
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [] {}});
        layer->open_panel();
    });
    tap(240, 470);  // below the safe area
    ASSERT_TRUE(panel_settled());
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        layer->open_panel();
    });
    tap(240, 400);  // the empty part of the safe area
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
}

// An upward swipe across the panel closes it when its content does not
// scroll.
TEST_F(SystemLayerTest, PanelSwipeUpCloses) {
    start("rect");
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [] {}});
        layer->open_panel();
    });
    feed({{240, 300, 0}});
    feed({{240, 300, 0}});
    feed({{240, 250, 0}});
    feed({{240, 200, 0}});
    feed({});
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
}

// At the bottom of its scroll, an upward swipe closes the panel too;
// elsewhere it scrolls.
TEST_F(SystemLayerTest, PanelSwipeUpAtTheBottomCloses) {
    start("circle");
    on_ui([&] {
        layer->set_panel_handlers({.on_open = [] {}});
        wisp::ui::PanelModel m = sample_model();
        for (int i = 0; i < 8; ++i) {
            m.notes.push_back({"a", wisp::os::LargeString(("x" + std::to_string(i)).c_str()), "A",
                               "More", "Body", ""});
        }
        layer->update_panel(m);
        layer->open_panel();
        lv_obj_update_layout(layer->panel());
        ASSERT_GT(lv_obj_get_scroll_bottom(layer->panel()), 0);
    });
    auto swipe_up = [&] {
        feed({{240, 300, 0}});
        feed({{240, 300, 0}});
        feed({{240, 270, 0}});
        feed({{240, 230, 0}});
        feed({{240, 200, 0}});
        feed({});
    };
    swipe_up();
    on_ui([&] {
        ASSERT_TRUE(layer->panel_open());
        EXPECT_GT(lv_obj_get_scroll_y(layer->panel()), 0);  // it scrolled
        lv_obj_scroll_to_y(layer->panel(), LV_COORD_MAX, LV_ANIM_OFF);
        lv_obj_update_layout(layer->panel());
        EXPECT_LE(lv_obj_get_scroll_bottom(layer->panel()), 0);
    });
    swipe_up();
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
}

// --- The pull-down panel following the finger (scene-device spec 5.6) ---

// A SystemLayerTest whose panel shows sample_model(), with the panel's
// sheet (its backdrop) and the screen's content measurable.
class PanelFollowTest : public SystemLayerTest {
protected:
    void begin(const char* display_shape = "rect", int corner_radius = 0) {
        start(display_shape, wisp::ui::Lang::Zh, corner_radius);
        wisp::ui::PanelHandlers h;
        h.on_open = [this] {
            ++opens;
            layer->update_panel(sample_model());
        };
        h.on_close = [this] { ++closes; };
        h.open_scene = [this](const std::string& id) { calls.push_back("open " + id); };
        h.tap_note = [this](const std::string& s, const std::string& id) {
            calls.push_back("tap " + s + "/" + id);
        };
        h.clear_all = [this] { calls.push_back("clear"); };
        on_ui([&] {
            layer->set_panel_handlers(h);
            content = make_counting_button(0, 200, kSide, 200, &clicks);
            content_at = coords_of_obj(content);
        });
    }

    static lv_area_t coords_of_obj(lv_obj_t* o) {
        lv_obj_update_layout(o);
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        return a;
    }

    // The full-screen sheet the content box moves with: the top-layer
    // child right under the content box.
    lv_obj_t* sheet() const {
        lv_obj_t* p = layer->panel();
        if (p == nullptr) return nullptr;
        return lv_obj_get_child(lv_layer_top(), lv_obj_get_index(p) - 1);
    }

    // How far down the sheet's bottom edge is (the screen's height once
    // fully open), checked against the content box moving with it; -1
    // without a panel.
    int32_t panel_bottom() {
        int32_t bottom = -1;
        on_ui([&] {
            if (layer->panel() == nullptr) return;
            lv_obj_update_layout(lv_layer_top());
            const lv_area_t s = coords_of_obj(sheet());
            const lv_area_t p = coords_of_obj(layer->panel());
            EXPECT_EQ(lv_area_get_height(&s), shape.height);
            EXPECT_EQ(p.y1 - s.y1, safe.y1);
            bottom = s.y2 + 1;
        });
        return bottom;
    }

    void down(lv_point_t p) {
        step({{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), 0}}, 0);
    }
    void to(lv_point_t p, uint32_t ms = 100) {
        step({{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), 0}}, ms);
    }
    void up(uint32_t ms = 100) { step({}, ms); }
    // From `from` straight down (dy > 0) or up by |dy|, in steps of at
    // most 10 px 100 ms apart (0.1 px/ms, far from a fling); the finger
    // stays down.
    void slow(lv_point_t from, int32_t dy) {
        down(from);
        const int32_t n = std::abs(dy) / 10 + 1;
        for (int32_t i = 1; i <= n; ++i) to({from.x, from.y + dy * i / n});
    }

    bool content_unmoved() {
        bool same = false;
        on_ui([&] {
            const lv_area_t a = coords_of_obj(content);
            same = a.x1 == content_at.x1 && a.y1 == content_at.y1 && a.x2 == content_at.x2 &&
                   a.y2 == content_at.y2;
        });
        return same;
    }

    int opens = 0, closes = 0, clicks = 0;
    std::vector<std::string> calls;
    lv_obj_t* content = nullptr;
    lv_area_t content_at{};
};

// From the trigger on, the sheet's bottom edge is at the finger, as far
// down as it has moved from where it went down; the content under it
// stays put.
TEST_F(PanelFollowTest, OpeningFollowsTheFinger) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        opens = closes = clicks = 0;
        begin(c.shape, c.corner_radius);
        down({240, 4});
        to({240, 4 + SystemLayer::kTopTriggerDy - 1});
        EXPECT_EQ(opens, 0);
        EXPECT_EQ(panel_bottom(), -1);
        to({240, 40});
        EXPECT_EQ(opens, 1);
        on_ui([&] {
            EXPECT_TRUE(layer->panel_moving());
            // The model on_open gave is there already.
            EXPECT_NE(find_label_containing(layer->panel(), "Alpha"), nullptr);
        });
        EXPECT_EQ(panel_bottom(), 36);
        for (int16_t y : {100, 300, 180, 479}) {
            to({250, y});
            EXPECT_EQ(panel_bottom(), y - 4);
            EXPECT_TRUE(content_unmoved());
        }
        // Back above where it started: all the way up, still following.
        to({240, 0});
        EXPECT_EQ(panel_bottom(), 0);
        to({240, 200});
        EXPECT_EQ(panel_bottom(), 196);
        EXPECT_EQ(opens, 1);
        up();
        ASSERT_TRUE(panel_settled());
        EXPECT_EQ(panel_bottom(), shape.height);
        EXPECT_EQ(clicks, 0);
        EXPECT_TRUE(content_unmoved());
    }
}

// Released short of a third of the height, slowly, it slides back up and
// goes as if it never opened; from a third on it slides open.
TEST_F(PanelFollowTest, ReleaseDecidesByAThird) {
    begin();
    const int32_t third = shape.height / wisp::ui::kDragCompleteDivisor;
    slow({240, 4}, third - 1);
    EXPECT_EQ(panel_bottom(), third - 1);
    up();
    on_ui([&] { EXPECT_TRUE(layer->panel_moving()); });
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    EXPECT_EQ(opens, 1);
    EXPECT_EQ(closes, 1);
    EXPECT_TRUE(calls.empty());
    EXPECT_EQ(clicks, 0);

    slow({240, 4}, third);
    up();
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    EXPECT_EQ(panel_bottom(), shape.height);
    EXPECT_EQ(opens, 2);
    EXPECT_EQ(closes, 1);
    EXPECT_TRUE(calls.empty());
}

// A short fast flick opens it; the same distance slowly does not.
TEST_F(PanelFollowTest, AFlickOpensASlowShortDragDoesNot) {
    begin();
    down({240, 4});
    to({240, 30}, 10);
    to({240, 60}, 10);
    to({240, 90}, 10);  // 3 px/ms
    up(10);
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    on_ui([&] { layer->close_panel(); });

    slow({240, 4}, 86);
    up();
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
}

// An upward swipe on the open panel (nothing to scroll) takes the sheet up
// with the finger; short and slow it springs back open, past a third (or
// flicked) it closes.
TEST_F(PanelFollowTest, ClosingFollowsTheFinger) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        opens = closes = clicks = 0;
        begin(c.shape, c.corner_radius);
        // At the bottom of its scroll, if it scrolls.
        auto open_at_bottom = [&] {
            on_ui([&] {
                layer->open_panel();
                lv_obj_update_layout(layer->panel());
                lv_obj_scroll_to_y(layer->panel(), LV_COORD_MAX, LV_ANIM_OFF);
            });
        };
        open_at_bottom();
        const int16_t y0 = static_cast<int16_t>(safe.y2 - 20);
        down({240, y0});
        to({240, static_cast<int16_t>(y0 - SystemLayer::kTopTriggerDy)});
        on_ui([&] { EXPECT_TRUE(layer->panel_moving()); });
        EXPECT_EQ(panel_bottom(), shape.height - SystemLayer::kTopTriggerDy);
        for (int16_t dy : {60, 140, 100}) {
            to({240, static_cast<int16_t>(y0 - dy)});
            EXPECT_EQ(panel_bottom(), shape.height - dy);
            EXPECT_TRUE(content_unmoved());
        }
        up();
        ASSERT_TRUE(panel_settled());
        on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
        EXPECT_EQ(panel_bottom(), shape.height);
        EXPECT_EQ(closes, 0);

        slow({240, y0}, -(shape.height / 3 + 10));
        up();
        ASSERT_TRUE(panel_settled());
        on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
        EXPECT_EQ(closes, 1);
        EXPECT_TRUE(calls.empty());
        EXPECT_EQ(clicks, 0);

        // Flicked up a little: it closes.
        open_at_bottom();
        down({240, y0});
        to({240, static_cast<int16_t>(y0 - 30)}, 10);
        to({240, static_cast<int16_t>(y0 - 60)}, 10);
        up(10);
        ASSERT_TRUE(panel_settled());
        on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
        EXPECT_EQ(closes, 2);
    }
}

// A tap on no row and the back swipe slide the panel up; it is still
// there while it slides and gone once it has.
TEST_F(PanelFollowTest, TapOutsideAndBackSwipeSlideItUp) {
    begin("circle");
    on_ui([&] { layer->open_panel(); });
    tap(240, 470);
    on_ui([&] {
        EXPECT_TRUE(layer->panel_open());
        EXPECT_TRUE(layer->panel_moving());
    });
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    EXPECT_EQ(closes, 1);

    on_ui([&] { layer->open_panel(); });
    const int16_t x0 = static_cast<int16_t>(wisp::hal::row_span(shape, 240).x1 + 2);
    feed({{x0, 240, 0}});
    feed({{static_cast<int16_t>(x0 + 60), 240, 0}});
    feed({});
    on_ui([&] { EXPECT_TRUE(layer->panel_moving()); });
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    EXPECT_EQ(closes, 2);
    EXPECT_TRUE(calls.empty());
}

// While the sheet slides, a press does nothing: no row is tapped, even
// once the slide is over before the finger lifts, no second panel starts
// and tap_panel_row() refuses.
TEST_F(PanelFollowTest, PressesWhileItSlidesDoNothing) {
    begin("circle");
    slow({240, 4}, 200);
    up();
    on_ui([&] {
        ASSERT_TRUE(layer->panel_moving());
        auto r = layer->tap_panel_row(0);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().message, "the panel is moving");
    });
    // Where the first row is once open.
    lv_point_t row{};
    on_ui([&] {
        lv_obj_t* label = find_label_containing(layer->panel(), "Alpha");
        ASSERT_NE(label, nullptr);
        lv_obj_update_layout(lv_layer_top());
        const lv_area_t a = coords_of_obj(clickable_of(label));
        const lv_area_t s = coords_of_obj(sheet());
        row = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 - s.y1};
    });
    feed({{static_cast<int16_t>(row.x), static_cast<int16_t>(row.y), 0}});
    ASSERT_TRUE(panel_settled());
    feed({{static_cast<int16_t>(row.x), static_cast<int16_t>(row.y), 0}});
    feed({});
    EXPECT_TRUE(calls.empty());
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });

    // A top swipe while it slides shut starts nothing.
    tap(240, 470);
    feed({{240, 4, 0}});
    feed({{240, 4, 0}});
    feed({{240, 200, 0}});
    feed({});
    ASSERT_TRUE(panel_settled());
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    EXPECT_EQ(opens, 1);
    EXPECT_EQ(clicks, 0);
}

// The panel closed by code while the finger holds it goes at once, and
// the release then does nothing; the layer going mid-slide leaves no
// animation behind.
TEST_F(PanelFollowTest, ClosedOrDestroyedWhileMoving) {
    begin();
    slow({240, 4}, 100);
    on_ui([&] {
        layer->close_panel();
        EXPECT_FALSE(layer->panel_open());
        EXPECT_FALSE(layer->panel_moving());
    });
    up();
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    EXPECT_EQ(closes, 1);

    slow({240, 4}, 300);
    up();
    on_ui([&] {
        ASSERT_TRUE(layer->panel_moving());
        void* var = layer.get();
        EXPECT_NE(lv_anim_get(var, nullptr), nullptr);
        layer.reset();
        EXPECT_EQ(lv_anim_get(var, nullptr), nullptr);
    });
    wisp::os::sleep_ms(300);
    on_ui([] { lv_timer_handler(); });
    EXPECT_EQ(closes, 1);  // never from the destructor
}

// A row tap still closes the panel at once: it opens something.
TEST_F(PanelFollowTest, RowTapClosesAtOnce) {
    begin("circle");
    on_ui([&] { layer->open_panel(); });
    lv_point_t p{};
    on_ui([&] {
        lv_obj_t* row = clickable_of(find_label_containing(layer->panel(), "Alpha"));
        ASSERT_NE(row, nullptr);
        lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
        p = center_of(row);
    });
    tap(p);
    on_ui([&] {
        EXPECT_FALSE(layer->panel_open());
        EXPECT_FALSE(layer->panel_moving());
    });
    EXPECT_EQ(calls, (std::vector<std::string>{"open a"}));
}

class BannerTest : public SystemLayerTest {
protected:
    void use_fake_tick() {
        on_ui([] { lv_tick_set_cb(&fake_tick); });
    }
    // The runtime stops first; the next UiRuntime::start() installs its
    // own clock again.
    void TearDown() override {
        SystemLayerTest::TearDown();
        g_tick_offset = 0;
    }
    // Moves the LVGL clock forward and runs the timers that came due.
    void advance(uint32_t ms) {
        g_tick_offset += ms;
        on_ui([] { lv_timer_handler(); });
    }
};

TEST_F(BannerTest, QueuesAndAutoHides) {
    start("circle");
    use_fake_tick();
    on_ui([&] {
        EXPECT_EQ(layer->banner(), nullptr);
        layer->show_banner({"a", "n1", "A", "Alpha", "First", "one"}, [] {});
        layer->show_banner({"b", "n2", "B", "Beta", "Second", "two"}, [] {});
        ASSERT_NE(layer->banner(), nullptr);
        EXPECT_NE(find_label_containing(layer->banner(), "First"), nullptr);
        EXPECT_NE(find_label_containing(layer->banner(), "Alpha"), nullptr);
    });
    advance(SystemLayer::kBannerMs - 100);
    on_ui([&] {
        ASSERT_NE(layer->banner(), nullptr);
        EXPECT_NE(find_label_containing(layer->banner(), "First"), nullptr);
    });
    advance(100);
    on_ui([&] {
        ASSERT_NE(layer->banner(), nullptr);
        EXPECT_NE(find_label_containing(layer->banner(), "Second"), nullptr);
    });
    advance(SystemLayer::kBannerMs);
    on_ui([&] { EXPECT_EQ(layer->banner(), nullptr); });
}

// Each banner logs when it shows and when it goes, so a script that only
// reads the log (the simulator smoke test) can follow it.
TEST_F(BannerTest, LogsWhenShownAndGone) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    start("circle");
    use_fake_tick();
    on_ui([&] {
        layer->show_banner({"a", "n1", "A", "Alpha", "First", "one"}, [] {});
        layer->show_banner({"b", "n2", "B", "Beta", "Second", "two"}, [] {});
    });
    advance(SystemLayer::kBannerMs);
    advance(SystemLayer::kBannerMs);
    std::vector<std::string> lines;
    for (const auto& e : wisp::log::ring_entries())
        if (e.tag == "ui" && e.msg.rfind("banner: ", 0) == 0) lines.push_back(e.msg);
    EXPECT_EQ(lines, (std::vector<std::string>{"banner: shown a/n1", "banner: gone",
                                               "banner: shown b/n2", "banner: gone"}));
    wisp::log::reset_for_test();
}

TEST_F(BannerTest, WidthFollowsRowSpan) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        on_ui([&] {
            layer->show_banner({"system", "w1", "", "", "System", std::string(400, 'x')}, [] {});
            lv_obj_t* b = layer->banner();
            ASSERT_NE(b, nullptr);
            lv_obj_update_layout(b);
            lv_area_t a;
            lv_obj_get_coords(b, &a);
            EXPECT_EQ(a.y1, safe.y1 + 4);
            EXPECT_EQ(lv_area_get_height(&a), SystemLayer::kBannerH);
            EXPECT_TRUE(fully_visible(shape, a)) << area_str(a);
            // The visible width its rows share, less 4 px each side.
            int32_t x1 = 0;
            int32_t x2 = kSide - 1;
            for (int32_t y = a.y1; y <= a.y2; ++y) {
                const wisp::hal::Span s = wisp::hal::row_span(shape, y);
                x1 = std::max(x1, s.x1);
                x2 = std::min(x2, s.x2);
            }
            EXPECT_EQ(a.x1, x1 + 4) << area_str(a);
            EXPECT_EQ(a.x2, x2 - 4) << area_str(a);
            // Its text stays inside it.
            EXPECT_NE(find_label_containing(b, LV_SYMBOL_BELL), nullptr);
            std::vector<std::pair<std::string, lv_area_t>> labels;
            wisp::test::collect_labels(b, labels);
            for (const auto& [text, la] : labels) {
                EXPECT_TRUE(inside(la, {a.x1, a.y1, a.x2, a.y2})) << area_str(la);
            }
        });
    }
}

TEST_F(BannerTest, TapCallsBackAndSwipeUpOnlyHides) {
    start("rect");
    int taps = 0;
    int clicks = 0;
    lv_point_t at{};
    on_ui([&] {
        make_counting_button(0, 0, kSide, 200, &clicks);
        layer->show_banner({"a", "n1", "A", "Alpha", "First", "one"}, [&] { ++taps; });
        at = center_of(layer->banner());
    });
    tap(at);
    EXPECT_EQ(taps, 1);
    EXPECT_EQ(clicks, 0);
    on_ui([&] {
        EXPECT_EQ(layer->banner(), nullptr);
        layer->show_banner({"a", "n2", "A", "Alpha", "Second", "two"}, [&] { ++taps; });
        at = center_of(layer->banner());
    });
    feed({{static_cast<int16_t>(at.x), static_cast<int16_t>(at.y), 0}});
    feed({{static_cast<int16_t>(at.x), static_cast<int16_t>(at.y), 0}});
    feed({{static_cast<int16_t>(at.x), static_cast<int16_t>(at.y - 20), 0}});
    feed({{static_cast<int16_t>(at.x), static_cast<int16_t>(at.y - 40), 0}});
    feed({});
    EXPECT_EQ(taps, 1);
    EXPECT_EQ(clicks, 0);
    on_ui([&] { EXPECT_EQ(layer->banner(), nullptr); });
}

TEST_F(BannerTest, StacksBelowConfirm) {
    start("circle");
    on_ui([&] {
        layer->set_status({1, 1});
        layer->show_banner({"a", "n1", "A", "Alpha", "First", "one"}, [] {});
        lv_obj_t* banner = top_level(layer->banner());
        EXPECT_GT(lv_obj_get_index(banner), lv_obj_get_index(layer->capsule()));
        lv_obj_t* box = layer->open_confirm(StrId::ResetTitle, "Sure?", [](bool) {});
        ASSERT_NE(box, nullptr);
        layer->raise();
        lv_obj_t* backdrop = lv_obj_get_parent(box);
        EXPECT_GT(lv_obj_get_index(backdrop), lv_obj_get_index(banner));
        EXPECT_GT(lv_obj_get_index(banner), lv_obj_get_index(layer->capsule()));
        layer->close_confirm(box);
    });
    // A banner shown while a dialog is open goes under it too.
    on_ui([&] {
        lv_obj_t* box = layer->open_confirm(StrId::ResetTitle, "Again?", [](bool) {});
        ASSERT_NE(box, nullptr);
        layer->show_banner({"b", "n2", "B", "Beta", "Second", "two"}, [] {});
        layer->show_banner({"c", "n3", "C", "Gamma", "Third", "three"}, [] {});
        lv_obj_t* banner = top_level(layer->banner());
        EXPECT_GT(lv_obj_get_index(lv_obj_get_parent(box)), lv_obj_get_index(banner));
        layer->close_confirm(box);
    });
}

// A banner with actions (a todo reminder, mvp spec 5.2): its two buttons
// lie on the visible screen, it stays up for kActionBannerMs, and a button
// hides it and reports its index instead of a tap.
class SystemLayerBanner : public BannerTest {};

TEST_F(SystemLayerBanner, ActionsShowAndStayUp) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        on_ui([&] {
            wisp::ui::BannerInfo b{"system.reminder", "todo-1", "", "", "带伞", ""};
            b.action_label = "完成";
            b.action2_label = "10 分钟后";
            layer->show_banner(b, [] {});
            lv_obj_t* banner = layer->banner();
            ASSERT_NE(banner, nullptr);
            lv_obj_update_layout(banner);
            lv_area_t a;
            lv_obj_get_coords(banner, &a);
            EXPECT_TRUE(wisp::test::box_visible(shape, a)) << area_str(a);
            for (const char* label : {"完成", "10 分钟后"}) {
                lv_obj_t* btn = find_button_with_label(banner, label);
                ASSERT_NE(btn, nullptr) << label;
                lv_area_t ba;
                lv_obj_get_coords(btn, &ba);
                EXPECT_TRUE(wisp::test::box_visible(shape, ba)) << label << " " << area_str(ba);
                EXPECT_TRUE(inside(ba, {a.x1, a.y1, a.x2, a.y2})) << label << " " << area_str(ba);
                EXPECT_GE(lv_area_get_height(&ba), SystemLayer::kPanelRowMinH) << label;
            }
            std::vector<std::pair<std::string, lv_area_t>> labels;
            wisp::test::collect_labels(banner, labels);
            for (const auto& [text, la] : labels) {
                EXPECT_TRUE(inside(la, {a.x1, a.y1, a.x2, a.y2})) << text << " " << area_str(la);
            }
            layer.reset();
            layer = std::make_unique<SystemLayer>(shape);
        });
    }

    TearDown();
    start("circle");
    use_fake_tick();
    int taps = 0;
    std::vector<int> actions;
    auto show = [&] {
        on_ui([&] {
            wisp::ui::BannerInfo b{"system.reminder", "todo-1", "", "", "带伞", ""};
            b.action_label = "完成";
            b.action2_label = "10 分钟后";
            layer->show_banner(b, [&] { ++taps; }, [&](int index) { actions.push_back(index); });
        });
    };
    show();
    advance(SystemLayer::kActionBannerMs - 1000);
    on_ui([&] { EXPECT_NE(layer->banner(), nullptr); });
    advance(1000);
    on_ui([&] { EXPECT_EQ(layer->banner(), nullptr); });

    show();
    lv_point_t at{};
    on_ui([&] { at = center_of(find_button_with_label(layer->banner(), "10 分钟后")); });
    tap(at);
    on_ui([&] { EXPECT_EQ(layer->banner(), nullptr); });
    show();
    on_ui([&] { at = center_of(find_button_with_label(layer->banner(), "完成")); });
    tap(at);
    EXPECT_EQ(actions, (std::vector<int>{1, 0}));
    EXPECT_EQ(taps, 0);
    // A banner without actions keeps the short stay.
    on_ui([&] { layer->show_banner({"a", "n1", "A", "Alpha", "First", "one"}, [] {}); });
    advance(SystemLayer::kBannerMs);
    on_ui([&] { EXPECT_EQ(layer->banner(), nullptr); });
}

// tap_banner_action(i) presses the banner's button i as a finger would:
// the banner goes and on_action hears i. Without a banner, or without that
// button, nothing happens.
TEST_F(SystemLayerBanner, TapBannerActionPressesTheIndexedButton) {
    start("rounded_rect");
    use_fake_tick();
    int taps = 0;
    std::vector<int> actions;
    on_ui([&] {
        auto none = layer->tap_banner_action(0);
        ASSERT_FALSE(none);
        EXPECT_EQ(none.error().message, "no banner is shown");

        layer->show_banner(
            {"a", "n1", "A", "Alpha", "First", "one"}, [&] { ++taps; },
            [&](int index) { actions.push_back(index); });
        auto plain = layer->tap_banner_action(0);
        ASSERT_FALSE(plain);
        EXPECT_EQ(plain.error().message, "no such action");
        EXPECT_NE(layer->banner(), nullptr);
    });
    advance(SystemLayer::kBannerMs);
    on_ui([&] { EXPECT_EQ(layer->banner(), nullptr); });
    auto show = [&] {
        on_ui([&] {
            wisp::ui::BannerInfo b{"system.reminder", "todo-1", "", "", "带伞", ""};
            b.action_label = "完成";
            b.action2_label = "10 分钟后";
            layer->show_banner(b, [&] { ++taps; }, [&](int index) { actions.push_back(index); });
        });
    };
    show();
    on_ui([&] {
        auto past = layer->tap_banner_action(2);
        ASSERT_FALSE(past);
        EXPECT_EQ(past.error().message, "no such action");
        ASSERT_TRUE(layer->tap_banner_action(1));
        EXPECT_EQ(layer->banner(), nullptr);
    });
    show();
    on_ui([&] { ASSERT_TRUE(layer->tap_banner_action(0)); });
    EXPECT_EQ(actions, (std::vector<int>{1, 0}));
    EXPECT_EQ(taps, 0);
}

class SystemLayerPanel : public SystemLayerTest {};

TEST_F(SystemLayerPanel, NoteWithTwoActions) {
    for (const ShapeCase& c : kShapeCases) {
        SCOPED_TRACE(c.shape);
        TearDown();
        start(c.shape, wisp::ui::Lang::Zh, c.corner_radius);
        std::vector<std::string> calls;
        auto model = [] {
            wisp::ui::PanelModel m = sample_model();
            wisp::ui::PanelNote n{"system.reminder", "todo-1", "", "带伞", "", "完成"};
            n.action2_label = "10 分钟后";
            m.notes.insert(m.notes.begin(), n);
            return m;
        };
        wisp::ui::PanelHandlers h;
        h.on_open = [&] { layer->update_panel(model()); };
        h.note_action = [&](const std::string& s, const std::string& id, int index) {
            calls.push_back(s + "/" + id + " " + std::to_string(index));
        };
        on_ui([&] { layer->set_panel_handlers(h); });
        auto locate = [&](const char* label) {
            lv_point_t p{-1, -1};
            on_ui([&] {
                if (!layer->panel_open()) layer->open_panel();
                lv_obj_t* btn = find_button_with_label(layer->panel(), label);
                ASSERT_NE(btn, nullptr) << label;
                lv_obj_scroll_to_view_recursive(btn, LV_ANIM_OFF);
                lv_obj_update_layout(lv_layer_top());
                lv_area_t a;
                lv_obj_get_coords(btn, &a);
                EXPECT_TRUE(wisp::test::box_visible(shape, a)) << label << " " << area_str(a);
                EXPECT_TRUE(inside(a, safe)) << label << " " << area_str(a);
                EXPECT_GE(lv_area_get_height(&a), SystemLayer::kPanelRowMinH) << label;
                p = center_of(btn);
            });
            return p;
        };
        tap(locate("10 分钟后"));
        tap(locate("完成"));
        tap(locate("Release"));
        EXPECT_EQ(calls, (std::vector<std::string>{"system.reminder/todo-1 1",
                                                   "system.reminder/todo-1 0", "system/w1 0"}));
    }
}

}  // namespace
