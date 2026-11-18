#pragma once

// The board kit: the bring-up every bmgr-based ESP board shares (device
// model spec 7). A board describes itself with a BoardSpec constant and
// brings up only its own chips in bring_up_chips(); init() runs the
// shared sequence:
//
//   internal storage (before bmgr, which it does not need: a bmgr failure
//   must not take storage.int down with it); esp_board_manager_init();
//   display; touch; bring_up_chips(); system clock from the RTC; buttons;
//   audio out, audio in; SD card; outputs held through light sleep; Wi-Fi.
//
// Wi-Fi comes last on every path, even when bmgr fails: nothing on the
// board depends on it, starting it after the display keeps the Wi-Fi
// driver's internal-RAM allocations from crowding out the panel's DMA
// buffers, and its radio being on is what makes Platform::random a true
// RNG (firmware/main/main.cpp).
//
// Every failure is recorded in the boot report and bring-up continues
// with the next device: a missing panel or touch controller degrades the
// device (App::start() falls back to headless without display.main), it
// never blocks boot (AGENTS.md section 3 "Device model").
//
// Pins live only in the bmgr YAML: buttons are bmgr `gpio` peripherals
// (bmgr configures the input and its pull), and the outputs held through
// light sleep are read from bmgr's device configs.
//
// ESP- and bmgr-specific, so neither a portable layer nor part of the
// port (the port must not depend on bmgr device types or chip drivers);
// firmware/main compiles it with the selected board's board.cpp.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "wisp/app/board.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/port/esp/audio_downmix.hpp"
#include "wisp/port/esp/esp_display.hpp"

namespace wisp::board_kit {

// A device's identity in the registry and the boot report.
struct Slot {
    const char* id;
    hal::DeviceClass cls;
    const char* role;  // caps "role"; "" writes none
};

class Kit;

// bmgr carries the wiring and the panel geometry; these are the panel
// facts it does not carry, published as Wisp caps.
struct DisplaySpec {
    Slot slot{"display.main", hal::DeviceClass::Display, hal::role::kMain};
    const char* bmgr = "display_lcd";
    const char* model;
    const char* shape;
    int corner_radius = 0;
    const char* panel;
    int area_align = 1;
    port::esp::EspDisplayConfig::PanelSleep sleep =
        port::esp::EspDisplayConfig::PanelSleep::VendorDispSleep;
    uint32_t sleep_out_settle_ms = 120;
};

struct TouchSpec {
    Slot slot{"touch.main", hal::DeviceClass::Touch, hal::role::kMain};
    const char* bmgr = "lcd_touch";
    const char* model;
};

struct AudioOutSpec {
    Slot slot{"audio_out.main", hal::DeviceClass::AudioOut, hal::role::kMain};
    const char* bmgr = "audio_dac";
    const char* model;
};

struct AudioInSpec {
    Slot slot{"audio_in.main", hal::DeviceClass::AudioIn, hal::role::kMain};
    const char* bmgr = "audio_adc";
    const char* model;
    // One role per physical capture channel, in the codec's order.
    std::span<const port::esp::AudioChannel> channels;
};

struct SdSpec {
    Slot slot{"storage.sd", hal::DeviceClass::Storage, hal::role::kRemovable};
    const char* bmgr = "fs_sdcard";
    const char* model;
};

struct ButtonSpec {
    Slot slot;
    const char* bmgr_gpio;  // a `gpio` peripheral in board_peripherals.yaml
    bool active_low;        // pressed reads 0
    const char* model = "gpio-button";
    hal::ButtonInputMode mode = hal::ButtonInputMode::EdgeEvent;
    // Used when the GPIO path is unavailable; when set, that path also
    // needs the edge service (EspGpioButton::prepare_edge_input()).
    Result<std::shared_ptr<hal::Device>> (*fallback)(Kit&, const ButtonSpec&) = nullptr;
};

struct BoardSpec {
    const char* name;
    std::optional<DisplaySpec> display;
    std::optional<TouchSpec> touch;
    std::optional<AudioOutSpec> audio_out;
    std::optional<AudioInSpec> audio_in;
    std::optional<SdSpec> sd;
    std::span<const ButtonSpec> buttons;
    // The devices bring_up_chips() registers, recorded failed when
    // esp_board_manager_init() fails.
    std::span<const Slot> chips;
    // Called after display and touch, before the RTC sync, the buttons
    // and audio.
    void (*bring_up_chips)(Kit&) = nullptr;
    Slot internal_storage{"storage.int", hal::DeviceClass::Storage, hal::role::kInternal};
    Slot wifi{"net.wifi", hal::DeviceClass::Network, hal::role::kMain};
};

// bmgr's error names extend ESP-IDF's; this names either.
std::string err_name(esp_err_t err);

// What bring_up_chips() and a button fallback use to register devices
// and record failures.
class Kit {
public:
    Kit(core::DeviceRegistry& registry, core::BootReport& report)
        : registry_(registry), report_(report) {}

    // A bmgr I2C master bus, or null (logged) when bmgr has none.
    i2c_master_bus_handle_t i2c(const char* bmgr_periph);

    // Registers `made`. False when it is an error or the registry refuses
    // it; either is recorded failed under `slot`.
    template <class D>
    bool add(const Slot& slot, Result<std::shared_ptr<D>> made) {
        if (!made) {
            failed(slot, made.error().message);
            return false;
        }
        return add_device(slot, std::move(made.value()));
    }
    bool add_device(const Slot& slot, std::shared_ptr<hal::Device> device);

    void missing(const Slot& slot, std::string reason);  // DeviceState::Missing
    void failed(const Slot& slot, std::string reason);   // DeviceState::Failed, logged

    core::DeviceRegistry& registry() { return registry_; }

private:
    core::DeviceRegistry& registry_;
    core::BootReport& report_;
};

void init(const BoardSpec& spec, core::DeviceRegistry& registry, core::BootReport& report);

// An app::Board whose name() is spec.name and whose init() is init(spec).
// `spec` must outlive the board (boards pass a namespace-scope constant).
std::unique_ptr<app::Board> make_board(const BoardSpec& spec);

}  // namespace wisp::board_kit
