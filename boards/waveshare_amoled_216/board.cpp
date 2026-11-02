// Board definition for the Waveshare ESP32-S3-Touch-AMOLED-2.16 (see
// docs/hardware/waveshare_amoled_216.md).
//
// Buses and the esp_lcd-level display/touch devices are brought up by
// esp_board_manager (bmgr) from this directory's board_*.yaml +
// setup_device.c (generated into firmware/components/gen_bmgr_codes/ by
// `idf.py bmgr -c ../boards -b waveshare_amoled_216`). This file then
// wraps bmgr's handles in Wisp HAL devices and registers them:
//   - display_lcd -> display.main (wisp::port::esp::EspDisplay, CO5300)
//   - lcd_touch   -> touch.main   (wisp::port::esp::EspTouch, CST9217)
//   - i2c_master (bmgr peripheral, shared bus) + drivers/axp2101 ->
//     power.main (wisp::port::esp::EspPower, AXP2101)
//   - i2c_master + drivers/pcf85063 -> clock.main (wisp::port::esp::EspClock,
//     PCF85063); on success the system clock is set from the RTC
//     (settimeofday()) right after this board's init() returns -- see
//     sync_system_clock_from_rtc() below.
//   - i2c_master + drivers/qmi8658 -> imu.main (wisp::port::esp::EspImu,
//     QMI8658)
//   - plain GPIOs -> button.power/vol_up/vol_down (wisp::port::esp::
//     EspGpioButton)
// Audio/SD are added by later tasks.
//
// Every failure is recorded in the boot report and bring-up continues
// with the next device -- a missing panel or touch controller degrades
// the device (App::start() falls back to headless without display.main),
// it never blocks boot (AGENTS.md's device model rule).
//
// wisp::board::create_board() (declared in
// components/wisp_app/include/wisp/app/board.hpp) is defined here, once,
// for whichever single board a given firmware binary is built for --
// selected at CMake configure time by firmware/CMakeLists.txt's
// WISP_BOARD cache variable, which picks this very file as one of
// firmware/main's sources.

#include "wisp/app/board.hpp"

#include <sys/time.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_board_manager_err.h"
#include "esp_board_periph.h"
#include "esp_err.h"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/esp_button.hpp"
#include "wisp/port/esp/esp_clock.hpp"
#include "wisp/port/esp/esp_display.hpp"
#include "wisp/port/esp/esp_imu.hpp"
#include "wisp/port/esp/esp_power.hpp"
#include "wisp/port/esp/esp_touch.hpp"

namespace {

constexpr const char* kTag = "board";

// Device names as declared in board_devices.yaml.
constexpr const char* kBmgrDisplay = "display_lcd";
constexpr const char* kBmgrTouch = "lcd_touch";

// Shared I2C bus peripheral name as declared in board_peripherals.yaml
// (touch, AXP2101, QMI8658, PCF85063 and the audio codecs all share it).
constexpr const char* kBmgrI2cMaster = "i2c_master";

// Panel facts not carried by bmgr's config (see the hardware doc).
constexpr int kCornerRadius = 40;
// CO5300 partial updates must start/end on even coordinates.
constexpr int kAreaAlign = 2;

// Button GPIOs (see the hardware doc): all active-low. The power key's
// detect pin already has an external pull-up on this board, so it must
// not also enable the internal one; Vol+/Vol- have no external pull-up
// and need the internal one enabled.
constexpr gpio_num_t kPinBtnPower = GPIO_NUM_16;
constexpr gpio_num_t kPinBtnVolUp = GPIO_NUM_18;
constexpr gpio_num_t kPinBtnVolDown = GPIO_NUM_0;

std::string err_name(esp_err_t err) {
    if (err >= ESP_BOARD_ERR_BASE) {
        return esp_board_manager_err_to_name(err);
    }
    return esp_err_to_name(err);
}

// Whether the panel IO runs in QSPI mode (board_devices.yaml's
// io_spi_config.flags.quad_mode): QSPI panels frame raw register writes
// as (0x02 << 24) | (reg << 8), see EspDisplayConfig::qspi_cmd_framing.
bool uses_qspi(const dev_display_lcd_config_t* cfg) {
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_SPI_SUPPORT
    if (cfg->sub_type != nullptr && std::strcmp(cfg->sub_type, "spi") == 0) {
        return cfg->sub_cfg.spi.io_spi_config.flags.quad_mode;
    }
#else
    (void)cfg;
#endif
    return false;
}

void record_failed(wisp::core::BootReport& report, const char* id, wisp::hal::DeviceClass cls,
                   std::string reason) {
    WLOG_E(kTag, "%s failed: %s", id, reason.c_str());
    report.record({id, cls, wisp::core::DeviceState::Failed, std::move(reason)});
}

void add_device(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                std::shared_ptr<wisp::hal::Device> device) {
    const wisp::hal::DeviceInfo& info = device->info();
    auto added = registry.add(device);
    if (!added) {
        record_failed(report, info.id.c_str(), info.cls, "registry.add: " + added.error().message);
    }
}

void init_display(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    constexpr const char* id = "display.main";
    constexpr auto cls = wisp::hal::DeviceClass::Display;

    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(kBmgrDisplay, &handle);
    if (err != ESP_OK || handle == nullptr) {
        record_failed(report, id, cls, "bmgr display_lcd handle: " + err_name(err));
        return;
    }
    void* config = nullptr;
    err = esp_board_manager_get_device_config(kBmgrDisplay, &config);
    if (err != ESP_OK || config == nullptr) {
        record_failed(report, id, cls, "bmgr display_lcd config: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_display_lcd_handles_t*>(handle);
    auto* cfg = static_cast<const dev_display_lcd_config_t*>(config);

    wisp::port::esp::EspDisplayConfig dc;
    dc.model = "CO5300";
    dc.width = cfg->lcd_width;
    dc.height = cfg->lcd_height;
    dc.shape = "rounded_rect";
    dc.corner_radius = kCornerRadius;
    dc.panel = "amoled";
    dc.area_align = kAreaAlign;
    dc.qspi_cmd_framing = uses_qspi(cfg);
    dc.swap_rgb565_bytes = cfg->frame_format == DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_BE;

    auto display =
        wisp::port::esp::EspDisplay::create(handles->panel_handle, handles->io_handle, dc);
    if (!display) {
        record_failed(report, id, cls, display.error().message);
        return;
    }
    add_device(registry, report, display.value());
}

void init_touch(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    constexpr const char* id = "touch.main";
    constexpr auto cls = wisp::hal::DeviceClass::Touch;

    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(kBmgrTouch, &handle);
    if (err != ESP_OK || handle == nullptr) {
        record_failed(report, id, cls, "bmgr lcd_touch handle: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_lcd_touch_handles_t*>(handle);
    if (handles->touch_handle == nullptr) {
        record_failed(report, id, cls, "bmgr lcd_touch: null touch handle");
        return;
    }

    // Coordinate range: the touch driver's own x_max/y_max (480x480 here;
    // the driver applies the swap_xy/mirror_y that setup_device.c
    // configures). If bmgr can't give us its config the range stays 0x0,
    // which EspTouch treats as "unknown, don't clamp".
    int width = 0;
    int height = 0;
    void* config = nullptr;
    err = esp_board_manager_get_device_config(kBmgrTouch, &config);
    if (err == ESP_OK && config != nullptr) {
        auto* cfg = static_cast<const dev_lcd_touch_config_t*>(config);
        width = cfg->touch_config.x_max;
        height = cfg->touch_config.y_max;
    } else {
        WLOG_W(kTag, "touch.main: no bmgr lcd_touch config (%s); coordinates won't be clamped",
               err_name(err).c_str());
    }

    wisp::port::esp::EspTouchConfig tc;
    tc.model = "CST9217";
    tc.width = width;
    tc.height = height;
    add_device(registry, report,
               std::make_shared<wisp::port::esp::EspTouch>(handles->touch_handle, tc));
}

// The shared I2C bus (touch above gets its handle from bmgr's lcd_touch
// device directly; power/clock/imu below instead add their own device
// onto this same bus, fetched as a *peripheral* handle -- see
// board_peripherals.yaml's i2c_master entry).
i2c_master_bus_handle_t get_i2c_bus() {
    void* handle = nullptr;
    esp_err_t err = esp_board_periph_get_handle(kBmgrI2cMaster, &handle);
    if (err != ESP_OK || handle == nullptr) {
        WLOG_W(kTag, "i2c_master bus handle unavailable: %s", err_name(err).c_str());
        return nullptr;
    }
    return static_cast<i2c_master_bus_handle_t>(handle);
}

void init_power(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                i2c_master_bus_handle_t i2c_bus) {
    constexpr const char* id = "power.main";
    constexpr auto cls = wisp::hal::DeviceClass::Power;
    if (i2c_bus == nullptr) {
        record_failed(report, id, cls, "no i2c_master bus handle");
        return;
    }
    auto power = wisp::port::esp::EspPower::create(i2c_bus, "AXP2101");
    if (!power) {
        record_failed(report, id, cls, power.error().message);
        return;
    }
    add_device(registry, report, power.value());
}

void init_clock(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                i2c_master_bus_handle_t i2c_bus) {
    constexpr const char* id = "clock.main";
    constexpr auto cls = wisp::hal::DeviceClass::Clock;
    if (i2c_bus == nullptr) {
        record_failed(report, id, cls, "no i2c_master bus handle");
        return;
    }
    auto clock = wisp::port::esp::EspClock::create(i2c_bus, "PCF85063");
    if (!clock) {
        record_failed(report, id, cls, clock.error().message);
        return;
    }
    add_device(registry, report, clock.value());
}

void init_imu(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
              i2c_master_bus_handle_t i2c_bus) {
    constexpr const char* id = "imu.main";
    constexpr auto cls = wisp::hal::DeviceClass::Imu;
    if (i2c_bus == nullptr) {
        record_failed(report, id, cls, "no i2c_master bus handle");
        return;
    }
    auto imu = wisp::port::esp::EspImu::create(i2c_bus, "QMI8658");
    if (!imu) {
        record_failed(report, id, cls, imu.error().message);
        return;
    }
    add_device(registry, report, imu.value());
}

struct ButtonSpec {
    gpio_num_t pin;
    bool pull_up;  // Internal pull-up: see kPinBtnPower's comment above.
    const char* id;
    const char* model;
};

void init_buttons(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    constexpr auto cls = wisp::hal::DeviceClass::Button;
    constexpr ButtonSpec kButtons[] = {
        {kPinBtnPower, false, "button.power", "axp2101-pwrkey"},
        {kPinBtnVolUp, true, "button.vol_up", "gpio-button"},
        {kPinBtnVolDown, true, "button.vol_down", "gpio-button"},
    };
    for (const ButtonSpec& spec : kButtons) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << spec.pin;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = spec.pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            record_failed(report, spec.id, cls, "gpio_config: " + err_name(err));
            continue;
        }
        add_device(registry, report,
                   std::make_shared<wisp::port::esp::EspGpioButton>(spec.pin, /*active_low=*/true,
                                                                    spec.id, spec.model));
    }
}

// Sets the system clock from clock.main's RTC, if (and only if) it was
// registered and currently holds a plausible time -- EspClock::get_unix()
// (drivers/pcf85063) already fails rather than return a bogus reading
// when the chip's oscillator-stop flag is set or the decoded year looks
// wrong, so this never needs to re-check that itself; it just must not
// settimeofday() on a failed Result.
void sync_system_clock_from_rtc(wisp::core::DeviceRegistry& registry) {
    auto clock = registry.get<wisp::hal::Clock>("clock.main");
    if (!clock) {
        return;
    }
    auto unix_seconds = clock->get_unix();
    if (!unix_seconds) {
        WLOG_W(kTag, "clock.main: RTC time unavailable/invalid, system clock left unset (%s)",
               unix_seconds.error().message.c_str());
        return;
    }
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_seconds.value());
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    WLOG_I(kTag, "system clock set from clock.main RTC (unix=%lld)",
           static_cast<long long>(unix_seconds.value()));
}

class WaveshareAmoled216Board : public wisp::app::Board {
public:
    std::string name() const override { return "waveshare_amoled_216"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        // esp_board_manager_init() only fails if a *peripheral* (bus) or
        // its own setup fails -- an individual device that fails to come
        // up is logged by bmgr and simply has no handle afterwards, which
        // init_display()/init_touch() below report per device.
        esp_err_t err = esp_board_manager_init();
        if (err != ESP_OK) {
            std::string reason = "esp_board_manager_init: " + err_name(err);
            record_failed(report, "display.main", wisp::hal::DeviceClass::Display, reason);
            record_failed(report, "touch.main", wisp::hal::DeviceClass::Touch, reason);
            record_failed(report, "power.main", wisp::hal::DeviceClass::Power, reason);
            record_failed(report, "clock.main", wisp::hal::DeviceClass::Clock, reason);
            record_failed(report, "imu.main", wisp::hal::DeviceClass::Imu, reason);
            record_failed(report, "button.power", wisp::hal::DeviceClass::Button, reason);
            record_failed(report, "button.vol_up", wisp::hal::DeviceClass::Button, reason);
            record_failed(report, "button.vol_down", wisp::hal::DeviceClass::Button, reason);
            return;
        }

        init_display(registry, report);
        init_touch(registry, report);

        i2c_master_bus_handle_t i2c_bus = get_i2c_bus();
        init_power(registry, report, i2c_bus);
        init_clock(registry, report, i2c_bus);
        init_imu(registry, report, i2c_bus);
        // Only after clock.main has had a chance to register -- see
        // sync_system_clock_from_rtc()'s own comment.
        sync_system_clock_from_rtc(registry);
        init_buttons(registry, report);
    }
};

}  // namespace

namespace wisp::board {

std::unique_ptr<wisp::app::Board> create_board() {
    return std::make_unique<WaveshareAmoled216Board>();
}

}  // namespace wisp::board
