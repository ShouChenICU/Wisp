#pragma once

// ESP-IDF wisp::hal::Display backed by an esp_lcd panel + panel IO pair
// (e.g. the handles esp_board_manager's display_lcd device hands out --
// see boards/waveshare_amoled_216/board.cpp). Chip/board-specific facts
// (model, size, screen shape, flush alignment, command framing, pixel
// byte order) are not hard-coded here -- AGENTS.md's "板子无关" rule --
// but passed in via EspDisplayConfig by the board.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
    // issues itself (set_brightness(), and -- with panel_sleep ==
    // DcsSleep below -- the sleep/wake DCS commands): when true, a DCS
    // register `reg` is sent as the QSPI "write command" frame
    // (0x02 << 24) | (reg << 8) (the framing esp_lcd's QSPI panel
    // drivers use for tx_param); when false it is sent as the bare
    // register number.
    bool qspi_cmd_framing = false;
    // True if the panel expects RGB565 pixels most-significant byte first
    // (big-endian on the wire) -- flush() then byte-swaps the native
    // little-endian pixels around the transfer.
    bool swap_rgb565_bytes = false;

    // How set_power(false)/set_power(true) put the panel to/from sleep
    // -- see docs/hardware/waveshare_amoled_216.md for this panel's own
    // sleep behavior. Chosen per board, not per
    // chip model: both options are implemented entirely in terms of the
    // standard esp_lcd_panel_* API and standard MIPI DCS commands, never
    // anything chip-specific -- that stays board-agnostic regardless of
    // which one a given board picks (AGENTS.md's "板子无关" rule).
    enum class PanelSleep {
        // esp_lcd_panel_disp_on_off(false) + esp_lcd_panel_disp_sleep
        // (true) to sleep, reversed to wake -- the esp_lcd panel
        // driver's own notion of "sleep". On a panel wired with a
        // hardware reset GPIO in its esp_lcd_panel_dev_config_t (e.g.
        // this board's CO5300), some vendor drivers escalate this to a
        // *deep* standby that clears GRAM and forces a full re-init on
        // wake (slow, and visibly flashes uninitialized memory until
        // init finishes) -- this option does not work around that; see
        // DcsSleep below.
        VendorDispSleep,
        // Sends the standard MIPI DCS commands directly via
        // esp_lcd_panel_io_tx_param() -- the same path set_brightness()
        // already uses -- bypassing esp_lcd_panel_disp_sleep()/
        // disp_on_off() entirely: DISPOFF (0x28) then SLPIN (0x10) to
        // sleep; SLPOUT (0x11), a sleep_out_settle_ms wait, then DISPON
        // (0x29) to wake. This is ordinary MIPI DCS sleep, which
        // preserves GRAM (the panel shows its last frame immediately on
        // wake, rather than uninitialized memory) and never touches the
        // panel's hardware reset line -- so it never re-triggers
        // whatever deep-standby/full-reinit behavior VendorDispSleep
        // can trigger on some panels.
        //
        // Once this is selected, esp_lcd_panel_disp_sleep()/
        // disp_on_off() must never be called on this same panel handle
        // by anyone else either -- their internal sleep_mode bookkeeping
        // would get out of sync with the panel's real state (which this
        // class is now managing directly via raw DCS commands instead).
        DcsSleep,
    };
    PanelSleep panel_sleep = PanelSleep::VendorDispSleep;

    // DcsSleep only: how long to wait, after sending SLPOUT, before
    // sending DISPON on wake (the panel's own settle time; the MIPI DCS
    // spec's own floor for this is 120ms). This class also enforces it
    // as the minimum spacing between a SLPIN and the *next* SLPOUT (most
    // panels are out of spec if woken sooner than that after being put
    // to sleep) -- see set_power_dcs_locked()'s
    // comment. Board-configured, since it is a panel-specific timing
    // constant, not a chip-family one.
    uint32_t sleep_out_settle_ms = 120;
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
    // Writes the DCS "write display brightness" register (0x51), and
    // remembers `level` (see set_power()'s I3 note below).
    Result<void> set_brightness(uint8_t level) override;
    // Panel sleep (spec 6.7), not just display-off. Dispatches to one of
    // two implementations per config_.panel_sleep (see
    // EspDisplayConfig::PanelSleep's doc comment):
    //   - VendorDispSleep: set_power(false) calls
    //     esp_lcd_panel_disp_on_off(false) then
    //     esp_lcd_panel_disp_sleep(true); set_power(true) reverses the
    //     order (disp_sleep(false) then disp_on_off(true)).
    //   - DcsSleep: set_power(false) sends DISPOFF then SLPIN;
    //     set_power(true) sends SLPOUT, waits
    //     config_.sleep_out_settle_ms, then sends DISPON.
    // Either way, a successful set_power(true) re-sends the last
    // brightness set_brightness() was called with, if any: some panels
    // reset their brightness register on wake regardless of which sleep
    // mode put them there (e.g. this
    // board's CO5300 board-level init sequence, which VendorDispSleep's
    // deep-standby wake re-runs in full, ends with brightness = 0xFF)
    // -- without this, every wake would silently override whatever
    // brightness the user had configured.
    Result<void> set_power(bool on) override;
    bool powered() const override;

private:
    EspDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, EspDisplayConfig config,
               SemaphoreHandle_t done);

    static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t* edata, void* user_ctx);

    // Sends one DCS command (optionally with parameter bytes) via
    // esp_lcd_panel_io_tx_param(), applying config_.qspi_cmd_framing the
    // same way set_brightness() always has. Assumes mutex_ is already
    // held by the caller.
    Result<void> tx_dcs_locked(int reg, const uint8_t* param = nullptr, size_t param_len = 0);
    // The two set_power() implementations (see EspDisplayConfig::
    // PanelSleep). Assume mutex_ is already held; neither touches
    // powered_ or re-applies brightness -- set_power() itself does both,
    // once, after whichever of these succeeds.
    Result<void> set_power_vendor_locked(bool on);
    Result<void> set_power_dcs_locked(bool on);

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

    // The last level set_brightness() was called with, re-applied by a
    // successful set_power(true) (see set_power()'s doc comment). Unset
    // until the first set_brightness() call (board init sequences
    // set an initial brightness themselves, outside this class).
    std::optional<uint8_t> last_brightness_;
    // DcsSleep only: when the last SLPIN was sent (os::now_ms()), used to
    // enforce sleep_out_settle_ms as the minimum spacing before the next
    // SLPOUT (see EspDisplayConfig::PanelSleep's doc
    // comment). Unset once that minimum has clearly already elapsed (so
    // repeated set_power(true) calls long after sleeping don't all pay a
    // sleep_out_settle_ms-sized check for nothing).
    //
    // N6: this used to instead record the last SLPOUT time and check it
    // on the *next* SLPIN -- but set_power_dcs_locked(true) itself
    // always already waits out sleep_out_settle_ms (between SLPOUT and
    // DISPON) before returning, so by the time another SLPIN could ever
    // run, that minimum had necessarily already elapsed: the check could
    // never actually trip. The real MIPI DCS requirement this needs to
    // enforce is the other direction -- at least sleep_out_settle_ms
    // between a SLPIN and the *next* SLPOUT -- which nothing previously
    // guarded at all (a user pressing the power key to wake the panel
    // right after pressing it to sleep -- e.g. two presses within the
    // ~50ms standby button-poll interval -- could send SLPOUT less than
    // sleep_out_settle_ms after SLPIN).
    std::optional<uint64_t> last_sleep_in_ms_;
};

}  // namespace wisp::port::esp
