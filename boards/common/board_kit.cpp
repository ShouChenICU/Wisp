#include "board_kit.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "dev_audio_codec.h"
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "driver/gpio.h"
#include "esp_board_manager.h"
#include "esp_board_manager_err.h"
#include "esp_board_periph.h"
#include "held_pins.hpp"
#include "periph_gpio.h"
#include "sdkconfig.h"
#include "wisp/core/log.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/port/esp/button_input.hpp"
#include "wisp/port/esp/esp_audio.hpp"
#include "wisp/port/esp/esp_button.hpp"
#include "wisp/port/esp/esp_storage.hpp"
#include "wisp/port/esp/esp_touch.hpp"
#include "wisp/port/esp/esp_wifi.hpp"
#include "wisp/port/esp/wall_clock.hpp"

// bmgr compiles a device type only for a board that declares one.
#ifdef CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT
#include "dev_fs_fat.h"
#endif

namespace wisp::board_kit {

namespace {

constexpr const char* kTag = "board";

using port::esp::EspDisplayConfig;

// bmgr's static config table for `name`, or null (with `err` set).
template <class Config>
const Config* device_config(const char* name, esp_err_t& err) {
    void* config = nullptr;
    err = esp_board_manager_get_device_config(name, &config);
    if (err != ESP_OK || config == nullptr) {
        return nullptr;
    }
    return static_cast<const Config*>(config);
}

// The panel IO's SPI part, or null for a panel on any other bus.
const dev_display_lcd_spi_sub_config_t* spi_part(const dev_display_lcd_config_t* cfg) {
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_SPI_SUPPORT
    if (cfg->sub_type != nullptr && std::strcmp(cfg->sub_type, "spi") == 0) {
        return &cfg->sub_cfg.spi;
    }
#else
    (void)cfg;
#endif
    return nullptr;
}

void init_internal_storage(Kit& kit, const BoardSpec& spec) {
    kit.add(spec.internal_storage, port::esp::EspInternalStorage::create(
                                       spec.internal_storage.id, spec.internal_storage.role));
}

void init_display(Kit& kit, const DisplaySpec& spec) {
    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(spec.bmgr, &handle);
    if (err != ESP_OK || handle == nullptr) {
        kit.failed(spec.slot, std::string("bmgr ") + spec.bmgr + " handle: " + err_name(err));
        return;
    }
    const auto* cfg = device_config<dev_display_lcd_config_t>(spec.bmgr, err);
    if (cfg == nullptr) {
        kit.failed(spec.slot, std::string("bmgr ") + spec.bmgr + " config: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_display_lcd_handles_t*>(handle);
    const auto* spi = spi_part(cfg);

    EspDisplayConfig dc;
    dc.id = spec.slot.id;
    dc.role = spec.slot.role;
    dc.model = spec.model;
    dc.width = cfg->lcd_width;
    dc.height = cfg->lcd_height;
    dc.shape = spec.shape;
    dc.corner_radius = spec.corner_radius;
    dc.panel = spec.panel;
    dc.area_align = spec.area_align;
    // QSPI panels frame raw register writes as (0x02 << 24) | (reg << 8)
    // (EspDisplayConfig::qspi_cmd_framing); the YAML's
    // io_spi_config.flags.quad_mode says which.
    dc.qspi_cmd_framing = spi != nullptr && spi->io_spi_config.flags.quad_mode;
    dc.rgb565_be = cfg->frame_format == DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_BE;
    dc.panel_sleep = spec.sleep;
    dc.sleep_out_settle_ms = spec.sleep_out_settle_ms;
    kit.add(spec.slot,
            port::esp::EspDisplay::create(handles->panel_handle, handles->io_handle, dc));
}

void init_touch(Kit& kit, const TouchSpec& spec) {
    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(spec.bmgr, &handle);
    if (err != ESP_OK || handle == nullptr) {
        kit.failed(spec.slot, std::string("bmgr ") + spec.bmgr + " handle: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_lcd_touch_handles_t*>(handle);
    if (handles->touch_handle == nullptr) {
        kit.failed(spec.slot, std::string("bmgr ") + spec.bmgr + ": null touch handle");
        return;
    }

    // Coordinate range: the touch driver's own x_max/y_max (the driver
    // applies the YAML's swap_xy/mirror flags). Without bmgr's config the
    // range stays 0x0, which EspTouch treats as "unknown, don't clamp".
    port::esp::EspTouchConfig tc;
    tc.id = spec.slot.id;
    tc.role = spec.slot.role;
    tc.model = spec.model;
    if (const auto* cfg = device_config<dev_lcd_touch_config_t>(spec.bmgr, err)) {
        tc.width = cfg->touch_config.x_max;
        tc.height = cfg->touch_config.y_max;
    } else {
        WLOG_W(kTag, "%s: no bmgr %s config (%s); coordinates won't be clamped", spec.slot.id,
               spec.bmgr, err_name(err).c_str());
    }
    kit.add_device(spec.slot, std::make_shared<port::esp::EspTouch>(handles->touch_handle, tc));
}

// Sets the system clock from the main RTC, if one registered and it holds
// a plausible time. The clock's get_unix() already fails rather than
// return a bogus reading (oscillator stopped, implausible year), so this
// only must not set the clock from a failed Result.
void sync_system_clock_from_rtc(core::DeviceRegistry& registry) {
    auto clock = registry.by_role<hal::Clock>(hal::role::kMain);
    if (!clock) {
        return;
    }
    const char* id = clock->info().id.c_str();
    auto unix_seconds = clock->get_unix();
    if (!unix_seconds) {
        WLOG_W(kTag, "%s: RTC time unavailable/invalid, system clock left unset (%s)", id,
               unix_seconds.error().message.c_str());
        return;
    }
    if (auto set = port::esp::set_wall_clock(unix_seconds.value()); !set) {
        WLOG_W(kTag, "%s: system clock not set (%s)", id, set.error().message.c_str());
        return;
    }
    WLOG_I(kTag, "system clock set from %s RTC (unix=%lld)", id,
           static_cast<long long>(unix_seconds.value()));
}

std::shared_ptr<hal::Device> gpio_button(const ButtonSpec& spec, int pin) {
    return std::make_shared<port::esp::EspGpioButton>(static_cast<gpio_num_t>(pin), spec.active_low,
                                                      spec.slot.id, spec.model, spec.slot.role,
                                                      spec.mode);
}

// bmgr configured the pin (input and pull, from the YAML) during
// esp_board_manager_init(), which initialises every declared peripheral
// and only logs one that fails: the peripheral has a handle exactly when
// its gpio_config() succeeded.
void init_button(Kit& kit, const ButtonSpec& spec) {
    int pin = -1;
    void* config = nullptr;
    esp_err_t err = esp_board_manager_get_periph_config(spec.bmgr_gpio, &config);
    if (err == ESP_OK && config != nullptr) {
        pin = static_cast<const periph_gpio_config_t*>(config)->gpio_num;
        void* handle = nullptr;
        err = esp_board_manager_get_periph_handle(spec.bmgr_gpio, &handle);
    } else if (err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    const bool configured = err == ESP_OK;

    if (spec.fallback == nullptr) {
        if (!configured) {
            kit.failed(spec.slot, std::string("bmgr ") + spec.bmgr_gpio + ": " + err_name(err));
            return;
        }
        kit.add_device(spec.slot, gpio_button(spec, pin));
        return;
    }
    const bool edge_ready = configured && port::esp::EspGpioButton::prepare_edge_input().is_ok();
    if (port::esp::power_button_use_gpio(true, configured, edge_ready)) {
        kit.add_device(spec.slot, gpio_button(spec, pin));
        return;
    }
    kit.add(spec.slot, spec.fallback(kit, spec));
}

// Wraps bmgr's codec handle for `bmgr` plus the I2S channel its config
// names (the device's set_power() turns that channel off in standby, see
// wisp/port/esp/esp_audio.hpp) with `make`.
template <class Device, class Config>
void init_audio_device(Kit& kit, const Slot& slot, const char* bmgr, Config device_config_in,
                       Result<std::shared_ptr<Device>> (*make)(esp_codec_dev_handle_t,
                                                               i2s_chan_handle_t, Config)) {
    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(bmgr, &handle);
    if (err != ESP_OK || handle == nullptr) {
        kit.failed(slot, std::string("bmgr ") + bmgr + " handle: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_audio_codec_handles_t*>(handle);
    if (handles->codec_dev == nullptr) {
        kit.failed(slot, std::string("bmgr ") + bmgr + ": null codec_dev handle");
        return;
    }
    const auto* cfg = device_config<dev_audio_codec_config_t>(bmgr, err);
    if (cfg == nullptr) {
        kit.failed(slot, std::string("bmgr ") + bmgr + " config: " + err_name(err));
        return;
    }
    const char* i2s_name = cfg->i2s_cfg.name;
    void* i2s_channel = nullptr;
    err = i2s_name != nullptr ? esp_board_manager_get_periph_handle(i2s_name, &i2s_channel)
                              : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK || i2s_channel == nullptr) {
        kit.failed(slot, std::string("bmgr ") + bmgr + " i2s: " + err_name(err));
        return;
    }
    kit.add(slot, make(handles->codec_dev, static_cast<i2s_chan_handle_t>(i2s_channel),
                       std::move(device_config_in)));
}

void init_audio_out(Kit& kit, const AudioOutSpec& spec) {
    init_audio_device(kit, spec.slot, spec.bmgr,
                      port::esp::EspAudioOutConfig{
                          .id = spec.slot.id, .role = spec.slot.role, .model = spec.model},
                      &port::esp::EspAudioOut::create);
}

void init_audio_in(Kit& kit, const AudioInSpec& spec) {
    init_audio_device(kit, spec.slot, spec.bmgr,
                      port::esp::EspAudioInConfig{.id = spec.slot.id,
                                                  .role = spec.slot.role,
                                                  .model = spec.model,
                                                  .channels = std::vector<port::esp::AudioChannel>(
                                                      spec.channels.begin(), spec.channels.end())},
                      &port::esp::EspAudioIn::create);
}

// storage.sd registers even without a card at boot (Missing, "no card"),
// so the app sees a card inserted later (EspSdStorage owns the hot-plug
// worker). bmgr already attempted the mount in esp_board_manager_init().
void init_sd(Kit& kit, const SdSpec& spec) {
#ifndef CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT
    kit.failed(spec.slot, std::string("bmgr ") + spec.bmgr + ": no fs_fat device in this build");
#else
    esp_err_t err = ESP_OK;
    const auto* cfg = device_config<dev_fs_fat_config_t>(spec.bmgr, err);
    if (cfg != nullptr && cfg->mount_point == nullptr) {
        cfg = nullptr;
        err = ESP_ERR_INVALID_ARG;
    }
    if (cfg == nullptr) {
        kit.failed(spec.slot, std::string("bmgr ") + spec.bmgr + " config: " + err_name(err));
        return;
    }
    void* handle = nullptr;
    err = esp_board_manager_get_device_handle(spec.bmgr, &handle);
    const bool mounted = err == ESP_OK && handle != nullptr;
    sdmmc_card_t* card = mounted ? static_cast<dev_fs_fat_handle_t*>(handle)->card : nullptr;
    auto sd = std::make_shared<port::esp::EspSdStorage>(
        port::esp::EspSdStorageConfig{.id = spec.slot.id,
                                      .role = spec.slot.role,
                                      .mount_point = cfg->mount_point,
                                      .bmgr_device = spec.bmgr,
                                      .model = spec.model},
        mounted, card);
    if (kit.add_device(spec.slot, std::move(sd)) && !mounted) {
        kit.missing(spec.slot, "no card");
    }
#endif
}

// The outputs that must keep driving their pin through every automatic
// light sleep. firmware/sdkconfig.defaults' CONFIG_FREERTOS_USE_TICKLESS_
// IDLE=y pulls in CONFIG_PM_SLP_DISABLE_GPIO on the S3, which isolates
// every GPIO (direction, level and pull all disabled: floating) for the
// duration of each light sleep, so an output that must keep its level
// opts out with gpio_sleep_sel_dis():
//   - the panel's RST: with PanelSleep::DcsSleep a reset pulse while
//     asleep would cost the full re-init that option exists to avoid;
//   - the touch controller's RST: the same risk;
//   - the speaker PA enable: floating risks an audible pop or leakage
//     through the amplifier;
//   - the panel's CS: the panel keeps its state through standby, and a CS
//     glitching low while SCLK floats could latch noise as command bits.
// The pins come from bmgr's static config tables, so the list does not
// depend on which devices came up. Order: display reset, touch reset,
// PA, display CS. See docs/hardware/<board>.md "Standby and power saving".
void hold_outputs(const BoardSpec& spec) {
    HeldPins held;
    esp_err_t err = ESP_OK;
    const dev_display_lcd_spi_sub_config_t* spi = nullptr;
    if (spec.display) {
        if (const auto* cfg = device_config<dev_display_lcd_config_t>(spec.display->bmgr, err)) {
            spi = spi_part(cfg);
        }
    }
    if (spi != nullptr) {
        held.add(spi->panel_config.reset_gpio_num);
    }
    if (spec.touch) {
        if (const auto* cfg = device_config<dev_lcd_touch_config_t>(spec.touch->bmgr, err)) {
            held.add(cfg->touch_config.rst_gpio_num);
        }
    }
    if (spec.audio_out) {
        if (const auto* cfg = device_config<dev_audio_codec_config_t>(spec.audio_out->bmgr, err)) {
            held.add(cfg->pa_peripheral.port);
        }
    }
    if (spi != nullptr) {
        held.add(spi->io_spi_config.cs_gpio_num);
    }

    char line[8 * kMaxHeldPins + 1] = {};
    size_t used = 0;
    for (size_t i = 0; i < held.count; ++i) {
        const int pin = held.pins[i];
        err = gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
        if (err != ESP_OK) {
            WLOG_W(kTag, "gpio_sleep_sel_dis(%d) failed: %s", pin, err_name(err).c_str());
            continue;
        }
        int n = std::snprintf(line + used, sizeof(line) - used, " GPIO%d", pin);
        if (n > 0) {
            used += static_cast<size_t>(n) < sizeof(line) - used ? static_cast<size_t>(n)
                                                                 : sizeof(line) - used - 1;
        }
    }
    WLOG_I(kTag, "held through light sleep:%s", used > 0 ? line : " none");
}

void init_wifi(Kit& kit, const BoardSpec& spec) {
    port::esp::EspWifiConfig config;
    config.id = spec.wifi.id;
    config.role = spec.wifi.role;
    kit.add(spec.wifi, port::esp::EspWifi::create(std::move(config)));
}

class KitBoard : public app::Board {
public:
    explicit KitBoard(const BoardSpec& spec) : spec_(spec) {}
    std::string name() const override { return spec_.name; }
    void init(core::DeviceRegistry& registry, core::BootReport& report) override {
        board_kit::init(spec_, registry, report);
    }

private:
    const BoardSpec& spec_;
};

}  // namespace

std::string err_name(esp_err_t err) {
    if (err >= ESP_BOARD_ERR_BASE) {
        return esp_board_manager_err_to_name(err);
    }
    return esp_err_to_name(err);
}

i2c_master_bus_handle_t Kit::i2c(const char* bmgr_periph) {
    void* handle = nullptr;
    esp_err_t err = esp_board_periph_get_handle(bmgr_periph, &handle);
    if (err != ESP_OK || handle == nullptr) {
        WLOG_W(kTag, "%s bus handle unavailable: %s", bmgr_periph, err_name(err).c_str());
        return nullptr;
    }
    return static_cast<i2c_master_bus_handle_t>(handle);
}

bool Kit::add_device(const Slot& slot, std::shared_ptr<hal::Device> device) {
    auto added = registry_.add(std::move(device));
    if (!added) {
        failed(slot, "registry.add: " + added.error().message);
        return false;
    }
    return true;
}

void Kit::missing(const Slot& slot, std::string reason) {
    report_.record({slot.id, slot.cls, core::DeviceState::Missing, std::move(reason)});
}

void Kit::failed(const Slot& slot, std::string reason) {
    WLOG_E(kTag, "%s failed: %s", slot.id, reason.c_str());
    report_.record({slot.id, slot.cls, core::DeviceState::Failed, std::move(reason)});
}

void init(const BoardSpec& spec, core::DeviceRegistry& registry, core::BootReport& report) {
    Kit kit(registry, report);
    init_internal_storage(kit, spec);

    // esp_board_manager_init() fails only when a peripheral (bus) or its
    // own setup fails; a device that does not come up is logged by bmgr
    // and simply has no handle, which the steps below report per device.
    esp_err_t err = esp_board_manager_init();
    if (err != ESP_OK) {
        const std::string reason = "esp_board_manager_init: " + err_name(err);
        if (spec.display) kit.failed(spec.display->slot, reason);
        if (spec.touch) kit.failed(spec.touch->slot, reason);
        for (const Slot& chip : spec.chips) kit.failed(chip, reason);
        for (const ButtonSpec& button : spec.buttons) kit.failed(button.slot, reason);
        if (spec.audio_out) kit.failed(spec.audio_out->slot, reason);
        if (spec.audio_in) kit.failed(spec.audio_in->slot, reason);
        if (spec.sd) kit.failed(spec.sd->slot, reason);
        init_wifi(kit, spec);
        return;
    }

    if (spec.display) init_display(kit, *spec.display);
    if (spec.touch) init_touch(kit, *spec.touch);
    if (spec.bring_up_chips != nullptr) spec.bring_up_chips(kit);
    // Only after the RTC has had its chance to register.
    sync_system_clock_from_rtc(registry);
    for (const ButtonSpec& button : spec.buttons) init_button(kit, button);
    if (spec.audio_out) init_audio_out(kit, *spec.audio_out);
    if (spec.audio_in) init_audio_in(kit, *spec.audio_in);
    if (spec.sd) init_sd(kit, *spec.sd);
    hold_outputs(spec);
    init_wifi(kit, spec);
}

std::unique_ptr<app::Board> make_board(const BoardSpec& spec) {
    return std::make_unique<KitBoard>(spec);
}

}  // namespace wisp::board_kit
