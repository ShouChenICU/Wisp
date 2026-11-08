// Board definition for the Waveshare ESP32-S3-Touch-AMOLED-1.75C (see
// docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md).
//
// Same structure as boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp,
// whose comments explain the shared choices (bring-up order, DCS sleep,
// GPIOs held through light sleep, storage.int before bmgr, Wi-Fi last).
// Buses, display, touch and the audio codecs come from esp_board_manager
// (bmgr, this directory's board_*.yaml + setup_device.c); this file wraps
// them and brings up the rest:
//   - display_lcd -> display.main (EspDisplay, CO5300, 466x466 circle)
//   - lcd_touch   -> touch.main   (EspTouch, CST9217)
//   - i2c_master + drivers/axp2101 -> power.main (EspPower, AXP2101)
//   - i2c_master + drivers/pcf85063 -> clock.main (EspClock), only if a
//     PCF85063 answers at 0x51: the vendor's BSP, schematic and examples
//     show no RTC on this board, though its product page lists one. Absent,
//     clock.main is recorded as missing and the time comes from SNTP alone.
//   - i2c_master + drivers/qmi8658 at 0x6B or 0x6A -> imu.main (EspImu)
//   - the AXP2101's PWRON key -> button.power (EspPmuPowerKey: no GPIO
//     follows the key on this board, so its IRQ bits are polled over I2C)
//   - GPIO0 (BOOT) -> button.boot (EspGpioButton), the talk key
//   - bmgr's audio_dac/audio_adc -> audio_out.main/audio_in.main
//     (EspAudioOut/EspAudioIn, ES8311/ES7210)
//   - esp_vfs_littlefs_register() -> storage.int (EspInternalStorage)
//   - the chip's radio -> net.wifi (EspWifi)
// There is no SD card slot, so no storage.sd.
//
// Every failure is recorded in the boot report and bring-up continues
// (AGENTS.md's device model rule).

#include "wisp/app/board.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "dev_audio_codec.h"
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_board_manager_err.h"
#include "esp_board_periph.h"
#include "esp_err.h"
#include "qmi8658_conv.h"
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

// Shared I2C bus peripheral name as declared in board_peripherals.yaml.
constexpr const char* kBmgrI2cMaster = "i2c_master";

// CO5300 partial updates must start/end on even coordinates (the BSP's
// LVGL rounder does the same).
constexpr int kAreaAlign = 2;
// DCS sleep instead of esp_lcd_panel_disp_sleep(), for the same reason as
// on the 2.16: the panel has a hardware reset GPIO, which turns the
// driver's sleep into a GRAM-clearing deep standby.
constexpr uint32_t kDisplaySleepOutSettleMs = 120;

// The IMU's SA0 strap: Waveshare's examples pass SensorLib's
// QMI8658_L_SLAVE_ADDRESS, which SensorLib defines as 0x6B (its "L"/"H"
// names are the reverse of the datasheet's), so 0x6B is tried first and
// 0x6A second; init_imu() uses whichever answers.
constexpr uint8_t kImuAddrs[] = {QMI8658_I2C_ADDR_HIGH, QMI8658_I2C_ADDR_LOW};
constexpr int kImuProbeTimeoutMs = 50;
// The QMI8658 sits a quarter turn about z from the screen: measured on the
// device, held upright the chip's +x pointed up and tilted with the right
// edge raised its -y pointed up. So screen x = -chip y, screen y = chip x.
constexpr wisp::hal::ImuAxisMap kImuMount{.from = {1, 0, 2}, .negate = {true, false, false}};

// PCF85063's fixed 7-bit address; probed before EspClock::create().
constexpr uint16_t kRtcAddr = 0x51;
constexpr int kRtcProbeTimeoutMs = 20;

// BOOT key: active low, no external pull-up.
constexpr gpio_num_t kPinBtnBoot = GPIO_NUM_0;

// Outputs held through automatic light sleep (see the 2.16's
// hold_outputs_during_light_sleep()). They duplicate board_devices.yaml's
// lcd_panel_config.reset_gpio_num and io_spi_config.cs_gpio_num,
// setup_device.c's TOUCH_RST_GPIO and board_peripherals.yaml's
// gpio_pa_control pin; keep them in sync.
constexpr gpio_num_t kPinDisplayReset = GPIO_NUM_1;
constexpr gpio_num_t kPinTouchReset = GPIO_NUM_2;
constexpr gpio_num_t kPinAudioPaEnable = GPIO_NUM_46;
constexpr gpio_num_t kPinDisplayCs = GPIO_NUM_12;

std::string err_name(esp_err_t err) {
    if (err >= ESP_BOARD_ERR_BASE) {
        return esp_board_manager_err_to_name(err);
    }
    return esp_err_to_name(err);
}

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
    // Round panel: safe_rect() works from the inscribed circle, so no
    // corner_radius. The exact visible boundary is still to be checked on
    // the device (hardware doc's acceptance checklist).
    dc.shape = "circle";
    dc.panel = "amoled";
    dc.area_align = kAreaAlign;
    dc.qspi_cmd_framing = uses_qspi(cfg);
    dc.rgb565_be = cfg->frame_format == DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_BE;
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

i2c_master_bus_handle_t get_i2c_bus() {
    void* handle = nullptr;
    esp_err_t err = esp_board_periph_get_handle(kBmgrI2cMaster, &handle);
    if (err != ESP_OK || handle == nullptr) {
        WLOG_W(kTag, "i2c_master bus handle unavailable: %s", err_name(err).c_str());
        return nullptr;
    }
    return static_cast<i2c_master_bus_handle_t>(handle);
}

// Returns the PMU for button.power, or null when it did not come up.
std::shared_ptr<wisp::port::esp::EspPower> init_power(wisp::core::DeviceRegistry& registry,
                                                      wisp::core::BootReport& report,
                                                      i2c_master_bus_handle_t i2c_bus) {
    constexpr const char* id = "power.main";
    constexpr auto cls = wisp::hal::DeviceClass::Power;
    if (i2c_bus == nullptr) {
        record_failed(report, id, cls, "no i2c_master bus handle");
        return nullptr;
    }
    auto power = wisp::port::esp::EspPower::create(i2c_bus, "AXP2101");
    if (!power) {
        record_failed(report, id, cls, power.error().message);
        return nullptr;
    }
    esp_err_t chg_err = power.value()->charging_config_status();
    if (chg_err != ESP_OK) {
        WLOG_W(kTag, "power.main: charging configuration failed: %s", err_name(chg_err).c_str());
    }
    add_device(registry, report, power.value());
    return power.value();
}

void init_clock(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                i2c_master_bus_handle_t i2c_bus) {
    constexpr const char* id = "clock.main";
    constexpr auto cls = wisp::hal::DeviceClass::Clock;
    if (i2c_bus == nullptr) {
        record_failed(report, id, cls, "no i2c_master bus handle");
        return;
    }
    // A board without the chip is not a failure: nothing answers at 0x51.
    esp_err_t probe = i2c_master_probe(i2c_bus, kRtcAddr, kRtcProbeTimeoutMs);
    if (probe == ESP_ERR_NOT_FOUND) {
        WLOG_I(kTag, "clock.main: no RTC at 0x%02X; time comes from SNTP",
               static_cast<unsigned>(kRtcAddr));
        report.record({id, cls, wisp::core::DeviceState::Missing, "no RTC"});
        return;
    }
    if (probe != ESP_OK) {
        record_failed(report, id, cls, "i2c probe: " + err_name(probe));
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
    uint8_t addr = 0;
    for (uint8_t a : kImuAddrs) {
        if (i2c_master_probe(i2c_bus, a, kImuProbeTimeoutMs) == ESP_OK) {
            addr = a;
            break;
        }
    }
    if (addr == 0) {
        record_failed(report, id, cls, "no answer at 0x6B or 0x6A");
        return;
    }
    auto imu = wisp::port::esp::EspImu::create(i2c_bus, "QMI8658", addr, kImuMount);
    if (!imu) {
        record_failed(report, id, cls, imu.error().message);
        return;
    }
    add_device(registry, report, imu.value());
}

void init_buttons(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                  std::shared_ptr<wisp::port::esp::EspPower> power) {
    constexpr auto cls = wisp::hal::DeviceClass::Button;

    if (power) {
        add_device(registry, report,
                   std::make_shared<wisp::port::esp::EspPmuPowerKey>(
                       std::move(power), "button.power", "axp2101-pwrkey"));
    } else {
        record_failed(report, "button.power", cls, "power.main unavailable");
    }

    // The BOOT key is this board's only GPIO button and has no volume
    // label, so it gets its own id rather than the 2.16's button.vol_down;
    // the shell finds the talk key by its role, not its id.
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << kPinBtnBoot;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        record_failed(report, "button.boot", cls, "gpio_config: " + err_name(err));
        return;
    }
    add_device(registry, report,
               std::make_shared<wisp::port::esp::EspGpioButton>(kPinBtnBoot, true, "button.boot",
                                                                "gpio-button", "talk"));
}

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

void init_internal_storage(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    auto internal = wisp::port::esp::EspInternalStorage::create();
    if (!internal) {
        record_failed(report, "storage.int", wisp::hal::DeviceClass::Storage,
                      internal.error().message);
        return;
    }
    add_device(registry, report, internal.value());
}

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

// Sets the system clock from clock.main when that registered and holds a
// plausible time; see the 2.16's function of the same name.
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

class WaveshareEsp32S3TouchAmoled175cBoard : public wisp::app::Board {
public:
    std::string name() const override { return "waveshare_esp32_s3_touch_amoled_1_75c"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        init_internal_storage(registry, report);

        esp_err_t err = esp_board_manager_init();
        if (err != ESP_OK) {
            std::string reason = "esp_board_manager_init: " + err_name(err);
            record_failed(report, "display.main", wisp::hal::DeviceClass::Display, reason);
            record_failed(report, "touch.main", wisp::hal::DeviceClass::Touch, reason);
            record_failed(report, "power.main", wisp::hal::DeviceClass::Power, reason);
            record_failed(report, "clock.main", wisp::hal::DeviceClass::Clock, reason);
            record_failed(report, "imu.main", wisp::hal::DeviceClass::Imu, reason);
            record_failed(report, "button.power", wisp::hal::DeviceClass::Button, reason);
            record_failed(report, "button.boot", wisp::hal::DeviceClass::Button, reason);
            record_failed(report, "audio_out.main", wisp::hal::DeviceClass::AudioOut, reason);
            record_failed(report, "audio_in.main", wisp::hal::DeviceClass::AudioIn, reason);
            init_wifi(registry, report);
            return;
        }

        init_display(registry, report);
        init_touch(registry, report);

        i2c_master_bus_handle_t i2c_bus = get_i2c_bus();
        auto power = init_power(registry, report, i2c_bus);
        init_clock(registry, report, i2c_bus);
        init_imu(registry, report, i2c_bus);
        sync_system_clock_from_rtc(registry);
        init_buttons(registry, report, std::move(power));
        init_audio(registry, report);

        hold_outputs_during_light_sleep();

        init_wifi(registry, report);
    }
};

}  // namespace

namespace wisp::board {

std::unique_ptr<wisp::app::Board> create_board() {
    return std::make_unique<WaveshareEsp32S3TouchAmoled175cBoard>();
}

}  // namespace wisp::board
