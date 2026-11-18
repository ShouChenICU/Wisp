// Board definition for the Waveshare ESP32-S3-Touch-AMOLED-1.75C (see
// docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md).
//
// The board kit (boards/common/board_kit.hpp) brings up what bmgr
// declares in this directory's board_*.yaml: display_lcd -> display.main
// (CO5300, 466x466 circle), lcd_touch -> touch.main (CST9217), the btn_*
// gpio peripherals -> button.*, audio_dac/audio_adc ->
// audio_out.main/audio_in.main (ES8311/ES7210); plus storage.int and
// net.wifi. There is no SD card slot, so no storage.sd. This file adds the
// panel facts bmgr does not carry, bring_up_chips() for the chips on the
// shared i2c_master bus, and the PMU fallback for the PWR key:
//   - AXP2101 -> power.main;
//   - PCF85063 -> clock.main, only if one answers at 0x51: the vendor's
//     BSP, schematic and examples show no RTC on this board, though its
//     product page lists one. Absent, clock.main is recorded as missing and
//     the time comes from SNTP alone;
//   - QMI8658 at 0x6B or 0x6A -> imu.main.
//
// wisp::board::create_board() (components/wisp_app/include/wisp/app/
// board.hpp) is defined here for the board firmware/CMakeLists.txt's
// WISP_BOARD selects.

#include <memory>
#include <optional>
#include <string>
#include <utility>

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
// the audio codecs and a PCF85063, if fitted).
constexpr const char* kI2c = "i2c_master";

constexpr Slot kPower{"power.main", DeviceClass::Power, role::kMain};
constexpr Slot kClock{"clock.main", DeviceClass::Clock, role::kMain};
constexpr Slot kImu{"imu.main", DeviceClass::Imu, role::kMain};
constexpr Slot kChips[] = {kPower, kClock, kImu};

// The IMU's SA0 strap: Waveshare's examples pass SensorLib's
// QMI8658_L_SLAVE_ADDRESS, which SensorLib defines as 0x6B (its "L"/"H"
// names are the reverse of the datasheet's), so 0x6B is tried first and
// 0x6A second; bring_up_chips() uses whichever answers.
constexpr uint8_t kImuAddrs[] = {QMI8658_I2C_ADDR_HIGH, QMI8658_I2C_ADDR_LOW};
constexpr int kImuProbeTimeoutMs = 50;
// The QMI8658 sits a quarter turn about z from the screen: measured on the
// device, held upright the chip's +x pointed up and tilted with the right
// edge raised its -y pointed up. So screen x = -chip y, screen y = chip x.
constexpr wisp::hal::ImuAxisMap kImuMount{.from = {1, 0, 2}, .negate = {true, false, false}};

// PCF85063's fixed 7-bit address; probed before Pcf85063Clock::create().
constexpr uint16_t kRtcAddr = 0x51;
constexpr int kRtcProbeTimeoutMs = 20;

constexpr wisp::port::esp::AudioChannel kMics[] = {wisp::port::esp::AudioChannel::Mic,
                                                   wisp::port::esp::AudioChannel::Mic};

// The PMU, for pmu_power_key(); null when it did not come up.
std::shared_ptr<wisp::drivers::Axp2101Power> g_power;

// The PWR key through the AXP2101's PWRON status, polled: the kit uses it
// only when GPIO3 (or the shared edge service) cannot be prepared.
wisp::Result<std::shared_ptr<wisp::hal::Device>> pmu_power_key(Kit&, const ButtonSpec& spec) {
    using Made = wisp::Result<std::shared_ptr<wisp::hal::Device>>;
    if (!g_power) {
        return Made::err(wisp::ErrorCode::DeviceFailure, "power.main unavailable");
    }
    WLOG_W(kTag,
           "button.power: PMU fallback; GPIO3 polarity/strapping/wake NOT hardware validated; "
           "standby metadata 100 ms");
    return Made::ok(std::make_shared<wisp::drivers::Axp2101PowerKey>(
        g_power, wisp::drivers::Axp2101PowerKeyConfig{.id = spec.slot.id, .role = spec.slot.role}));
}

// PWR drives the AXP2101's PWRON and, through a MOSFET, SYS_OUT on GPIO3,
// which is high while the key is held (schematic; the electrical checks
// are pending in the hardware doc). BOOT (GPIO0) is active low. It has no
// volume label, so it gets its own id rather than the 2.16's
// button.vol_down; the shell finds the talk key by its role. Pins and
// pulls are the btn_* peripherals in board_peripherals.yaml.
const ButtonSpec kButtons[] = {
    {.slot = {"button.power", DeviceClass::Button, role::kPower},
     .bmgr_gpio = "btn_power",
     .active_low = false,
     .fallback = &pmu_power_key},
    {.slot = {"button.boot", DeviceClass::Button, role::kTalk},
     .bmgr_gpio = "btn_boot",
     .active_low = true},
};

void bring_up_power(Kit& kit, i2c_master_bus_handle_t bus) {
    auto power = wisp::drivers::Axp2101Power::create(bus, {.id = kPower.id, .role = kPower.role});
    if (!power) {
        kit.failed(kPower, power.error().message);
        return;
    }
    // Charging-parameter configuration is best-effort (axp2101_init()'s
    // header comment): it must not stop the PMU from registering, but a
    // failure is still reported.
    esp_err_t chg_err = power.value()->charging_config_status();
    if (chg_err != ESP_OK) {
        WLOG_W(kTag, "power.main: charging configuration failed: %s",
               wisp::board_kit::err_name(chg_err).c_str());
    }
    // The power key's fallback needs the PMU even if the registry refused
    // power.main itself.
    g_power = power.value();
    kit.add_device(kPower, g_power);
}

void bring_up_clock(Kit& kit, i2c_master_bus_handle_t bus) {
    // A board without the chip is not a failure: nothing answers at 0x51.
    esp_err_t probe = i2c_master_probe(bus, kRtcAddr, kRtcProbeTimeoutMs);
    if (probe == ESP_ERR_NOT_FOUND) {
        WLOG_I(kTag, "clock.main: no RTC at 0x%02X; time comes from SNTP",
               static_cast<unsigned>(kRtcAddr));
        kit.missing(kClock, "no RTC");
        return;
    }
    if (probe != ESP_OK) {
        kit.failed(kClock, "i2c probe: " + wisp::board_kit::err_name(probe));
        return;
    }
    kit.add(kClock,
            wisp::drivers::Pcf85063Clock::create(bus, {.id = kClock.id, .role = kClock.role}));
}

void bring_up_imu(Kit& kit, i2c_master_bus_handle_t bus) {
    uint8_t addr = 0;
    for (uint8_t a : kImuAddrs) {
        if (i2c_master_probe(bus, a, kImuProbeTimeoutMs) == ESP_OK) {
            addr = a;
            break;
        }
    }
    if (addr == 0) {
        kit.failed(kImu, "no answer at 0x6B or 0x6A");
        return;
    }
    kit.add(kImu, wisp::drivers::Qmi8658Imu::create(
                      bus, {.id = kImu.id, .role = kImu.role, .addr = addr, .mount = kImuMount}));
}

void bring_up_chips(Kit& kit) {
    i2c_master_bus_handle_t bus = kit.i2c(kI2c);
    if (bus == nullptr) {
        for (const Slot& chip : kChips) {
            kit.failed(chip, "no i2c_master bus handle");
        }
        return;
    }
    bring_up_power(kit, bus);
    bring_up_clock(kit, bus);
    bring_up_imu(kit, bus);
}

const wisp::board_kit::BoardSpec kSpec{
    .name = "waveshare_esp32_s3_touch_amoled_1_75c",
    .display =
        wisp::board_kit::DisplaySpec{
            .model = "CO5300",
            // Round panel: safe_rect() works from the inscribed circle, so
            // no corner_radius. The exact visible boundary is still to be
            // checked on the device (hardware doc's acceptance checklist).
            .shape = "circle",
            .panel = "amoled",
            // CO5300 partial updates must start and end on even
            // coordinates (the BSP's LVGL rounder does the same).
            .area_align = 2,
            // DCS sleep instead of esp_lcd_panel_disp_sleep(), as on the
            // 2.16: the panel has a hardware reset GPIO, which turns the
            // driver's sleep into a GRAM-clearing deep standby. 120 ms is
            // the MIPI DCS floor for SLPOUT to DISPON.
            .sleep = wisp::port::esp::EspDisplayConfig::PanelSleep::DcsSleep,
            .sleep_out_settle_ms = 120,
        },
    .touch = wisp::board_kit::TouchSpec{.model = "CST9217"},
    .audio_out = wisp::board_kit::AudioOutSpec{.model = "ES8311"},
    .audio_in = wisp::board_kit::AudioInSpec{.model = "ES7210", .channels = kMics},
    .sd = std::nullopt,  // no SD card slot
    .buttons = kButtons,
    .chips = kChips,
    .bring_up_chips = &bring_up_chips,
};

}  // namespace

namespace wisp::board {

std::unique_ptr<wisp::app::Board> create_board() { return wisp::board_kit::make_board(kSpec); }

}  // namespace wisp::board
