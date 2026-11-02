#include "wisp/port/esp/esp_display.hpp"

#include <mutex>
#include <string>
#include <utility>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "wisp/base/error.hpp"

namespace wisp::port::esp {

namespace {

// Upper bound on one flush()'s wait for the color-transfer-done callback.
// A full 480x480 RGB565 frame at 40 MHz QSPI takes ~12 ms, so this only
// trips if the panel IO is genuinely stuck.
constexpr uint32_t kFlushTimeoutMs = 250;

// MIPI DCS "write display brightness".
constexpr int kDcsWriteBrightness = 0x51;
// esp_lcd QSPI panel drivers' "write command" opcode (see e.g.
// esp_lcd_co5300's tx_param()).
constexpr uint32_t kQspiOpcodeWriteCmd = 0x02;

TickType_t ms_to_ticks(uint32_t ms) {
    // Round up so a short timeout never becomes a zero-tick (non-blocking)
    // wait.
    return static_cast<TickType_t>((static_cast<uint64_t>(ms) * configTICK_RATE_HZ + 999) / 1000);
}

void swap_bytes(uint16_t* px, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        px[i] = static_cast<uint16_t>((px[i] << 8) | (px[i] >> 8));
    }
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
    };
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
    if (rgb565 == nullptr || area.x1 > area.x2 || area.y1 > area.y2 || area.x1 < 0 || area.y1 < 0 ||
        area.x2 >= config_.width || area.y2 >= config_.height) {
        return Result<void>::err(ErrorCode::InvalidArgument, "invalid flush area or buffer");
    }

    std::lock_guard<wisp::os::Mutex> lock(mutex_);

    if (pending_) {
        if (xSemaphoreTake(done_, ms_to_ticks(kFlushTimeoutMs)) != pdTRUE) {
            return Result<void>::err(ErrorCode::Timeout, "previous flush still in flight");
        }
        pending_ = false;
    }
    // Drop a stray give (e.g. from a transfer issued outside flush()) so
    // the wait below only ever matches this call's own transfer.
    xSemaphoreTake(done_, 0);

    size_t count = static_cast<size_t>(area.x2 - area.x1 + 1) * (area.y2 - area.y1 + 1);
    // The Display contract hands us a const buffer; it is only mutated
    // for the duration of this (synchronous) call and restored below, so
    // the caller never observes the change.
    auto* px = const_cast<uint16_t*>(rgb565);
    if (config_.swap_rgb565_bytes) {
        swap_bytes(px, count);
    }

    esp_err_t err =
        esp_lcd_panel_draw_bitmap(panel_, area.x1, area.y1, area.x2 + 1, area.y2 + 1, px);
    if (err != ESP_OK) {
        if (config_.swap_rgb565_bytes) {
            swap_bytes(px, count);
        }
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("draw_bitmap: ") + esp_err_to_name(err));
    }

    if (xSemaphoreTake(done_, ms_to_ticks(kFlushTimeoutMs)) != pdTRUE) {
        pending_ = true;
        return Result<void>::err(ErrorCode::Timeout, "flush transfer did not complete");
    }

    if (config_.swap_rgb565_bytes) {
        swap_bytes(px, count);
    }
    return Result<void>::ok();
}

Result<void> EspDisplay::set_brightness(uint8_t level) {
    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    int cmd = kDcsWriteBrightness;
    if (config_.qspi_cmd_framing) {
        cmd = static_cast<int>((kQspiOpcodeWriteCmd << 24) |
                               (static_cast<uint32_t>(kDcsWriteBrightness) << 8));
    }
    esp_err_t err = esp_lcd_panel_io_tx_param(io_, cmd, &level, 1);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("tx_param(0x51): ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

Result<void> EspDisplay::set_power(bool on) {
    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    esp_err_t err = esp_lcd_panel_disp_on_off(panel_, on);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("disp_on_off: ") + esp_err_to_name(err));
    }
    powered_ = on;
    return Result<void>::ok();
}

bool EspDisplay::powered() const {
    std::lock_guard<wisp::os::Mutex> lock(mutex_);
    return powered_;
}

}  // namespace wisp::port::esp
