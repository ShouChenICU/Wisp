// Board definition for the Waveshare ESP32-S3-Touch-AMOLED-2.16 (see
// docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md).
//
// Buses and the esp_lcd-level display/touch devices are brought up by
// esp_board_manager (bmgr) from this directory's board_*.yaml +
// setup_device.c (generated into firmware/components/gen_bmgr_codes/ by
// `idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16`). This file then
// wraps bmgr's handles in Wisp HAL devices and registers them:
//   - display_lcd -> display.main (wisp::port::esp::EspDisplay, CO5300)
//   - lcd_touch   -> touch.main   (wisp::port::esp::EspTouch, CST9217)
//   - i2c_master (bmgr peripheral, shared bus) + drivers/axp2101 ->
//     power.main (wisp::port::esp::EspPower, AXP2101)
//   - i2c_master + drivers/pcf85063 -> clock.main (wisp::port::esp::EspClock,
//     PCF85063); on success the system clock is set from the RTC
//     (port wall_clock.hpp) right after this board's init() returns -- see
//     sync_system_clock_from_rtc() below.
//   - i2c_master + drivers/qmi8658 -> imu.main (wisp::port::esp::EspImu,
//     QMI8658)
//   - plain GPIOs -> button.power/vol_up/vol_down (wisp::port::esp::
//     EspGpioButton)
//   - bmgr's audio_dac/audio_adc esp_codec_dev handles ->
//     audio_out.main/audio_in.main (wisp::port::esp::EspAudioOut/
//     EspAudioIn, ES8311/ES7210)
//   - esp_vfs_littlefs_register() directly (not a bmgr device) ->
//     storage.int (wisp::port::esp::EspInternalStorage).
//     Registered before esp_board_manager_init() below, since it doesn't
//     depend on bmgr at all -- a bmgr/peripheral failure must not force
//     storage.int missing too.
//   - bmgr's fs_sdcard SDMMC device -> storage.sd (wisp::port::esp::
//     EspSdStorage) -- registered even when no card is present
//     at boot (DeviceState::Missing/"no card"), since it hot-plugs later
//     via a worker task EspSdStorage itself owns (see that class's
//     comment on why: online()/capacity() must stay cheap, since they're
//     polled by the UI thread and by a shared-dispatch Timer task)
//   - the chip's own radio -> net.wifi (wisp::port::esp::EspWifi), brought
//     up last, on every path (even when bmgr fails): nothing else on the
//     board depends on it, and starting it after the display keeps the
//     Wi-Fi driver's internal-RAM allocations from crowding out the
//     panel's DMA buffers. Its radio being on is also what makes
//     Platform::random a true RNG (see firmware/main/main.cpp).
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

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "dev_audio_codec.h"
#include "dev_display_lcd.h"
#include "dev_fs_fat.h"
#include "dev_lcd_touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_board_manager_err.h"
#include "esp_board_periph.h"
#include "esp_err.h"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/esp_audio.hpp"
#include "wisp/port/esp/esp_button.hpp"
#include "wisp/port/esp/esp_clock.hpp"
#include "wisp/port/esp/esp_display.hpp"
#include "wisp/port/esp/esp_imu.hpp"
#include "wisp/port/esp/esp_power.hpp"
#include "wisp/port/esp/esp_storage.hpp"
#include "wisp/port/esp/esp_touch.hpp"
#include "wisp/port/esp/esp_wifi.hpp"
#include "wisp/port/esp/wall_clock.hpp"

namespace {

constexpr const char* kTag = "board";

// Device names as declared in board_devices.yaml.
constexpr const char* kBmgrDisplay = "display_lcd";
constexpr const char* kBmgrTouch = "lcd_touch";
constexpr const char* kBmgrAudioDac = "audio_dac";
constexpr const char* kBmgrAudioAdc = "audio_adc";
constexpr const char* kBmgrSdcard = "fs_sdcard";

// Must match board_devices.yaml's fs_sdcard device's config.mount_point.
constexpr const char* kSdMountPoint = "/sd";
// Chip/module string reported as storage.sd's DeviceInfo::model. Passed
// into EspSdStorage's constructor rather than hard-coded in wisp_port_esp
// (AGENTS.md's "板子无关" rule).
constexpr const char* kSdModel = "sdcard-sdmmc";

// Shared I2C bus peripheral name as declared in board_peripherals.yaml
// (touch, AXP2101, QMI8658, PCF85063 and the audio codecs all share it).
constexpr const char* kBmgrI2cMaster = "i2c_master";

// Panel facts not carried by bmgr's config (see the hardware doc). The
// vendor's nominal corner radius (40) is too small: arcs of known radius
// drawn on the real panel and compared by eye put the visible boundary
// on the diagonal at about 20 px from the corner, which corresponds to
// r=70.
constexpr int kCornerRadius = 70;
// CO5300 partial updates must start/end on even coordinates.
constexpr int kAreaAlign = 2;
// Deliberate, user-approved deviation from calling
// esp_lcd_panel_disp_sleep() directly -- see
// docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md's "待机与省电" section: this
// board's CO5300 is wired with a hardware reset GPIO
// (board_devices.yaml's lcd_panel_config.reset_gpio_num), which makes
// esp_lcd_panel_disp_sleep() escalate to a deep standby that clears GRAM
// and forces a full ~1.36s re-init sequence on wake. Standard MIPI DCS
// sleep (EspDisplayConfig::PanelSleep::DcsSleep) avoids both: wake is
// just SLPOUT + settle + DISPON, with GRAM intact. 120ms is the MIPI DCS
// spec's own floor for the SLPOUT-to-DISPON wait; this panel's datasheet
// gives no tighter number, so this board uses that floor as-is.
constexpr uint32_t kDisplaySleepOutSettleMs = 120;

// Button GPIOs (see the hardware doc). GPIO16 follows the PWR key's
// state, measured on this board: driven low while released and high
// while held -- it is not the AXP2101 IRQ output. The internal pull-down
// only keeps it defined should the driver ever release the line.
// Vol+/Vol- are active-low with no external pull-up, so they need the
// internal pull-up.
constexpr gpio_num_t kPinBtnPower = GPIO_NUM_16;
constexpr gpio_num_t kPinBtnVolUp = GPIO_NUM_18;
constexpr gpio_num_t kPinBtnVolDown = GPIO_NUM_0;

// Outputs that must keep driving their pin through every automatic
// light sleep, not just
// the gpio_config() these are set up with once at boot -- see
// hold_outputs_during_light_sleep()'s comment below for why this is
// needed at all. Pin numbers match board_devices.yaml's
// lcd_panel_config.reset_gpio_num (display reset), setup_device.c's
// touch_cfg.rst_gpio_num (touch reset), board_peripherals.yaml's
// gpio_pa_control.config.pin (speaker PA enable), and
// board_devices.yaml's io_spi_config.cs_gpio_num (panel chip-select) --
// duplicated here as plain numbers (rather than shared constants with
// those YAML/C files) because bmgr's generated code and this
// hand-written file have no shared header to put one in; keep these
// four in sync if any of those four pins ever changes.
constexpr gpio_num_t kPinDisplayReset = GPIO_NUM_39;
constexpr gpio_num_t kPinTouchReset = GPIO_NUM_40;
constexpr gpio_num_t kPinAudioPaEnable = GPIO_NUM_46;
// With EspDisplayConfig::PanelSleep::DcsSleep, the panel stays
// powered and keeps its register/GRAM state through standby -- CS
// floating (rather than held at its idle-high level) while SPI's SCLK
// also floats risks noise on SCLK being latched as spurious command
// bits if CS happens to glitch low. Low risk (this board's panel is
// otherwise untouched during light sleep), but cheap to close.
constexpr gpio_num_t kPinDisplayCs = GPIO_NUM_12;

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
    dc.rgb565_be = cfg->frame_format == DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_BE;
    // Standard MIPI DCS sleep instead of this panel's deep-standby path
    // (see kDisplaySleepOutSettleMs's comment above), so EspDisplay must
    // never call esp_lcd_panel_disp_sleep()/disp_on_off() on this handle
    // either.
    dc.panel_sleep = wisp::port::esp::EspDisplayConfig::PanelSleep::DcsSleep;
    dc.sleep_out_settle_ms = kDisplaySleepOutSettleMs;

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
    // Charging-parameter configuration is best-effort (see
    // axp2101_init()'s header comment): it must not stop this device
    // from registering, but a failure must still be reported.
    esp_err_t chg_err = power.value()->charging_config_status();
    if (chg_err != ESP_OK) {
        WLOG_W(kTag, "power.main: charging configuration failed: %s", err_name(chg_err).c_str());
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
    bool active_low;  // Pressed reads 0; also selects pull-up vs. pull-down.
    const char* id;
    const char* model;
    const char* role;  // caps "role", or "" (wisp/hal/button.hpp)
};

void init_buttons(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    constexpr auto cls = wisp::hal::DeviceClass::Button;
    constexpr ButtonSpec kButtons[] = {
        {kPinBtnPower, false, "button.power", "axp2101-pwrkey", ""},
        {kPinBtnVolUp, true, "button.vol_up", "gpio-button", ""},
        // Vol- is the BOOT key (GPIO0): hold-to-talk (mvp spec 6.3).
        {kPinBtnVolDown, true, "button.vol_down", "gpio-button", "talk"},
    };
    for (const ButtonSpec& spec : kButtons) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << spec.pin;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = spec.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = spec.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            record_failed(report, spec.id, cls, "gpio_config: " + err_name(err));
            continue;
        }
        add_device(registry, report,
                   std::make_shared<wisp::port::esp::EspGpioButton>(
                       spec.pin, spec.active_low, spec.id, spec.model, spec.role));
    }
}

// firmware/sdkconfig.defaults'
// CONFIG_FREERTOS_USE_TICKLESS_IDLE=y pulls in
// CONFIG_PM_SLP_DISABLE_GPIO on this chip (ESP-IDF's
// ESP_SLEEP_GPIO_RESET_WORKAROUND Kconfig default for S3), which
// isolates *every* GPIO -- direction, level and pull all disabled, i.e.
// floating -- for the duration of each automatic light sleep. Any output
// this board needs to keep actually driving through that must opt out
// with gpio_sleep_sel_dis(); otherwise it floats for as long as the chip
// is light-sleeping, repeatedly, for the entire time the device is in
// standby:
//   - the display panel's RST: with EspDisplayConfig::PanelSleep::
//     DcsSleep (see kDisplaySleepOutSettleMs's comment), an unexpected
//     reset pulse while asleep would undo the whole point of that
//     option -- the panel would come back needing a full re-init,
//     exactly the slow/flashing wake this board chose DcsSleep to avoid;
//   - the touch controller's RST: same risk, for CST9217;
//   - the speaker PA's enable line: floating (rather than held low, its
//     configured default) risks an audible pop or needless leakage
//     current through the amplifier while in standby;
//   - the display panel's CS: see kPinDisplayCs's
//     comment -- low risk, but cheap to hold at its idle-high level
//     alongside the other three.
// See docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md's "待机与省电" section for
// the on-device verification step this motivates.
void hold_outputs_during_light_sleep() {
    constexpr gpio_num_t kPins[] = {kPinDisplayReset, kPinTouchReset, kPinAudioPaEnable,
                                    kPinDisplayCs};
    for (gpio_num_t pin : kPins) {
        esp_err_t err = gpio_sleep_sel_dis(pin);
        if (err != ESP_OK) {
            WLOG_W(kTag, "gpio_sleep_sel_dis(%d) failed: %s", static_cast<int>(pin),
                   err_name(err).c_str());
        }
    }
}

// Fetches bmgr's dev_audio_codec_handles_t for `dev_name`, plus the I2S
// channel its config's i2s_cfg names (the device's set_power() turns that
// channel off in standby -- see wisp/port/esp/esp_audio.hpp), and wraps
// both in an EspAudioOut/EspAudioIn via `make`. Shared by
// init_audio_out()/init_audio_in() below -- the only difference between
// the two is which wisp::port::esp::Esp* type and HAL id/class `make`
// builds.
template <class Device>
void init_audio_device(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                       const char* dev_name, const char* id, wisp::hal::DeviceClass cls,
                       const char* model,
                       wisp::Result<std::shared_ptr<Device>> (*make)(esp_codec_dev_handle_t,
                                                                     i2s_chan_handle_t,
                                                                     std::string)) {
    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(dev_name, &handle);
    if (err != ESP_OK || handle == nullptr) {
        record_failed(report, id, cls,
                      std::string("bmgr ") + dev_name + " handle: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_audio_codec_handles_t*>(handle);
    if (handles->codec_dev == nullptr) {
        record_failed(report, id, cls, std::string("bmgr ") + dev_name + ": null codec_dev handle");
        return;
    }
    void* config = nullptr;
    err = esp_board_manager_get_device_config(dev_name, &config);
    if (err != ESP_OK || config == nullptr) {
        record_failed(report, id, cls,
                      std::string("bmgr ") + dev_name + " config: " + err_name(err));
        return;
    }
    const char* i2s_name = static_cast<dev_audio_codec_config_t*>(config)->i2s_cfg.name;
    void* i2s_channel = nullptr;
    err = i2s_name != nullptr ? esp_board_manager_get_periph_handle(i2s_name, &i2s_channel)
                              : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK || i2s_channel == nullptr) {
        record_failed(report, id, cls, std::string("bmgr ") + dev_name + " i2s: " + err_name(err));
        return;
    }
    auto device = make(handles->codec_dev, static_cast<i2s_chan_handle_t>(i2s_channel), model);
    if (!device) {
        record_failed(report, id, cls, device.error().message);
        return;
    }
    add_device(registry, report, device.value());
}

void init_audio(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    init_audio_device<wisp::port::esp::EspAudioOut>(
        registry, report, kBmgrAudioDac, "audio_out.main", wisp::hal::DeviceClass::AudioOut,
        "ES8311", &wisp::port::esp::EspAudioOut::create);
    init_audio_device<wisp::port::esp::EspAudioIn>(registry, report, kBmgrAudioAdc, "audio_in.main",
                                                   wisp::hal::DeviceClass::AudioIn, "ES7210",
                                                   &wisp::port::esp::EspAudioIn::create);
}

// Internal LittleFS storage: esp_vfs_littlefs_register() directly, not a
// bmgr device at all (see EspInternalStorage's comment) -- so this must
// run *before* esp_board_manager_init() below, not after it. Previously
// this ran only on the success path of that call, so a bmgr/peripheral
// failure (e.g. the display bus) fell through to a blanket
// record_failed("storage.int", ...) even though internal storage was
// never actually attempted, forcing the app's read-only degrade for a
// reason unrelated to storage.
void init_internal_storage(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    auto internal = wisp::port::esp::EspInternalStorage::create();
    if (!internal) {
        record_failed(report, "storage.int", wisp::hal::DeviceClass::Storage,
                      internal.error().message);
        return;
    }
    add_device(registry, report, internal.value());
}

// The SD card: bmgr's fs_sdcard device, already attempted by
// esp_board_manager_init() just before this is called (so only called on
// that success path -- unlike init_internal_storage() above). storage.sd
// is always registered, even absent a card at boot -- see EspSdStorage's
// comment -- so the app can observe it being hot-plugged in later; a
// missing card degrades to DeviceState::Missing, never Failed, and never
// blocks boot.
void init_sd_storage(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    constexpr auto cls = wisp::hal::DeviceClass::Storage;

    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(kBmgrSdcard, &handle);
    sdmmc_card_t* card = nullptr;
    bool mounted = (err == ESP_OK && handle != nullptr);
    if (mounted) {
        card = static_cast<dev_fs_fat_handle_t*>(handle)->card;
    }
    auto sd = std::make_shared<wisp::port::esp::EspSdStorage>(kSdMountPoint, kBmgrSdcard, kSdModel,
                                                              mounted, card);
    auto added = registry.add(sd);
    if (!added) {
        record_failed(report, "storage.sd", cls, "registry.add: " + added.error().message);
    } else if (!mounted) {
        report.record({"storage.sd", cls, wisp::core::DeviceState::Missing, "no card"});
    }
}

// The ESP32-S3's built-in 2.4 GHz radio. A failure only takes networking
// away (App records the "net" note as "missing").
void init_wifi(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    wisp::port::esp::EspWifiConfig config;
    config.id = "net.wifi";
    config.model = "ESP32-S3";
    config.bands = {"2.4GHz"};
    auto wifi = wisp::port::esp::EspWifi::create(std::move(config));
    if (!wifi) {
        record_failed(report, "net.wifi", wisp::hal::DeviceClass::Network, wifi.error().message);
        return;
    }
    add_device(registry, report, wifi.value());
}

// Sets the system clock from clock.main's RTC, if (and only if) it was
// registered and currently holds a plausible time -- EspClock::get_unix()
// (drivers/pcf85063) already fails rather than return a bogus reading
// when the chip's oscillator-stop flag is set or the decoded year looks
// wrong, so this never needs to re-check that itself; it just must not
// set the clock from a failed Result.
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
    if (auto set = wisp::port::esp::set_wall_clock(unix_seconds.value()); !set) {
        WLOG_W(kTag, "clock.main: system clock not set (%s)", set.error().message.c_str());
        return;
    }
    WLOG_I(kTag, "system clock set from clock.main RTC (unix=%lld)",
           static_cast<long long>(unix_seconds.value()));
}

class WaveshareEsp32S3TouchAmoled216Board : public wisp::app::Board {
public:
    std::string name() const override { return "waveshare_esp32_s3_touch_amoled_2_16"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        // Independent of esp_board_manager entirely -- see
        // init_internal_storage()'s comment on why this must run first.
        init_internal_storage(registry, report);

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
            record_failed(report, "audio_out.main", wisp::hal::DeviceClass::AudioOut, reason);
            record_failed(report, "audio_in.main", wisp::hal::DeviceClass::AudioIn, reason);
            // storage.int was already handled above, independent of bmgr.
            record_failed(report, "storage.sd", wisp::hal::DeviceClass::Storage, reason);
            init_wifi(registry, report);
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
        init_audio(registry, report);
        init_sd_storage(registry, report);

        // Independent of whether any of the above actually succeeded --
        // these pins must not float during light sleep regardless of
        // which devices came up (see hold_outputs_during_light_sleep()'s
        // comment).
        hold_outputs_during_light_sleep();

        init_wifi(registry, report);
    }
};

}  // namespace

namespace wisp::board {

std::unique_ptr<wisp::app::Board> create_board() {
    return std::make_unique<WaveshareEsp32S3TouchAmoled216Board>();
}

}  // namespace wisp::board
