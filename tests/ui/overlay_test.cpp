// Opaque overlays (wisp/ui/overlay.hpp): what changes under an opaque
// top-layer card is not redrawn where the card covers it, and the panel
// still ends up showing exactly what a full redraw would.

#include "wisp/ui/overlay.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/lvgl_probe.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace {

constexpr int kSide = 240;

// Keeps every flushed pixel in a framebuffer, like a panel's own memory.
class FramebufferDisplay : public wisp::hal::Display {
public:
    FramebufferDisplay() : fb_(static_cast<size_t>(kSide) * kSide, 0) {
        info_.id = "display.main";
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"width", kSide}, {"height", kSide}, {"area_align", 1}, {"shape", wisp::Value("rect")}};
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<void> flush(const wisp::hal::Area& area, const uint16_t* px) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const int w = area.x2 - area.x1 + 1;
        for (int y = area.y1; y <= area.y2; ++y) {
            for (int x = area.x1; x <= area.x2; ++x) {
                fb_[static_cast<size_t>(y) * kSide + x] = px[(y - area.y1) * w + (x - area.x1)];
            }
        }
        areas_.push_back(area);
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> set_brightness(uint8_t) override { return wisp::Result<void>::ok(); }
    wisp::Result<void> set_power(wisp::hal::PowerLevel) override {
        return wisp::Result<void>::ok();
    }
    bool powered() const override { return true; }

    std::vector<uint16_t> framebuffer() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fb_;
    }
    std::vector<wisp::hal::Area> take_areas() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(areas_, {});
    }

private:
    wisp::hal::DeviceInfo info_;
    mutable std::mutex mutex_;
    std::vector<uint16_t> fb_;
    std::vector<wisp::hal::Area> areas_;
};

struct Scene {
    lv_obj_t* list = nullptr;   // scrolls under the card
    lv_obj_t* card = nullptr;   // on the top layer
    lv_obj_t* inner = nullptr;  // a label in the card
};

// The list of build_scene().
lv_obj_t* build_list() {
    lv_obj_t* list = lv_obj_create(lv_screen_active());
    lv_obj_set_size(list, kSide, kSide);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < 30; ++i) {
        lv_obj_t* l = lv_label_create(list);
        lv_label_set_text(l, ("row " + std::to_string(i) + " ##########").c_str());
    }
    return list;
}

// A scrolling list of labels filling the screen, and a card with a
// rounded border on the top layer over the middle of it.
Scene build_scene(bool opaque) {
    Scene s;
    s.list = build_list();
    s.card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s.card, 120, 100);
    lv_obj_center(s.card);
    lv_obj_set_style_radius(s.card, 12, 0);
    lv_obj_set_style_border_width(s.card, 2, 0);
    lv_obj_set_style_bg_opa(s.card, opaque ? LV_OPA_COVER : LV_OPA_50, 0);
    s.inner = lv_label_create(s.card);
    lv_label_set_text(s.inner, "card");
    wisp::ui::mark_opaque_overlay(s.card);
    lv_obj_update_layout(s.card);
    return s;
}

// The card's area that is opaque on every row: its full width, minus the
// rounded corners' rows.
lv_area_t opaque_core(lv_obj_t* card) {
    lv_area_t a;
    lv_obj_get_coords(card, &a);
    const int32_t r = lv_obj_get_style_radius(card, LV_PART_MAIN);
    a.y1 += r;
    a.y2 -= r;
    return a;
}

bool overlaps(const wisp::hal::Area& f, const lv_area_t& a) {
    return f.x1 <= a.x2 && f.x2 >= a.x1 && f.y1 <= a.y2 && f.y2 >= a.y1;
}

// One step applied to the scene between frames.
using Step = std::function<void(Scene&)>;

struct Result {
    std::vector<uint16_t> fb;
    std::vector<std::vector<wisp::hal::Area>> flushed;  // per step
    lv_area_t core{};                                   // the card's, before the steps
};

// Builds the scene on a fresh display and draws it; then, if
// `incremental`, applies each step and redraws only what LVGL
// invalidated. Otherwise applies all steps and draws the whole display
// once, which is what the panel must end up showing.
Result render_scene(const std::function<Scene()>& build, const std::vector<Step>& steps,
                    bool incremental) {
    auto disp = std::make_shared<FramebufferDisplay>();
    wisp::ui::UiRuntime ui(disp, nullptr);
    Result r;
    if (!ui.start()) {
        ADD_FAILURE() << "UiRuntime did not start";
        return r;
    }
    wisp::test::run_on_ui_blocking(&ui, [&] {
        // Recorded before any overlay exists, so nothing is cut from it.
        lv_obj_invalidate(lv_screen_active());
        Scene s = build();
        r.core = opaque_core(s.card);
        if (incremental) {
            lv_refr_now(nullptr);
            disp->take_areas();
        }
        for (const Step& step : steps) {
            step(s);
            if (incremental) {
                lv_refr_now(nullptr);
                r.flushed.push_back(disp->take_areas());
            }
        }
        if (!incremental) lv_refr_now(nullptr);
        r.fb = disp->framebuffer();
    });
    ui.stop();
    return r;
}

Result render(bool opaque, const std::vector<Step>& steps, bool incremental) {
    return render_scene([opaque] { return build_scene(opaque); }, steps, incremental);
}

// Whether drawing step by step leaves the panel as a full redraw would.
bool same_as_full_redraw_of(const std::function<Scene()>& build, const std::vector<Step>& steps) {
    return render_scene(build, steps, true).fb == render_scene(build, steps, false).fb;
}

bool same_as_full_redraw(bool opaque, const std::vector<Step>& steps) {
    return same_as_full_redraw_of([opaque] { return build_scene(opaque); }, steps);
}

// The list, and a key band across the whole width of the lower part of the
// screen, like the on-screen keyboard: a button matrix that only changes
// from its own events. A press on it (sent as LV_EVENT_VALUE_CHANGED)
// switches its map.
Scene build_band() {
    Scene s;
    s.list = build_list();
    // Rows across the whole width, as in a scene's list.
    lv_obj_set_style_pad_all(s.list, 0, 0);
    lv_obj_set_style_border_width(s.list, 0, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s.list); ++i) {
        lv_obj_set_width(lv_obj_get_child(s.list, static_cast<int32_t>(i)), LV_PCT(100));
    }
    s.card = lv_buttonmatrix_create(lv_layer_top());
    lv_obj_set_pos(s.card, 0, kSide / 2);
    lv_obj_set_size(s.card, kSide, kSide / 2);
    lv_obj_set_style_radius(s.card, 0, 0);
    lv_obj_set_style_bg_opa(s.card, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(
        s.card,
        [](lv_event_t* e) {
            static const char* const kOther[] = {"x", "y", "\n", "z", ""};
            lv_buttonmatrix_set_map(static_cast<lv_obj_t*>(lv_event_get_current_target(e)), kOther);
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    wisp::ui::mark_opaque_overlay(s.card, wisp::ui::OwnChanges::InOwnEvents);
    lv_obj_update_layout(s.card);
    return s;
}

void scroll(Scene& s) { lv_obj_scroll_by(s.list, 0, -37, LV_ANIM_OFF); }

TEST(Overlay, ScrollingUnderAnOpaqueCardSkipsTheCoveredPart) {
    const Result r = render(true, {scroll, scroll}, true);
    ASSERT_EQ(r.flushed.size(), 2u);
    for (const auto& step : r.flushed) {
        ASSERT_FALSE(step.empty());
        for (const auto& f : step) {
            EXPECT_FALSE(overlaps(f, r.core)) << f.x1 << "," << f.y1 << "-" << f.x2 << "," << f.y2;
        }
    }
    EXPECT_TRUE(same_as_full_redraw(true, {scroll, scroll}));
}

TEST(Overlay, ChangesInsideTheCardAreStillDrawn) {
    EXPECT_TRUE(same_as_full_redraw(true, {scroll, [](Scene& s) {
                                               lv_label_set_text(s.inner, "changed text");
                                               lv_obj_set_style_bg_color(
                                                   s.card, lv_color_make(0x20, 0x60, 0xa0), 0);
                                               scroll(s);
                                           }}));
}

TEST(Overlay, ATranslucentCardIsRedrawnOverWhatScrolls) {
    EXPECT_TRUE(same_as_full_redraw(false, {scroll, scroll}));
}

// Something else drawn above the card (the system layer's performance
// label) is redrawn in full even where it crosses the card's edge.
TEST(Overlay, OtherLayersAboveTheCardAreStillDrawn) {
    lv_obj_t* tag = nullptr;
    const Step add_tag = [&tag](Scene& s) {
        tag = lv_label_create(lv_layer_sys());
        lv_label_set_text(tag, "....");
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        lv_area_t c;
        lv_obj_get_coords(s.card, &c);
        lv_obj_set_pos(tag, c.x1 - 10, (c.y1 + c.y2) / 2);
    };
    const Step grow_tag = [&tag](Scene&) { lv_label_set_text(tag, "########"); };
    EXPECT_TRUE(same_as_full_redraw(true, {add_tag, grow_tag}));
}

// While it renders, LVGL sends the display a one-column probe area
// ({0, 0, 0, rows - 1}) through LV_EVENT_INVALIDATE_AREA to learn how many
// rows a band may have. A card spanning x = 0 must neither split that
// probe (invalidating during rendering halts LVGL) nor shorten the bands.
TEST(Overlay, AFullWidthCardLeavesTheRowProbeAlone) {
    const Step widen = [](Scene& s) {
        lv_obj_set_style_radius(s.card, 0, 0);
        lv_obj_set_size(s.card, kSide, 14);
        lv_obj_align(s.card, LV_ALIGN_TOP_LEFT, 0, 2);
    };
    const Result r = render(true, {widen, scroll, scroll}, true);
    ASSERT_EQ(r.flushed.size(), 3u);
    for (size_t i = 1; i < r.flushed.size(); ++i) {
        ASSERT_FALSE(r.flushed[i].empty());
        // Bands of the scrolled list below the card are the buffer's
        // full height (24 lines), apart from each area's last one.
        bool full_band = false;
        for (const auto& f : r.flushed[i]) full_band |= (f.y2 - f.y1 + 1) == 24;
        EXPECT_TRUE(full_band);
    }
    EXPECT_TRUE(same_as_full_redraw(true, {widen, scroll, scroll}));
}

// Hiding or fading the whole top layer invalidates the layer, a
// full-screen area, while the card is still shown, or before the change
// takes effect; what the card covered must still be redrawn.
TEST(Overlay, HidingTheTopLayerRevealsWhatScrolledBeneath) {
    EXPECT_TRUE(same_as_full_redraw(
        true, {scroll, [](Scene&) { lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN); }}));
}

TEST(Overlay, FadingTheTopLayerOutAndBackIsRedrawn) {
    const Step fade = [](Scene& s) {
        lv_obj_set_style_opa(lv_layer_top(), LV_OPA_50, 0);
        scroll(s);
    };
    const Step unfade = [](Scene& s) {
        lv_obj_set_style_opa(lv_layer_top(), LV_OPA_COVER, 0);
        scroll(s);
    };
    EXPECT_TRUE(same_as_full_redraw(true, {scroll, fade, scroll, unfade, scroll}));
}

TEST(Overlay, ATransformedCardIsNotCut) {
    EXPECT_TRUE(same_as_full_redraw(true, {scroll,
                                           [](Scene& s) {
                                               lv_obj_set_style_transform_width(s.card, -8, 0);
                                               scroll(s);
                                           },
                                           scroll}));
}

TEST(Overlay, ClosingTheCardRevealsWhatScrolledBeneath) {
    EXPECT_TRUE(same_as_full_redraw(true, {scroll, [](Scene& s) { lv_obj_delete(s.card); }}));
}

// The rows scrolling under a full-width band lie inside its box; they are
// still not redrawn where it covers them.
TEST(Overlay, ScrollingUnderAFullWidthBandSkipsIt) {
    const Result r = render_scene(build_band, {scroll, scroll}, true);
    ASSERT_EQ(r.flushed.size(), 2u);
    for (const auto& step : r.flushed) {
        ASSERT_FALSE(step.empty());
        for (const auto& f : step) {
            EXPECT_FALSE(overlaps(f, r.core)) << f.x1 << "," << f.y1 << "-" << f.x2 << "," << f.y2;
        }
    }
    EXPECT_TRUE(same_as_full_redraw_of(build_band, {scroll, scroll}));
}

// What the band changes from its own events is drawn.
TEST(Overlay, ABandChangingInItsOwnEventIsDrawn) {
    const Step press = [](Scene& s) { lv_obj_send_event(s.card, LV_EVENT_VALUE_CHANGED, nullptr); };
    EXPECT_TRUE(same_as_full_redraw_of(build_band, {scroll, press, scroll}));
}

TEST(Overlay, HidingOrDeletingABandRevealsWhatScrolledBeneath) {
    EXPECT_TRUE(same_as_full_redraw_of(
        build_band, {scroll, [](Scene& s) { lv_obj_add_flag(s.card, LV_OBJ_FLAG_HIDDEN); }}));
    EXPECT_TRUE(
        same_as_full_redraw_of(build_band, {scroll, [](Scene& s) { lv_obj_delete(s.card); }}));
}

// A marked sheet that moves (the pull-down panel following the finger)
// cuts what it moves over unless it is paused: an area invalidated before
// a move is cut by where the sheet still is, though the move uncovers it.
Scene build_sheet() {
    Scene s = build_band();
    lv_obj_set_pos(s.card, 0, 0);
    lv_obj_set_size(s.card, kSide, kSide);
    lv_obj_update_layout(s.card);
    return s;
}

void pause(Scene& s) { wisp::ui::pause_opaque_overlay(s.card, true); }
void resume(Scene& s) { wisp::ui::pause_opaque_overlay(s.card, false); }
Step move_to(int32_t y) {
    return [y](Scene& s) { lv_obj_set_y(s.card, y); };
}

TEST(Overlay, APausedSheetMovingUpAndDownIsDrawn) {
    EXPECT_TRUE(
        same_as_full_redraw_of(build_sheet, {scroll, pause, move_to(-60), move_to(-150),
                                             move_to(-40), move_to(0), resume, scroll, scroll}));
}

// Paused, nothing is cut; resumed, it counts again once a frame has drawn
// it (here the first scroll's), and the scrolls after that are skipped
// under it again: wholly covered, what is left is a single pixel.
TEST(Overlay, APausedSheetCutsNothingUntilResumed) {
    const Result r = render_scene(build_sheet, {pause, scroll, resume, scroll, scroll}, true);
    ASSERT_EQ(r.flushed.size(), 5u);
    EXPECT_FALSE(r.flushed[1].empty());  // paused: the scroll is drawn
    EXPECT_FALSE(r.flushed[3].empty());
    for (const auto& f : r.flushed[4]) {
        EXPECT_TRUE(f.x1 == f.x2 && f.y1 == f.y2)
            << f.x1 << "," << f.y1 << "-" << f.x2 << "," << f.y2;
    }
    EXPECT_TRUE(same_as_full_redraw_of(build_sheet, {pause, scroll, resume, scroll, scroll}));
}

}  // namespace
