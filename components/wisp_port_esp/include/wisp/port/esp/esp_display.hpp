#pragma once

// ESP-IDF wisp::hal::Display backed by an esp_lcd panel + panel IO pair
// (e.g. the handles esp_board_manager's display_lcd device hands out --
// see boards/waveshare_amoled_216/board.cpp). Chip/board-specific facts
// (model, size, screen shape, flush alignment, command framing, pixel
// byte order) are not hard-coded here -- AGENTS.md's "板子无关" rule --
// but passed in via EspDisplayConfig by the board.

#include <cstdint>
#include <memory>
#include <string>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::esp {

struct EspDisplayConfig {
    std::string model;           // DeviceInfo::model, e.g. the panel controller chip
    int width = 0;               // caps "width"
    int height = 0;              // caps "height"
    std::string shape = "rect";  // caps "shape" (see wisp/hal/display_geometry.hpp)
    int corner_radius = 0;       // caps "corner_radius"
    std::string panel = "lcd";   // caps "panel"
    int area_align = 1;          // caps "area_align"
    // Panel IO command framing for the raw register writes this class
    // issues itself (set_brightness()): when true, a DCS register `reg`
    // is sent as the QSPI "write command" frame (0x02 << 24) | (reg << 8)
    // (the framing esp_lcd's QSPI panel drivers use for tx_param); when
    // false it is sent as the bare register number.
    bool qspi_cmd_framing = false;
    // True if the panel expects RGB565 pixels most-significant byte first
    // (big-endian on the wire) -- flush() then byte-swaps the native
    // little-endian pixels around the transfer.
    bool swap_rgb565_bytes = false;
};

class EspDisplay : public wisp::hal::Display {
public:
    // Registers a color-transfer-done callback on `io` (replacing any
    // previously registered one) and creates the semaphore flush() waits
    // on. Fails with DeviceFailure/NoMemory if either step fails.
    static Result<std::shared_ptr<EspDisplay>> create(esp_lcd_panel_handle_t panel,
                                                      esp_lcd_panel_io_handle_t io,
                                                      EspDisplayConfig config);

    ~EspDisplay() override;

    EspDisplay(const EspDisplay&) = delete;
    EspDisplay& operator=(const EspDisplay&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // Synchronous: issues esp_lcd_panel_draw_bitmap(x1, y1, x2 + 1, y2 + 1)
    // and waits (up to 250 ms) for the IO's color-transfer-done callback
    // before returning, so `rgb565` may be reused as soon as this returns
    // ok. On Timeout the transfer may still be in flight: the next call
    // first waits for it again (failing with Timeout if it still hasn't
    // finished) before starting a new transfer. With swap_rgb565_bytes,
    // the buffer is byte-swapped in place for the duration of the
    // transfer and restored before returning (except on Timeout or a
    // draw_bitmap failure, where the DMA may still be reading it and it is
    // left swapped).
    Result<void> flush(const wisp::hal::Area& area, const uint16_t* rgb565) override;
    // Writes the DCS "write display brightness" register (0x51).
    Result<void> set_brightness(uint8_t level) override;
    // Panel sleep (spec 6.7), not just display-off: set_power(false)
    // calls esp_lcd_panel_disp_on_off(false) then
    // esp_lcd_panel_disp_sleep(true); set_power(true) reverses the order
    // (disp_sleep(false) then disp_on_off(true)).
    Result<void> set_power(bool on) override;
    bool powered() const override;

private:
    EspDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, EspDisplayConfig config,
               SemaphoreHandle_t done);

    static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t* edata, void* user_ctx);

    wisp::hal::DeviceInfo info_;
    esp_lcd_panel_handle_t panel_;
    esp_lcd_panel_io_handle_t io_;
    EspDisplayConfig config_;
    SemaphoreHandle_t done_;

    // Serializes flush()/set_brightness()/set_power(): they all drive the
    // same panel IO, which is not safe to use from two tasks at once.
    mutable wisp::os::Mutex mutex_;
    bool pending_ = false;  // a timed-out transfer may still be in flight
    bool powered_ = true;   // esp_board_manager turns the panel on during init
};

}  // namespace wisp::port::esp
