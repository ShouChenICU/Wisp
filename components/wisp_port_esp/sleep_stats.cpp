#include "wisp/port/esp/sleep_stats.hpp"

#include <cstdio>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/sleep_record.hpp"

#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif

namespace wisp::port::esp {

namespace {

namespace sr = sleep_record;

// Survives a chip reset (not power loss); sleep_record::boot() sorts out
// what it holds at power-on.
RTC_NOINIT_ATTR SleepRecord s_record;

// Every change to s_record outside the light-sleep callback happens with
// this held: while a no-light-sleep lock is held the chip cannot enter
// light sleep, and taking it waits for ESP-IDF's sleep path (which holds
// the PM switch lock for the whole sleep, callbacks included) to finish,
// so the callback never runs concurrently with those changes.
esp_pm_lock_handle_t s_lock = nullptr;

struct Hold {
    Hold() {
        if (s_lock != nullptr) esp_pm_lock_acquire(s_lock);
    }
    ~Hold() {
        if (s_lock != nullptr) esp_pm_lock_release(s_lock);
    }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// Runs in the idle task with interrupts off, after every pass through the
// automatic light-sleep path (slept_us is 0 when it did not try). Not
// IRAM_ATTR: with CONFIG_PM_SLP_IRAM_OPT off the sleep path that calls it
// runs from flash itself, and the cache is on whenever it runs.
esp_err_t on_light_sleep_exit(int64_t slept_us, void*) {
    if (slept_us > 0 && (s_record.current.flags & sr::kActive)) {
        sr::note_sleep(s_record.current, esp_timer_get_time(), slept_us,
                       esp_sleep_get_wakeup_causes());
    }
    return ESP_OK;
}
#endif

const char* cause_name(int bit) {
    switch (bit) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return "undefined";
        case ESP_SLEEP_WAKEUP_EXT0:
            return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:
            return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:
            return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:
            return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:
            return "gpio";
        case ESP_SLEEP_WAKEUP_UART:
            return "uart0";
        case ESP_SLEEP_WAKEUP_UART1:
            return "uart1";
        case ESP_SLEEP_WAKEUP_UART2:
            return "uart2";
        case ESP_SLEEP_WAKEUP_WIFI:
            return "wifi";
        case ESP_SLEEP_WAKEUP_COCPU:
            return "cocpu";
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
            return "cocpu_trap";
        case ESP_SLEEP_WAKEUP_BT:
            return "bt";
        case ESP_SLEEP_WAKEUP_VBAT_UNDER_VOLT:
            return "vbat_low";
        case ESP_SLEEP_WAKEUP_USB:
            return "usb";
        default:
            return nullptr;
    }
}

SleepBattery read_battery(hal::Power* power) {
    SleepBattery b{};
    if (power == nullptr) return b;
    auto status = power->status();
    if (!status) return b;
    b.valid = 1;
    b.percent = static_cast<int16_t>(status.value().battery_percent);
    b.mv = static_cast<uint16_t>(status.value().battery_mv);
    b.charging = status.value().charging ? 1 : 0;
    b.usb_present = status.value().usb_present ? 1 : 0;
    return b;
}

bool usj_connected() {
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
    return usb_serial_jtag_is_connected();
#else
    return false;
#endif
}

// esp_pm_dump_locks() only writes to a FILE; this captures it. The buffer
// stays off internal RAM where there is PSRAM.
std::string pm_dump() {
    constexpr size_t kDumpBytes = 4096;
    static char* buf = [] {
        void* p = heap_caps_malloc(kDumpBytes, MALLOC_CAP_SPIRAM);
        if (p == nullptr) p = heap_caps_malloc(kDumpBytes, MALLOC_CAP_DEFAULT);
        return static_cast<char*>(p);
    }();
    if (buf == nullptr) return "pm: no memory for the dump\n";
    FILE* f = fmemopen(buf, kDumpBytes, "w");
    if (f == nullptr) return "pm: fmemopen failed\n";
    esp_pm_dump_locks(f);
    const long len = ftell(f);
    std::fclose(f);
    if (len <= 0) return {};
    return std::string(
        buf, static_cast<size_t>(len < static_cast<long>(kDumpBytes) ? len : kDumpBytes - 1));
}

}  // namespace

void sleep_stats_init() {
    if (s_lock == nullptr) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sleepstat", &s_lock);
        if (err != ESP_OK) {
            WLOG_W("power", "sleep stats: PM lock: %s", esp_err_to_name(err));
            s_lock = nullptr;
        }
    }
    const bool was_running = sr::valid(s_record) && (s_record.current.flags & sr::kActive);
    {
        Hold hold;
        sr::boot(s_record);
    }
    if (was_running) {
        WLOG_I("power", "%s (ended by a reset)", sr::summary(s_record.last, cause_name).c_str());
    }
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = {};
    cbs.exit_cb = on_light_sleep_exit;
    esp_err_t err = esp_pm_light_sleep_register_cbs(&cbs);
    if (err != ESP_OK) {
        WLOG_W("power", "sleep stats: light-sleep callback: %s", esp_err_to_name(err));
    }
#endif
}

void sleep_stats_begin(hal::Power* power) {
    const SleepBattery battery = read_battery(power);
    const bool usj = usj_connected();
    Hold hold;
    sr::begin(s_record, esp_timer_get_time(), battery, usj);
}

void sleep_stats_end(hal::Power* power) {
    if (!(s_record.current.flags & sr::kActive)) return;
    const SleepBattery battery = read_battery(power);
    bool ended = false;
    {
        Hold hold;
        ended = sr::end(s_record, esp_timer_get_time(), battery);
    }
    if (ended) {
        WLOG_I("power", "%s", sr::summary(s_record.last, cause_name).c_str());
    }
}

std::string sleep_stats_report() {
    SleepRecord copy;
    {
        Hold hold;
        copy = s_record;
    }
    std::string out;
#if !CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    out += "(CONFIG_PM_LIGHT_SLEEP_CALLBACKS is off: sleeps are not counted)\n";
#endif
    out += sr::report(copy.current, "current session", cause_name);
    out += sr::report(copy.last, "last session", cause_name);
    out += pm_dump();
    // The console prints the reply with a newline of its own.
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

void sleep_stats_reset() {
    Hold hold;
    const bool running = (s_record.current.flags & sr::kActive) != 0;
    const bool usj = (s_record.current.flags & sr::kUsjAtStart) != 0;
    sr::init(s_record);
    if (running) sr::begin(s_record, esp_timer_get_time(), SleepBattery{}, usj);
}

}  // namespace wisp::port::esp
