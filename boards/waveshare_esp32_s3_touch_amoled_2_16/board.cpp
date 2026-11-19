// Board definition for the Waveshare ESP32-S3-Touch-AMOLED-2.16 (see
// docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md).
//
// The board kit (boards/common/board_kit.hpp) brings up what bmgr
// declares in this directory's board_*.yaml: display_lcd -> display.main
// (CO5300), lcd_touch -> touch.main (CST9217), the btn_* gpio peripherals
// -> button.*, audio_dac/audio_adc -> audio_out.main/audio_in.main
// (ES8311/ES7210), fs_sdcard -> storage.sd; plus storage.int and
// net.wifi. This file adds the panel facts bmgr does not carry and
// bring_up_chips(): the AXP2101, PCF85063 and QMI8658 on the shared
// i2c_master bus, through their drivers' Wisp adapters.
//
// wisp::board::create_board() (components/wisp_app/include/wisp/app/
// board.hpp) is defined here for the board firmware/CMakeLists.txt's
// WISP_BOARD selects.

#include <memory>
#include <string>

#include "board_kit.hpp"
#include "wisp/core/log.hpp"
#include "wisp/drivers/axp2101_power.hpp"
#include "wisp/drivers/pcf85063_clock.hpp"
#include "wisp/drivers/qmi8658_imu.hpp"

namespace {

using wisp::board_kit::ButtonSpec;
using wisp::board_kit::Kit;
using wisp::board_kit::Slot;
using wisp::hal::DeviceClass;
namespace role = wisp::hal::role;

constexpr const char* kTag = "board";

// The shared I2C bus in board_peripherals.yaml (touch, AXP2101, QMI8658,
// PCF85063 and the audio codecs).
constexpr const char* kI2c = "i2c_master";

constexpr Slot kPower{"power.main", DeviceClass::Power, role::kMain};
constexpr Slot kClock{"clock.main", DeviceClass::Clock, role::kMain};
constexpr Slot kImu{"imu.main", DeviceClass::Imu, role::kMain};
constexpr Slot kChips[] = {kPower, kClock, kImu};

constexpr wisp::port::esp::AudioChannel kMics[] = {wisp::port::esp::AudioChannel::Mic,
                                                   wisp::port::esp::AudioChannel::Mic};

// GPIO16 follows the PWR key (driven low while released, high while held;
// it is not the AXP2101 IRQ output); Vol+ and Vol- are active-low. Pins
// and pulls are the btn_* peripherals in board_peripherals.yaml.
const ButtonSpec kButtons[] = {
    {.slot = {"button.power", DeviceClass::Button, role::kPower},
     .bmgr_gpio = "btn_power",
     .active_low = false,
     .model = "axp2101-pwrkey"},
    {.slot = {"button.vol_up", DeviceClass::Button, ""},
     .bmgr_gpio = "btn_vol_up",
     .active_low = true},
    // Vol- is the BOOT key (GPIO0): hold-to-talk (mvp spec 6.3).
    {.slot = {"button.vol_down", DeviceClass::Button, role::kTalk},
     .bmgr_gpio = "btn_vol_down",
     .active_low = true},
};

void bring_up_chips(Kit& kit) {
    i2c_master_bus_handle_t bus = kit.i2c(kI2c);
    if (bus == nullptr) {
        for (const Slot& chip : kChips) {
            kit.failed(chip, "no i2c_master bus handle");
        }
        return;
    }

    auto power = wisp::drivers::Axp2101Power::create(bus, {.id = kPower.id, .role = kPower.role});
    // Charging-parameter configuration is best-effort (axp2101_init()'s
    // header comment): it must not stop the PMU from registering, but a
    // failure is still reported.
    if (power) {
        esp_err_t chg_err = power.value()->charging_config_status();
        if (chg_err != ESP_OK) {
            WLOG_W(kTag, "power.main: charging configuration failed: %s",
                   wisp::board_kit::err_name(chg_err).c_str());
        }
    }
    kit.add(kPower, std::move(power));
    kit.add(kClock,
            wisp::drivers::Pcf85063Clock::create(bus, {.id = kClock.id, .role = kClock.role}));
    kit.add(kImu, wisp::drivers::Qmi8658Imu::create(bus, {.id = kImu.id, .role = kImu.role}));
}

const wisp::board_kit::BoardSpec kSpec{
    .name = "waveshare_esp32_s3_touch_amoled_2_16",
    .display =
        wisp::board_kit::DisplaySpec{
            .model = "CO5300",
            .shape = "rounded_rect",
            // The vendor's nominal radius (40) is too small: arcs of known
            // radius drawn on the real panel and compared by eye put the
            // visible boundary on the diagonal about 20 px from the corner,
            // which corresponds to r=70.
            .corner_radius = 70,
            .panel = "amoled",
            // CO5300 partial updates must start and end on even coordinates.
            .area_align = 2,
            // The CO5300 is wired with a hardware reset GPIO, which makes
            // esp_lcd_panel_disp_sleep() escalate to a deep standby that
            // clears GRAM and forces a full ~1.36 s re-init on wake. MIPI
            // DCS sleep keeps GRAM: wake is SLPOUT, a settle, DISPON
            // (hardware doc, "Standby and power saving"). So EspDisplay
            // must never call esp_lcd_panel_disp_sleep()/disp_on_off() on
            // this panel. 120 ms is the MIPI DCS floor for SLPOUT to DISPON;
            // the datasheet gives no tighter number.
            .sleep = wisp::port::esp::EspDisplayConfig::PanelSleep::DcsSleep,
            .sleep_out_settle_ms = 120,
        },
    .touch = wisp::board_kit::TouchSpec{.model = "CST9217"},
    .audio_out = wisp::board_kit::AudioOutSpec{.model = "ES8311"},
    .audio_in = wisp::board_kit::AudioInSpec{.model = "ES7210", .channels = kMics},
    .sd = wisp::board_kit::SdSpec{.model = "sdcard-sdmmc"},
    .buttons = kButtons,
    .chips = kChips,
    .bring_up_chips = &bring_up_chips,
};

}  // namespace

namespace wisp::board {

std::unique_ptr<wisp::app::Board> create_board() { return wisp::board_kit::make_board(kSpec); }

}  // namespace wisp::board
