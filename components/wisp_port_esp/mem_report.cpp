#include "wisp/port/esp/mem_report.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/freertos_debug.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

namespace wisp::port::esp {

namespace {

constexpr size_t kMaxTasks = 48;

struct TaskRow {
    char name[configMAX_TASK_NAME_LEN];
    size_t stack_bytes;
    size_t high_water;
    bool external;
};

void append(std::string& out, const char* fmt, auto... args) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), fmt, args...);
    out += buf;
}

void append_heap(std::string& out, const char* label, uint32_t caps) {
    append(out, "%s free=%u largest=%u min=%u", label,
           static_cast<unsigned>(heap_caps_get_free_size(caps)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(caps)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)));
}

}  // namespace

std::string mem_report() {
    std::string out;
    append_heap(out, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out += '\n';
    append_heap(out, "dma", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    out += '\n';
    append_heap(out, "psram", MALLOC_CAP_SPIRAM);

    // Snapshot first, format afterwards: nothing that allocates or blocks
    // may run while the scheduler is suspended. The rows live off internal
    // RAM where there is PSRAM, since internal RAM is what this reports on.
    static TaskRow* rows = [] {
        void* p = heap_caps_malloc(sizeof(TaskRow) * kMaxTasks, MALLOC_CAP_SPIRAM);
        if (p == nullptr) p = heap_caps_malloc(sizeof(TaskRow) * kMaxTasks, MALLOC_CAP_DEFAULT);
        return static_cast<TaskRow*>(p);
    }();
    if (rows == nullptr) return out;
    size_t count = 0;
    vTaskSuspendAll();
    TaskIterator_t it = {};
    while (count < kMaxTasks && xTaskGetNext(&it) != -1) {
        TaskHandle_t handle = it.pxTaskHandle;
        if (handle == nullptr) continue;
        TaskSnapshot_t snap = {};
        if (vTaskGetSnapshot(handle, &snap) != pdTRUE) continue;
        StackType_t* start = xTaskGetStackStart(handle);
        TaskRow& row = rows[count++];
        std::strncpy(row.name, pcTaskGetName(handle), sizeof(row.name) - 1);
        row.name[sizeof(row.name) - 1] = '\0';
        row.stack_bytes = static_cast<size_t>(reinterpret_cast<uint8_t*>(snap.pxEndOfStack) -
                                              reinterpret_cast<uint8_t*>(start)) +
                          sizeof(StackType_t);
        row.high_water = uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t);
        row.external = esp_ptr_external_ram(start);
    }
    xTaskResumeAll();

    for (size_t i = 0; i < count; ++i) {
        const TaskRow& row = rows[i];
        append(out, "\ntask %-16s stack=%5u hwm=%5u %s", row.name,
               static_cast<unsigned>(row.stack_bytes), static_cast<unsigned>(row.high_water),
               row.external ? "psram" : "internal");
    }
    return out;
}

}  // namespace wisp::port::esp
