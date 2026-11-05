// The device theme (wisp/ui/theme.hpp): the token values, the LVGL theme
// UiRuntime installs from them, and the button roles.

#include "wisp/ui/theme.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>

#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace {

namespace theme = wisp::ui::theme;
using wisp::test::FakeDisplay;
using wisp::test::run_on_ui_blocking;
using wisp::ui::ButtonRole;

uint32_t rgb_of(lv_color_t c) {
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) | c.blue;
}

uint32_t bg(lv_obj_t* obj, lv_part_t part = LV_PART_MAIN) {
    return rgb_of(lv_obj_get_style_bg_color(obj, part));
}

uint32_t fg(lv_obj_t* obj, lv_part_t part = LV_PART_MAIN) {
    return rgb_of(lv_obj_get_style_text_color(obj, part));
}

// The web console's dark tokens (web/styles/tokens.css), and the opaque
// stand-ins for its translucent ones, blended onto the surface colour.
TEST(Theme, TokensMatchTheWebConsole) {
    EXPECT_EQ(theme::kBg, 0x0B0F17u);
    EXPECT_EQ(theme::kBgRaised, 0x0E1420u);
    EXPECT_EQ(theme::kSurface, 0x121926u);
    EXPECT_EQ(theme::kSurface2, 0x19212Fu);
    EXPECT_EQ(theme::kSurface3, 0x212B3Cu);
    EXPECT_EQ(theme::kText, 0xE6EDF7u);
    EXPECT_EQ(theme::kTextDim, 0x93A0B4u);
    EXPECT_EQ(theme::kTextFaint, 0x8A94A5u);
    EXPECT_EQ(theme::kAccent, 0x5AB4FFu);
    EXPECT_EQ(theme::kAccentStrong, 0x84C7FFu);
    EXPECT_EQ(theme::kAccentInk, 0x04121Fu);
    EXPECT_EQ(theme::kOk, 0x3DD68Cu);
    EXPECT_EQ(theme::kWarn, 0xF5B74Fu);
    EXPECT_EQ(theme::kErr, 0xFF6B6Bu);

    // rgba(148, 170, 205, 0.12) and 0.22 over #121926.
    EXPECT_EQ(theme::kBorder, 0x222A3Au);
    EXPECT_EQ(theme::kBorderStrong, 0x2F394Bu);
    // Each status colour at 13% over #121926.
    EXPECT_EQ(theme::kOkSoft, 0x183233u);
    EXPECT_EQ(theme::kWarnSoft, 0x302E2Bu);
    EXPECT_EQ(theme::kErrSoft, 0x31242Fu);

    EXPECT_EQ(theme::kRadius, 14);
    EXPECT_EQ(theme::kRadiusSm, 9);
    EXPECT_EQ(theme::kRadiusLg, 20);
    EXPECT_EQ(theme::kGapXs, 4);
    EXPECT_EQ(theme::kGapSm, 8);
    EXPECT_EQ(theme::kGapMd, 12);
    EXPECT_EQ(theme::kGap, 16);
    EXPECT_EQ(theme::kGapLg, 24);
    EXPECT_EQ(theme::kGapXl, 32);
}

TEST(Theme, BlendRoundsEachChannel) {
    EXPECT_EQ(theme::blend(0xFFFFFF, 0x000000, 0), 0x000000u);
    EXPECT_EQ(theme::blend(0xFFFFFF, 0x000000, 100), 0xFFFFFFu);
    EXPECT_EQ(theme::blend(0xFF0000, 0x0000FF, 50), 0x800080u);
    // A press darkens a bright fill and lightens a dark one.
    EXPECT_LT(theme::pressed(theme::kAccent, theme::kAccentInk) & 0xFF, theme::kAccent & 0xFF);
    EXPECT_GT(theme::pressed(theme::kSurface3, theme::kText) & 0xFF, theme::kSurface3 & 0xFF);
}

class ThemeTest : public ::testing::Test {
protected:
    void SetUp() override { start(); }

    void start() {
        display = std::make_shared<FakeDisplay>("display.main", 480, 480);
        ui = std::make_unique<wisp::ui::UiRuntime>(display, nullptr);
        ASSERT_TRUE(ui->start());
    }

    void TearDown() override {
        if (ui) ui->stop();
    }

    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui.get(), fn); }

    // Puts `obj` in `state` and waits out the theme's style transitions.
    void enter_state(lv_obj_t* obj, lv_state_t state) {
        on_ui([&] { lv_obj_add_state(obj, state); });
        wisp::os::sleep_ms(400);
    }

    std::shared_ptr<FakeDisplay> display;
    std::unique_ptr<wisp::ui::UiRuntime> ui;
};

TEST_F(ThemeTest, ScreenAndCardsUseTheSurfaces) {
    on_ui([&] {
        lv_obj_t* scr = lv_screen_active();
        EXPECT_EQ(bg(scr), theme::kBg);
        EXPECT_EQ(fg(scr), theme::kText);
        lv_obj_t* card = lv_obj_create(scr);
        EXPECT_EQ(bg(card), theme::kSurface);
        EXPECT_EQ(lv_obj_get_style_bg_opa(card, LV_PART_MAIN), LV_OPA_COVER);
        EXPECT_EQ(rgb_of(lv_obj_get_style_border_color(card, LV_PART_MAIN)), theme::kBorder);
        EXPECT_EQ(lv_obj_get_style_border_width(card, LV_PART_MAIN), theme::kBorderWidth);
        EXPECT_EQ(lv_obj_get_style_radius(card, LV_PART_MAIN), theme::kRadius);
        EXPECT_EQ(fg(card), theme::kText);
        lv_obj_delete(card);
    });
}

TEST_F(ThemeTest, AStockButtonIsPrimary) {
    lv_obj_t* btn = nullptr;
    on_ui([&] {
        btn = lv_button_create(lv_screen_active());
        EXPECT_EQ(bg(btn), theme::kAccent);
        EXPECT_EQ(fg(btn), theme::kAccentInk);
        EXPECT_EQ(lv_obj_get_style_radius(btn, LV_PART_MAIN), theme::kRadiusSm);
        EXPECT_EQ(lv_obj_get_style_shadow_width(btn, LV_PART_MAIN), 0);
    });
    enter_state(btn, LV_STATE_PRESSED);
    on_ui([&] {
        EXPECT_EQ(bg(btn), theme::pressed(theme::kAccent, theme::kAccentInk));
        // No second darkening on top of the pressed fill.
        EXPECT_EQ(lv_obj_get_style_recolor_opa(btn, LV_PART_MAIN), LV_OPA_TRANSP);
        lv_obj_remove_state(btn, LV_STATE_PRESSED);
    });
    enter_state(btn, LV_STATE_CHECKED);
    on_ui([&] {
        EXPECT_EQ(bg(btn), theme::kAccentStrong);
        EXPECT_EQ(fg(btn), theme::kAccentInk);
        lv_obj_delete(btn);
    });
}

TEST_F(ThemeTest, ButtonRolesSetTheirColours) {
    struct Case {
        ButtonRole role;
        uint32_t fill;
        uint32_t ink;
    };
    for (Case c : {Case{ButtonRole::Primary, theme::kAccent, theme::kAccentInk},
                   Case{ButtonRole::Secondary, theme::kSurface3, theme::kText},
                   Case{ButtonRole::Danger, theme::kErr, theme::kAccentInk}}) {
        SCOPED_TRACE(static_cast<int>(c.role));
        lv_obj_t* btn = nullptr;
        on_ui([&] {
            btn = lv_button_create(lv_screen_active());
            wisp::ui::set_button_role(btn, c.role);
            EXPECT_EQ(bg(btn), c.fill);
            EXPECT_EQ(fg(btn), c.ink);
        });
        enter_state(btn, LV_STATE_PRESSED);
        on_ui([&] {
            EXPECT_EQ(bg(btn), theme::pressed(c.fill, c.ink));
            EXPECT_EQ(fg(btn), c.ink);
            lv_obj_delete(btn);
        });
    }
}

TEST_F(ThemeTest, ControlsUseTheAccent) {
    on_ui([&] {
        lv_obj_t* scr = lv_screen_active();

        lv_obj_t* sw = lv_switch_create(scr);
        EXPECT_EQ(bg(sw), theme::kSurface3);
        EXPECT_EQ(bg(sw, LV_PART_KNOB), theme::kText);
        lv_obj_add_state(sw, LV_STATE_CHECKED);

        lv_obj_t* slider = lv_slider_create(scr);
        EXPECT_EQ(bg(slider), theme::kSurface3);
        EXPECT_EQ(lv_obj_get_style_bg_opa(slider, LV_PART_MAIN), LV_OPA_COVER);
        EXPECT_EQ(bg(slider, LV_PART_INDICATOR), theme::kAccent);
        EXPECT_EQ(bg(slider, LV_PART_KNOB), theme::kText);

        lv_obj_t* bar = lv_bar_create(scr);
        EXPECT_EQ(bg(bar), theme::kSurface3);
        EXPECT_EQ(bg(bar, LV_PART_INDICATOR), theme::kAccent);

        lv_obj_t* cb = lv_checkbox_create(scr);
        EXPECT_EQ(bg(cb, LV_PART_INDICATOR), theme::kSurface2);
        EXPECT_EQ(rgb_of(lv_obj_get_style_border_color(cb, LV_PART_INDICATOR)),
                  theme::kBorderStrong);

        lv_obj_t* ta = lv_textarea_create(scr);
        EXPECT_EQ(bg(ta), theme::kSurface2);
        EXPECT_EQ(rgb_of(lv_obj_get_style_border_color(ta, LV_PART_MAIN)), theme::kBorderStrong);
        EXPECT_EQ(fg(ta, LV_PART_TEXTAREA_PLACEHOLDER), theme::kTextFaint);

        lv_obj_t* dd = lv_dropdown_create(scr);
        EXPECT_EQ(bg(dd), theme::kSurface2);
        EXPECT_EQ(fg(dd), theme::kText);

        lv_obj_t* arc = lv_arc_create(scr);
        EXPECT_EQ(rgb_of(lv_obj_get_style_arc_color(arc, LV_PART_MAIN)), theme::kSurface3);
        EXPECT_EQ(rgb_of(lv_obj_get_style_arc_color(arc, LV_PART_INDICATOR)), theme::kAccent);

        lv_obj_t* box = lv_msgbox_create(scr);
        EXPECT_EQ(bg(box), theme::kSurface);
        EXPECT_EQ(lv_obj_get_style_radius(box, LV_PART_MAIN), theme::kRadiusLg);
        lv_obj_t* footer_btn = lv_msgbox_add_footer_button(box, "OK");
        EXPECT_EQ(bg(footer_btn), theme::kAccent);
        EXPECT_EQ(fg(footer_btn), theme::kAccentInk);

        EXPECT_EQ(rgb_of(lv_obj_get_style_bg_color(scr, LV_PART_SCROLLBAR)), theme::kTextDim);
    });
    // The switch's indicator once its check transition is over.
    wisp::os::sleep_ms(400);
    on_ui([&] {
        lv_obj_t* sw = lv_obj_get_child_by_type(lv_screen_active(), 0, &lv_switch_class);
        ASSERT_NE(sw, nullptr);
        EXPECT_EQ(bg(sw, LV_PART_INDICATOR), theme::kAccent);
        lv_obj_clean(lv_screen_active());
    });
}

// The keyboard keys sit on a raised band, the action keys a step lower.
TEST_F(ThemeTest, KeyboardUsesTheSurfaces) {
    on_ui([&] {
        lv_obj_t* kb = lv_keyboard_create(lv_screen_active());
        EXPECT_EQ(bg(kb), theme::kBgRaised);
        EXPECT_EQ(bg(kb, LV_PART_ITEMS), theme::kSurface3);
        EXPECT_EQ(fg(kb, LV_PART_ITEMS), theme::kText);
        lv_obj_delete(kb);
    });
}

// UiRuntime builds the theme again on every start: LVGL frees its own
// on stop.
TEST_F(ThemeTest, SurvivesARestart) {
    ui->stop();
    ui.reset();
    start();
    on_ui([&] {
        lv_obj_t* btn = lv_button_create(lv_screen_active());
        EXPECT_EQ(bg(btn), theme::kAccent);
        wisp::ui::set_button_role(btn, ButtonRole::Secondary);
        EXPECT_EQ(bg(btn), theme::kSurface3);
        lv_obj_delete(btn);
    });
}

}  // namespace
