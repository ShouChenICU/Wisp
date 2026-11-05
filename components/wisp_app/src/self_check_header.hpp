#pragma once

// The top of the self-check screen:
//   - SelfCheckHeader: the page title ("Self Check", in the page's
//     language) as a large label that wraps, never clips, and the two
//     summary chips, "Devices n/m OK" and "Tests k/3 done". Tapping the
//     device chip asks the owner to show the first device that is not OK.
//   - BoardSection: the board card, aligned "label  value" rows for the
//     board name (it wraps), the target, the firmware version and the boot
//     time, and the page's language switcher, one button per language
//     named in that language.
//
// UI thread only. The chips only touch LVGL when their text or colour
// changes.

#include <cstdint>
#include <functional>
#include <string>

#include "lvgl.h"
#include "self_check_layout.hpp"
#include "self_check_model.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

class SelfCheckHeader {
public:
    // Appends the title and the chips to `col` (which owns the widgets).
    // `on_devices_tap` runs on the UI thread when the device chip is
    // tapped. `text` must outlive the widgets.
    void build(lv_obj_t* col, PageText& text, std::function<void()> on_devices_tap);

    void show_devices(const DeviceSummary& summary);
    void show_tests(int passed);

private:
    static void devices_chip_cb(lv_event_t* e);

    const PageText* text_ = nullptr;
    lv_obj_t* devices_chip_ = nullptr;
    lv_obj_t* devices_label_ = nullptr;
    lv_obj_t* tests_chip_ = nullptr;
    lv_obj_t* tests_label_ = nullptr;
    std::function<void()> on_devices_tap_;
};

class BoardSection {
public:
    // The switcher's buttons, left to right.
    static constexpr ui::Lang kLangs[] = {ui::Lang::En, ui::Lang::Zh, ui::Lang::Ja};

    struct Info {
        std::string name;
        // Empty: no target row.
        std::string target;
        std::string version;
    };

    // Appends the card to `col` (which owns the widgets). `on_lang` runs on
    // the UI thread when a language button is clicked.
    void build(lv_obj_t* col, PageText& text, const Info& info,
               std::function<void(ui::Lang)> on_lang);

    void set_boot_ms(uint64_t boot_ms);
    // Marks `lang`'s button as the selected one.
    void show_lang(ui::Lang lang);

private:
    static void lang_cb(lv_event_t* e);

    lv_obj_t* boot_ = nullptr;
    // Indexed like kLangs.
    lv_obj_t* lang_buttons_[sizeof(kLangs) / sizeof(kLangs[0])] = {};
    std::function<void(ui::Lang)> on_lang_;
};

}  // namespace wisp::app
