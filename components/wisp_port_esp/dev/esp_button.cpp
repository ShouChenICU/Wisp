#include "wisp/port/esp/esp_button.hpp"

#include <mutex>
#include <new>
#include <utility>

#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_ipc.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/soc_caps.h"
#include "wisp/core/log.hpp"

namespace wisp::port::esp {
namespace {
constexpr const char* kTag = "button";
os::Mutex service_mutex;
int service_core = -1;
}  // namespace

struct EspGpioButton::Input {
    Input(gpio_num_t input_pin, bool low, hal::ButtonEdgeSink edge_sink)
        : pin(input_pin), active_low(low), sink(edge_sink) {}

    gpio_num_t pin;
    bool active_low;
    hal::ButtonEdgeSink sink{};
    esp_pm_lock_handle_t sleep_lock = nullptr;
    ButtonWakeState wake;
    bool attached = false;
    bool warned = false;
    esp_err_t registration_error = ESP_OK;

    static void operator delete(void* ptr) noexcept { os::dealloc(ptr); }

    static void on_edge(void* context);

    static void add_on_core(void* context) {
        auto* input = static_cast<Input*>(context);
        input->registration_error = gpio_set_intr_type(input->pin, GPIO_INTR_ANYEDGE);
        if (input->registration_error == ESP_OK) {
            input->registration_error = gpio_isr_handler_add(input->pin, on_edge, input);
        }
        input->attached = input->registration_error == ESP_OK;
    }

    static void remove_on_core(void* context) {
        auto* input = static_cast<Input*>(context);
        // IDF's remove alone does not wait for a callback on the other core.
        // Running removal on the interrupt's core means its old ISR has
        // finished before this task can execute, and no new one can start.
        ESP_ERROR_CHECK(gpio_isr_handler_remove(input->pin));
        input->attached = false;
        input->sink = {};
    }

    bool lock_sleep() { return esp_pm_lock_acquire(sleep_lock) == ESP_OK; }
    void unlock_sleep() { ESP_ERROR_CHECK(esp_pm_lock_release(sleep_lock)); }
    bool enable_wake() {
#if SOC_RTCIO_WAKE_SUPPORTED
        // gpio_wakeup_enable() also overwrites the digital IRQ type. The
        // independent RTC IO level detector preserves ANYEDGE while awake.
        return rtc_gpio_wakeup_enable(
                   pin, active_low ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL) == ESP_OK;
#else
        return false;
#endif
    }
    bool disable_wake() {
#if SOC_RTCIO_WAKE_SUPPORTED
        return rtc_gpio_wakeup_disable(pin) == ESP_OK;
#else
        return false;
#endif
    }
};

void IRAM_ATTR EspGpioButton::Input::on_edge(void* context) {
    auto* input = static_cast<Input*>(context);
    const bool high = gpio_ll_get_level(&GPIO, input->pin) != 0;
    const bool down = high != input->active_low;
    input->sink.push(input->sink.context, down, static_cast<uint64_t>(esp_timer_get_time()));
}

Result<void> EspGpioButton::prepare_edge_input() {
    std::lock_guard<os::Mutex> lock(service_mutex);
    if (service_core >= 0) return Result<void>::ok();
    // A service installed without IRAM flags cannot promise cache-off delivery.
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1);
    if (err != ESP_OK) return Result<void>::err(ErrorCode::IoError, esp_err_to_name(err));
    service_core = xPortGetCoreID();
    return Result<void>::ok();
}

EspGpioButton::EspGpioButton(gpio_num_t pin, bool active_low, std::string id, std::string model,
                             std::string role, hal::ButtonInputMode mode)
    : pin_(pin), active_low_(active_low), mode_(mode) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{{"standby", wisp::Value("wake_source")}};
    if (!role.empty()) info_.caps.emplace("role", wisp::Value(std::move(role)));
}

EspGpioButton::~EspGpioButton() { detach(); }

void EspGpioButton::detach() {
    if (!input_) return;
    if (input_->attached) {
        ESP_ERROR_CHECK(esp_ipc_call_blocking(service_core, Input::remove_on_core, input_.get()));
    }
    ESP_ERROR_CHECK(input_->wake.reset(*input_) ? ESP_OK : ESP_FAIL);
    if (input_->sleep_lock != nullptr) ESP_ERROR_CHECK(esp_pm_lock_delete(input_->sleep_lock));
    input_.reset();
}

Result<void> EspGpioButton::set_edge_sink(hal::ButtonEdgeSink sink) {
    std::lock_guard<os::Mutex> lock(mutex_);
    detach();
    if (sink.push == nullptr) return Result<void>::ok();
    if (mode_ != hal::ButtonInputMode::EdgeEvent) return Result<void>::err(ErrorCode::Unsupported);
#if SOC_RTCIO_WAKE_SUPPORTED
    if (!rtc_gpio_is_valid_gpio(pin_)) {
        return Result<void>::err(ErrorCode::Unsupported, "independent level wake unavailable");
    }
#else
    return Result<void>::err(ErrorCode::Unsupported, "independent level wake unavailable");
#endif
    auto prepared = prepare_edge_input();
    if (!prepared) return prepared;
    void* memory = os::alloc(sizeof(Input), os::Mem::Fast);
    if (memory == nullptr) return Result<void>::err(ErrorCode::NoMemory);
    input_.reset(new (memory) Input{pin_, active_low_, sink});
    esp_err_t err =
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "button-held", &input_->sleep_lock);
    if (err != ESP_OK) {
        input_.reset();
        return Result<void>::err(ErrorCode::IoError, esp_err_to_name(err));
    }
    err = gpio_sleep_sel_dis(pin_);
    if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
    if (err == ESP_OK && !input_->wake.update(pressed(), *input_)) err = ESP_FAIL;
    if (err == ESP_OK) {
        err = esp_ipc_call_blocking(service_core, Input::add_on_core, input_.get());
        if (err == ESP_OK) err = input_->registration_error;
    }
    if (err != ESP_OK) {
        detach();
        return Result<void>::err(ErrorCode::IoError, esp_err_to_name(err));
    }
    // Recheck after installing the ISR, closing the sample/attach interval.
    if (!input_->wake.update(pressed(), *input_)) {
        detach();
        return Result<void>::err(ErrorCode::IoError, "GPIO wake setup failed");
    }
    return Result<void>::ok();
}

void EspGpioButton::service_input() {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (input_ && !input_->wake.update(pressed(), *input_) && !input_->warned) {
        input_->warned = true;
        WLOG_W(kTag, "%s: GPIO wake maintenance failed", info_.id.c_str());
    }
}

Result<void> EspGpioButton::set_power(wisp::hal::PowerLevel /*level*/) {
    service_input();
    return Result<void>::ok();
}

bool EspGpioButton::pressed() { return button_pressed(gpio_get_level(pin_) != 0, active_low_); }

EspPmuPowerKey::EspPmuPowerKey(std::shared_ptr<EspPower> power, std::string id, std::string model)
    : power_(std::move(power)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{{"standby", wisp::Value("wake_source")}};
    esp_err_t err = power_->enable_power_key_irqs();
    if (err != ESP_OK) {
        WLOG_W(kTag, "%s: enabling the PWRON IRQs failed: %s", info_.id.c_str(),
               esp_err_to_name(err));
    }
}

Result<void> EspPmuPowerKey::set_power(wisp::hal::PowerLevel /*level*/) {
    return Result<void>::ok();
}

bool EspPmuPowerKey::pressed() {
    // The read and the update stay under one lock: bits taken by one
    // caller must reach the tracker before another caller's later bits.
    mutex_.lock();
    uint8_t bits = 0;
    const bool read_ok = power_->take_power_key_irqs(&bits) == ESP_OK;
    const bool down = read_ok ? axp2101_pwrkey_update(&tracker_, bits) : tracker_.down;
    mutex_.unlock();
    return down;
}

}  // namespace wisp::port::esp
