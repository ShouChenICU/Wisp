#include "wisp/port/esp/esp_display.hpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "esp_display";

// Upper bound on one wait for the color-transfer-done callback. A full
// 480x480 RGB565 frame at 40 MHz QSPI takes ~26 ms on the wire, so this
// only trips if the panel IO is genuinely stuck.
constexpr uint32_t kFlushTimeoutMs = 250;

// Standard MIPI DCS commands used by set_brightness() and, with
// EspDisplayConfig::PanelSleep::DcsSleep, set_power() -- see
// EspDisplayConfig::PanelSleep's doc comment. None of these are chip
// -specific (AGENTS.md's "板子无关" rule): every panel this codebase
// targets implements the standard MIPI DCS command set.
constexpr int kDcsWriteBrightness = 0x51;
constexpr int kDcsDispOff = 0x28;
constexpr int kDcsSleepIn = 0x10;
constexpr int kDcsSleepOut = 0x11;
constexpr int kDcsDispOn = 0x29;
// esp_lcd QSPI panel drivers' "write command" opcode (see e.g.
// esp_lcd_co5300's tx_param()).
constexpr uint32_t kQspiOpcodeWriteCmd = 0x02;

TickType_t ms_to_ticks(uint32_t ms) {
    // Round up so a short timeout never becomes a zero-tick (non-blocking)
    // wait.
    return static_cast<TickType_t>((static_cast<uint64_t>(ms) * configTICK_RATE_HZ + 999) / 1000);
}

}  // namespace

Result<std::shared_ptr<EspDisplay>> EspDisplay::create(esp_lcd_panel_handle_t panel,
                                                       esp_lcd_panel_io_handle_t io,
                                                       EspDisplayConfig config) {
    using R = Result<std::shared_ptr<EspDisplay>>;
    if (panel == nullptr || io == nullptr) {
        return R::err(ErrorCode::InvalidArgument, "null panel or panel IO handle");
    }
    if (config.width <= 0 || config.height <= 0) {
        return R::err(ErrorCode::InvalidArgument, "display width/height must be positive");
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        return R::err(ErrorCode::NoMemory, "failed to create flush semaphore");
    }
    // Not make_shared: the constructor is private.
    std::shared_ptr<EspDisplay> display(new EspDisplay(panel, io, std::move(config), done));

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = &EspDisplay::on_color_trans_done;
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(io, &cbs, display.get());
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string("register_event_callbacks: ") + esp_err_to_name(err));
    }
    return R::ok(std::move(display));
}

EspDisplay::EspDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io,
                       EspDisplayConfig config, SemaphoreHandle_t done)
    : panel_(panel), io_(io), config_(std::move(config)), done_(done) {
    info_.id = "display.main";
    info_.cls = kClass;
    info_.model = config_.model;
    info_.caps = wisp::Properties{
        {"width", config_.width},
        {"height", config_.height},
        {"shape", wisp::Value(config_.shape)},
        {"corner_radius", config_.corner_radius},
        {"panel", wisp::Value(config_.panel)},
        {"brightness", true},
        {"area_align", config_.area_align},
        {"rgb565_be", config_.rgb565_be},
        {"standby", wisp::Value("off")},
    };
    if (config_.draw_buf_lines > 0) info_.caps["draw_buf_lines"] = config_.draw_buf_lines;
    if (!config_.draw_buf_mem.empty())
        info_.caps["draw_buf_mem"] = wisp::Value(config_.draw_buf_mem);
    if (!config_.render_mode.empty()) info_.caps["render_mode"] = wisp::Value(config_.render_mode);
}

EspDisplay::~EspDisplay() {
    // Detach the callback before the semaphore (and `this`) go away. Any
    // transfer still in flight would otherwise call back into freed memory.
    esp_lcd_panel_io_callbacks_t cbs = {};
    esp_lcd_panel_io_register_event_callbacks(io_, &cbs, nullptr);
    vSemaphoreDelete(done_);
}

// Runs in ISR context (the SPI master's post-transaction hook), hence
// IRAM_ATTR and the FromISR variant. The return value tells esp_lcd
// whether a higher-priority task was woken (it yields on our behalf).
bool IRAM_ATTR EspDisplay::on_color_trans_done(esp_lcd_panel_io_handle_t /*io*/,
                                               esp_lcd_panel_io_event_data_t* /*edata*/,
                                               void* user_ctx) {
    auto* self = static_cast<EspDisplay*>(user_ctx);
    if (self == nullptr) {
        return false;
    }
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->done_, &woken);
    return woken == pdTRUE;
}

Result<void> EspDisplay::flush(const wisp::hal::Area& area, const uint16_t* rgb565) {
    auto r = flush_async(area, rgb565);
    if (!r) {
        return r;
    }
    return wait_flush();
}

Result<void> EspDisplay::flush_async(const wisp::hal::Area& area, const uint16_t* rgb565) {
    assert_owner_thread();
    if (rgb565 == nullptr || area.x1 > area.x2 || area.y1 > area.y2 || area.x1 < 0 || area.y1 < 0 ||
        area.x2 >= config_.width || area.y2 >= config_.height) {
        return Result<void>::err(ErrorCode::InvalidArgument, "invalid flush area or buffer");
    }

    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    auto r = wait_pending_locked();
    if (!r) {
        return r;
    }
    // Drop a stray give (e.g. from a transfer issued outside this class) so
    // the next wait only ever matches this call's own transfer.
    xSemaphoreTake(done_, 0);

    esp_err_t err =
        esp_lcd_panel_draw_bitmap(panel_, area.x1, area.y1, area.x2 + 1, area.y2 + 1, rgb565);
    if (err != ESP_OK) {
        // No pending_: a partially queued transfer never fires
        // on_color_trans_done (it only fires for the last chunk). The next
        // panel IO call waits for whatever chunks were queued itself;
        // drain_ makes wait_flush() do so too, before a caller frees the
        // buffer.
        drain_ = true;
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("draw_bitmap: ") + esp_err_to_name(err));
    }
    pending_ = true;
    return Result<void>::ok();
}

Result<void> EspDisplay::wait_flush() {
    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    return wait_pending_locked();
}

Result<void> EspDisplay::wait_pending_locked() {
    if (drain_) {
        // No command and no data: esp_lcd only waits for every queued
        // transaction (the -1 command is documented as "none"), sending
        // nothing to the panel.
        drain_ = false;
        esp_err_t err = esp_lcd_panel_io_tx_param(io_, -1, nullptr, 0);
        if (err != ESP_OK) {
            return Result<void>::err(ErrorCode::DeviceFailure,
                                     std::string("drain: ") + esp_err_to_name(err));
        }
    }
    if (!pending_) {
        return Result<void>::ok();
    }
    if (xSemaphoreTake(done_, ms_to_ticks(kFlushTimeoutMs)) != pdTRUE) {
        // pending_ stays set: the next call waits for this transfer again.
        return Result<void>::err(ErrorCode::Timeout, "flush transfer did not complete");
    }
    pending_ = false;
    return Result<void>::ok();
}

Result<void> EspDisplay::tx_dcs_locked(int reg, const uint8_t* param, size_t param_len) {
    int cmd = reg;
    if (config_.qspi_cmd_framing) {
        cmd = static_cast<int>((kQspiOpcodeWriteCmd << 24) | (static_cast<uint32_t>(reg) << 8));
    }
    esp_err_t err = esp_lcd_panel_io_tx_param(io_, cmd, param, param_len);
    if (err != ESP_OK) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "tx_param(0x%02x): %s", reg, esp_err_to_name(err));
        return Result<void>::err(ErrorCode::DeviceFailure, msg);
    }
    return Result<void>::ok();
}

Result<void> EspDisplay::set_brightness(uint8_t level) {
    assert_owner_thread();
    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    auto r = wait_pending_locked();
    if (!r) {
        return r;
    }
    r = tx_dcs_locked(kDcsWriteBrightness, &level, 1);
    if (r) {
        last_brightness_ = level;  // re-applied by the next wake; see set_power().
    }
    return r;
}

Result<void> EspDisplay::set_power_vendor_locked(bool on) {
    // Spec 6.7: sleeping is "panel sleep", not just
    // "display off" -- turn the pixel output off first, then put the
    // controller itself to sleep (co5300: SLPIN, settling, and -- on a
    // board wired with a reset GPIO -- deep standby); waking
    // reverses that order, waking the controller before turning pixel
    // output back on. Either op can fail with ESP_ERR_NOT_SUPPORTED on a
    // panel driver that doesn't implement disp_sleep; that's a real
    // DeviceFailure here (unlike touch and IMU drivers, there's no
    // "Unsupported is expected" carve-out for the display -- every panel
    // this codebase targets does implement it; see
    // docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md).
    esp_err_t err;
    if (on) {
        err = esp_lcd_panel_disp_sleep(panel_, false);
        if (err != ESP_OK) {
            return Result<void>::err(ErrorCode::DeviceFailure,
                                     std::string("disp_sleep(false): ") + esp_err_to_name(err));
        }
        err = esp_lcd_panel_disp_on_off(panel_, true);
        if (err != ESP_OK) {
            return Result<void>::err(ErrorCode::DeviceFailure,
                                     std::string("disp_on_off(true): ") + esp_err_to_name(err));
        }
    } else {
        err = esp_lcd_panel_disp_on_off(panel_, false);
        if (err != ESP_OK) {
            return Result<void>::err(ErrorCode::DeviceFailure,
                                     std::string("disp_on_off(false): ") + esp_err_to_name(err));
        }
        err = esp_lcd_panel_disp_sleep(panel_, true);
        if (err != ESP_OK) {
            return Result<void>::err(ErrorCode::DeviceFailure,
                                     std::string("disp_sleep(true): ") + esp_err_to_name(err));
        }
    }
    return Result<void>::ok();
}

Result<void> EspDisplay::set_power_dcs_locked(bool on) {
    // EspDisplayConfig::PanelSleep::DcsSleep: ordinary MIPI DCS sleep
    // sent directly via tx_dcs_locked(), never
    // esp_lcd_panel_disp_sleep()/disp_on_off() (see that enum value's
    // doc comment for why the two must never be mixed on one panel).
    if (on) {
        // Enforce sleep_out_settle_ms as the minimum spacing between
        // the last SLPIN and this SLPOUT -- see
        // EspDisplayConfig::sleep_out_settle_ms's doc comment. Only
        // matters for a rapid sleep-then-immediately-wake (e.g. two
        // power-key presses within the ~50ms standby poll interval);
        // last_sleep_in_ms_ is otherwise unset (the common case: no
        // SLPIN since construction, or long enough ago that there is
        // nothing left to wait out).
        if (last_sleep_in_ms_.has_value()) {
            uint64_t elapsed = wisp::os::now_ms() - *last_sleep_in_ms_;
            if (elapsed < config_.sleep_out_settle_ms) {
                wisp::os::sleep_ms(static_cast<uint32_t>(config_.sleep_out_settle_ms - elapsed));
            }
            last_sleep_in_ms_.reset();
        }
        auto r = tx_dcs_locked(kDcsSleepOut);
        if (!r) {
            return r;
        }
        wisp::os::sleep_ms(config_.sleep_out_settle_ms);
        return tx_dcs_locked(kDcsDispOn);
    }
    auto r = tx_dcs_locked(kDcsDispOff);
    if (!r) {
        return r;
    }
    r = tx_dcs_locked(kDcsSleepIn);
    if (r) {
        last_sleep_in_ms_ = wisp::os::now_ms();
    }
    return r;
}

Result<void> EspDisplay::set_power(wisp::hal::PowerLevel level) {
    assert_owner_thread();
    const bool on = wisp::hal::powered_at(wisp::hal::StandbyPolicy::Off, level);
    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    if (on == powered_) {
        return Result<void>::ok();
    }
    // A stripe may still be on the wire; the panel must not go to sleep
    // halfway through it.
    Result<void> r = wait_pending_locked();
    if (!r) {
        return r;
    }
    r = config_.panel_sleep == EspDisplayConfig::PanelSleep::DcsSleep ? set_power_dcs_locked(on)
                                                                      : set_power_vendor_locked(on);
    if (!r) {
        return r;
    }
    powered_ = on;

    // Re-apply the last brightness the caller asked for (some panels
    // reset their brightness register across a sleep/wake cycle). Not
    // fatal to set_power() itself if this fails -- the
    // panel is already awake and displaying, just possibly at the wrong
    // brightness.
    if (on && last_brightness_.has_value()) {
        auto br = tx_dcs_locked(kDcsWriteBrightness, &*last_brightness_, 1);
        if (!br) {
            WLOG_W(kTag, "re-apply brightness 0x%02x after wake failed: %s", *last_brightness_,
                   br.error().message.c_str());
        }
    }
    return Result<void>::ok();
}

bool EspDisplay::powered() const {
    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    return powered_;
}

}  // namespace wisp::port::esp
